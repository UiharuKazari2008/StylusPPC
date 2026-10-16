#include "audio_host.h"
#include <cstring>
#include <dlfcn.h>
#include "util/log.h"

uint32_t FrameRing::write(const int16_t *src, uint32_t n) {
    uint64_t wp = w.load(std::memory_order_relaxed), rp = r.load(std::memory_order_acquire);
    uint32_t space = cap - (uint32_t)(wp - rp);
    if (n > space) n = space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)((wp + i) % cap) * 2;
        buf[idx] = src[i * 2];
        buf[idx + 1] = src[i * 2 + 1];
    }
    w.store(wp + n, std::memory_order_release);
    return n;
}

uint32_t FrameRing::read(int16_t *dst, uint32_t n) {
    uint64_t rp = r.load(std::memory_order_relaxed), wp = w.load(std::memory_order_acquire);
    uint32_t have = (uint32_t)(wp - rp);
    if (n > have) n = have;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)((rp + i) % cap) * 2;
        dst[i * 2] = buf[idx];
        dst[i * 2 + 1] = buf[idx + 1];
    }
    r.store(rp + n, std::memory_order_release);
    return n;
}

HostAudio::HostAudio() {}
HostAudio::~HostAudio() { stop(); }

aaudio_data_callback_result_t HostAudio::outCb(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *self = static_cast<HostAudio *>(user);
    auto *dst = static_cast<int16_t *>(data);
    uint32_t got = self->out.read(dst, (uint32_t)frames);
    if (got < (uint32_t)frames) memset(dst + got * 2, 0, (frames - got) * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

aaudio_data_callback_result_t HostAudio::inCb(AAudioStream *, void *user, void *data, int32_t frames) {
    auto *self = static_cast<HostAudio *>(user);
    self->in.write(static_cast<const int16_t *>(data), (uint32_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static AAudioStream *openStream(aaudio_direction_t dir, AAudioStream_dataCallback cb, void *user,
                                aaudio_usage_t usage = AAUDIO_USAGE_MEDIA) {
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return nullptr;
    AAudioStreamBuilder_setDirection(b, dir);
    AAudioStreamBuilder_setSampleRate(b, 44100);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(b, cb, user);
    if (dir == AAUDIO_DIRECTION_OUTPUT) {
        // Usage and content type are API 28; looked up at runtime because minSdk is 26 (Android 8.0
        // streams play as plain media).
        using SetInt = void (*)(AAudioStreamBuilder *, int32_t);
        static auto setUsage = (SetInt)dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
        static auto setContent = (SetInt)dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType");
        if (setUsage) setUsage(b, usage);
        if (setContent) setContent(b, usage == AAUDIO_USAGE_MEDIA ? AAUDIO_CONTENT_TYPE_MUSIC
                                                                  : AAUDIO_CONTENT_TYPE_SONIFICATION);
    }
    AAudioStream *s = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGW("audio: open %s failed: %s", dir == AAUDIO_DIRECTION_OUTPUT ? "output" : "input",
             AAudio_convertResultToText(r));
        return nullptr;
    }
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        return nullptr;
    }
    return s;
}

bool HostAudio::startOutput() {
    if (!outStream) outStream = openStream(AAUDIO_DIRECTION_OUTPUT, outCb, this, usage);
    return outStream != nullptr;
}

void HostAudio::setUsage(int u) {
    if (u == usage) return;
    usage = u;
    if (!outStream) return;
    // Android ties volume and silent mode to the usage, fixed when the stream opens: reopen it.
    AAudioStream_requestStop(outStream);
    AAudioStream_close(outStream);
    outStream = openStream(AAUDIO_DIRECTION_OUTPUT, outCb, this, usage);
}

bool HostAudio::startInput() {
    if (!inStream) inStream = openStream(AAUDIO_DIRECTION_INPUT, inCb, this);
    return inStream != nullptr;
}

void HostAudio::stopInput() {
    if (inStream) { AAudioStream_requestStop(inStream); AAudioStream_close(inStream); inStream = nullptr; }
    in.clear();
}

void HostAudio::capture(int16_t *frames, uint32_t n) {
    uint32_t got = inStream ? in.read(frames, n) : 0;
    if (got < n) memset(frames + got * 2, 0, (n - got) * 4);
}

void HostAudio::stop() {
    if (outStream) { AAudioStream_requestStop(outStream); AAudioStream_close(outStream); outStream = nullptr; }
    if (inStream) { AAudioStream_requestStop(inStream); AAudioStream_close(inStream); inStream = nullptr; }
    out.clear();
    in.clear();
}
