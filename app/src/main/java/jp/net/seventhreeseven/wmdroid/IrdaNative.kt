package jp.net.seventhreeseven.wmdroid

/**
 * JNI for the infrared bridge (app/src/main/cpp/irda_jni.cpp, core/src/board/irda.h). Call from the
 * main thread only: the session's machine is destroyed there too.
 */
object IrdaNative {
    init {
        System.loadLibrary("wmdroid")
    }

    /** A transport is connected: the guest's frames go out and other devices' frames come in. */
    external fun setLinked(handle: Long, on: Boolean)
    /** The next IrLAP frame Windows Mobile sent (address, control, information), or null. */
    external fun takeOutgoing(handle: Long): ByteArray?
    /** A frame from another device; [node] is the transport's id for it. */
    external fun fromPeer(handle: Long, node: Int, frame: ByteArray)
    /** The transport lost a device. */
    external fun peerGone(handle: Long, node: Int)
    /** "start" (a program began an infrared operation) and "idle" (a minute without IR), one per line. */
    external fun takeEvents(handle: Long): String
    /** Windows Mobile used infrared in the last minute. */
    external fun active(handle: Long): Boolean
}
