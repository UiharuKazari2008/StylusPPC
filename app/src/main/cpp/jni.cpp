// JNI bridge: owns a Machine, runs it on its own thread and presents the LCD into a Surface.
#include <algorithm>
#include <cstdlib>
#include <android/native_window_jni.h>
#include "board/pcmcia.h"
#include "board/codepatch.h"  // FakeRIL own number
#include "net/usernet.h"
#include <jni.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <dlfcn.h>
#include <sys/resource.h>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

#include "audio_host.h"
#include "gl_renderer.h"
#include "board/audio.h"
#include "board/de_devices.h"
#include "board/devices.h"
#include "board/folder_share.h"
#include "board/machine.h"
#include "board/modem.h"
#include "util/log.h"

using namespace wm;

namespace {

// Android Dynamic Performance Framework (API 33+), looked up at runtime because minSdk is 29.
// Reporting how long each 16 ms emulation frame was busy lets the scheduler boost CPU frequency
// and placement for the emulation thread, the same way games get boosted.
struct PerfHint {
    using GetManager = void *(*)();
    using CreateSession = void *(*)(void *, const int32_t *, size_t, int64_t);
    using Report = int (*)(void *, int64_t);
    using Close = void (*)(void *);
    void *session = nullptr;
    Report report = nullptr;
    Close close = nullptr;

    void start(int32_t tid, int64_t targetNs) {
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return;
        auto get = (GetManager)dlsym(lib, "APerformanceHint_getManager");
        auto create = (CreateSession)dlsym(lib, "APerformanceHint_createSession");
        report = (Report)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        close = (Close)dlsym(lib, "APerformanceHint_closeSession");
        void *mgr = get ? get() : nullptr;
        if (mgr && create) session = create(mgr, &tid, 1, targetNs);
        LOGI("performance hint session %s", session ? "active" : "unavailable");
    }
    void work(int64_t ns) { if (session && report && ns > 0) report(session, ns); }
    // APerformanceHint_sendHint (API 34): SESSION_HINT_CPU_LOAD_UP = 0, a sudden rise in load.
    using SendHint = int (*)(void *, int32_t);
    SendHint sendHint = nullptr;
    void loadUp() {
        if (!sendHint) sendHint = (SendHint)dlsym(RTLD_DEFAULT, "APerformanceHint_sendHint");
        if (session && sendHint) sendHint(session, 0);
    }
    void stop() { if (session && close) close(session); session = nullptr; }
};

struct InputEvent {
    enum { TOUCH, KEY } type;
    bool down;
    int a, b;
};

struct Session {
    // The network outlives the machine's use of it: stopped in the destructor before `m` goes.
    UserNet net;
    ~Session() { net.stop(); }
    std::unique_ptr<Machine> m;
    std::thread thread;
    std::atomic<bool> running{false};
    GlRenderer gl;
    std::mutex inMu;
    std::vector<InputEvent> inputs;
    std::vector<uint8_t> serialIn;  // bytes for the guest's COM1 (UART0), e.g. GPS NMEA
    bool powerPress = false, resetPress = false;
    // Percentage of the host CPU the guest may use while it is busy (100 = unlimited). Below 100
    // the loop sleeps after each slice so busy time stays near the share; idle time is free.
    std::atomic<int> cpuLimit{100};
    // Boost (see NativeBridge.boost): until this time the scheduler is told the work is at its target.
    std::atomic<uint64_t> boostUntilNs{0};
    std::atomic<bool> boostNow{false};
    bool penDown = false;
    uint64_t penDownNs = 0;
    static constexpr uint64_t kMinPenNs = 50000000;  // 50 ms: a few driver samples
    std::atomic<double> mips{0}, activeMips{0};
    std::atomic<int> idlePct{0};
    std::atomic<int> audioPeak{0};          // loudest output sample since the last status read
    std::atomic<uint64_t> audioFrames{0};   // output frames since the last status read
    std::atomic<uint64_t> lastCaptureNs{0};
    std::atomic<uint64_t> lastPlayNs{0};
    // Hang detection: the battery driver's poll stopped while the CPU stays busy (a crashed guest
    // spinning in an exception loop; a sleeping or idle one is idle).
    uint64_t heartbeatReads = 0, heartbeatNs = 0;
    std::atomic<bool> hung{false};    // last sound sent to the speaker (see the CPU share below)
    HostAudio audio;
    std::string serial;

    // Frames from the network for the card, while it has room (the rest wait in the queue).
    void pumpNet() {
        std::vector<uint8_t> f;
        while (net.nextForGuest(f)) {
            if (!m->pcmcia->receive(f.data(), f.size())) break;
            net.popForGuest();
        }
    }

    // Draws the LCD through the GPU renderer (upscaling happens in its shader).
    void render() {
        Lcd &lcd = *m->lcd;
        int w = lcd.width(), h = lcd.height();
        if (!lcd.enabled() || w <= 1 || h <= 1) return;
        int bpp = lcd.bpp();
        uint32_t bytes = (uint32_t)w * h * (bpp / 8);
        bool wr;
        uint8_t *first = m->bus.hostPage(lcd.framebufferPa(), wr);
        uint8_t *last = m->bus.hostPage(lcd.framebufferPa() + bytes - 1, wr);
        if (!first || last != first + bytes - 1) return;  // frame buffer must be in contiguous RAM
        gl.draw(first, w, h, bpp);
    }

    void loop() {
        // The guest CPU is latency-bound work: ask the scheduler to keep it on fast cores.
        if (setpriority(PRIO_PROCESS, (id_t)gettid(), -10) != 0) LOGW("could not raise emulation thread priority");
        PerfHint hint;
        hint.start((int32_t)gettid(), 12000000);  // below the 16 ms frame, so a busy guest reads as "behind"
        audio.startOutput();
        uint64_t lastInsns = m->cpu.insnCount;
        uint64_t lastT = m->nowNs(), lastIdle = m->idleNs;
        // Power: while the guest idles (home screen, nothing playing) the thread drops to normal
        // priority and stops reporting to the hint session (each report is a binder call that
        // wakes system_server); the first busy frame restores both.
        bool raised = true, hinting = true;
        uint64_t idleSinceNs = 0;
        while (running) {
            {
                std::lock_guard<std::mutex> lk(inMu);
                // Apply in order, but keep the pen down long enough for the touch driver to sample it:
                // a quick tap's down and up often arrive in the same 16 ms batch.
                size_t i = 0;
                for (; i < inputs.size(); i++) {
                    const InputEvent &e = inputs[i];
                    if (e.type == InputEvent::TOUCH) {
                        if (!e.down && penDown && m->nowNs() - penDownNs < kMinPenNs) break;
                        if (e.down && !penDown) penDownNs = m->nowNs();
                        penDown = e.down;
                        m->touch(e.down, e.a, e.b);
                    } else {
                        m->key(e.a, e.down);
                    }
                }
                inputs.erase(inputs.begin(), inputs.begin() + (long)i);
                // COM1 is the radio once the guest's RIL talks to the virtual modem: no GPS bytes then.
                if (!m->modem->claimed()) for (uint8_t b : serialIn) m->uart0.feed(b);
                serialIn.clear();
                if (powerPress) { m->gpio.powerButton(); powerPress = false; }
                pumpNet();
                if (resetPress) { m->resetRequested = true; resetPress = false; }
            }
            uint64_t idle0 = m->idleNs, t0 = m->nowNs();
            m->runFor(16000000);
            render();
            uint64_t span = m->nowNs() - t0, idleSpan = m->idleNs - idle0;
            uint64_t busy = span > idleSpan ? span - idleSpan : 0;
            if (boostNow.exchange(false)) hint.loadUp();
            // While boosted, report a full frame of work so the CPU stays clocked up during the redraw.
            bool boosted = m->nowNs() < boostUntilNs.load();
            int lim = cpuLimit.load();
            // Sound playing (music in the background): no step-down, or decoding starves and the
            // audio breaks up. Two seconds of grace bridges gaps between buffers and tracks.
            bool sounding = m->nowNs() - lastPlayNs.load(std::memory_order_relaxed) < 2000000000ull;
            {
                uint64_t now = m->nowNs();
                bool quiet = !boosted && !sounding && busy < 1000000;  // < 1 ms of a 16 ms frame
                if (!quiet) idleSinceNs = 0;
                else if (!idleSinceNs) idleSinceNs = now;
                bool settled = idleSinceNs && now - idleSinceNs > 1000000000ull;
                // One last (small) report after going quiet, so the session's boost winds down.
                if (!settled || hinting) hint.work(boosted ? 16000000 : (int64_t)busy);
                hinting = !settled;
                if (settled == raised) {
                    raised = !settled;
                    setpriority(PRIO_PROCESS, (id_t)gettid(), raised ? -10 : 0);
                }
            }
            if (lim < 100 && busy > 0 && !boosted && !sounding) {
                // Stretch busy time so the guest gets about `lim`% of a core in the background:
                // music and timers keep running, a runaway guest app can't drain the battery.
                uint64_t pauseNs = busy * (100 - lim) / lim;
                if (pauseNs > 200000000ull) pauseNs = 200000000ull;
                std::this_thread::sleep_for(std::chrono::nanoseconds(pauseNs));
            }
            uint64_t t = m->nowNs();
            if (t - lastT > 1000000000ull) {
                uint64_t n = m->cpu.insnCount - lastInsns, span = t - lastT, idle = m->idleNs - lastIdle;
                mips = (double)n / (span / 1000.0);
                activeMips = span > idle ? (double)n / ((span - idle) / 1000.0) : 0;
                idlePct = idle >= span ? 100 : (int)(idle * 100 / span);
                lastInsns = m->cpu.insnCount;
                lastT = t;
                lastIdle = m->idleNs;
                if (!m->bootComplete) LOGI("perf: %.0f MIPS active, %d%% idle", activeMips.load(), idlePct.load());
                uint64_t reads = m->dePage->batteryReads;
                if (reads != heartbeatReads || !m->bootComplete || heartbeatNs == 0) {
                    heartbeatReads = reads;
                    heartbeatNs = t;
                }
                bool stale = m->bootComplete && t - heartbeatNs > 10000000000ull && idlePct.load() <= 20;
                if (stale != hung.load()) {
                    hung = stale;
                    if (stale) LOGW("guest stopped responding (no battery poll for 10 s, CPU busy)");
                }
            }
        }
        hint.stop();
        gl.releaseThread();
    }
};

Session *S(jlong h) { return reinterpret_cast<Session *>(h); }

}  // namespace

// Camera bridge (camera_jni.cpp) reaches the session's machine through this.
Machine *wmdroidSessionMachine(jlong h) { return h ? S(h)->m.get() : nullptr; }

extern "C" {

JNIEXPORT jlong JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_create(
    JNIEnv *env, jobject, jstring path, jint memMB, jboolean armv5, jint w, jint h) {
    MachineConfig cfg;
    const char *p = env->GetStringUTFChars(path, nullptr);
    cfg.imagePath = p;
    env->ReleaseStringUTFChars(path, p);
    cfg.memMB = (uint32_t)memMB;
    cfg.core = armv5 ? CORE_ARMV5 : CORE_ARMV4;
    cfg.screenW = w;
    cfg.screenH = h;
    auto *s = new Session();
    s->m.reset(new Machine(cfg));
    std::string err;
    if (!s->m->load(err)) {
        LOGE("load failed: %s", err.c_str());
        delete s;
        return 0;
    }
    auto *sp = s;
    s->m->dma->onPlay = [sp](const int16_t *f, uint32_t n) {
        int peak = sp->audioPeak.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n * 2; i++) peak = std::max(peak, std::abs((int)f[i]));
        sp->audioPeak.store(peak, std::memory_order_relaxed);
        sp->audioFrames.fetch_add(n, std::memory_order_relaxed);
        sp->lastPlayNs.store(sp->m->nowNs(), std::memory_order_relaxed);
        sp->audio.play(f, n);
    };
    s->m->dma->onCapture = [sp](int16_t *f, uint32_t n) {
        sp->lastCaptureNs = sp->m->nowNs();  // the guest is recording: the app opens the mic
        sp->audio.capture(f, n);
    };
    // Network card: NE2000 in the PCMCIA slot on the user-mode network. The MAC address is
    // locally administered and stable per image path.
    {
        uint32_t h = 2166136261u;
        for (char c : cfg.imagePath) h = (h ^ (uint8_t)c) * 16777619u;
        const uint8_t mac[6] = {0x02, 0x57, 0x4D, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
        s->m->pcmcia->setMac(mac);
        s->m->pcmcia->onTransmit = [sp](const uint8_t *f, size_t n) { sp->net.fromGuest(f, n); };
        s->m->pcmcia->setInserted(true);
        s->net.start();
    }
    s->m->uart0.onTx = [sp](int idx, uint8_t c) {
        if (sp->m->modem->claimed()) return;  // AT commands for the virtual modem (modem.h)
        if (sp->serial.size() > 512) sp->serial.clear();
        if (c == '\n') { LOGI("[uart%d] %s", idx, sp->serial.c_str()); sp->serial.clear(); }
        else if (c != '\r') sp->serial += (char)c;
    };
    return reinterpret_cast<jlong>(s);
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_displayOn(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    return s && s->m->lcd->enabled();
}

// Windows Mobile is in standby (CLKCON POWER_OFF): only the power button wakes it.
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_guestAsleep(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    return s && (s->m->clk.clkcon & 8) != 0;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_powerButton(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->inMu);
    s->powerPress = true;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_softReset(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->inMu);
    s->resetPress = true;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setCpuLimit(JNIEnv *, jobject, jlong h,
                                                                                     jint percent) {
    Session *s = S(h);
    if (s) s->cpuLimit = percent < 1 ? 1 : percent > 100 ? 100 : percent;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setAudioGain(JNIEnv *, jobject, jlong h,
                                                                                      jfloat gain) {
    Session *s = S(h);
    if (s) s->audio.gain = gain;
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_serialOpen(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s) return false;
    // The serial driver turns RX interrupts on at boot; only opening the port unmasks them.
    const IntController &ic = s->m->intc;
    return s->m->uart0.rxEnabled() && !(ic.intsubmsk & 1u) && !(ic.intmsk & (1u << IRQ_UART0)) && !s->m->modem->claimed();
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_serialWrite(JNIEnv *env, jobject, jlong h,
                                                                                     jbyteArray data) {
    Session *s = S(h);
    if (!s) return;
    jsize n = env->GetArrayLength(data);
    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte *>(buf.data()));
    std::lock_guard<std::mutex> lk(s->inMu);
    if (s->serialIn.size() < 65536) s->serialIn.insert(s->serialIn.end(), buf.begin(), buf.end());
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_start(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s || s->running) return;
    s->running = true;
    s->m->stopRequested = false;
    s->thread = std::thread([s] { s->loop(); });
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_pause(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s || !s->running) return;
    s->running = false;
    s->m->stopRequested = true;
    if (s->thread.joinable()) s->thread.join();
    s->audio.stop();
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_destroy(JNIEnv *env, jobject self, jlong h) {
    Session *s = S(h);
    if (!s) return;
    Java_jp_net_seventhreeseven_wmdroid_NativeBridge_pause(env, self, h);
    {
        s->gl.setWindow(nullptr, false);
    }
    delete s;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setSurface(JNIEnv *env, jobject, jlong h,
                                                                                    jobject surface) {
    Session *s = S(h);
    if (!s) return;
    ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    s->gl.setWindow(win, s->running);
    if (win) ANativeWindow_release(win);  // the renderer holds its own reference
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setFilter(JNIEnv *, jobject, jlong h, jint f) {
    Session *s = S(h);
    if (s) s->gl.setFilter(f);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setAudioUsage(JNIEnv *, jobject, jlong h, jint u) {
    Session *s = S(h);
    if (s) s->audio.setUsage(u);
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_guestHung(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    return s && s->hung.load();
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_boost(JNIEnv *, jobject, jlong h, jint ms) {
    Session *s = S(h);
    if (!s) return;
    s->boostUntilNs = s->m->nowNs() + (uint64_t)ms * 1000000ull;
    s->boostNow = true;
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setRotation(JNIEnv *, jobject, jlong h, jint q) {
    Session *s = S(h);
    if (s) s->gl.setRotation(q);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_touch(JNIEnv *, jobject, jlong h,
                                                                               jboolean down, jint x, jint y) {
    Session *s = S(h);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->inMu);
    s->inputs.push_back({InputEvent::TOUCH, (bool)down, x, y});
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_key(JNIEnv *, jobject, jlong h, jint code,
                                                                             jboolean down) {
    Session *s = S(h);
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->inMu);
    s->inputs.push_back({InputEvent::KEY, (bool)down, code, 0});
}

JNIEXPORT jintArray JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_screenSize(JNIEnv *env, jobject,
                                                                                         jlong h) {
    Session *s = S(h);
    jint v[2] = {0, 0};
    if (s) { v[0] = s->m->lcd->width(); v[1] = s->m->lcd->height(); }
    jintArray a = env->NewIntArray(2);
    env->SetIntArrayRegion(a, 0, 2, v);
    return a;
}

JNIEXPORT jstring JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_status(JNIEnv *env, jobject, jlong h) {
    Session *s = S(h);
    if (!s) return env->NewStringUTF("");
    char buf[160];
    int peak = s->audioPeak.exchange(0);
    uint64_t frames = s->audioFrames.exchange(0);
    char snd[32] = "";
    if (frames) snprintf(snd, sizeof snd, "  sound %d%%", peak * 100 / 32767);
    snprintf(buf, sizeof buf, "%.0f MIPS active  %d%% idle%s%s", s->activeMips.load(), s->idlePct.load(), snd,
             s->m->bootComplete ? "" : "  booting");
    return env->NewStringUTF(buf);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setMicEnabled(JNIEnv *, jobject, jlong h,
                                                                                       jboolean on) {
    Session *s = S(h);
    if (!s) return;
    if (on) s->audio.startInput();
    else s->audio.stopInput();
}

// True while the guest records (or did within the last two seconds).
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_micWanted(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s) return false;
    uint64_t last = s->lastCaptureNs.load();
    return last && s->m->nowNs() - last < 2000000000ull;
}

// Snapshots: call only while paused. A failed restore leaves the machine unusable (destroy it).
JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_saveState(JNIEnv *env, jobject, jlong h,
                                                                                      jstring path) {
    Session *s = S(h);
    if (!s || s->running) return false;
    const char *p = env->GetStringUTFChars(path, nullptr);
    std::string err, sp = p;
    env->ReleaseStringUTFChars(path, p);
    bool ok = s->m->saveState(sp, err);
    if (!ok) LOGW("snapshot save failed: %s", err.c_str());
    return ok;
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_loadState(JNIEnv *env, jobject, jlong h,
                                                                                      jstring path) {
    Session *s = S(h);
    if (!s || s->running) return false;
    const char *p = env->GetStringUTFChars(path, nullptr);
    std::string err, sp = p;
    env->ReleaseStringUTFChars(path, p);
    bool ok = s->m->loadState(sp, err);
    if (!ok) LOGW("snapshot restore failed: %s", err.c_str());
    // Remount \Storage Card so the guest sees the current share (and runs the WMDroid helper).
    if (ok) s->m->folderShare->onGuestReset();
    return ok;
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_bootComplete(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    return s && s->m->bootComplete.load();
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setShareMounts(JNIEnv *env, jobject, jlong h,
                                                                                        jobjectArray names,
                                                                                        jobjectArray paths) {
    Session *s = S(h);
    if (!s) return;
    std::vector<FolderShare::Mount> mounts;
    jsize n = env->GetArrayLength(names);
    for (jsize i = 0; i < n && i < env->GetArrayLength(paths); i++) {
        auto str = [&](jobjectArray a) {
            jstring js = (jstring)env->GetObjectArrayElement(a, i);
            const char *c = env->GetStringUTFChars(js, nullptr);
            std::string out = c;
            env->ReleaseStringUTFChars(js, c);
            env->DeleteLocalRef(js);
            return out;
        };
        mounts.push_back({str(names), str(paths)});
    }
    s->m->folderShare->setMounts(mounts);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setShareOverlays(JNIEnv *env, jobject, jlong h,
                                                                                          jobjectArray names,
                                                                                          jobjectArray paths) {
    Session *s = S(h);
    if (!s) return;
    std::vector<FolderShare::Mount> overlays;
    jsize n = env->GetArrayLength(names);
    for (jsize i = 0; i < n && i < env->GetArrayLength(paths); i++) {
        auto str = [&](jobjectArray a) {
            jstring js = (jstring)env->GetObjectArrayElement(a, i);
            const char *c = env->GetStringUTFChars(js, nullptr);
            std::string out = c;
            env->ReleaseStringUTFChars(js, c);
            env->DeleteLocalRef(js);
            return out;
        };
        overlays.push_back({str(names), str(paths)});
    }
    s->m->folderShare->setOverlays(overlays);
}

// DE battery device, as battdrvr.dll reads it every 500 ms (docs/research-battery.md):
// byte 0 = 0 on AC / non-zero on battery, byte 1 = percent used, u16 at +2 = temperature in 0.1 C.
JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_setBattery(JNIEnv *, jobject, jlong h,
                                                                                    jboolean ac, jint percent,
                                                                                    jint tenthsC) {
    Session *s = S(h);
    if (!s) return;
    uint8_t *b = s->m->dePage->battery;
    int pct = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    int t = tenthsC < 0 ? 0 : tenthsC > 0xFFFF ? 0xFFFF : tenthsC;
    b[0] = ac ? 0 : 1;
    b[1] = (uint8_t)(100 - pct);
    b[2] = (uint8_t)(t & 0xFF);
    b[3] = (uint8_t)(t >> 8);
}

// Notification LED / vibrator (core/src/board/nled.h, docs/research-notification-led.md):
// bit0 light on, bit1 blinking, bit2 vibrate (on now or pulsed since the previous call),
// bits 8..15 blink on time in 50 ms units (0 = unknown), bits 16..23 vibrator start count mod 256.
// Each call consumes the vibrate latch, so poll it from one place only.
JNIEXPORT jint JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_ledState(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (!s || !s->m) return 0;
    return (jint)s->m->dePage->nled.state();
}

// ---- Virtual GSM modem (core/src/board/modem.h): rilgsm images (WM 6 / 6.1 / 6.1.4) ----

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemNetwork(
    JNIEnv *env, jobject, jlong h, jboolean service, jint signal, jboolean roaming, jstring op, jstring numeric,
    jstring data, jstring number) {
    Session *s = S(h);
    if (!s || !s->m) return;
    auto str = [env](jstring j) {
        if (!j) return std::string();
        const char *c = env->GetStringUTFChars(j, nullptr);
        std::string r = c ? c : "";
        if (c) env->ReleaseStringUTFChars(j, c);
        return r;
    };
    Modem::Network n;
    n.service = service;
    n.signal = signal;
    n.roaming = roaming;
    n.op = str(op);
    n.numeric = str(numeric);
    n.data = str(data);
    n.number = str(number);
    // A quote would end the AT string early.
    for (std::string *t : {&n.op, &n.numeric, &n.number}) t->erase(std::remove(t->begin(), t->end(), '"'), t->end());
    s->m->modem->setNetwork(n);
    s->m->codePatch->setOwnNumber(n.number);  // FakeRIL images (WM 6.5.3, WM 5): the SIM's own number
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemCallState(JNIEnv *env, jobject, jlong h,
                                                                                       jint id, jstring state) {
    Session *s = S(h);
    if (!s || !s->m || !state) return;
    const char *c = env->GetStringUTFChars(state, nullptr);
    if (!c) return;
    s->m->modem->callState(id, c);
    env->ReleaseStringUTFChars(state, c);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemRing(JNIEnv *env, jobject, jlong h,
                                                                                  jstring number) {
    Session *s = S(h);
    if (!s || !s->m) return;
    const char *c = number ? env->GetStringUTFChars(number, nullptr) : nullptr;
    s->m->modem->ring(c ? c : "");
    if (c) env->ReleaseStringUTFChars(number, c);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemSms(JNIEnv *env, jobject, jlong h, jstring pdu) {
    Session *s = S(h);
    if (!s || !s->m || !pdu) return;
    const char *c = env->GetStringUTFChars(pdu, nullptr);
    if (!c) return;
    s->m->modem->smsDeliver(c);
    env->ReleaseStringUTFChars(pdu, c);
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemSmsResult(JNIEnv *, jobject, jlong h, jint mr,
                                                                                       jboolean sent) {
    Session *s = S(h);
    if (s && s->m) s->m->modem->smsResult(mr, sent);
}

JNIEXPORT jstring JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemEvents(JNIEnv *env, jobject, jlong h) {
    Session *s = S(h);
    std::string e = s && s->m ? s->m->modem->takeEvents() : std::string();
    return env->NewStringUTF(e.c_str());
}

JNIEXPORT void JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemRefresh(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    if (s && s->m) s->m->modem->refresh();
}

JNIEXPORT jboolean JNICALL Java_jp_net_seventhreeseven_wmdroid_NativeBridge_modemActive(JNIEnv *, jobject, jlong h) {
    Session *s = S(h);
    return s && s->m && s->m->modem->claimed();
}

}  // extern "C"
