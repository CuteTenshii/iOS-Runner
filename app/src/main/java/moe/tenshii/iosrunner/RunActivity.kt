package moe.tenshii.iosrunner

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class RunActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var action: Button
    private lateinit var app: InstalledApp
    private val handler = Handler(Looper.getMainLooper())
    private var finished = false

    private val poll = object : Runnable {
        override fun run() {
            render()
            if (!finished) handler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val found = AppLibrary(this).find(intent.getStringExtra(EXTRA_APP).orEmpty())
        if (found == null) {
            finish()
            return
        }
        app = found

        status = TextView(this).apply { setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f) }
        log = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setTextIsSelectable(true)
        }
        logScroll = ScrollView(this).apply {
            addView(HorizontalScrollView(context).apply { addView(log) })
        }
        action = Button(this)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val gutter = dp(16)
            setPadding(gutter, gutter, gutter, gutter)
            addView(TextView(context).apply {
                text = app.name
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
            })
            addView(status, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply {
                topMargin = dp(8)
                bottomMargin = dp(16)
            })
            addView(logScroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(action, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        applySystemBarInsets(root)
        setContentView(root)
        launch()
    }

    override fun onDestroy() {
        handler.removeCallbacks(poll)
        if (::app.isInitialized && !finished) EngineService.stop(this)
        super.onDestroy()
    }

    private fun launch() {
        finished = false
        EngineService.logFile(this).delete()
        EngineService.start(this, app)
        handler.removeCallbacks(poll)
        handler.post(poll)
    }

    private fun render() {
        val text = EngineService.logFile(this).takeIf { it.isFile }?.readText().orEmpty()
        log.text = text
        val outcome = when {
            text.contains("LOAD FAILED") -> R.string.status_load_failed
            text.contains("\nSTOP:") -> R.string.status_stopped_missing
            text.contains("\nCRASH:") -> R.string.status_crashed
            text.contains("\nEXIT:") -> R.string.status_exited
            else -> null
        }
        if (outcome != null) {
            finished = true
            status.setText(outcome)
            action.setText(R.string.run_again)
            action.setOnClickListener { recreateEngine() }
        } else {
            status.setText(if (text.isEmpty()) R.string.status_starting else R.string.status_running)
            action.setText(R.string.stop)
            action.setOnClickListener {
                EngineService.stop(this)
                finish()
            }
        }
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun recreateEngine() {
        // The previous guest may still be exiting; the service only starts once per process.
        handler.postDelayed({ launch() }, RESTART_DELAY_MS)
    }

    companion object {
        private const val EXTRA_APP = "app"
        private const val POLL_INTERVAL_MS = 300L
        private const val RESTART_DELAY_MS = 500L

        fun intent(context: Context, app: InstalledApp): Intent =
            Intent(context, RunActivity::class.java).putExtra(EXTRA_APP, app.directory.name)
    }
}
