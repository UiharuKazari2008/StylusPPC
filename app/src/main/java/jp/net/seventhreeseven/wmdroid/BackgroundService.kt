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
import android.media.session.MediaSession
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
    /** Now playing, or null for the ordinary "running in the background" notice. */
    class Playback(val title: String, val playing: Boolean, val token: MediaSession.Token,
        val artist: String = "", val suspendable: Boolean = true)

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
        intent?.action?.let { if (it.startsWith(ACTION_MEDIA)) onMedia?.invoke(it.removePrefix(ACTION_MEDIA)) }
        try {
            if (android.os.Build.VERSION.SDK_INT >= 29)
                startForeground(ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
            else startForeground(ID, notification())
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
        val playing = playback
        if (playing == null) return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_running)
            .setContentTitle("Windows Mobile is running")
            .setContentText("In the background, at reduced speed")
            .setCategory(Notification.CATEGORY_SERVICE)
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null as Icon?, "Suspend", suspend).build())
            .build()
        // Now playing, with no play / pause / skip buttons.
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_running)
            .setContentTitle(playing.title)
            .setContentText(playing.artist.ifBlank { if (playing.playing) "Playing" else "Paused" })
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(open)
            .setStyle(Notification.MediaStyle().setMediaSession(playing.token))
            .apply { if (playing.suspendable) addAction(Notification.Action.Builder(null as Icon?, "Suspend", suspend).build()) }
            .build()
    }

    private fun refresh() {
        if (android.os.Build.VERSION.SDK_INT >= 29)
            startForeground(ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        else startForeground(ID, notification())
    }

    companion object {
        private const val TAG = "WMDroid"
        private const val ID = 1
        private const val CHANNEL = "background"
        private const val ACTION_SUSPEND = "jp.net.seventhreeseven.wmdroid.action.SUSPEND"
        private const val ACTION_MEDIA = "jp.net.seventhreeseven.wmdroid.action.MEDIA_"

        private var instance: BackgroundService? = null
        private var wanted = false
        private var playback: Playback? = null

        /** Set by the emulator while it lives: the notification's Suspend (and a failed start). */
        var onSuspend: (() -> Unit)? = null
        /** Play, pause, next, prev from the media notification and the session. */
        var onMedia: ((String) -> Unit)? = null

        /** Switches the foreground notice to the player, or back when [now] is null. */
        fun setPlayback(now: Playback?) {
            if (now == playback || (now != null && playback != null && now.title == playback!!.title &&
                    now.artist == playback!!.artist && now.playing == playback!!.playing && now.token == playback!!.token &&
                    now.suspendable == playback!!.suspendable)) return
            playback = now
            instance?.refresh()
        }

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
