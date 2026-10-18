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
    // Passive-matrix (CSTN) LCD look, each effect 0..100: pixel grid, washed-out contrast and tint,
    // crosstalk streaks, uneven edge-lit backlight, and ghosting (the slow liquid crystal).
    void setStn(bool on, int grid, int contrast, int crosstalk, int backlight, int ghost);
    ~GlRenderer();

    // Emulation thread. Presents only when the frame changed (or the window/filter did), uploading
    // just the rows that differ: an idle Windows Mobile screen costs no GPU work and no swaps.
    void draw(const uint8_t *pixels, int w, int h, int bpp);
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
    GLuint programs[3] = {}, tex = 0, vbo = 0;
    // Passive-matrix look: the crystal's state (guest resolution, ping-pong), the frame's row and
    // column load for crosstalk, and the backlight's brightness map.
    struct Stn { bool on = false; float grid = 0, contrast = 0, crosstalk = 0, backlight = 0, ghost = 0; };
    Stn stnWanted, stn;  // stnWanted under mu
    GLuint stnState = 0, stnFinal = 0, stnTex[2] = {}, stnFbo = 0, rowTex = 0, colTex = 0, lightTex = 0;
    int stnCur = 0, stnW = 0, stnH = 0;
    bool stnPrimed = false;
    uint64_t stnLastNs = 0, settleUntilNs = 0;
    float meanLoad = 0;
    std::vector<uint8_t> rowLoad, colLoad;
    int texW = 0, texH = 0, texBpp = 0;
    int filter = SHARP;
    std::atomic<bool> redraw{true};
    std::atomic<bool> presenting{true};
    std::atomic<int> rotation{0};
    std::vector<uint8_t> shadow;  // the frame last uploaded
    int surfW = 0, surfH = 0;

    void applyPending();
    bool ensureContext();
    void destroySurface();
    GLuint buildProgram(const char *fragment, const char *vertex = nullptr);
    void stnSetup(int w, int h);
    void stnLoad(int w, int h, int bpp);
    void stnStep(int w, int h, uint64_t now);
};
