// JNI for the infrared bridge (InfraredBridge.kt <-> core/src/board/irda.h). Called on the app's
// main thread only (like the session's own destroy), so a frame never meets a destroyed machine.
#include <jni.h>

#include "board/irda.h"
#include "board/machine.h"

wm::Machine *wmdroidSessionMachine(jlong h);  // jni.cpp

namespace {

wm::Irda *ir(jlong h) {
    wm::Machine *m = wmdroidSessionMachine(h);
    return m ? m->irda.get() : nullptr;
}

}  // namespace

extern "C" {

/** A transport is up: the guest's frames go out and frames from other devices come in. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_setLinked(JNIEnv *, jobject, jlong h, jboolean on) {
    if (wm::Irda *i = ir(h)) i->setLinked(on);
}

/** The next frame the guest sent (IrLAP: address, control, information), or null. Never blocks. */
JNIEXPORT jbyteArray JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_takeOutgoing(JNIEnv *env, jobject, jlong h) {
    wm::Irda *i = ir(h);
    std::vector<uint8_t> f;
    if (!i || !i->takeOutgoing(f, 0)) return nullptr;
    jbyteArray a = env->NewByteArray((jsize)f.size());
    if (a) env->SetByteArrayRegion(a, 0, (jsize)f.size(), (const jbyte *)f.data());
    return a;
}

/** A frame from another device; node is the transport's id for that device. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_fromPeer(JNIEnv *env, jobject, jlong h, jint node,
                                                                            jbyteArray frame) {
    wm::Irda *i = ir(h);
    if (!i || !frame) return;
    jsize n = env->GetArrayLength(frame);
    if (n <= 0 || n > (jsize)wm::Irda::kMaxFrame) return;
    std::vector<uint8_t> f((size_t)n);
    env->GetByteArrayRegion(frame, 0, n, (jbyte *)f.data());
    i->fromPeer((uint32_t)node, f.data(), f.size());
}

/** The transport lost a device: forget its discovery answer. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_peerGone(JNIEnv *, jobject, jlong h, jint node) {
    if (wm::Irda *i = ir(h)) i->peerGone((uint32_t)node);
}

/** "start" / "idle" lines since the last call (Irda::takeEvents). */
JNIEXPORT jstring JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_takeEvents(JNIEnv *env, jobject, jlong h) {
    wm::Irda *i = ir(h);
    return env->NewStringUTF(i ? i->takeEvents().c_str() : "");
}

/** The guest has used infrared in the last minute. */
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_IrdaNative_active(JNIEnv *, jobject, jlong h) {
    wm::Irda *i = ir(h);
    return i && i->active();
}

}  // extern "C"
