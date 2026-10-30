package moe.tenshii.iosrunner

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.util.TypedValue
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Launches the guest, and shows why it stopped when control comes back here. */
class RunActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var app: InstalledApp
    private var launched = false

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
            addView(ScrollView(context).apply {
                addView(HorizontalScrollView(context).apply { addView(log) })
            }, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
            addView(Button(context).apply {
                setText(R.string.run_again)
                setOnClickListener { launch() }
            }, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        applySystemBarInsets(root)
        setContentView(root)
        launch()
    }

    override fun onResume() {
        super.onResume()
        if (launched) render()
    }

    private fun launch() {
        launched = true
        GuestActivity.logFile(this).delete()
        startActivity(GuestActivity.intent(this, app))
    }

    private fun render() {
        val text = GuestActivity.logFile(this).takeIf { it.isFile }?.readText().orEmpty()
        log.text = text
        status.setText(
            when {
                text.contains("LOAD FAILED") -> R.string.status_load_failed
                text.contains("\nSTOP:") -> R.string.status_stopped_missing
                text.contains("\nCRASH:") -> R.string.status_crashed
                else -> R.string.status_exited
            },
        )
    }

    companion object {
        private const val EXTRA_APP = "app"

        fun intent(context: Context, app: InstalledApp): Intent =
            Intent(context, RunActivity::class.java).putExtra(EXTRA_APP, app.directory.name)
    }
}
