// JNI for the camera bridge (CameraBridge.kt <-> core/src/board/camera.h). Frames arrive on the
// bridge's camera thread; the core converts and pre-renders them there, so the emulation thread
// only copies finished frames into the guest.
#include <jni.h>

#include "board/camera.h"
#include "board/machine.h"

wm::Machine *wmdroidSessionMachine(jlong h);  // jni.cpp

namespace {

wm::Camera *cam(jlong h) {
    wm::Machine *m = wmdroidSessionMachine(h);
    return m ? m->camera.get() : nullptr;
}

}  // namespace

extern "C" {

/** True while the guest has its camera open (or asked for a frame in the last two seconds). */
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_cameraWanted(JNIEnv *, jobject, jlong h) {
    wm::Camera *c = cam(h);
    return c && c->wanted();
}

/** Guest flash request: low byte 0 off / 1 on / 2 auto, bit 8 an app holding the LED on. */
JNIEXPORT jint JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_cameraFlash(JNIEnv *, jobject, jlong h) {
    wm::Camera *c = cam(h);
    return c ? c->flash() : 0;
}

/** hostCamera: a phone camera feeds the guest (else it sees a test pattern); era: 2009 look, 0..100. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_cameraConfig(JNIEnv *, jobject, jlong h,
                                                                                    jboolean hostCamera,
                                                                                    jint era) {
    if (wm::Camera *c = cam(h)) {
        c->setHostCamera(hostCamera);
        c->setEra(era);
    }
}

/** The phone camera closed: forget its last frame. */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_cameraClear(JNIEnv *, jobject, jlong h) {
    if (wm::Camera *c = cam(h)) c->clearFrames();
}

/** One YUV_420_888 frame (direct buffers of an android.media.Image's planes). */
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_cameraFrame(
    JNIEnv *env, jobject, jlong h, jobject y, jint yRowStride, jobject u, jobject v, jint uvRowStride,
    jint uvPixelStride, jint w, jint hgt, jint rotation, jboolean mirror) {
    wm::Camera *c = cam(h);
    if (!c) return;
    auto *py = (const uint8_t *)env->GetDirectBufferAddress(y);
    auto *pu = (const uint8_t *)env->GetDirectBufferAddress(u);
    auto *pv = (const uint8_t *)env->GetDirectBufferAddress(v);
    if (!py || !pu || !pv) return;
    // Plane sizes guard the reads: the last row of each plane may be shorter than its stride.
    jlong ny = env->GetDirectBufferCapacity(y), nu = env->GetDirectBufferCapacity(u),
          nv = env->GetDirectBufferCapacity(v);
    jlong needY = (jlong)yRowStride * (hgt - 1) + w;
    jlong needUV = (jlong)uvRowStride * ((hgt + 1) / 2 - 1) + (jlong)uvPixelStride * ((w + 1) / 2 - 1) + 1;
    if (w <= 0 || hgt <= 0 || ny < needY || nu < needUV || nv < needUV) return;
    c->pushYuv420(py, yRowStride, pu, pv, uvRowStride, uvPixelStride, w, hgt, rotation, mirror);
}

}  // extern "C"
