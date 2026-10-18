package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState

/**
 * Android's media session for Windows Mobile's player (AudioClass). Registers now playing.
 * Transport controls stay off: play, pause, and skip are not offered.
 */
class NowPlaying(ctx: Context, private val onCommand: (String) -> Unit) {
    private val session = MediaSession(ctx, "WMDroid").apply {
        setCallback(object : MediaSession.Callback() {
            override fun onPlay() = onCommand("play")
            override fun onPause() = onCommand("pause")
            override fun onSkipToNext() = onCommand("next")
            override fun onSkipToPrevious() = onCommand("prev")
            override fun onStop() = onCommand("stop")
            override fun onSeekTo(pos: Long) = onCommand("seek $pos")
        })
    }

    val token: MediaSession.Token get() = session.sessionToken

    /**
     * [active] is a player running. [positionMs] is -1 when the player has not reported a position;
     * otherwise milliseconds, and a speed of 1 lets the bar move between reports.
     */
    fun update(active: Boolean, playing: Boolean, title: String, artist: String = "", album: String = "",
               positionMs: Long = -1L, durationMs: Long = 0L) {
        session.isActive = active
        if (!active) return
        val state = if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED
        val pos = if (positionMs < 0) PlaybackState.PLAYBACK_POSITION_UNKNOWN else positionMs
        session.setPlaybackState(PlaybackState.Builder()
            .setActions(0)
            .setState(state, pos, if (playing) 1f else 0f)
            .build())
        val meta = MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, title.ifBlank { "Windows Mobile" })
            .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, album)
        if (durationMs > 0) meta.putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs)
        session.setMetadata(meta.build())
    }

    fun release() {
        session.isActive = false
        session.release()
    }
}
