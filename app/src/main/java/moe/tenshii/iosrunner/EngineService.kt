package moe.tenshii.iosrunner

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.Process
import java.io.File

/** Hosts the guest app in the isolated :engine process, so a guest crash cannot take down the launcher. */
class EngineService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            Process.killProcess(Process.myPid())
            return START_NOT_STICKY
        }
        val bundle = intent?.getStringExtra(EXTRA_BUNDLE)
        val executable = intent?.getStringExtra(EXTRA_EXECUTABLE)
        val log = intent?.getStringExtra(EXTRA_LOG)
        if (bundle == null || executable == null || log == null || started) return START_NOT_STICKY
        started = true
        Thread {
            nativeRun(bundle, executable, log)
            Process.killProcess(Process.myPid())
        }.start()
        return START_NOT_STICKY
    }

    companion object {
        private const val EXTRA_BUNDLE = "bundle"
        private const val EXTRA_EXECUTABLE = "executable"
        private const val EXTRA_LOG = "log"
        private const val ACTION_STOP = "moe.tenshii.iosrunner.STOP"
        private var started = false

        init {
            System.loadLibrary("iosrunner")
        }

        fun logFile(context: Context) = File(context.cacheDir, "engine.log")

        fun start(context: Context, app: InstalledApp) {
            context.startService(Intent(context, EngineService::class.java).apply {
                putExtra(EXTRA_BUNDLE, app.directory.path)
                putExtra(EXTRA_EXECUTABLE, app.executable)
                putExtra(EXTRA_LOG, logFile(context).path)
            })
        }

        fun stop(context: Context) {
            context.startService(Intent(context, EngineService::class.java).setAction(ACTION_STOP))
        }

        @JvmStatic
        private external fun nativeRun(bundleDir: String, executable: String, logPath: String): Int
    }
}
