// Symmetric (de)serializer for machine snapshots: every device implements serialize(StateIO&)
// once, and the same code both writes and reads.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace wm {

class StateIO {
public:
    // Writing: start empty. Reading: give the bytes to parse.
    explicit StateIO(int64_t nowNs) : writing(true), now(nowNs) {}
    StateIO(std::vector<uint8_t> data, int64_t nowNs) : writing(false), buf(std::move(data)), now(nowNs) {}

    bool writing;
    bool ok = true;
    std::vector<uint8_t> buf;

    void bytes(void *p, size_t n) {
        if (writing) {
            const uint8_t *b = (const uint8_t *)p;
            buf.insert(buf.end(), b, b + n);
        } else if (pos + n <= buf.size()) {
            memcpy(p, &buf[pos], n);
            pos += n;
        } else {
            ok = false;
            memset(p, 0, n);
        }
    }
    template <typename T> void io(T &v) { bytes(&v, sizeof v); }
    template <typename T, size_t N> void io(T (&a)[N]) { bytes(a, sizeof a); }
    void io(bool &v) { uint8_t b = v; bytes(&b, 1); v = b != 0; }
    void io(std::string &s) {
        uint32_t n = (uint32_t)s.size();
        io(n);
        if (!writing) s.resize(ok ? n : 0);
        bytes(&s[0], s.size());
    }
    void io(std::deque<uint8_t> &q) {
        uint32_t n = (uint32_t)q.size();
        io(n);
        if (writing) {
            for (uint8_t c : q) buf.push_back(c);
        } else {
            q.clear();
            for (uint32_t i = 0; i < n && ok; i++) { uint8_t c; io(c); q.push_back(c); }
        }
    }
    // Absolute machine times are stored relative to "now" so they survive a clock restart.
    void time(uint64_t &t) {
        int64_t rel = (int64_t)t - now;
        io(rel);
        if (!writing) t = rel + now < 0 ? 0 : (uint64_t)(rel + now);
    }
    // Section marker: catches layout mismatches early.
    void tag(const char *name) {
        uint32_t v = 0;
        for (int i = 0; i < 4 && name[i]; i++) v |= (uint32_t)(uint8_t)name[i] << (8 * i);
        uint32_t got = v;
        io(got);
        if (got != v) ok = false;
    }

    void setNow(int64_t n) { now = n; }
    /** Reading: nothing left (optional trailing sections added in later versions are absent). */
    bool atEnd() const { return !writing && pos >= buf.size(); }

private:
    size_t pos = 0;
    int64_t now;
};

}  // namespace wm
