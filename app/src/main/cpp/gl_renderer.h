// Presents the guest frame buffer through OpenGL ES at the window's full resolution, so the
// upscaling filter is ours (smooth, sharp pixels, or bicubic) rather than the compositor's.
// All EGL/GL calls happen on the emulation thread.
#pragma once
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <vector>

class GlRenderer {
public:
    enum Filter { SMOOTH = 0, SHARP = 1, ENHANCED = 2 };
    enum Panel { PANEL_CLEAN = 0, PANEL_TN = 1, PANEL_STN = 2, PANEL_AMOLED = 3 };

    // Realism filters: the kind of panel and how strongly each of its traits shows (0..1), then the
    // layers in front of it that any panel can have.
    struct Realism {
        int panel = PANEL_CLEAN;
        float grid = 0, contrast = 0, crosstalk = 0, backlight = 0, ghost = 0, angle = 0;
        bool cold = false;      // a cold phone makes the liquid crystal slower
        float touchLayer = 0;   // resistive film: haze, grain, Newton rings, the stylus dimple
        float ripple = 0;       // the LCD pooling round a hard press
        float wear = 0;         // scratches and smudges on the screen protector
        float glare = 0;        // sunlight on the glass (from the light sensor)
        float tear = 0;         // no vsync: a frame sometimes shows half old, half new
        int dead = 0;           // dead or stuck pixels (up to 8)
        bool fade = false;      // the backlight fades up on wake and down on sleep
        uint32_t seed = 1;      // where the dead pixels, scratches and prints are, per install
        float hdr = 1;          // the screen's white, times SDR white (1 = SDR output)
        bool any() const {
            return panel != PANEL_CLEAN || touchLayer > 0 || ripple > 0 || wear > 0 || glare > 0 ||
                   tear > 0 || dead > 0 || fade || hdr > 1.01f;
        }
    };

    // Called from the UI thread. With `wait`, blocks until the emulation thread has let go of the
    // previous window (required when the surface is being destroyed).
    void setWindow(ANativeWindow *win, bool emuRunning);
    void setFilter(int f) { filter = f; redraw = true; }
    // False while the phone's screen is off. Swapping then blocks in the buffer queue and the
    // emulation thread (the guest) does not run again until the screen is back.
    void setPresenting(bool on) { presenting = on; }
    // Shows the frame turned by quarter turns clockwise (0 upright, 1 = 90 degrees, 3 = 270),
    // e.g. a landscape Windows Mobile on a phone whose window is landscape too.
    void setRotation(int q) { rotation = q; redraw = true; }
    void setRealism(const Realism &r);
    // The phone's sensors (UI thread): where the viewer is relative to the resting pose (-1..1 per
    // axis, + = looking from the right / from above), ambient light in lux (0 = unknown), the
    // phone's temperature in C, and its screen brightness 0..1.
    void setSensors(float tiltX, float tiltY, float lux, float tempC, float brightness);
    // True once the window surface is extended-range (HDR): the app then lifts its brightness.
    bool hdrActive() const { return hdrSurface.load(); }
    ~GlRenderer();

    // Emulation thread. Presents only when the frame changed (or the window/filter did), uploading
    // just the rows that differ: an idle Windows Mobile screen costs no GPU work and no swaps.
    void draw(const uint8_t *pixels, int w, int h, int bpp);
    // Emulation thread, while Windows Mobile's display is off: lets the backlight fade out and the
    // crystal settle. Draws nothing without the realism filters.
    void drawOff();
    // Emulation thread: the stylus, in guest pixels (the resistive film and the press ripple).
    void touch(bool down, int x, int y);
    uint64_t presented = 0;  // frames actually drawn
    void releaseThread();  // before the emulation thread exits

private:
    std::mutex mu;
    std::condition_variable cv;
    ANativeWindow *pending = nullptr;
    bool hasPending = false;
    ANativeWindow *window = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
    bool contextHdr = false;  // the context's config is FP16 (scRGB surfaces)
    bool hdrUnsupported = false;  // no FP16 scRGB window surfaces on this phone
    std::atomic<bool> hdrSurface{false};
    GLuint programs[3] = {}, tex = 0, vbo = 0;
    int texW = 0, texH = 0, texBpp = 0;
    int filter = SHARP;
    std::atomic<bool> redraw{true};
    std::atomic<bool> presenting{true};
    std::atomic<int> rotation{0};
    std::vector<uint8_t> shadow;  // the frame last uploaded
    int surfW = 0, surfH = 0;

    // ---- Realism ----
    // Pass 1 (guest resolution) keeps the panel's state: it creeps towards each frame (ghosting,
    // OLED smear), with crosstalk added and a tear line holding back the rows below it. Pass 2 draws
    // that state through the panel and its layers.
    Realism wanted, rl;  // wanted under mu
    struct Sensors { float tiltX = 0, tiltY = 0, lux = 0, tempC = 25, brightness = 1; };
    Sensors sensorsIn, sensors, sensorsDrawn;  // sensorsIn under mu
    GLuint stateProg = 0, finalProg = 0, stateTex[2] = {}, fbo = 0, rowTex = 0, colTex = 0, lightTex = 0, wearTex = 0;
    int stateCur = 0, stateW = 0, stateH = 0;
    bool statePrimed = false;
    uint64_t lastStepNs = 0, settleUntilNs = 0, animUntilNs = 0;
    float meanLoad = 0;
    std::vector<uint8_t> rowLoad, colLoad;
    // Display on / off and the backlight's fade level.
    bool lcdOn = true;
    float level = 1;
    // Stylus: down, where, when it went down / up; the dimple and the ripple's strength now.
    bool penDown = false;
    float penX = 0, penY = 0;
    uint64_t penDownNs = 0, penUpNs = 0;
    float rippleLevel = 0, dimpleLevel = 0;
    uint64_t lastAnimNs = 0;  // the fade, dimple and ripple were last advanced
    float tearY = 2;  // this frame's tear line (2 = none)
    uint32_t tearRand = 12345;
    float deadPx[24] = {};  // x, y, kind per dead pixel (kind 0 dead, 1-3 stuck R/G/B)
    float ringX = 0, ringY = 0;
    uint32_t deadSeed = 0;
    uint32_t wearSeed = 0;
    int deadCount = -1, deadW = 0, deadH = 0;

    void applyPending();
    bool ensureContext(bool wantHdr);
    void destroySurface();
    void resetGl();
    GLuint buildProgram(const char *fragment, const char *vertex = nullptr);
    void present(const uint8_t *pixels, int w, int h, int bpp, bool off);
    void realismSetup(int w, int h);
    void realismLoad(int w, int h, int bpp);
    void realismStep(int w, int h, uint64_t now, bool off);
    void realismUniforms(GLuint p, int w, int h, uint64_t now);
    bool realismAnimating(uint64_t now) const;
};
