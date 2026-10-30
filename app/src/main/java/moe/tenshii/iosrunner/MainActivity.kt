package moe.tenshii.iosrunner

import android.app.Activity
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private lateinit var library: AppLibrary
    private lateinit var content: LinearLayout
    private lateinit var importButton: Button
    private val worker = Executors.newSingleThreadExecutor()
    private val mainThread = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        library = AppLibrary(this)

        content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        importButton = Button(this).apply {
            setText(R.string.import_ipa)
            setOnClickListener { pickFile() }
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val gutter = dp(16)
            setPadding(gutter, gutter, gutter, gutter)
            addView(TextView(context).apply {
                setText(R.string.app_name)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
                setPadding(0, 0, 0, dp(16))
            })
            addView(ScrollView(context).apply { addView(content) }, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(importButton, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        applySystemBarInsets(root)
        setContentView(root)
        refresh()
        handleViewIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleViewIntent(intent)
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    @Deprecated("Activity result API from the platform; this app has no AndroidX dependency")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == REQUEST_PICK && resultCode == RESULT_OK && uri != null) importFile(uri)
    }

    private fun handleViewIntent(intent: Intent?) {
        if (intent?.action == Intent.ACTION_VIEW) intent.data?.let(::importFile)
    }

    @Suppress("DEPRECATION")
    private fun pickFile() {
        startActivityForResult(
            Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"),
            REQUEST_PICK,
        )
    }

    private fun importFile(uri: Uri) {
        importButton.isEnabled = false
        importButton.setText(R.string.importing)
        worker.execute {
            val outcome = runCatching { library.import(this, uri) }
            mainThread.post {
                importButton.isEnabled = true
                importButton.setText(R.string.import_ipa)
                refresh(outcome.exceptionOrNull()?.let { getString(R.string.import_failed, it.message ?: it.toString()) })
            }
        }
    }

    private fun refresh(errorMessage: String? = null) {
        content.removeAllViews()
        if (errorMessage != null) {
            content.addView(TextView(this).apply {
                text = errorMessage
                setPadding(0, 0, 0, dp(16))
            })
        }
        val apps = library.list()
        if (apps.isEmpty()) {
            content.addView(TextView(this).apply {
                setText(R.string.empty_library)
                setPadding(0, dp(24), 0, dp(24))
            })
        }
        apps.forEach { content.addView(row(it)) }
    }

    private fun row(app: InstalledApp): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(0, dp(8), 0, dp(8))
        addView(ImageView(context).apply {
            contentDescription = null
            app.iconFile?.let { file ->
                setImageBitmap(AppleIcon.decode(file.readBytes()) ?: BitmapFactory.decodeFile(file.path))
            }
        }, LinearLayout.LayoutParams(dp(56), dp(56)).apply { marginEnd = dp(12) })
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(context).apply {
                text = app.name
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
            })
            addView(TextView(context).apply {
                text = getString(R.string.app_subtitle, app.version, app.minimumOs)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            })
        }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addView(Button(context).apply {
            setText(R.string.open_app)
            setOnClickListener { startActivity(RunActivity.intent(context, app)) }
        })
        addView(Button(context).apply {
            setText(R.string.remove_app)
            setOnClickListener {
                library.remove(app)
                refresh()
            }
        })
    }

    private companion object {
        const val REQUEST_PICK = 1
    }
}

internal fun View.dp(value: Int): Int =
    TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics).toInt()

internal fun Activity.dp(value: Int): Int =
    TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics).toInt()

internal fun applySystemBarInsets(view: View) {
    val left = view.paddingLeft
    val top = view.paddingTop
    val right = view.paddingRight
    val bottom = view.paddingBottom
    view.setOnApplyWindowInsetsListener { target, insets ->
        val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
        target.setPadding(left + bars.left, top + bars.top, right + bars.right, bottom + bars.bottom)
        insets
    }
}
