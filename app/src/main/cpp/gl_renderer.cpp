#include "gl_renderer.h"
#include <EGL/eglext.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include "util/log.h"

#ifndef EGL_GL_COLORSPACE_KHR
#define EGL_GL_COLORSPACE_KHR 0x309D
#endif
#ifndef EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT
#define EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT 0x3350
#endif
#ifndef EGL_COLOR_COMPONENT_TYPE_EXT
#define EGL_COLOR_COMPONENT_TYPE_EXT 0x3339
#endif
#ifndef EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT
#define EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT 0x333B
#endif

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

// ---- Realism filters ----
// Pass 1 runs at the guest's resolution into a texture that holds the panel's state: the frame plus
// crosstalk is the target, and the state creeps towards it (a slow liquid crystal leaves trails
// behind a moving cursor and smears scrolling text; an OLED smears when dark pixels light up). On
// a torn frame the rows below the tear line keep the last frame for one more step. Pass 2 draws the
// state through the panel (colour response, viewing angle, pixel grid, backlight) and the layers in
// front of it (resistive film, protector wear, sunlight).
const char *kStateVertex = R"(
attribute vec2 pos;
varying vec2 uv;
void main() {
    uv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
})";

const char *kState = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;    // the guest frame
uniform sampler2D prev;   // the panel last step
uniform sampler2D rows;   // how dark each row is on average (h x 1)
uniform sampler2D cols;   // and each column (w x 1)
uniform float meanLoad;
uniform float crosstalk;
uniform float smear;      // OLED: pixels coming up from black are the slowest
uniform float off;        // the display is off: everything heads for black
uniform float tearY;      // rows below this (0 = top) keep the last frame this step
uniform vec2 alpha;       // share of the way covered this step: towards brighter (x), darker (y)
void main() {
    vec3 t = off > 0.5 ? vec3(0.0) : texture2D(tex, uv).rgb;
    // Crosstalk (shadowing): a passive matrix drives a whole column and row at once, so a pixel
    // picks up some of the voltage meant for the rest of its line. Dark content casts faint streaks
    // along its columns and rows; light lines come out a little lighter.
    float rl = texture2D(rows, vec2(uv.y, 0.5)).r;
    float cl = texture2D(cols, vec2(uv.x, 0.5)).r;
    float xt = crosstalk * (0.7 * (cl - meanLoad) + 0.3 * (rl - meanLoad));
    t = clamp(t * (1.0 - 0.9 * xt), 0.0, 1.0);
    vec3 p = texture2D(prev, uv).rgb;
    vec3 d = t - p;
    vec3 up = step(0.0, d);
    vec3 a = mix(vec3(alpha.y), vec3(alpha.x), up);
    float pl = dot(p, vec3(0.299, 0.587, 0.114));
    a = mix(a, a * mix(0.12, 1.0, smoothstep(0.0, 0.3, pl)), smear * up);
    vec3 s = d * a;
    // An 8-bit state would stall just short of the target: always move at least one step.
    s = sign(d) * max(abs(s), min(abs(d), vec3(1.0 / 255.0)));
    s *= step(uv.y, tearY);
    gl_FragColor = vec4(p + s, 1.0);
})";

const char *kFinal = R"(
precision highp float;
varying vec2 uv;
uniform sampler2D tex;      // the panel's state
uniform sampler2D light;    // backlight brightness (stored / 1.5)
uniform sampler2D wearMap;  // r: scratches, a: finger grease
uniform vec2 texSize;
uniform vec2 outSize;
uniform float landscape;
// The panel.
uniform float gap, notch, pentile;
uniform float desat, gammaBase, gammaView, crush;
uniform vec3 leak, white;
uniform float backlightK, muraK, ablK, angleK, amoled;
uniform vec2 tilt;          // where the viewer is (+x from the right, +y from above)
// The layers in front of it.
uniform float touchLayer, ripple, wear, glare, dimple;
uniform vec2 penPos, ringPos;  // guest pixels
uniform vec3 dead[8];          // x, y, kind (0 dead, 1-3 stuck red / green / blue)
uniform float deadCount;
uniform float level;           // backlight (or OLED drive) for the wake / sleep fade
uniform float hdrGain;         // > 0: linear extended-range output, the screen's white x this
uniform float seedF;

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233)) + seedF) * 43758.5453); }

// Share of the screen pixel [x - hw, x + hw] (in guest pixels) inside [a, b].
float cov(float x, float hw, float a, float b) {
    return clamp((min(x + hw, b) - max(x - hw, a)) / (2.0 * hw), 0.0, 1.0);
}

// Share of it on the lit part of a guest pixel, with a gap of g on each side of every pixel.
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
    vec2 texel = uv * texSize;
    vec2 scale = max(outSize / texSize, vec2(1.0));
    float rad = 0.04 * min(texSize.x, texSize.y);
    // The resistive film pressed in under the stylus bends the picture round the dimple.
    vec2 dp = texel - penPos;
    float bump = dimple * exp(-dot(dp, dp) / (rad * rad));
    texel -= dp * bump * 0.12;
    // Sharp bilinear, as the Sharp pixels filter.
    vec2 base = floor(texel - 0.5) + 0.5;
    vec2 f = clamp((texel - base - 0.5) * scale + 0.5, 0.0, 1.0);
    vec3 t = texture2D(tex, (base + f) / texSize).rgb;
    // The panel in its own upright frame (the backlight's LEDs sit along its bottom edge).
    vec2 bp = landscape > 0.5 ? vec2(uv.y, 1.0 - uv.x) : uv;
    vec2 ip = floor(texel);

    // Dead pixels stay black; a stuck one has a subpixel always on.
    for (int i = 0; i < 8; i++) {
        if (float(i) < deadCount && ip.x == dead[i].x && ip.y == dead[i].y) {
            float k = dead[i].z;
            t = k < 0.5 ? vec3(0.0) : max(t, vec3(equal(vec3(k), vec3(1.0, 2.0, 3.0))));
        }
    }

    // Where the viewer is, seen from this point of the panel (its edges are seen at an angle even
    // when the middle faces the eye).
    vec2 v = (tilt + (bp - 0.5) * vec2(-0.25, 0.35)) * angleK;
    float oa = min(length(v), 1.0);
    float lcd = 1.0 - amoled;

    // Colour response: weak (or, on OLED, oversaturated) colour, the panel's gamma, the viewing
    // angle, light leaking through "off" pixels, and the panel's white.
    float y = dot(t, vec3(0.299, 0.587, 0.114));
    t = clamp(mix(vec3(y), t, 1.0 - desat), 0.0, 1.0);
    // LCD: from above it washes out, from below it darkens and finally inverts; from the side the
    // colour drains away. OLED: colour drifts blue-green off axis.
    float gam = (gammaBase + crush + gammaView * (bp.y - 0.5)) * exp(-v.y * 1.4 * lcd);
    t = pow(max(t, vec3(0.0)), vec3(max(gam, 0.2)));
    t = mix(t, vec3(1.0) - t, lcd * 0.6 * smoothstep(0.5, 1.1, -v.y));
    t = mix(t, vec3(dot(t, vec3(0.333))), lcd * min(abs(v.x) * 0.7, 0.6));
    t *= mix(vec3(1.0), vec3(0.88, 0.98, 1.06), amoled * oa);
    vec3 c = mix(leak * (1.0 + 2.5 * oa * lcd), white, t);
    // OLED: a mostly white screen draws more current and dims; uneven pixels show in dark greys.
    c *= 1.0 - ablK;
    c *= 1.0 + muraK * (hash(ip) - 0.5) * 0.14 * (1.0 - 0.7 * y);

    // Pixel grid: the gaps between the electrodes, and the subpixels once a guest pixel is big
    // enough on the phone to show them.
    vec2 fr = fract(texel);
    vec2 hw = 0.5 / scale;
    float g = gap;
    float sub = clamp(g * 10.0, 0.0, 1.0) * clamp((scale.x - 2.0) / 3.0, 0.0, 1.0);
    vec3 mx;
    if (pentile > 0.5) {
        // PenTile RGBG: every pixel has a small green and either a red or a blue (twice the green's
        // width, so each colour covers the same share of the panel), alternating.
        float wrb = 0.55 - g;
        float rb = cov(fr.x, hw.x, g, 0.55), gg = cov(fr.x, hw.x, 0.65, 0.65 + 0.5 * wrb);
        float odd = mod(ip.x + ip.y, 2.0);
        vec3 pm = vec3(rb * (1.0 - odd), gg, rb * odd) / max(0.5 * wrb, 0.05);
        mx = mix(vec3(lit(fr.x, hw.x, g)), pm, sub);
    } else {
        mx = mix(vec3(lit(fr.x, hw.x, g)), 3.0 * stripes(fr.x, hw.x, g), sub);
    }
    vec3 mask = mx * lit(fr.y, hw.y, g);
    // TFT: the transistor takes a corner of every subpixel.
    float nk = notch * clamp((scale.x - 3.0) / 2.0, 0.0, 1.0);
    mask *= 1.0 - 0.8 * nk * cov(fr.y, hw.y, 0.74, 1.0 - g) * cov(fract(fr.x * 3.0), hw.x * 3.0, 0.0, 0.35);
    // The eye adapts to the darker grid somewhat; give half of the lost light back.
    mask *= mix(1.0, 1.0 / ((1.0 - 2.0 * g) * (1.0 - 2.0 * g)), 0.5);
    c *= mask * mix(vec3(1.0), texture2D(light, bp).rgb * 1.5, backlightK);

    // Pressing hard on an LCD pushes the crystal aside: a dark pool with coloured fringes.
    float pr = length(dp) / (rad * 1.8);
    float pool = ripple * exp(-pr * pr * 1.5);
    float fringe = ripple * exp(-(pr - 1.0) * (pr - 1.0) * 5.0);
    c *= 1.0 - 0.6 * pool;
    c *= 1.0 - 0.3 * fringe * (0.5 + 0.5 * cos(6.2832 * (pr * 1.3 + vec3(0.0, 0.33, 0.67))));

    // Light from the room reflects off the glass; its highlight slides with the tilt.
    vec2 lp = vec2(0.5, 0.25) - tilt * 0.8;
    float sheen = exp(-dot(bp - lp, bp - lp) / 0.06);
    vec3 refl = vec3(0.0);
    // Resistive film: a little haze and grain, and rainbow Newton rings where it touches the glass
    // (one spot it always does, and round the pressed stylus).
    float tl = touchLayer;
    c *= (1.0 - 0.08 * tl) * (1.0 + (hash(floor(gl_FragCoord.xy)) - 0.5) * 0.06 * tl);
    refl += 0.03 * tl * (0.5 + sheen + 2.0 * glare);
    float rd = length(texel - ringPos) / texSize.x;
    float pd = length(dp) / texSize.x;
    vec3 rings = cos(6.2832 * (vec3(rd * rd * 260.0) + vec3(0.0, 0.33, 0.67)));
    vec3 ringsT = cos(6.2832 * (vec3(pd * pd * 700.0) + vec3(0.0, 0.33, 0.67)));
    c *= 1.0 + tl * 0.08 * (exp(-rd * rd / 0.008) * rings + bump * ringsT);
    c *= 1.0 - 0.12 * bump;
    // Screen protector: grease scatters the picture; scratches and grease catch the light.
    vec4 wm = texture2D(wearMap, bp);
    c = mix(c, vec3(dot(c, vec3(0.333))), wear * wm.a * 0.25);
    refl += wear * (wm.a * 0.06 + wm.r * 0.30) * (0.3 + 0.9 * sheen + 1.5 * glare) * vec3(0.95, 0.97, 1.0);
    // Sunlight: reflected light on top of the picture washes it out.
    refl += glare * (0.35 + 0.25 * sheen) * vec3(1.0, 0.99, 0.95);

    c *= level;
    if (hdrGain > 0.0)
        gl_FragColor = vec4(pow(max(c, vec3(0.0)), vec3(2.2)) * hdrGain + pow(refl, vec3(2.2)), 1.0);
    else
        gl_FragColor = vec4(clamp(c + refl, 0.0, 1.0), 1.0);
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

// A year of use on a screen protector, panel-upright, as luminance + alpha: L = fine scratches (a
// few long arcs from keys in a pocket, many short scuffs), A = finger grease (a broad smear where
// the thumb works the lower middle, blotches, and a few fingerprints).
std::vector<uint8_t> wearMap(int W, int H, uint32_t seed) {
    std::vector<float> sc((size_t)W * H, 0.f), sm((size_t)W * H, 0.f);
    uint32_t r = seed * 2654435761u + 0x6D2B79F5u;
    auto rnd = [&]() {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return (r & 0xFFFFFF) / 16777216.f;
    };
    auto plot = [&](float x, float y, float a) {
        int ix = (int)std::floor(x), iy = (int)std::floor(y);
        float fx = x - ix, fy = y - iy;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++) {
                int px = ix + dx, py = iy + dy;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                float &v = sc[(size_t)py * W + px];
                v = std::min(1.f, v + a * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy));
            }
    };
    for (int i = 0; i < 110; i++) {
        bool arc = i < 14;
        float x = rnd() * W, y = rnd() * H, ang = rnd() * 6.2832f;
        float curve = (rnd() - 0.5f) * (arc ? 0.004f : 0.03f);
        float len = (arc ? 0.15f + 0.5f * rnd() : 0.01f + 0.05f * rnd()) * W;
        float a = arc ? 0.5f + 0.5f * rnd() : 0.25f + 0.35f * rnd();
        for (float s = 0; s < len; s += 0.5f) {
            plot(x, y, a * 0.5f * (0.4f + 0.6f * std::sin(3.14159f * s / len)));
            x += std::cos(ang) * 0.5f;
            y += std::sin(ang) * 0.5f;
            ang += curve;
        }
    }
    struct Print { float x, y, r, c, s; };
    Print prints[5];
    for (Print &p : prints) {
        float ang = rnd() * 6.2832f;
        p = {(0.2f + 0.6f * rnd()) * W, (0.3f + 0.6f * rnd()) * H, (0.05f + 0.04f * rnd()) * W, std::cos(ang), std::sin(ang)};
    }
    float off = (float)(seed % 997);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float u = (float)x / W, v = (float)y / H;
            float du = u - 0.5f, dv = v - 0.78f;
            float s = 0.35f * std::exp(-(du * du / 0.08f + dv * dv / 0.03f)) * (0.6f + 0.4f * valueNoise(u * 6 + off, v * 10));
            s += 0.12f * std::max(0.f, valueNoise(u * 4 + off + 31, v * 7 + 11));
            for (const Print &p : prints) {
                float dx = x - p.x, dy = y - p.y;
                float rx = dx * p.c + dy * p.s, ry = (dy * p.c - dx * p.s) * 1.3f;
                float d = std::sqrt(rx * rx + ry * ry) / p.r;
                if (d > 1.2f) continue;
                // Ridges about half a millimetre apart, wobbling like a real print's whorl.
                float ridge = 0.5f + 0.5f * std::sin(std::sqrt(rx * rx + ry * ry) * 1.4f + 2.5f * valueNoise(x * 0.05f, y * 0.05f + off));
                s = std::max(s, ridge * std::pow(std::min(1.f, 1.2f - d), 0.7f) * 0.7f);
            }
            sm[(size_t)y * W + x] = std::min(1.f, s);
        }
    std::vector<uint8_t> out((size_t)W * H * 2);
    for (size_t i = 0; i < sc.size(); i++) {
        out[i * 2] = (uint8_t)(sc[i] * 255.f + 0.5f);
        out[i * 2 + 1] = (uint8_t)(sm[i] * 255.f + 0.5f);
    }
    return out;
}

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

// How fast the panel follows the frame: time constants towards lighter and towards darker.
void panelTaus(const GlRenderer::Realism &r, float tempC, float &tl, float &td) {
    float g = r.ghost;
    switch (r.panel) {
    case GlRenderer::PANEL_STN: tl = 0.30f * g; td = 0.20f * g; break;  // about a quarter second
    case GlRenderer::PANEL_TN: tl = 0.035f * g; td = 0.025f * g; break;
    case GlRenderer::PANEL_AMOLED: tl = 0.10f * g; td = 0.008f * g; break;  // smear on the way up only
    default: tl = td = 0; break;
    }
    // Liquid crystal thickens in the cold: below 15 C it gets slower, up to four times.
    if (r.cold && (r.panel == GlRenderer::PANEL_STN || r.panel == GlRenderer::PANEL_TN)) {
        float k = std::min(4.f, std::max(1.f, 1 + (15 - tempC) / 8));
        tl *= k;
        td *= k;
    }
}

uint64_t steadyNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

// Drops the context (to switch between SDR and FP16 configs). Every GL object goes with it.
void GlRenderer::resetGl() {
    if (display == EGL_NO_DISPLAY) return;
    destroySurface();
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    context = EGL_NO_CONTEXT;
    for (GLuint &p : programs) p = 0;
    tex = vbo = stateProg = finalProg = fbo = rowTex = colTex = lightTex = wearTex = 0;
    stateTex[0] = stateTex[1] = 0;
    texW = texH = texBpp = 0;
    stateW = stateH = 0;
    statePrimed = false;
    shadow.clear();
    hdrSurface = false;
    redraw = true;
}

GLuint GlRenderer::buildProgram(const char *fragment, const char *vertex) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vertex ? vertex : kVertex));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    return p;
}

bool GlRenderer::ensureContext(bool wantHdr) {
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
    }
    if (context == EGL_NO_CONTEXT) {
        bool hdr = false;
        EGLint n = 0;
        if (wantHdr && !hdrUnsupported) {
            // HDR: FP16 buffers in extended-range linear sRGB (scRGB), where 1.0 is SDR white and
            // the screen can go above it.
            const char *ext = eglQueryString(display, EGL_EXTENSIONS);
            if (ext && strstr(ext, "EGL_EXT_gl_colorspace_scrgb_linear") && strstr(ext, "EGL_EXT_pixel_format_float")) {
                const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                        EGL_COLOR_COMPONENT_TYPE_EXT, EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
                                        EGL_RED_SIZE, 16, EGL_GREEN_SIZE, 16, EGL_BLUE_SIZE, 16, EGL_ALPHA_SIZE, 16, EGL_NONE};
                hdr = eglChooseConfig(display, attrs, &config, 1, &n) && n > 0;
            }
            if (!hdr) {
                hdrUnsupported = true;
                LOGW("gl: no extended-range (scRGB) surfaces; HDR stays off");
            }
        }
        if (!hdr) {
            const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
            eglChooseConfig(display, attrs, &config, 1, &n);
        }
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
        if (context == EGL_NO_CONTEXT) { LOGE("gl: no context"); return false; }
        contextHdr = hdr;
    }
    if (surface == EGL_NO_SURFACE) {
        if (!window) return false;
        const EGLint hdrAttrs[] = {EGL_GL_COLORSPACE_KHR, EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT, EGL_NONE};
        surface = eglCreateWindowSurface(display, config, window, contextHdr ? hdrAttrs : nullptr);
        if (surface == EGL_NO_SURFACE) return false;
        hdrSurface = contextHdr;
        eglMakeCurrent(display, surface, surface, context);
        eglSwapInterval(display, 0);  // never block emulation on vsync
        if (!programs[0]) {
            programs[SMOOTH] = buildProgram(kSmooth);
            programs[SHARP] = buildProgram(kSharp);
            programs[ENHANCED] = buildProgram(kEnhanced);
            stateProg = buildProgram(kState, kStateVertex);
            finalProg = buildProgram(kFinal);
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

void GlRenderer::setRealism(const Realism &r) {
    std::lock_guard<std::mutex> lk(mu);
    wanted = r;
    redraw = true;
}

void GlRenderer::setSensors(float tiltX, float tiltY, float lux, float tempC, float brightness) {
    std::lock_guard<std::mutex> lk(mu);
    sensorsIn = {tiltX, tiltY, lux, tempC, brightness};
}

void GlRenderer::touch(bool down, int x, int y) {
    uint64_t now = steadyNs();
    if (down && !penDown) penDownNs = now;
    if (!down && penDown) penUpNs = now;
    penDown = down;
    penX = (float)x;
    penY = (float)y;
}

// The panel's state textures (and the crosstalk lines) at the guest's resolution, the backlight
// map, and the protector's wear.
void GlRenderer::realismSetup(int w, int h) {
    if (!lightTex) {
        const int W = 64, H = 108;
        std::vector<uint8_t> map = backlightMap(W, H);
        lightTex = makeTexture(W, H, GL_RGB, map.data());
    }
    if (rl.wear > 0 && (!wearTex || wearSeed != rl.seed)) {
        if (wearTex) glDeleteTextures(1, &wearTex);
        const int W = 512, H = 852;
        std::vector<uint8_t> map = wearMap(W, H, rl.seed);
        wearTex = makeTexture(W, H, GL_LUMINANCE_ALPHA, map.data());
        wearSeed = rl.seed;
    }
    if (w == stateW && h == stateH && fbo) return;
    if (stateTex[0]) glDeleteTextures(2, stateTex);
    if (rowTex) glDeleteTextures(1, &rowTex);
    if (colTex) glDeleteTextures(1, &colTex);
    if (!fbo) glGenFramebuffers(1, &fbo);
    stateTex[0] = makeTexture(w, h, GL_RGBA, nullptr);
    stateTex[1] = makeTexture(w, h, GL_RGBA, nullptr);
    rowLoad.assign(h, 0);
    colLoad.assign(w, 0);
    rowTex = makeTexture(h, 1, GL_LUMINANCE, rowLoad.data());
    colTex = makeTexture(w, 1, GL_LUMINANCE, colLoad.data());
    stateW = w;
    stateH = h;
    statePrimed = false;
}

// How dark each row and column of the frame is on average (every other pixel is plenty).
void GlRenderer::realismLoad(int w, int h, int bpp) {
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

// One step of the panel towards the current frame (or towards black with the display off).
void GlRenderer::realismStep(int w, int h, uint64_t now, bool off) {
    float dt = statePrimed ? (now - lastStepNs) / 1e9f : 1e9f;
    lastStepNs = now;
    statePrimed = true;
    float tl, td;
    panelTaus(rl, sensors.tempC, tl, td);
    float aLight = tl > 0 ? 1 - std::exp(-dt / tl) : 1;
    float aDark = td > 0 ? 1 - std::exp(-dt / td) : 1;
    float xt = rl.panel == PANEL_STN ? rl.crosstalk : rl.panel == PANEL_TN ? 0.2f * rl.crosstalk : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stateTex[1 - stateCur], 0);
    glViewport(0, 0, w, h);
    GLuint p = stateProg;
    glUseProgram(p);
    GLuint units[4] = {tex, stateTex[stateCur], rowTex, colTex};
    const char *names[4] = {"tex", "prev", "rows", "cols"};
    for (int i = 3; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, units[i]);
        glUniform1i(glGetUniformLocation(p, names[i]), i);
    }
    glUniform1f(glGetUniformLocation(p, "meanLoad"), meanLoad);
    glUniform1f(glGetUniformLocation(p, "crosstalk"), xt);
    glUniform1f(glGetUniformLocation(p, "smear"), rl.panel == PANEL_AMOLED ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(p, "off"), off ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(p, "tearY"), tearY);
    glUniform2f(glGetUniformLocation(p, "alpha"), aLight, aDark);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    stateCur = 1 - stateCur;
}

// The panel's character for the final pass, from its type and the sliders.
void GlRenderer::realismUniforms(GLuint p, int w, int h, uint64_t now) {
    float c = rl.contrast, gr = rl.grid;
    float gap = 0, notch = 0, pentile = 0, desat = 0, gammaBase = 1, gammaView = 0, angleK = 0, amoled = 0;
    float blK = 0, muraK = 0, ablK = 0, glareK = 1, rippleK = rl.ripple, crushK = 0;
    float leak[3] = {0, 0, 0}, white[3] = {1, 1, 1};
    auto setWhite = [&](float r, float g, float b, float k) {
        white[0] = 1 + (r - 1) * k;
        white[1] = 1 + (g - 1) * k;
        white[2] = 1 + (b - 1) * k;
    };
    switch (rl.panel) {
    case PANEL_STN:
        // Colour STN: weak colour filters, a shallow response that changes from top to bottom,
        // grey-blue blacks and a greenish, dim white. Transflective, so sunlight hurts it less.
        gap = 0.11f * gr; desat = 0.4f * c; gammaBase = 1 - 0.18f * c; gammaView = 0.3f * c;
        leak[0] = 0.12f * c; leak[1] = 0.14f * c; leak[2] = 0.21f * c;
        setWhite(0.88f, 0.92f, 0.82f, c);
        blK = rl.backlight; angleK = 1.3f * rl.angle; glareK = 0.6f; crushK = 0.6f;
        break;
    case PANEL_TN:
        // TN TFT: a slim black matrix with the transistor in a corner, slightly cool and a little
        // washed out, a narrow viewing cone.
        gap = 0.07f * gr; notch = gr; desat = 0.15f * c; gammaBase = 1 - 0.06f * c; gammaView = 0.12f * c;
        leak[0] = 0.05f * c; leak[1] = 0.055f * c; leak[2] = 0.07f * c;
        setWhite(0.96f, 0.97f, 1.0f, c);
        blK = 0.7f * rl.backlight; angleK = rl.angle; crushK = 0.6f;
        break;
    case PANEL_AMOLED:
        // AMOLED: PenTile subpixels, punchy colour, true black, bluish white; it dims on bright
        // screens, gets grainy in dark greys, washes out badly in sun, and never pools.
        gap = 0.10f * gr; pentile = 1; desat = -0.3f * c; gammaBase = 1 + 0.1f * c;
        setWhite(0.97f, 0.99f, 1.04f, c);
        muraK = rl.backlight; ablK = 0.3f * rl.crosstalk * (1 - meanLoad); angleK = 0.5f * rl.angle;
        amoled = 1; glareK = 1.3f; rippleK = 0; crushK = 0.4f;
        break;
    default:
        break;
    }
    // A dim phone: the darkest greys sink into black (and OLED grain shows more).
    float dim = 1 - std::min(1.f, std::max(0.f, sensors.brightness));
    float crush = crushK * dim * dim;
    muraK *= 1 + 2 * dim;
    // Sunlight from about 1000 lux (a bright window) to full sun.
    float glare = 0;
    if (sensors.lux > 0) {
        float l = (std::log10(sensors.lux) - 3.f) / 1.7f;
        l = std::min(1.f, std::max(0.f, l));
        glare = rl.glare * glareK * l * l * (3 - 2 * l);
    }
    // Dead pixels and the Newton-ring spot: fixed per install (seed) and screen size.
    if (rl.dead != deadCount || rl.seed != deadSeed || w != deadW || h != deadH) {
        uint32_t r = rl.seed * 747796405u + 2891336453u;
        auto next = [&]() { r = r * 1664525u + 1013904223u; return r >> 8; };
        for (int i = 0; i < 8; i++) {
            deadPx[i * 3] = (float)(next() % (uint32_t)w);
            deadPx[i * 3 + 1] = (float)(next() % (uint32_t)h);
            uint32_t k = next() % 10;  // mostly dead, some stuck
            deadPx[i * 3 + 2] = k < 4 ? 0.f : (float)(1 + k % 3);
        }
        ringX = (0.2f + 0.6f * (next() % 1000) / 1000.f) * w;
        ringY = (0.2f + 0.6f * (next() % 1000) / 1000.f) * h;
        deadCount = rl.dead;
        deadSeed = rl.seed;
        deadW = w;
        deadH = h;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lightTex);
    glUniform1i(glGetUniformLocation(p, "light"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, wearTex ? wearTex : lightTex);
    glUniform1i(glGetUniformLocation(p, "wearMap"), 2);
    glUniform1f(glGetUniformLocation(p, "landscape"), w > h ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(p, "gap"), gap);
    glUniform1f(glGetUniformLocation(p, "notch"), notch);
    glUniform1f(glGetUniformLocation(p, "pentile"), pentile);
    glUniform1f(glGetUniformLocation(p, "desat"), desat);
    glUniform1f(glGetUniformLocation(p, "gammaBase"), gammaBase);
    glUniform1f(glGetUniformLocation(p, "gammaView"), gammaView);
    glUniform1f(glGetUniformLocation(p, "crush"), crush);
    glUniform3fv(glGetUniformLocation(p, "leak"), 1, leak);
    glUniform3fv(glGetUniformLocation(p, "white"), 1, white);
    glUniform1f(glGetUniformLocation(p, "backlightK"), blK);
    glUniform1f(glGetUniformLocation(p, "muraK"), muraK);
    glUniform1f(glGetUniformLocation(p, "ablK"), ablK);
    glUniform1f(glGetUniformLocation(p, "angleK"), angleK);
    glUniform1f(glGetUniformLocation(p, "amoled"), amoled);
    glUniform2f(glGetUniformLocation(p, "tilt"), sensors.tiltX, sensors.tiltY);
    glUniform1f(glGetUniformLocation(p, "touchLayer"), rl.touchLayer);
    glUniform1f(glGetUniformLocation(p, "ripple"), rippleK * rippleLevel);
    glUniform1f(glGetUniformLocation(p, "wear"), wearTex ? rl.wear : 0.f);
    glUniform1f(glGetUniformLocation(p, "glare"), glare);
    glUniform1f(glGetUniformLocation(p, "dimple"), rl.touchLayer * dimpleLevel);
    glUniform2f(glGetUniformLocation(p, "penPos"), penX, penY);
    glUniform2f(glGetUniformLocation(p, "ringPos"), ringX, ringY);
    glUniform3fv(glGetUniformLocation(p, "dead"), 8, deadPx);
    glUniform1f(glGetUniformLocation(p, "deadCount"), (float)std::min(rl.dead, 8));
    glUniform1f(glGetUniformLocation(p, "level"), level);
    glUniform1f(glGetUniformLocation(p, "hdrGain"), hdrSurface.load() ? std::max(1.f, rl.hdr) : 0.f);
    glUniform1f(glGetUniformLocation(p, "seedF"), (float)(rl.seed % 1000) * 0.1f);
    (void)now;
}

// Something on screen is still moving without a new frame: the panel catching up, the fade, the
// stylus effects, a torn frame to finish, or the viewer moved (tilt, room light, brightness).
bool GlRenderer::realismAnimating(uint64_t now) const {
    if (rl.panel != PANEL_CLEAN && rl.ghost > 0 && now < settleUntilNs) return true;
    if (now < animUntilNs) return true;
    if (rl.fade && std::fabs(level - (lcdOn ? 1.f : 0.f)) > 0.003f) return true;
    if ((rl.touchLayer > 0 || rl.ripple > 0) && (penDown || dimpleLevel > 0.003f || rippleLevel > 0.003f)) return true;
    bool viewer = (rl.panel != PANEL_CLEAN && rl.angle > 0) || rl.wear > 0 || rl.glare > 0;
    if (viewer && (std::fabs(sensors.tiltX - sensorsDrawn.tiltX) > 0.01f || std::fabs(sensors.tiltY - sensorsDrawn.tiltY) > 0.01f))
        return true;
    if (rl.glare > 0 && std::fabs(sensors.lux - sensorsDrawn.lux) > 0.15f * std::max(sensorsDrawn.lux, 100.f)) return true;
    if (rl.panel != PANEL_CLEAN && std::fabs(sensors.brightness - sensorsDrawn.brightness) > 0.01f) return true;
    return false;
}

void GlRenderer::draw(const uint8_t *pixels, int w, int h, int bpp) { present(pixels, w, h, bpp, false); }

void GlRenderer::drawOff() { present(nullptr, texW, texH, texBpp, true); }

void GlRenderer::present(const uint8_t *pixels, int w, int h, int bpp, bool off) {
    Realism want;
    Sensors sens;
    {
        std::lock_guard<std::mutex> lk(mu);
        applyPending();
        want = wanted;
        sens = sensorsIn;
    }
    rl = want;
    sensors = sens;
    bool real = rl.any();
    bool turned = off == lcdOn;  // the display just went on or off
    lcdOn = !off;
    // With the display off there is only something to draw while the backlight fades out.
    if (off && (!real || !rl.fade || texW == 0)) return;
    // Phone asleep, or the window is gone: do not swap. The guest keeps running.
    if (!window || !presenting.load()) return;
    uint64_t now = steadyNs();
    bool wantHdr = rl.hdr > 1.01f && !hdrUnsupported;
    if (context != EGL_NO_CONTEXT && wantHdr != contextHdr) {
        resetGl();
        if (off) return;  // the frame is gone with the context; it comes back with the display
    }
    // Which rows changed since the last upload (cheap next to a full upload and swap).
    size_t row = (size_t)w * (bpp / 8), bytes = row * h;
    // A resized window (same surface) needs the whole frame drawn again at the new size.
    int ww = ANativeWindow_getWidth(window), wh = ANativeWindow_getHeight(window);
    bool resized = ww != surfW || wh != surfH;
    surfW = ww;
    surfH = wh;
    bool full = redraw.exchange(false) || resized;
    int y0 = 0, y1 = h - 1;
    bool changed = false;
    if (!off) {
        full = full || w != texW || h != texH || bpp != texBpp || shadow.size() != bytes;
        if (!full) {
            while (y0 < h && memcmp(pixels + y0 * row, shadow.data() + y0 * row, row) == 0) y0++;
            while (y1 > y0 && memcmp(pixels + y1 * row, shadow.data() + y1 * row, row) == 0) y1--;
        }
        changed = full || y0 < h;
    }
    // Unchanged: nothing to present, unless something is still moving (then about 60 a second).
    if (!changed && !full && !(real && turned)) {
        if (!real || !realismAnimating(now) || now - lastStepNs < 15000000ull) return;
    }
    if (!ensureContext(wantHdr)) { redraw = true; return; }
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
    }
    if (real) {
        // Long enough for the slowest transition to finish; then the screen goes idle again.
        if (changed || turned) {
            float tl, td;
            panelTaus(rl, sensors.tempC, tl, td);
            settleUntilNs = now + (uint64_t)((6 * std::max(tl, td) + 0.5f) * 1e9f);
        }
        // The fade and the stylus effects, by the time since they were last advanced.
        float dt = std::min(0.05f, lastAnimNs ? (now - lastAnimNs) / 1e9f : 0.016f);
        if (turned) dt = 0.016f;
        lastAnimNs = now;
        float target = lcdOn ? 1.f : 0.f;
        if (!rl.fade) level = target;
        else {
            // An LED backlight comes up in about a tenth of a second and dies away slower.
            float tau = target > level ? 0.10f : 0.30f;
            level += (target - level) * (1 - std::exp(-dt / tau));
            if (std::fabs(target - level) < 0.003f) level = target;
        }
        // The film springs back in a few tens of milliseconds; the crystal pools while pressed and
        // takes about half a second to flow back.
        float dimpleTarget = penDown ? 1.f : 0.f;
        dimpleLevel += (dimpleTarget - dimpleLevel) * (1 - std::exp(-dt / (penDown ? 0.03f : 0.05f)));
        rippleLevel += ((penDown ? 1.f : 0.f) - rippleLevel) * (1 - std::exp(-dt / (penDown ? 0.25f : 0.45f)));
        if (!penDown && dimpleLevel < 0.003f) dimpleLevel = 0;
        if (!penDown && rippleLevel < 0.003f) rippleLevel = 0;
        // No vsync: now and then a new frame lands mid-scan and its lower part shows one step late.
        tearY = 2;
        if (changed && !full && rl.tear > 0) {
            tearRand ^= tearRand << 13;
            tearRand ^= tearRand >> 17;
            tearRand ^= tearRand << 5;
            float r1 = (tearRand & 0xFFFF) / 65536.f, r2 = (tearRand >> 16) / 65536.f;
            if (r1 < 0.7f * rl.tear) {
                tearY = 0.15f + 0.7f * r2;
                animUntilNs = now + 40000000ull;
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    if (real) {
        realismSetup(w, h);
        if (changed && rl.crosstalk > 0 && rl.panel != PANEL_CLEAN) realismLoad(w, h, bpp);
        realismStep(w, h, now, off);
    }
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
    glViewport(0, 0, sw, sh);
    int f = filter < 0 || filter > 2 ? SHARP : filter;
    // An HDR surface is linear: only the realism pass writes it correctly.
    GLuint p = real || hdrSurface.load() ? finalProg : programs[f];
    glUseProgram(p);
    if (p == finalProg) realismUniforms(p, w, h, now);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, real ? stateTex[stateCur] : tex);
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
    sensorsDrawn = sensors;
    if (!real) lastStepNs = now;
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
