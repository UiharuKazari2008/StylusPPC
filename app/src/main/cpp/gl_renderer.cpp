#include "gl_renderer.h"
#include <chrono>
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
    }
    cv.notify_all();
}

void GlRenderer::destroySurface() {
    if (display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    surface = EGL_NO_SURFACE;
}

GLuint GlRenderer::buildProgram(const char *fragment) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, kVertex));
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

void GlRenderer::draw(const uint8_t *pixels, int w, int h, int bpp) {
    {
        std::lock_guard<std::mutex> lk(mu);
        applyPending();
    }
    if (!window) return;
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
        if (y0 == h) return;  // unchanged: nothing to present
        while (y1 > y0 && memcmp(pixels + y1 * row, shadow.data() + y1 * row, row) == 0) y1--;
    }
    if (!ensureContext()) { redraw = true; return; }
    if (shadow.size() != bytes) shadow.resize(bytes);
    memcpy(shadow.data() + y0 * row, pixels + y0 * row, (size_t)(y1 - y0 + 1) * row);
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
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
    glViewport(0, 0, sw, sh);
    int f = filter < 0 || filter > 2 ? SHARP : filter;
    GLuint p = programs[f];
    glUseProgram(p);
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
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(display, surface);
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
