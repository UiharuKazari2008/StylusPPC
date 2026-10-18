#include "gl_renderer.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include "util/log.h"

namespace {

const char *kVertex = R"(
attribute vec2 pos;
varying vec2 uv;
uniform mat2 rot;     // turns the frame by quarter turns (see GlRenderer::setRotation)
uniform vec2 rotOff;
void main() {
    uv = rot * vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5) + rotOff;
    gl_Position = vec4(pos, 0.0, 1.0);
})";

// Plain bilinear.
const char *kSmooth = R"(
precision mediump float;
varying vec2 uv;
uniform sampler2D tex;
void main() { gl_FragColor = texture2D(tex, uv); })";

// Sharp bilinear: nearest-neighbour inside each guest pixel, a one-screen-pixel blend at its edges.
const char *kSharp = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;
uniform vec2 texSize;
uniform vec2 outSize;
void main() {
    vec2 texel = uv * texSize;
    vec2 scale = max(outSize / texSize, vec2(1.0));
    vec2 base = floor(texel - 0.5) + 0.5;
    vec2 f = clamp((texel - base - 0.5) * scale + 0.5, 0.0, 1.0);
    gl_FragColor = texture2D(tex, (base + f) / texSize);
})";

// Catmull-Rom bicubic using nine bilinear taps.
const char *kEnhanced = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;
uniform vec2 texSize;
void main() {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 texPos0 = (texPos1 - 1.0) / texSize;
    vec2 texPos3 = (texPos1 + 2.0) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;
    vec4 c = vec4(0.0);
    c += texture2D(tex, vec2(texPos0.x, texPos0.y)) * w0.x * w0.y;
    c += texture2D(tex, vec2(texPos12.x, texPos0.y)) * w12.x * w0.y;
    c += texture2D(tex, vec2(texPos3.x, texPos0.y)) * w3.x * w0.y;
    c += texture2D(tex, vec2(texPos0.x, texPos12.y)) * w0.x * w12.y;
    c += texture2D(tex, vec2(texPos12.x, texPos12.y)) * w12.x * w12.y;
    c += texture2D(tex, vec2(texPos3.x, texPos12.y)) * w3.x * w12.y;
    c += texture2D(tex, vec2(texPos0.x, texPos3.y)) * w0.x * w3.y;
    c += texture2D(tex, vec2(texPos12.x, texPos3.y)) * w12.x * w3.y;
    c += texture2D(tex, vec2(texPos3.x, texPos3.y)) * w3.x * w3.y;
    gl_FragColor = vec4(clamp(c.rgb, 0.0, 1.0), 1.0);
})";

// ---- Passive-matrix (CSTN) LCD ----
// Pass 1 runs at the guest's resolution into a texture that holds the liquid crystal's state: the
// frame plus crosstalk is the target, and the state creeps towards it (the slow STN response that
// leaves trails behind a moving cursor and makes scrolling text smear). Pass 2 draws that state to
// the window through the panel: washed-out colour, the pixel grid and the edge-lit backlight.
const char *kStnVertex = R"(
attribute vec2 pos;
varying vec2 uv;
void main() {
    uv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
})";

const char *kStnState = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;    // the guest frame
uniform sampler2D prev;   // the crystal last frame
uniform sampler2D rows;   // how dark each row is on average (h x 1)
uniform sampler2D cols;   // and each column (w x 1)
uniform float meanLoad;
uniform float crosstalk;
uniform vec2 alpha;       // share of the way covered this frame: towards brighter (x), darker (y)
void main() {
    vec3 t = texture2D(tex, uv).rgb;
    // Crosstalk (shadowing): a passive matrix drives a whole column and row at once, so a pixel
    // picks up some of the voltage meant for the rest of its line. Dark content casts faint streaks
    // along its columns and rows; light lines come out a little lighter.
    float rl = texture2D(rows, vec2(uv.y, 0.5)).r;
    float cl = texture2D(cols, vec2(uv.x, 0.5)).r;
    float xt = crosstalk * (0.7 * (cl - meanLoad) + 0.3 * (rl - meanLoad));
    t = clamp(t * (1.0 - 0.9 * xt), 0.0, 1.0);
    vec3 p = texture2D(prev, uv).rgb;
    vec3 d = t - p;
    vec3 s = d * mix(vec3(alpha.y), vec3(alpha.x), step(0.0, d));
    // An 8-bit state would stall just short of the target: always move at least one step.
    s = sign(d) * max(abs(s), min(abs(d), vec3(1.0 / 255.0)));
    gl_FragColor = vec4(p + s, 1.0);
})";

const char *kStnFinal = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;    // the crystal's state
uniform sampler2D light;  // backlight brightness (stored / 1.5)
uniform vec2 texSize;
uniform vec2 outSize;
uniform float grid, contrast, backlight, landscape;

// Share of the screen pixel [x - hw, x + hw] (in guest pixels) that falls on the lit part of a
// guest pixel, with a gap of g on each side of every pixel.
float lit(float x, float hw, float g) {
    float s = 0.0;
    for (int k = -1; k <= 1; k++)
        s += max(min(x + hw, float(k) + 1.0 - g) - max(x - hw, float(k) + g), 0.0);
    return s / (2.0 * hw);
}

// The same per R, G and B stripe of the lit part.
vec3 stripes(float x, float hw, float g) {
    float w = (1.0 - 2.0 * g) / 3.0;
    vec3 s = vec3(0.0);
    for (int k = -1; k <= 1; k++) {
        vec3 a = float(k) + g + vec3(0.0, w, 2.0 * w);
        s += max(min(vec3(x + hw), a + w) - max(vec3(x - hw), a), 0.0);
    }
    return s / (2.0 * hw);
}

void main() {
    // Sharp bilinear, as the Sharp pixels filter.
    vec2 texel = uv * texSize;
    vec2 scale = max(outSize / texSize, vec2(1.0));
    vec2 base = floor(texel - 0.5) + 0.5;
    vec2 f = clamp((texel - base - 0.5) * scale + 0.5, 0.0, 1.0);
    vec3 t = texture2D(tex, (base + f) / texSize).rgb;
    // The panel in its own upright frame (the backlight's LEDs sit along its bottom edge).
    vec2 bp = landscape > 0.5 ? vec2(uv.y, 1.0 - uv.x) : uv;

    // Colour STN: weak colour filters (muddy, desaturated), a shallow response that also shifts
    // with the viewing angle (lighter towards the top), light leaking through "off" pixels (grey-
    // blue blacks) and a greenish, dimmer white.
    float y = dot(t, vec3(0.299, 0.587, 0.114));
    t = mix(t, vec3(y), 0.4 * contrast);
    t = pow(max(t, vec3(0.0)), vec3(1.0 + contrast * (0.3 * (bp.y - 0.5) - 0.18)));
    vec3 black = vec3(0.12, 0.14, 0.21) * contrast;
    vec3 white = mix(vec3(1.0), vec3(0.88, 0.92, 0.82), contrast);
    vec3 c = mix(black, white, t);

    // Pixel grid: the gaps between the electrodes, and the R, G, B stripes once a guest pixel is
    // big enough on the phone to show them.
    vec2 fr = fract(texel);
    vec2 hw = 0.5 / scale;
    float g = 0.11 * grid;
    float sub = grid * clamp((scale.x - 2.0) / 3.0, 0.0, 1.0);
    vec3 mx = mix(vec3(lit(fr.x, hw.x, g)), 3.0 * stripes(fr.x, hw.x, g), sub);
    vec3 mask = mx * lit(fr.y, hw.y, g);
    // The eye adapts to the darker grid somewhat; give half of the lost light back.
    mask *= mix(1.0, 1.0 / ((1.0 - 2.0 * g) * (1.0 - 2.0 * g)), 0.5);

    vec3 bl = mix(vec3(1.0), texture2D(light, bp).rgb * 1.5, backlight);
    gl_FragColor = vec4(clamp(c * mask * bl, 0.0, 1.0), 1.0);
})";

// Smooth value noise on a fixed lattice, -1..1.
float valueNoise(float x, float y) {
    auto hash = [](int i, int j) {
        uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + 0x9E3779B9u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 32767.5f - 1.f;
    };
    int i = (int)std::floor(x), j = (int)std::floor(y);
    float fx = x - i, fy = y - j;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash(i, j) + (hash(i + 1, j) - hash(i, j)) * fx;
    float b = hash(i, j + 1) + (hash(i + 1, j + 1) - hash(i, j + 1)) * fx;
    return a + (b - a) * fy;
}

// An edge-lit backlight of a 2008 phone: four white LEDs under the bottom edge whose hot spots fan
// into the light guide, brightness falling off towards the top and the far corners, a blotchy
// diffuser, light leaking around the frame, and blue-white near the LEDs turning warmer far away.
// RGB, portrait, stored / 1.5 (so values up to 1.5 fit), mean brightness 1.
std::vector<uint8_t> backlightMap(int W, int H) {
    std::vector<float> f((size_t)W * H * 3);
    double sum = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (x + 0.5f) / W, v = (y + 0.5f) / H;  // v = 1 at the LED edge
            float b = 0.78f + 0.24f * v * v;
            for (int i = 0; i < 4; i++) {
                float dx = (u - (i + 0.5f) / 4) / 0.075f, dy = (1 - v) / 0.09f;
                b += 0.32f * std::exp(-(dx * dx + dy * dy));
            }
            float cx = 2 * u - 1, top = 1 - v;
            b *= 1 - 0.12f * cx * cx * cx * cx - 0.10f * top * top * top * cx * cx;
            b *= 1 + 0.07f * valueNoise(u * 3.1f, v * 5.3f) + 0.035f * valueNoise(u * 7.7f + 13, v * 12.9f + 7);
            float edge = std::min(std::min(u, 1 - u) * W / (float)H * 1.7f, std::min(v, 1 - v));
            b += 0.12f * std::exp(-edge / 0.012f);
            float warm = top * top;
            float *o = &f[((size_t)y * W + x) * 3];
            o[0] = b * (0.97f + 0.07f * warm);
            o[1] = b;
            o[2] = b * (1.05f - 0.14f * warm);
            sum += b;
        }
    float norm = (float)(W * H / sum);
    std::vector<uint8_t> out(f.size());
    for (size_t i = 0; i < f.size(); i++) out[i] = (uint8_t)std::min(255.f, f[i] * norm / 1.5f * 255.f + 0.5f);
    return out;
}

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader: %s", log);
    }
    return s;
}

}  // namespace

void GlRenderer::setWindow(ANativeWindow *win, bool emuRunning) {
    if (win) ANativeWindow_acquire(win);
    std::unique_lock<std::mutex> lk(mu);
    if (hasPending && pending) ANativeWindow_release(pending);
    pending = win;
    hasPending = true;
    if (!emuRunning) {
        applyPending();  // no emulation thread holds the context: switch right here
        return;
    }
    // The surface may be torn down as soon as we return: wait for the emulation thread to let go.
    cv.wait_for(lk, std::chrono::milliseconds(500), [this] { return !hasPending; });
}

void GlRenderer::applyPending() {
    if (!hasPending) return;
    redraw = true;
    destroySurface();
    if (window) ANativeWindow_release(window);
    window = pending;
    pending = nullptr;
    hasPending = false;
    if (window) {
        // Tell the compositor the content is 60 Hz at most, so a 120 Hz LTPO panel can drop its
        // refresh rate (and power) while Windows Mobile is shown. API 30+, looked up at runtime.
        using SetFrameRate = int32_t (*)(ANativeWindow *, float, int8_t);
        static auto setRate = (SetFrameRate)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
        if (setRate) setRate(window, 60.f, 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
        // Give up a swap after 50 ms. With the screen off the compositor stops taking buffers and
        // eglSwapBuffers otherwise waits until the phone wakes, which freezes the guest.
        using SetDequeueTimeout = int32_t (*)(ANativeWindow *, int64_t);
        static auto setTimeout = (SetDequeueTimeout)dlsym(RTLD_DEFAULT, "ANativeWindow_setDequeueTimeout");
        if (setTimeout) setTimeout(window, 50000000);
    }
    cv.notify_all();
}

void GlRenderer::destroySurface() {
    if (display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    surface = EGL_NO_SURFACE;
}

GLuint GlRenderer::buildProgram(const char *fragment, const char *vertex) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vertex ? vertex : kVertex));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    return p;
}

bool GlRenderer::ensureContext() {
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(display, attrs, &config, 1, &n);
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
        if (context == EGL_NO_CONTEXT) { LOGE("gl: no context"); return false; }
    }
    if (surface == EGL_NO_SURFACE) {
        if (!window) return false;
        surface = eglCreateWindowSurface(display, config, window, nullptr);
        if (surface == EGL_NO_SURFACE) return false;
        eglMakeCurrent(display, surface, surface, context);
        eglSwapInterval(display, 0);  // never block emulation on vsync
        if (!programs[0]) {
            programs[SMOOTH] = buildProgram(kSmooth);
            programs[SHARP] = buildProgram(kSharp);
            programs[ENHANCED] = buildProgram(kEnhanced);
            stnState = buildProgram(kStnState, kStnVertex);
            stnFinal = buildProgram(kStnFinal);
            const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
            glGenTextures(1, &tex);
        }
    }
    eglMakeCurrent(display, surface, surface, context);
    return true;
}

void GlRenderer::setStn(bool on, int grid, int contrast, int crosstalk, int backlight, int ghost) {
    auto k = [](int v) { return (v < 0 ? 0 : v > 100 ? 100 : v) / 100.f; };
    std::lock_guard<std::mutex> lk(mu);
    stnWanted = {on, k(grid), k(contrast), k(crosstalk), k(backlight), k(ghost)};
    redraw = true;
}

namespace {
GLuint makeTexture(int w, int h, GLenum fmt, const void *data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
}  // namespace

// The crystal's state textures (and the crosstalk lines) at the guest's resolution.
void GlRenderer::stnSetup(int w, int h) {
    if (!lightTex) {
        const int W = 64, H = 108;
        std::vector<uint8_t> map = backlightMap(W, H);
        lightTex = makeTexture(W, H, GL_RGB, map.data());
    }
    if (w == stnW && h == stnH && stnFbo) return;
    if (stnTex[0]) glDeleteTextures(2, stnTex);
    if (rowTex) glDeleteTextures(1, &rowTex);
    if (colTex) glDeleteTextures(1, &colTex);
    if (!stnFbo) glGenFramebuffers(1, &stnFbo);
    stnTex[0] = makeTexture(w, h, GL_RGBA, nullptr);
    stnTex[1] = makeTexture(w, h, GL_RGBA, nullptr);
    rowLoad.assign(h, 0);
    colLoad.assign(w, 0);
    rowTex = makeTexture(h, 1, GL_LUMINANCE, rowLoad.data());
    colTex = makeTexture(w, 1, GL_LUMINANCE, colLoad.data());
    stnW = w;
    stnH = h;
    stnPrimed = false;
}

// How dark each row and column of the frame is on average (every other pixel is plenty).
void GlRenderer::stnLoad(int w, int h, int bpp) {
    std::vector<uint32_t> rows(h, 0), cols(w, 0);
    const uint8_t *px = shadow.data();
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            int r, g, b;
            if (bpp == 16) {
                uint16_t v = ((const uint16_t *)px)[(size_t)y * w + x];
                r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
            } else {
                const uint8_t *q = px + ((size_t)y * w + x) * 4;
                r = q[0], g = q[1], b = q[2];
            }
            uint32_t dark = 255 - ((r * 77 + g * 150 + b * 29) >> 8);
            rows[y] += dark;
            cols[x] += dark;
        }
    int perRow = (w + 1) / 2, perCol = (h + 1) / 2;
    uint64_t all = 0;
    for (int y = 0; y < h; y += 2) {
        all += rows[y];
        rowLoad[y] = (uint8_t)(rows[y] / perRow);
        if (y + 1 < h) rowLoad[y + 1] = rowLoad[y];
    }
    for (int x = 0; x < w; x += 2) {
        colLoad[x] = (uint8_t)(cols[x] / perCol);
        if (x + 1 < w) colLoad[x + 1] = colLoad[x];
    }
    meanLoad = (float)all / ((uint64_t)perRow * perCol) / 255.f;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, rowTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, h, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, rowLoad.data());
    glBindTexture(GL_TEXTURE_2D, colTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, colLoad.data());
}

// One step of the crystal towards the current frame, `now` - the last step later.
void GlRenderer::stnStep(int w, int h, uint64_t now) {
    float dt = stnPrimed ? (now - stnLastNs) / 1e9f : 1e9f;
    stnLastNs = now;
    stnPrimed = true;
    // STN takes about a quarter of a second: relaxing (going lighter) slower than being driven.
    float tauLight = 0.30f * stn.ghost, tauDark = 0.20f * stn.ghost;
    float aLight = tauLight > 0 ? 1 - std::exp(-dt / tauLight) : 1;
    float aDark = tauDark > 0 ? 1 - std::exp(-dt / tauDark) : 1;
    glBindFramebuffer(GL_FRAMEBUFFER, stnFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stnTex[1 - stnCur], 0);
    glViewport(0, 0, w, h);
    GLuint p = stnState;
    glUseProgram(p);
    GLuint units[4] = {tex, stnTex[stnCur], rowTex, colTex};
    const char *names[4] = {"tex", "prev", "rows", "cols"};
    for (int i = 3; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, units[i]);
        glUniform1i(glGetUniformLocation(p, names[i]), i);
    }
    glUniform1f(glGetUniformLocation(p, "meanLoad"), meanLoad);
    glUniform1f(glGetUniformLocation(p, "crosstalk"), stn.crosstalk);
    glUniform2f(glGetUniformLocation(p, "alpha"), aLight, aDark);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    stnCur = 1 - stnCur;
}

void GlRenderer::draw(const uint8_t *pixels, int w, int h, int bpp) {
    Stn want;
    {
        std::lock_guard<std::mutex> lk(mu);
        applyPending();
        want = stnWanted;
    }
    // Phone asleep, or the window is gone: do not swap. The guest keeps running.
    if (!window || !presenting.load()) return;
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // Which rows changed since the last upload (cheap next to a full upload and swap).
    size_t row = (size_t)w * (bpp / 8), bytes = row * h;
    // A resized window (same surface) needs the whole frame drawn again at the new size.
    int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
    bool resized = ww != surfW || wh != surfH;
    surfW = ww;
    surfH = wh;
    bool full = redraw.exchange(false) || resized || w != texW || h != texH || bpp != texBpp || shadow.size() != bytes;
    int y0 = 0, y1 = h - 1;
    if (!full) {
        while (y0 < h && memcmp(pixels + y0 * row, shadow.data() + y0 * row, row) == 0) y0++;
        while (y1 > y0 && memcmp(pixels + y1 * row, shadow.data() + y1 * row, row) == 0) y1--;
    }
    bool changed = full || y0 < h;
    stn = want;
    bool ghosting = stn.on && stn.ghost > 0;
    // Unchanged: nothing to present, unless the crystal is still catching up with the frame (then
    // about 60 steps a second until it has).
    if (!changed && (!ghosting || now >= settleUntilNs || now - stnLastNs < 15000000ull)) return;
    if (!ensureContext()) { redraw = true; return; }
    if (changed) {
        if (shadow.size() != bytes) shadow.resize(bytes);
        memcpy(shadow.data() + y0 * row, pixels + y0 * row, (size_t)(y1 - y0 + 1) * row);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, bpp == 16 ? 2 : 4);
        GLenum fmt = bpp == 16 ? GL_RGB : GL_RGBA;
        GLenum type = bpp == 16 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
        if (w != texW || h != texH || bpp != texBpp) {
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, type, pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            texW = w;
            texH = h;
            texBpp = bpp;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0 + 1, fmt, type, pixels + y0 * row);
        }
        // Long enough for the slowest transition to finish; then the screen goes idle again.
        if (ghosting) settleUntilNs = now + (uint64_t)((6 * 0.30f * stn.ghost + 0.5f) * 1e9f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    if (stn.on) {
        stnSetup(w, h);
        if (changed && stn.crosstalk > 0) stnLoad(w, h, bpp);
        stnStep(w, h, now);
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
    glViewport(0, 0, sw, sh);
    int f = filter < 0 || filter > 2 ? SHARP : filter;
    GLuint p = stn.on ? stnFinal : programs[f];
    glUseProgram(p);
    if (stn.on) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lightTex);
        glUniform1i(glGetUniformLocation(p, "light"), 1);
        glUniform1f(glGetUniformLocation(p, "grid"), stn.grid);
        glUniform1f(glGetUniformLocation(p, "contrast"), stn.contrast);
        glUniform1f(glGetUniformLocation(p, "backlight"), stn.backlight);
        glUniform1f(glGetUniformLocation(p, "landscape"), w > h ? 1.f : 0.f);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, stn.on ? stnTex[stnCur] : tex);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glUniform2f(glGetUniformLocation(p, "texSize"), (float)w, (float)h);
    // Quarter turns: the frame is shown turned clockwise (1) or anticlockwise (3).
    int q = rotation.load() & 3;
    static const float kRot[4][6] = {
        {1, 0, 0, 1, 0, 0}, {0, -1, 1, 0, 0, 1}, {-1, 0, 0, -1, 1, 1}, {0, 1, -1, 0, 1, 0}};
    glUniformMatrix2fv(glGetUniformLocation(p, "rot"), 1, GL_FALSE, kRot[q]);
    glUniform2f(glGetUniformLocation(p, "rotOff"), kRot[q][4], kRot[q][5]);
    if (q & 1) glUniform2f(glGetUniformLocation(p, "outSize"), (float)sh, (float)sw);
    else glUniform2f(glGetUniformLocation(p, "outSize"), (float)sw, (float)sh);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!presenting.load()) return;
    if (!eglSwapBuffers(display, surface)) {
        EGLint err = eglGetError();
        // Timed out or the window was taken down (screen off). Drop this surface and try again
        // next frame; the context stays.
        if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) destroySurface();
        redraw = true;
        return;
    }
    presented++;
}

GlRenderer::~GlRenderer() {
    // Only called once no thread has the context current.
    if (display != EGL_NO_DISPLAY) {
        destroySurface();
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    if (window) ANativeWindow_release(window);
    if (pending) ANativeWindow_release(pending);
}

void GlRenderer::releaseThread() {
    std::lock_guard<std::mutex> lk(mu);
    applyPending();
    if (display != EGL_NO_DISPLAY) eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
