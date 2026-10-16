package jp.net.seventhreeseven.wmdroid

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.os.IBinder
import android.util.Log

/**
 * Keeps the process alive while Windows Mobile runs on in the background (the emulation itself
 * runs on its own native thread), with a quiet ongoing notification that offers Suspend.
 *
 * Android ends an app that asked for a foreground service and stops it before it went
 * foreground, so [stop] never stops it directly: a service still starting stops itself right
 * after [startForeground].
 */
class BackgroundService : Service() {
    private var lastStartId = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        lastStartId = startId
        if (intent?.action == ACTION_SUSPEND) {
            // From the notification; the emulator then calls stop().
            val suspend = onSuspend
            if (suspend != null) suspend() else { wanted = false; stopSelfResult(startId) }
            return START_NOT_STICKY
        }
        try {
            startForeground(ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } catch (e: RuntimeException) {
            Log.w(TAG, "could not run in the foreground", e)
            wanted = false
            stopSelfResult(startId)
            onSuspend?.invoke()
            return START_NOT_STICKY
        }
        instance = this
        if (!wanted) stopSelfResult(startId)
        return START_NOT_STICKY
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Swiped away: the emulator activity is going; nothing left to keep alive.
        wanted = false
        stopSelf()
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    private fun notification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Running in the background",
            NotificationManager.IMPORTANCE_LOW).apply {
            description = "Shown while Windows Mobile keeps running with the app in the background"
            setShowBadge(false)
        })
        val flags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        // The same intent as the launcher icon: brings the existing task back to the front.
        val open = packageManager.getLaunchIntentForPackage(packageName)
            ?.addFlags(Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
            ?.let { PendingIntent.getActivity(this, 0, it, flags) }
        val suspend = PendingIntent.getService(this, 1,
            Intent(this, BackgroundService::class.java).setAction(ACTION_SUSPEND), flags)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_running)
            .setContentTitle("Windows Mobile is running")
            .setContentText("In the background, at reduced speed")
            .setCategory(Notification.CATEGORY_SERVICE)
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null as Icon?, "Suspend", suspend).build())
            .build()
    }

    companion object {
        private const val TAG = "WMDroid"
        private const val ID = 1
        private const val CHANNEL = "background"
        private const val ACTION_SUSPEND = "jp.net.seventhreeseven.wmdroid.action.SUSPEND"

        private var instance: BackgroundService? = null
        private var wanted = false

        /** Set by the emulator while it lives: the notification's Suspend (and a failed start). */
        var onSuspend: (() -> Unit)? = null

        /** Starts the service; false if Android refuses (the caller suspends instead). */
        fun start(ctx: Context): Boolean {
            wanted = true
            return try {
                ctx.startForegroundService(Intent(ctx, BackgroundService::class.java))
                true
            } catch (e: RuntimeException) {
                // ForegroundServiceStartNotAllowedException (12+) is an IllegalStateException.
                Log.w(TAG, "background service not allowed", e)
                wanted = false
                false
            }
        }

        fun stop() {
            wanted = false
            instance?.let { it.stopSelfResult(it.lastStartId) }
        }
    }
}
