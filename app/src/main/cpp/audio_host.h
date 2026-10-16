// Host audio for the guest: AAudio output (and, when permitted, input) at the guest's fixed
// 44.1 kHz / 16-bit / stereo format, fed through single-producer/single-consumer rings.
#pragma once
#include <aaudio/AAudio.h>
#include <atomic>
#include <cstdint>
#include <vector>

class FrameRing {
public:
    explicit FrameRing(uint32_t frames) : buf(frames * 2), cap(frames) {}
    uint32_t write(const int16_t *src, uint32_t n);  // returns frames written
    uint32_t read(int16_t *dst, uint32_t n);         // returns frames read
    uint32_t available() const { return (uint32_t)(w.load() - r.load()); }
    void clear() { r.store(w.load()); }

private:
    std::vector<int16_t> buf;
    uint32_t cap;
    std::atomic<uint64_t> w{0}, r{0};
};

class HostAudio {
public:
    HostAudio();
    ~HostAudio();
    bool startOutput();
    /** Android output usage (AAUDIO_USAGE_*): media, notification or ringtone volume. */
    void setUsage(int usage);
    bool startInput();
    void stopInput();
    void stop();
    /**
     * Guest output, with a gain: WM's wave driver peaks around -14 dB even at full volume. On top,
     * an automatic level lift (up to [maxLift]) for content that stays quiet, such as ring tones
     * and alerts played at the guest's lower system volume: it rises slowly over about a second
     * and drops at once when the sound gets loud. Above the knee the result is compressed smoothly
     * towards full scale instead of clipping.
     */
    void play(const int16_t *frames, uint32_t n) {
        if (gain <= 1.0f && maxLift <= 1.0f) { out.write(frames, n); return; }
        int16_t buf[512];
        while (n) {
            uint32_t k = n > 256 ? 256 : n;
            int peak = 0;
            for (uint32_t i = 0; i < k * 2; i++) { int a = frames[i] < 0 ? -frames[i] : frames[i]; if (a > peak) peak = a; }
            // Envelope: fast attack, slow decay (~1 s), so the lift follows phrases, not samples.
            env = peak > env ? (float)peak : env * 0.994f + peak * 0.006f;
            float want = 1.0f;
            if (env > 40.0f) {  // not the noise floor of silence
                want = kTarget / (env * gain);
                if (want > maxLift) want = maxLift;
                if (want < 1.0f) want = 1.0f;
            }
            lift = want < lift ? want : lift + (want - lift) * 0.02f;
            float g = gain * lift;
            for (uint32_t i = 0; i < k * 2; i++) {
                float v = frames[i] * g;
                float a = v < 0 ? -v : v;
                const float knee = 24000.0f, top = 32767.0f;
                if (a > knee) a = knee + (top - knee) * (1.0f - 1.0f / (1.0f + (a - knee) / (top - knee)));
                buf[i] = (int16_t)(v < 0 ? -a : a);
            }
            out.write(buf, k);
            frames += k * 2;
            n -= k;
        }
    }
    float maxLift = 4.0f;  // up to +12 dB more for quiet sounds (1 = off)
    float gain = 4.0f;  // +12 dB
    void capture(int16_t *frames, uint32_t n);
    bool outputRunning() const { return outStream != nullptr; }

private:
    static aaudio_data_callback_result_t outCb(AAudioStream *, void *user, void *data, int32_t frames);
    static aaudio_data_callback_result_t inCb(AAudioStream *, void *user, void *data, int32_t frames);
    AAudioStream *outStream = nullptr, *inStream = nullptr;
    FrameRing out{44100}, in{44100};
    static constexpr float kTarget = 20000.0f;  // level the lift aims quiet sounds at
    float env = 0.0f, lift = 1.0f;
    int usage = AAUDIO_USAGE_MEDIA;
};
