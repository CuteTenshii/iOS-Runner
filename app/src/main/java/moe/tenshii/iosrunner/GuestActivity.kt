package moe.tenshii.iosrunner

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.inputmethod.EditorInfo
import android.webkit.CookieManager
import android.webkit.JavascriptInterface
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.util.Collections
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

/** Hosts the guest app in the isolated :engine process and draws the view tree it reports. */
class GuestActivity : Activity() {
    private lateinit var root: FrameLayout
    private var tree = JSONArray()
    private var selectedTab = 0
    private val expanded = HashSet<String>()
    private val scrollPositions = HashMap<String, Int>()
    private var sheetDialog: Dialog? = null
    private val webViews = HashMap<Int, WebView>()
    private val pendingScripts: MutableSet<Int> = Collections.newSetFromMap(ConcurrentHashMap())
    private val mainThread = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        root = FrameLayout(this)
        applySystemBarInsets(root)
        setContentView(root)
        showMessage(getString(R.string.status_starting))

        current = this
        recordUncaughtExceptions()
        val found = AppLibrary(this).find(intent.getStringExtra(EXTRA_APP).orEmpty())
        if (found == null) {
            finish()
            return
        }
        val logPath = logFile(this).path
        val language = preferredLanguage(found.directory)
        Thread({ nativeRun(found.directory.path, found.executable, logPath, language) }, "guest-launcher").start()
    }

    // logcat can be empty on some phones, and the engine's log is where a failed run gets read.
    private fun recordUncaughtExceptions() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        val log = java.io.File(cacheDir, "engine.log")
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching { log.appendText("HOST EXCEPTION on ${thread.name}: ${error.stackTraceToString()}\n") }
            previous?.uncaughtException(thread, error)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // The guest cannot be unloaded, so leaving the app ends its process.
        if (isFinishing) Process.killProcess(Process.myPid())
    }

    private fun preferredLanguage(bundle: File): String {
        val language = Locale.getDefault().language
        return if (File(bundle, "$language.lproj").isDirectory) language else FALLBACK_LANGUAGE
    }

    private fun receive(kind: String, payload: String) {
        when (kind) {
            "tree" -> {
                tree = JSONArray(payload)
                draw()
            }
            "open" -> openExternal(payload)
            "webnew" -> createWebView(JSONObject(payload))
            "webload" -> JSONObject(payload).let { webViews[it.getInt("id")]?.loadUrl(it.getString("url")) }
            "webjs" -> runScript(JSONObject(payload))
        }
    }

    private fun createWebView(spec: JSONObject) {
        val id = spec.getInt("id")
        val scripts = spec.getJSONArray("scripts").let { array -> List(array.length()) { array.getString(it) } }
        val web = WebView(this)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true
        // WKWebView's default agent plus the app's own suffix, which it relies on to be served the mobile sites.
        web.settings.userAgentString = "$IOS_WEBKIT_AGENT ${spec.optString("name")}".trim()
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, true)
        web.addJavascriptInterface(ScriptBridge(), SCRIPT_BRIDGE)
        web.webViewClient = object : WebViewClient() {
            override fun onPageStarted(view: WebView, url: String, favicon: Bitmap?) {
                // Without AndroidX there is no true document-start injection, so this is as early as it gets.
                scripts.forEach { view.evaluateJavascript(it, null) }
                nativeUrlChanged(id, url)
            }

            override fun onPageFinished(view: WebView, url: String) {
                rememberCookieHost(url)
                nativeUrlChanged(id, url)
                nativeCookiesChanged()
            }
        }
        webViews[id] = web
    }

    private fun rememberCookieHost(url: String) {
        val host = Uri.parse(url).host ?: return
        val preferences = getSharedPreferences(COOKIE_PREFERENCES, MODE_PRIVATE)
        val hosts = preferences.getStringSet(COOKIE_HOSTS, emptySet()).orEmpty()
        if (host !in hosts) preferences.edit().putStringSet(COOKIE_HOSTS, hosts + host).apply()
    }

    private fun runScript(call: JSONObject) {
        val id = call.getInt("call")
        val web = webViews[call.getInt("id")]
        if (web == null) {
            nativeScriptFinished(id, "", true)
            return
        }
        val arguments = call.getJSONObject("args")
        val names = arguments.keys().asSequence().toList()
        val values = names.joinToString(",") { name ->
            val value = arguments.get(name)
            if (value is String) JSONObject.quote(value) else value.toString()
        }
        val script = "(async function(${names.joinToString(",")}){${call.getString("body")}}).call(null,$values)" +
            ".then(r=>$SCRIPT_BRIDGE.finish($id,JSON.stringify(r===undefined?null:r)),e=>$SCRIPT_BRIDGE.fail($id));"
        pendingScripts.add(id)
        web.evaluateJavascript(script, null)
        // A page mid-navigation may never answer, and the app polls with its own deadlines.
        mainThread.postDelayed({ if (pendingScripts.remove(id)) nativeScriptFinished(id, "", true) }, SCRIPT_TIMEOUT_MS)
    }

    private inner class ScriptBridge {
        @JavascriptInterface
        fun finish(id: Int, json: String?) {
            if (pendingScripts.remove(id)) nativeScriptFinished(id, json ?: "null", false)
        }

        @JavascriptInterface
        fun fail(id: Int) {
            if (pendingScripts.remove(id)) nativeScriptFinished(id, "", true)
        }
    }

    private fun detachedWebView(id: Int): WebView? {
        val web = webViews[id] ?: return null
        (web.parent as? ViewGroup)?.removeView(web)
        return web
    }

    private fun openExternal(url: String) {
        try {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: ActivityNotFoundException) {
            showToastless(getString(R.string.no_browser, url))
        }
    }

    private fun showToastless(text: String) {
        AlertDialog.Builder(this).setMessage(text).setPositiveButton(android.R.string.ok, null).show()
    }

    private fun showMessage(text: String) {
        root.removeAllViews()
        root.addView(TextView(this).apply {
            this.text = text
            gravity = Gravity.CENTER
        }, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    }

    private fun draw() {
        rememberScroll(root)
        webViews.values.forEach { (it.parent as? ViewGroup)?.removeView(it) }
        root.removeAllViews()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(themeColor(android.R.attr.colorBackground))
        }
        forEachChild(tree) { node ->
            when (node.optString("k")) {
                "sheet" -> Unit
                // A top-level web view works behind the UI: its page still needs real layout to run.
                "native" -> detachedWebView(node.optInt("w", -1))?.let { web ->
                    root.addView(web, FrameLayout.LayoutParams(dp(HIDDEN_WEB_WIDTH_DP), dp(HIDDEN_WEB_HEIGHT_DP)))
                }
                else -> build(node, column, "r")
            }
        }
        root.addView(column, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        restoreScroll(root)
        showSheet(findSheet(tree))
    }

    private fun findSheet(nodes: JSONArray): JSONObject? {
        for (i in 0 until nodes.length()) {
            val node = nodes.getJSONObject(i)
            if (node.optString("k") == "sheet") return node
            node.optJSONArray("c")?.let { children -> findSheet(children)?.let { return it } }
        }
        return null
    }

    private fun showSheet(sheet: JSONObject?) {
        if (sheet == null) {
            sheetDialog?.setOnCancelListener(null)
            sheetDialog?.dismiss()
            sheetDialog = null
            return
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(themeColor(android.R.attr.colorBackground))
        }
        applySystemBarInsets(content)
        children(sheet).forEach { build(it, content, "sheet") }
        val action = sheet.optInt("d", -1)
        val dialog = sheetDialog ?: Dialog(this, android.R.style.Theme_DeviceDefault_NoActionBar).also { sheetDialog = it }
        dialog.setOnCancelListener { if (action >= 0) nativeAction(action) }
        dialog.setContentView(content, ViewGroup.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        if (!dialog.isShowing) dialog.show()
    }

    private fun build(node: JSONObject, parent: ViewGroup, path: String) {
        val kind = node.optString("k")
        val key = "$path/$kind"
        when (kind) {
            "tabs" -> buildTabs(node, parent, key)
            "nav" -> buildNavigation(node, parent, key)
            "titled", "toolbar", "tab", "toolbaritem" -> buildChildren(node, parent, key)
            "list" -> buildList(node, parent, key)
            "section" -> buildSection(node, parent, key)
            "v" -> parent.addView(LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                buildChildren(node, this, key)
            })
            "h" -> parent.addView(buildRow(node, key))
            "text" -> parent.addView(buildText(node))
            "image" -> buildImage(node)?.let { parent.addView(it) }
            "spacer" -> parent.addView(View(this), LinearLayout.LayoutParams(0, 0, 1f))
            "button" -> parent.addView(buildButton(node, key))
            "toggle" -> parent.addView(buildToggle(node, key))
            "field" -> parent.addView(buildField(node))
            "stepper" -> parent.addView(buildStepper(node, key))
            "picker" -> parent.addView(buildPicker(node))
            "labeled" -> parent.addView(buildLabeled(node, key))
            "menu" -> parent.addView(buildMenu(node, key))
            "disclosure" -> buildDisclosure(node, parent, key)
            "progress" -> parent.addView(ProgressBar(this), LinearLayout.LayoutParams(dp(24), dp(24)))
            "unavailable" -> parent.addView(buildUnavailable(node))
            "native" -> detachedWebView(node.optInt("w", -1))?.let { parent.addView(it, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f)) }
            else -> buildChildren(node, parent, key)
        }
    }

    private fun buildChildren(node: JSONObject, parent: ViewGroup, path: String) {
        children(node).forEachIndexed { index, child -> if (child.optString("k") != "sheet") build(child, parent, "$path/$index") }
    }

    private fun buildTabs(node: JSONObject, parent: ViewGroup, path: String) {
        val tabs = children(node).flatMap { collectTabs(it) }
        if (tabs.isEmpty()) return
        if (selectedTab >= tabs.size) selectedTab = 0
        val content = FrameLayout(this)
        build(tabs[selectedTab], content, "$path/$selectedTab")
        parent.addView(content, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        tabs.forEachIndexed { index, tab ->
            bar.addView(tabButton(tab, index), LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
        parent.addView(bar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
    }

    private fun collectTabs(node: JSONObject): List<JSONObject> =
        if (node.optString("k") == "tab") listOf(node) else children(node).flatMap { collectTabs(it) }

    private fun tabButton(tab: JSONObject, index: Int): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        minimumHeight = dp(56)
        isClickable = true
        isFocusable = true
        contentDescription = tab.optString("t")
        background = selectableBackground()
        val selected = index == selectedTab
        val color = if (selected) accentColor() else secondaryColor()
        addView(TextView(context).apply {
            showIcon(tab.optString("i"))
            gravity = Gravity.CENTER
            setTextColor(color)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
        })
        addView(TextView(context).apply {
            text = tab.optString("t")
            gravity = Gravity.CENTER
            setTextColor(color)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
        })
        setOnClickListener {
            selectedTab = index
            draw()
        }
    }

    private fun buildNavigation(node: JSONObject, parent: ViewGroup, path: String) {
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(56)
            setPadding(dp(16), 0, dp(8), 0)
        }
        bar.addView(TextView(this).apply {
            text = findTitle(node)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
            typeface = Typeface.DEFAULT_BOLD
            setSingleLine()
        }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        children(node).flatMap { findToolbarItems(it) }.forEachIndexed { index, item -> build(item, bar, "$path/toolbar/$index") }
        column.addView(bar)
        val content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        children(node).forEachIndexed { index, child -> buildNavigationContent(child, content, "$path/$index") }
        column.addView(content, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        parent.addView(column, ViewGroup.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    }

    // A toolbar's first child is the content it decorates; the rest belong in the bar.
    private fun buildNavigationContent(node: JSONObject, parent: ViewGroup, path: String) {
        when (node.optString("k")) {
            "toolbar" -> children(node).firstOrNull()?.let { buildNavigationContent(it, parent, "$path/0") }
            "titled" -> children(node).forEachIndexed { index, child -> buildNavigationContent(child, parent, "$path/$index") }
            else -> build(node, parent, path)
        }
    }

    private fun findTitle(node: JSONObject): String {
        if (node.optString("k") == "titled") return node.optString("t")
        return children(node).firstNotNullOfOrNull { findTitle(it).ifEmpty { null } }.orEmpty()
    }

    private fun findToolbarItems(node: JSONObject): List<JSONObject> = when (node.optString("k")) {
        "toolbar" -> children(node).let { parts -> parts.drop(1) + parts.take(1).flatMap { findToolbarItems(it) } }
        "titled" -> children(node).flatMap { findToolbarItems(it) }
        else -> emptyList()
    }

    private fun buildList(node: JSONObject, parent: ViewGroup, path: String) {
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(4), dp(16), dp(24))
        }
        buildChildren(node, column, path)
        parent.addView(ScrollView(this).apply {
            tag = path
            isFillViewport = true
            addView(column)
        }, ViewGroup.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    }

    private fun buildSection(node: JSONObject, parent: ViewGroup, path: String) {
        val rows = children(node)
        if (rows.isEmpty()) return
        val footer = rows.last().takeIf { rows.size > 1 && it.optString("k") == "text" }
        val group = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(surfaceColor())
                cornerRadius = dp(12).toFloat()
            }
            setPadding(dp(16), dp(4), dp(16), dp(4))
        }
        rows.forEachIndexed { index, row ->
            if (row === footer) return@forEachIndexed
            val before = group.childCount
            build(row, group, "$path/$index")
            for (i in before until group.childCount) group.getChildAt(i).minimumHeight = dp(48)
        }
        if (group.childCount > 0) parent.addView(group, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(16) })
        footer?.let { text ->
            parent.addView(buildText(text).apply {
                setPadding(dp(16), dp(8), dp(16), 0)
                setTextColor(secondaryColor())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            })
        }
    }

    private fun buildRow(node: JSONObject, path: String): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        children(node).forEachIndexed { index, child ->
            val before = childCount
            build(child, this, "$path/$index")
            for (i in before until childCount) {
                val params = getChildAt(i).layoutParams as? LinearLayout.LayoutParams
                if (params != null && child.optString("k") != "spacer") params.marginEnd = dp(8)
            }
        }
    }

    private fun buildText(node: JSONObject): TextView = TextView(this).apply {
        text = node.optString("t")
        setTextSize(TypedValue.COMPLEX_UNIT_SP, when (node.optString("font")) {
            "caption" -> 12f
            "caption2" -> 11f
            "footnote" -> 13f
            else -> 16f
        })
        if (node.optBoolean("bold")) typeface = Typeface.DEFAULT_BOLD
        when (node.optString("color")) {
            "red" -> setTextColor(Color.rgb(0xD7, 0x26, 0x1E))
            "green" -> setTextColor(Color.rgb(0x2E, 0x7D, 0x32))
            "accent" -> setTextColor(accentColor())
            "secondary" -> setTextColor(secondaryColor())
        }
    }

    private fun buildImage(node: JSONObject): View? {
        val name = node.optString("i")
        if (name.isEmpty()) return null
        return TextView(this).apply {
            showIcon(name)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
    }

    private fun buildButton(node: JSONObject, path: String): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(48)
        isClickable = true
        isFocusable = true
        background = selectableBackground()
        val action = node.optInt("a", -1)
        buildChildren(node, this, path)
        tintText(this, accentColor())
        setOnClickListener { if (action >= 0) nativeAction(action) }
    }

    private fun tintText(view: View, color: Int) {
        when (view) {
            is TextView -> view.setTextColor(color)
            is ViewGroup -> for (i in 0 until view.childCount) tintText(view.getChildAt(i), color)
        }
    }

    // The value is sent when editing ends: every update redraws the tree, which would drop focus mid-word.
    private fun buildField(node: JSONObject): View = EditText(this).apply {
        val action = node.optInt("a", -1)
        hint = node.optString("t")
        setText(node.optString("v"))
        inputType = if (node.optBoolean("secure")) {
            InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        } else {
            InputType.TYPE_CLASS_TEXT
        }
        isSingleLine = true
        imeOptions = EditorInfo.IME_ACTION_DONE
        val commit = { if (action >= 0) nativeTextInput(action, text.toString()) }
        setOnFocusChangeListener { _, focused -> if (!focused) commit() }
        setOnEditorActionListener { view, _, _ ->
            commit()
            view.clearFocus()
            false
        }
    }

    private fun buildToggle(node: JSONObject, path: String): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(48)
        val label = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        node.optString("t").takeIf { it.isNotEmpty() }?.let { title -> label.addView(TextView(context).apply { text = title; setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f) }) }
        buildChildren(node, label, path)
        addView(label, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val action = node.optInt("a", -1)
        addView(Switch(context).apply {
            isChecked = node.optBoolean("v")
            setOnClickListener { if (action >= 0) nativeAction(action) }
        })
    }

    private fun buildStepper(node: JSONObject, path: String): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(48)
        val label = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        buildChildren(node, label, path)
        addView(label, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addView(stepButton("−", node.optInt("d", -1)))
        addView(stepButton("+", node.optInt("a", -1)))
    }

    private fun stepButton(text: String, action: Int): View = Button(this).apply {
        this.text = text
        isEnabled = action >= 0
        setOnClickListener { nativeAction(action) }
    }

    private fun buildPicker(node: JSONObject): View {
        val options = collect(node, "option")
        val labelText = children(node).firstOrNull { it.optString("k") == "text" }?.optString("t").orEmpty()
        val selected = options.firstOrNull { it.optBoolean("v") }
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(48)
            isClickable = true
            isFocusable = true
            background = selectableBackground()
            addView(TextView(context).apply {
                text = labelText
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            addView(TextView(context).apply {
                text = selected?.let { collectText(it) }.orEmpty()
                setTextColor(accentColor())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            })
            setOnClickListener {
                val titles = options.map { collectText(it) }.toTypedArray()
                AlertDialog.Builder(context)
                    .setTitle(labelText)
                    .setSingleChoiceItems(titles, options.indexOf(selected)) { dialog, which ->
                        dialog.dismiss()
                        nativeAction(options[which].optInt("a"))
                    }
                    .show()
            }
        }
    }

    private fun buildLabeled(node: JSONObject, path: String): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        minimumHeight = dp(48)
        addView(TextView(context).apply {
            text = node.optString("t")
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val value = node.optString("i")
        if (value.isNotEmpty()) {
            addView(TextView(context).apply {
                text = value
                setTextColor(secondaryColor())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            })
        }
        buildChildren(node, this, path)
    }

    private fun buildMenu(node: JSONObject, path: String): View {
        val parts = children(node)
        val label = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            minimumHeight = dp(48)
            minimumWidth = dp(48)
            isClickable = true
            isFocusable = true
            background = selectableBackground()
        }
        parts.firstOrNull()?.let { build(it, label, "$path/label") }
        tintText(label, accentColor())
        label.setOnClickListener { showMenu(collectText(parts.firstOrNull() ?: node), parts.drop(1)) }
        return label
    }

    private fun showMenu(title: String, entries: List<JSONObject>) {
        val items = entries.flatMap { flattenMenu(it) }
        AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(items.map { menuTitle(it) }.toTypedArray()) { _, which ->
                val item = items[which]
                when (item.optString("k")) {
                    "menu" -> children(item).let { showMenu(collectText(it.first()), it.drop(1)) }
                    "picker" -> buildPicker(item).performClick()
                    else -> nativeAction(item.optInt("a"))
                }
            }
            .show()
    }

    private fun flattenMenu(node: JSONObject): List<JSONObject> =
        if (node.optString("k") in setOf("button", "option", "toggle", "menu", "picker")) listOf(node)
        else children(node).flatMap { flattenMenu(it) }

    private fun menuTitle(node: JSONObject): String {
        val prefix = if (node.optBoolean("v")) "✓ " else ""
        return prefix + if (node.optString("k") == "picker" || node.optString("k") == "menu") {
            children(node).firstOrNull()?.let { collectText(it) }.orEmpty()
        } else {
            node.optString("t").ifEmpty { collectText(node) }
        }
    }

    private fun buildDisclosure(node: JSONObject, parent: ViewGroup, path: String) {
        val parts = children(node)
        val open = path in expanded
        parent.addView(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(48)
            isClickable = true
            isFocusable = true
            background = selectableBackground()
            val label = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            parts.firstOrNull()?.let { build(it, label, "$path/label") }
            addView(label, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            addView(TextView(context).apply {
                text = if (open) "▾" else "▸"
                setTextColor(secondaryColor())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            })
            setOnClickListener {
                if (!expanded.add(path)) expanded.remove(path)
                draw()
            }
        })
        if (open) parts.drop(1).forEachIndexed { index, child -> build(child, parent, "$path/$index") }
    }

    private fun buildUnavailable(node: JSONObject): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        setPadding(dp(24), dp(48), dp(24), dp(48))
        addView(TextView(context).apply {
            showIcon(node.optString("i"))
            gravity = Gravity.CENTER
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 40f)
            setTextColor(secondaryColor())
        })
        addView(TextView(context).apply {
            text = node.optString("t")
            gravity = Gravity.CENTER
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
            typeface = Typeface.DEFAULT_BOLD
        })
        node.optString("s").takeIf { it.isNotEmpty() }?.let { description ->
            addView(TextView(context).apply {
                text = description
                gravity = Gravity.CENTER
                setTextColor(secondaryColor())
            })
        }
    }

    private fun collect(node: JSONObject, kind: String): List<JSONObject> =
        children(node).flatMap { if (it.optString("k") == kind) listOf(it) else collect(it, kind) }

    private fun collectText(node: JSONObject): String {
        if (node.optString("k") == "text") return node.optString("t")
        return children(node).map { collectText(it) }.filter { it.isNotEmpty() }.joinToString(" ")
    }

    private fun children(node: JSONObject): List<JSONObject> {
        val array = node.optJSONArray("c") ?: return emptyList()
        return List(array.length()) { array.getJSONObject(it) }
    }

    private inline fun forEachChild(array: JSONArray, visit: (JSONObject) -> Unit) {
        for (i in 0 until array.length()) visit(array.getJSONObject(i))
    }

    private fun rememberScroll(view: View) {
        if (view is ScrollView) (view.tag as? String)?.let { scrollPositions[it] = view.scrollY }
        if (view is ViewGroup) for (i in 0 until view.childCount) rememberScroll(view.getChildAt(i))
    }

    private fun restoreScroll(view: View) {
        if (view is ScrollView) (view.tag as? String)?.let { key -> scrollPositions[key]?.let { y -> view.post { view.scrollTo(0, y) } } }
        if (view is ViewGroup) for (i in 0 until view.childCount) restoreScroll(view.getChildAt(i))
    }

    private fun themeColor(attribute: Int): Int {
        val value = TypedValue()
        theme.resolveAttribute(attribute, value, true)
        return if (value.resourceId != 0) getColor(value.resourceId) else value.data
    }

    private fun accentColor(): Int = themeColor(com.google.android.material.R.attr.colorPrimary)
    private fun secondaryColor(): Int = themeColor(android.R.attr.textColorSecondary)
    private fun surfaceColor(): Int = (themeColor(android.R.attr.colorForeground) and 0x00FFFFFF) or SURFACE_ALPHA

    private fun selectableBackground() = TypedValue().let {
        theme.resolveAttribute(android.R.attr.selectableItemBackground, it, true)
        getDrawable(it.resourceId)
    }

    // SF Symbols are Apple's own artwork, so these are the nearest plain Unicode glyphs.
    private val iconFont: Typeface by lazy { Typeface.createFromAsset(assets, "fonts/MaterialIcons-Regular.ttf") }

    private fun TextView.showIcon(name: String) {
        typeface = iconFont
        text = materialIcon(name)
    }

    // The font draws an icon from its name through a ligature. Prefixes cover an SF Symbol's .fill and .circle variants.
    private fun materialIcon(name: String): String = when {
        name.isEmpty() -> ""
        name.startsWith("hand.raised") -> "front_hand"
        name.startsWith("person.3") -> "groups"
        name.startsWith("person.text.rectangle") -> "badge"
        name.startsWith("person.crop.circle") -> "account_circle"
        name.startsWith("person") -> "person"
        name.startsWith("gearshape") -> "settings"
        name.startsWith("gear") -> "settings"
        name.startsWith("chevron.down") -> "expand_more"
        name.startsWith("chevron.up") -> "expand_less"
        name.startsWith("chevron.right") -> "chevron_right"
        name.startsWith("chevron.left") -> "chevron_left"
        name.startsWith("arrow.clockwise") -> "refresh"
        name.startsWith("arrow.counterclockwise") -> "restart_alt"
        name.startsWith("arrow.down.circle") -> "download"
        name.startsWith("line.3.horizontal.decrease") -> "filter_list"
        name.startsWith("checkmark.shield") -> "verified_user"
        name.startsWith("checkmark.circle") -> "check_circle"
        name.startsWith("checkmark") -> "check"
        name.startsWith("xmark") -> "close"
        name.startsWith("exclamationmark") -> "warning"
        name.startsWith("magnifyingglass") -> "search"
        name.startsWith("plus") -> "add"
        name.startsWith("minus") -> "remove"
        name.startsWith("globe") -> "public"
        name.startsWith("square.grid") -> "apps"
        name.startsWith("square.and.arrow.up") -> "ios_share"
        name.startsWith("square.and.arrow.down") -> "save_alt"
        name.startsWith("folder.badge.gearshape") -> "folder_special"
        name.startsWith("folder") -> "folder"
        name.startsWith("safari") -> "explore"
        name.startsWith("heart.text") -> "favorite_border"
        name.startsWith("heart") -> "favorite"
        name.startsWith("trash") -> "delete"
        name.startsWith("doc.on.clipboard") -> "content_paste"
        name.startsWith("doc.badge.gearshape") -> "description"
        name.startsWith("doc") -> "description"
        name.startsWith("paintbrush") -> "brush"
        name.startsWith("app.badge") -> "apps"
        name.startsWith("app") -> "apps"
        name.startsWith("timer") -> "timer"
        name.startsWith("clock") -> "schedule"
        name.startsWith("ellipsis.curlybraces") -> "code"
        name.startsWith("ellipsis") -> "more_horiz"
        name.startsWith("questionmark") -> "help_outline"
        name.startsWith("info") -> "info"
        name.startsWith("lock") -> "lock"
        name.startsWith("key") -> "key"
        name.startsWith("link") -> "link"
        name.startsWith("star") -> "star"
        name.startsWith("bell") -> "notifications"
        name.startsWith("wifi") -> "wifi"
        name.startsWith("network") -> "lan"
        name.startsWith("signature") -> "draw"
        name.startsWith("wrench") -> "build"
        name.startsWith("hammer") -> "construction"
        name.startsWith("pencil") -> "edit"
        name.startsWith("photo") -> "image"
        name.startsWith("tray") -> "inbox"
        name.startsWith("bolt") -> "bolt"
        name.startsWith("moon") -> "dark_mode"
        name.startsWith("sun") -> "light_mode"
        else -> "circle"
    }

    companion object {
        private const val EXTRA_APP = "app"
        private const val FALLBACK_LANGUAGE = "en"
        private const val NETWORK_TIMEOUT_MS = 30_000
        private const val SCRIPT_TIMEOUT_MS = 5_000L
        private const val SCRIPT_BRIDGE = "__iosRunnerBridge"
        private const val COOKIE_PREFERENCES = "guest_cookies"
        private const val COOKIE_HOSTS = "hosts"
        private const val HIDDEN_WEB_WIDTH_DP = 360
        private const val HIDDEN_WEB_HEIGHT_DP = 640
        private const val IOS_WEBKIT_AGENT = "Mozilla/5.0 (iPhone; CPU iPhone OS 27_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko)"
        private const val HTTP_FIRST_ERROR = 400
        private const val SURFACE_ALPHA = 0x14000000
        private var current: GuestActivity? = null

        init {
            System.loadLibrary("iosrunner")
        }

        fun logFile(context: Context) = File(context.cacheDir, "engine.log")

        fun intent(context: Context, app: InstalledApp): Intent =
            Intent(context, GuestActivity::class.java).putExtra(EXTRA_APP, app.directory.name)

        /** Called from the guest thread by the native engine. */
        @JvmStatic
        fun onEngineMessage(kind: String, payload: String) {
            val activity = current ?: return
            activity.mainThread.post { activity.receive(kind, payload) }
        }

        /** Called on the guest thread, which may block: its status comes first, as four bytes. */
        @JvmStatic
        fun httpFetch(url: String, method: String, headers: String, body: ByteArray): ByteArray? = try {
            val connection = URL(url).openConnection() as HttpURLConnection
            connection.requestMethod = method
            connection.connectTimeout = NETWORK_TIMEOUT_MS
            connection.readTimeout = NETWORK_TIMEOUT_MS
            headers.lineSequence().filter { it.isNotBlank() }.forEach { line ->
                connection.setRequestProperty(line.substringBefore(": "), line.substringAfter(": "))
            }
            if (body.isNotEmpty()) {
                connection.doOutput = true
                connection.outputStream.use { it.write(body) }
            }
            val status = connection.responseCode
            val stream = if (status >= HTTP_FIRST_ERROR) connection.errorStream else connection.inputStream
            val bytes = stream?.use { it.readBytes() } ?: ByteArray(0)
            ByteBuffer.allocate(Int.SIZE_BYTES + bytes.size).putInt(status).put(bytes).array()
        } catch (_: IOException) {
            null
        }

        /** Every cookie for the hosts visited so far, as tab-separated domain, name and value lines. */
        @JvmStatic
        fun readCookies(): String {
            val activity = current ?: return ""
            val manager = CookieManager.getInstance()
            val hosts = activity.getSharedPreferences(COOKIE_PREFERENCES, MODE_PRIVATE).getStringSet(COOKIE_HOSTS, emptySet()).orEmpty()
            return hosts.flatMap { host ->
                manager.getCookie("https://$host").orEmpty().split("; ").filter { '=' in it }.map { cookie ->
                    "$host\t${cookie.substringBefore('=')}\t${cookie.substringAfter('=')}"
                }
            }.joinToString("\n")
        }

        @JvmStatic
        private external fun nativeScriptFinished(call: Int, json: String, failed: Boolean)

        @JvmStatic
        private external fun nativeUrlChanged(webView: Int, url: String)

        @JvmStatic
        private external fun nativeCookiesChanged()

        private external fun nativeTextInput(action: Int, text: String)

        @JvmStatic
        private external fun nativeRun(bundleDir: String, executable: String, logPath: String, language: String): Int

        @JvmStatic
        private external fun nativeAction(action: Int)
    }
}
