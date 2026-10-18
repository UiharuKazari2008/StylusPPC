// JNI for the motion sensor (MotionBridge.kt <-> core/src/board/motion.h).
#include <jni.h>

#include "board/machine.h"
#include "board/motion.h"

wm::Machine *wmdroidSessionMachine(jlong h);  // jni.cpp

namespace {

wm::Motion *motion(jlong h) {
    wm::Machine *m = wmdroidSessionMachine(h);
    return m ? m->motion.get() : nullptr;
}

}  // namespace

extern "C" {

/** True while a guest app reads the G-sensor (SEN0:). */
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_motionWanted(JNIEnv *, jobject, jlong h) {
    wm::Motion *mo = motion(h);
    return mo && mo->wanted();
}

/** One accelerometer sample, m/s^2 as Android reports it, already in the guest panel's frame. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_motionAccel(JNIEnv *, jobject, jlong h, jfloat x,
                                                                                    jfloat y, jfloat z) {
    if (wm::Motion *mo = motion(h)) mo->setAccel(x, y, z);
}

/** The phone sensor stopped: the guest sees a phone lying face up. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_motionClear(JNIEnv *, jobject, jlong h) {
    if (wm::Motion *mo = motion(h)) mo->clearAccel();
}

}  // extern "C"
