// Audio as Device Emulator exposes it: IIS registers (0x55000000) are stored only, and PCM moves
// through DMA channel 2 (playback, RAM -> IISFIFO) and channel 1 (record, IISFIFO -> RAM).
// The host format is fixed: 44.1 kHz, 16-bit, stereo.
#pragma once
#include "bus.h"
#include "s3c2410.h"
#include <cstdint>
#include <functional>

namespace wm {

class StateIO;

class AudioIis : public Device {
public:
    void serialize(StateIO &s);
    const char *name() const override { return "iis"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    uint32_t regs[4] = {};
};

class AudioDma : public Device {
public:
    void serialize(StateIO &s);
    AudioDma(Bus &bus, IntController &ic) : bus(bus), ic(ic) {}
    const char *name() const override { return "dma"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void advance(uint64_t nowNs);
    uint64_t nextEventNs() const;
    /** A playback or capture transfer is in flight (sound is being streamed). */
    bool active() const { return ch[1].running || ch[2].running; }

    static constexpr uint32_t kRate = 44100;
    // Host hooks. Playback gets interleaved stereo frames; capture fills them (silence if unset).
    std::function<void(const int16_t *frames, uint32_t count)> onPlay;
    std::function<void(int16_t *frames, uint32_t count)> onCapture;

private:
    struct Chan {
        uint32_t r[9] = {};       // DISRC DISRCC DIDST DIDSTC DCON DSTAT DCSRC DCDST DMASKTRIG
        bool running = false;
        uint64_t startNs = 0, doneNs = 0;
        uint32_t bytes = 0;
        bool queued = false;      // DISRC (or DIDST for capture) rewritten while running
        uint32_t queuedAddr = 0;
    };
    Bus &bus;
    IntController &ic;
    Chan ch[4];
    uint64_t now = 0;
    void start(int n, uint32_t addr);
    void finish(int n);
    uint8_t *ramPtr(uint32_t pa, uint32_t len);
};

}  // namespace wm
