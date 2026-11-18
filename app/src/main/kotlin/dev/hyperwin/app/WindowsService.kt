package dev.hyperwin.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.util.Log

/**
 * A foreground service while Windows runs. Android silences the microphone of an app in the
 * background unless a service of type microphone runs, and the service also keeps the app, and so
 * the VM, from being put away while the user is elsewhere. Type mediaPlayback covers Windows' sound.
 */
class WindowsService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Windows running", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val note = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle("Windows is running")
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        val media = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
        val type = if (GuestAudio.micPermitted(this)) media or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else media
        runCatching { startForeground(NOTE_ID, note, type) }.recoverCatching {
            Log.w(TAG, "foreground with the microphone refused", it)
            startForeground(NOTE_ID, note, media)
        }.onFailure { Log.w(TAG, "foreground service refused", it) }
        return START_NOT_STICKY
    }

    companion object {
        private const val TAG = "HyperWin"
        private const val CHANNEL = "windows"
        private const val NOTE_ID = 1
        @Volatile private var app: Context? = null

        /** When the VM starts, while the app is in front. */
        fun start(ctx: Context) {
            app = ctx.applicationContext
            runCatching { ctx.startForegroundService(Intent(ctx, WindowsService::class.java)) }
                .onFailure { Log.w(TAG, "could not start the foreground service", it) }
        }

        /** Again with the service types the permissions allow now. */
        fun refresh(ctx: Context) {
            if (app != null) start(ctx)
        }

        fun stop() {
            app?.let { it.stopService(Intent(it, WindowsService::class.java)) }
            app = null
        }
    }
}
