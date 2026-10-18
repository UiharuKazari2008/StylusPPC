// Headless runner: boots an image, prints serial output, and dumps the framebuffer.
// Usage: wmcore-cli <image> [--mem MB] [--seconds N] [--v4] [--dump out.ppm]
//                   [--trace N] [--trace-at PC] [--log debug]
#include "../src/board/machine.h"
#include "../src/arm/disasm.h"
#include "../src/arm/jit.h"
#include "../src/board/devices.h"
#include "../src/board/de_devices.h"
#include "../src/board/audio.h"
#include "../src/board/pcmcia.h"
#include "../src/net/usernet.h"
#include "../src/net/revival.h"
#include "../src/net/mwsgate.h"
#include "../src/board/folder_share.h"
#include "../src/board/camera.h"
#include "../src/board/modem.h"
#include "../src/board/codepatch.h"
#include "../src/board/motion.h"
#include "../src/board/irda.h"  // infrared bridge on UART2
#include "irda_link.h"
#include "../src/util/log.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <thread>
#include <dirent.h>
#include <sys/stat.h>

using namespace wm;

int mwsSelfTest(int argc, char **argv);  // mws_test.cpp
int thumbJitSelfTest();                  // thumb_test.cpp

static uint64_t g_traceLeft = 0;
static uint32_t g_traceAt = 0xFFFFFFFF;
static uint32_t g_tracePid = 0xFFFFFFFF;  // --trace-pid: --trace-at only fires in this FCSE slot
static uint32_t g_rangeLo = 1, g_rangeHi = 0;
static bool g_ringVecLoop = false;  // --ring-vec-loop: trigger when a vector-page instruction runs twice in a row (the vector itself faults)
static uint32_t g_ringLastPc = 0;
static uint64_t g_ringAtInsn = ~0ull;  // --ring-at-insn N: trigger at that instruction count
static bool g_ringLowMmu = false;  // --ring-low-mmu: trigger when code runs below 8 KB with the MMU on  // --trace-range: trigger when pc enters [lo, hi)
static bool g_tracing = false;
static const char *g_camPpm = nullptr;  // --camera-* (camera bridge)
static int g_camRot = 0;
static bool g_camMirror = false;
static int g_camEra = 0;  // 2009 look strength (0 = off)
static FILE *g_traceFile = nullptr;
// --ring N: remember the last N instructions and dump them when --trace-at is reached.
struct RingEntry { uint32_t pc, insn, cpsr, r[16]; bool thumb; };
static std::vector<RingEntry> g_ring;
static size_t g_ringPos = 0;
static bool g_ringDumped = false;

static void dumpEntry(FILE *f, const RingEntry &e) {
    std::string d = e.thumb ? disasmThumb(e.pc, (uint16_t)e.insn) : disasmArm(e.pc, e.insn);
    fprintf(f, "%08x %s%0*x  %-40s cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r12=%08x sp=%08x lr=%08x\n",
            e.pc, e.thumb ? "T " : "", e.thumb ? 4 : 8, e.insn, d.c_str(), e.cpsr, e.r[0], e.r[1], e.r[2], e.r[3],
            e.r[4], e.r[5], e.r[12], e.r[13], e.r[14]);
}

static void ringHook(void *, Cpu &cpu, uint32_t pc, uint32_t insn, bool thumb) {
    if (g_ringDumped) return;
    if (pc == g_traceAt || (pc >= g_rangeLo && pc < g_rangeHi) ||
        (g_ringLowMmu && (cpu.cp15_ctrl & 1) && pc < 0x2000) ||
        (g_ringVecLoop && pc >= 0xFFFF0000 && pc == g_ringLastPc) || cpu.insnCount >= g_ringAtInsn) {
        FILE *f = g_traceFile ? g_traceFile : stdout;
        for (size_t i = 0; i < g_ring.size(); i++) {
            const RingEntry &e = g_ring[(g_ringPos + i) % g_ring.size()];
            if (e.pc || e.insn) dumpEntry(f, e);
        }
        fflush(f);
        g_ringDumped = true;
        return;
    }
    g_ringLastPc = pc;
    RingEntry &e = g_ring[g_ringPos];
    g_ringPos = (g_ringPos + 1) % g_ring.size();
    e.pc = pc; e.insn = insn; e.thumb = thumb; e.cpsr = cpu.getCpsr();
    for (int i = 0; i < 16; i++) e.r[i] = cpu.reg(i);
}

static void traceHook(void *, Cpu &cpu, uint32_t pc, uint32_t insn, bool thumb) {
    if (!g_tracing && pc == g_traceAt && (g_tracePid == 0xFFFFFFFF || cpu.cp15_pid == g_tracePid)) g_tracing = true;
    if (!g_tracing || !g_traceLeft) return;
    g_traceLeft--;
    std::string d = thumb ? disasmThumb(pc, (uint16_t)insn) : disasmArm(pc, insn);
    fprintf(g_traceFile, "%08x %s%0*x  %-36s cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x sp=%08x lr=%08x\n",
            pc, thumb ? "T " : "", thumb ? 4 : 8, insn, d.c_str(), cpu.getCpsr(), cpu.reg(0), cpu.reg(1),
            cpu.reg(2), cpu.reg(3), cpu.reg(4), cpu.reg(13), cpu.reg(14));
}

// --nmea: a stationary GPS receiver on COM1 (only while the guest has the port open).
static std::string nmeaLine(const std::string &body) {
    unsigned sum = 0;
    for (char c : body) sum ^= (unsigned char)c;
    char tail[8];
    snprintf(tail, sizeof tail, "*%02X\r\n", sum);
    return "$" + body + tail;
}

static std::string nmeaBurst(int sec) {
    char t[16];
    snprintf(t, sizeof t, "%02d%02d%02d.00", 12, (sec / 60) % 60, sec % 60);
    std::string ts = t;
    return nmeaLine("GPGGA," + ts + ",4740.1234,N,12218.5678,W,1,07,1.1,54.0,M,-17.0,M,,") +
           nmeaLine("GPGSA,A,3,02,05,12,15,18,24,29,,,,,,1.9,1.1,1.5") +
           nmeaLine("GPGSV,3,1,09,02,45,050,38,05,62,120,41,12,30,200,35,15,15,310,28") +
           nmeaLine("GPGSV,3,2,09,18,70,260,44,24,25,080,33,29,10,170,22,31,05,020,00") +
           nmeaLine("GPGSV,3,3,09,25,40,350,00") +
           nmeaLine("GPRMC," + ts + ",A,4740.1234,N,12218.5678,W,0.0,0.0,151026,,,A");
}

static void dumpPpm(Machine &m, const char *path) {
    std::vector<uint32_t> px;
    int w, h;
    if (!m.snapshotFramebuffer(px, w, h)) { LOGW("no framebuffer to dump"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t c : px) {
        uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    LOGI("framebuffer %dx%d written to %s", w, h, path);
}

// The helper's mailbox (--box): commands go in as the app writes them (in\NNNNNNNN.tmp renamed to
// .cmd, numbered on from the highest name there); boxReport logs what waits and what was answered.
static std::vector<std::string> boxList(const std::string &dir, const char *ext) {
    std::vector<std::string> v;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ext) == 0) v.push_back(n);
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

static int g_boxSeq = -1;

static void boxCommand(const std::string &box, const std::string &text) {
    std::string in = box + "/in";
    mkdir(in.c_str(), 0777);
    if (g_boxSeq < 0) {
        g_boxSeq = 0;
        for (auto &n : boxList(in, ".cmd")) g_boxSeq = std::max(g_boxSeq, atoi(n.c_str()) + 1);
    }
    char name[32];
    snprintf(name, sizeof name, "%08d", g_boxSeq++);
    std::string tmp = in + "/" + name + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fputs(text.c_str(), f);
    fclose(f);
    rename(tmp.c_str(), (in + "/" + name + ".cmd").c_str());
}

static void boxReport(const std::string &box, double t) {
    static size_t lastPending = ~(size_t)0, lastOut = ~(size_t)0;
    static double quietSince = 0;
    size_t pending = boxList(box + "/in", ".cmd").size(), out = boxList(box + "/out", ".msg").size();
    if (pending != lastPending || out != lastOut) quietSince = t;
    // Every change, and every 10 s while commands wait unanswered.
    if (pending != lastPending || out != lastOut || (pending && (int)(t - quietSince) % 10 == 0))
        LOGI("box t=%.0f: %zu pending, %zu answers%s", t, pending, out,
             pending && t - quietSince >= 10 ? " (helper silent)" : "");
    lastPending = pending;
    lastOut = out;
}

// Runs the interpreter and the JIT side by side on the deterministic clock and reports the first
// virtual millisecond after which their CPU state differs.
static uint64_t g_compareStep = 1000000;

static int runCompare(MachineConfig cfg, double seconds) {
    cfg.deterministic = true;
    MachineConfig ci = cfg, cj = cfg;
    ci.jit = false;
    cj.jit = true;
    Machine a(ci), b(cj);
    std::string err;
    if (!a.load(err) || !b.load(err)) { LOGE("load failed: %s", err.c_str()); return 1; }
    a.uart1.onTx = [](int, uint8_t c) { if (c != '\r') fputc(c, stdout); };
    const uint64_t step = g_compareStep;
    uint64_t steps = (uint64_t)(seconds * 1e9 / step);
    auto t0 = std::chrono::steady_clock::now();
    double ta = 0, tb = 0;
    for (uint64_t k = 0; k < steps; k++) {
        auto s0 = std::chrono::steady_clock::now();
        a.runFor(step);
        auto s1 = std::chrono::steady_clock::now();
        b.runFor(step);
        auto s2 = std::chrono::steady_clock::now();
        ta += std::chrono::duration<double>(s1 - s0).count();
        tb += std::chrono::duration<double>(s2 - s1).count();
        bool same = a.cpu.getCpsr() == b.cpu.getCpsr() && a.cpu.insnCount == b.cpu.insnCount;
        for (int r = 0; r < 16 && same; r++) same = a.cpu.reg(r) == b.cpu.reg(r);
        if (!same) {
            // Replay the last millisecond block by block to find the first bad block.
            Machine a2(ci), b2(cj);
            a2.load(err);
            b2.load(err);
            for (uint64_t q = 0; q < k; q++) { a2.runFor(step); b2.runFor(step); }
            b2.cpu.singleBlock = true;
            for (int guard = 0; guard < 2000000; guard++) {
                uint32_t pc = b2.cpu.pc();
                uint32_t before[16];
                for (int r = 0; r < 16; r++) before[r] = b2.cpu.reg(r);
                uint32_t cpsrBefore = b2.cpu.getCpsr();
                uint64_t n0 = b2.cpu.insnCount;
                b2.cpu.run(64);
                uint64_t n = b2.cpu.insnCount - n0;
                a2.cpu.run(n ? n : 1);
                bool ok = a2.cpu.getCpsr() == b2.cpu.getCpsr() && a2.cpu.insnCount == b2.cpu.insnCount;
                for (int r = 0; r < 16 && ok; r++) ok = a2.cpu.reg(r) == b2.cpu.reg(r);
                if (ok) continue;
                printf("first bad block at pc %08x (%llu insns), cpsr before %08x\n", pc, (unsigned long long)n, cpsrBefore);
                for (uint64_t i = 0; i < n + 1 && i < 64; i++) {
                    uint32_t w = 0;
                    b2.cpu.debugRead32(pc + 4 * (uint32_t)i, w);
                    printf("  %08x %08x  %s\n", pc + 4 * (uint32_t)i, w, disasmArm(pc + 4 * (uint32_t)i, w).c_str());
                }
                printf("        before     interp     jit\n");
                for (int r = 0; r < 16; r++)
                    printf("r%-2d   %08x  %08x  %08x%s\n", r, before[r], a2.cpu.reg(r), b2.cpu.reg(r),
                           a2.cpu.reg(r) != b2.cpu.reg(r) ? "  <--" : "");
                printf("cpsr  %08x  %08x  %08x\n", cpsrBefore, a2.cpu.getCpsr(), b2.cpu.getCpsr());
                return 3;
            }
        }
        if (!same || (k % 1000 == 999 && memcmp(a.ram.data(), b.ram.data(), a.ram.size()) != 0)) {
            printf("\nDIVERGED after %.3f virtual s (step %llu)%s\n", (k + 1) / 1000.0, (unsigned long long)k,
                   same ? " (RAM only)" : "");
            printf("        interp     jit\n");
            for (int r = 0; r < 16; r++)
                printf("r%-2d   %08x  %08x%s\n", r, a.cpu.reg(r), b.cpu.reg(r), a.cpu.reg(r) != b.cpu.reg(r) ? "  <--" : "");
            printf("cpsr  %08x  %08x\ninsns %llu %llu\n", a.cpu.getCpsr(), b.cpu.getCpsr(),
                   (unsigned long long)a.cpu.insnCount, (unsigned long long)b.cpu.insnCount);
            if (same) {
                for (size_t i = 0; i < a.ram.size(); i += 4)
                    if (memcmp(&a.ram[i], &b.ram[i], 4)) { printf("first RAM difference at pa %08zx\n", 0x30000000 + i); break; }
            }
            return 3;
        }
        if (k % (1000000000ull / step) == 1000000000ull / step - 1) {
            JitStats js = jitStats(b.cpu.jit);
            LOGI("ok to %.0f s virtual | interp %.1f MIPS, jit %.1f MIPS | blocks %llu run %llu interpExits %llu flushes %llu code %llu KB",
                 (k + 1) / 1000.0, a.cpu.insnCount / ta / 1e6, b.cpu.insnCount / tb / 1e6,
                 (unsigned long long)js.blocksCompiled, (unsigned long long)js.blocksRun,
                 (unsigned long long)js.interpExits, (unsigned long long)js.flushes,
                 (unsigned long long)(js.codeBytes >> 10));
        }
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("no divergence over %.1f virtual s (%.1f s wall)", seconds, el);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--mws-selftest")) return mwsSelfTest(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--thumb-selftest")) return thumbJitSelfTest();
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [--mem MB] [--seconds N] [--v4] [--dump out.ppm] [--trace N] "
                        "[--trace-at HEXPC] [--trace-file F] [--log debug]\n"
                        "  sleep/suspend tests (T = run time in s, quarter-second steps):\n"
                        "  --pause T:S      stop running the guest for S s of wall time (the app's pause)\n"
                        "  --warp T:S       the same jump without waiting: host clock and RTC move S s ahead\n"
                        "  --rtc-skew S     after --load-state: RTC S s ahead (a hibernation that long)\n"
                        "  --save-at T:F    write a snapshot at T and run on\n"
                        "  --card-out T / --card-in T   storage card removed / inserted (folder share off/on)\n"
                        "  --box DIR        mailbox (.wmdroid) for --cmd; logs pending/answered per second\n"
                        "  --cmd T:text     the app's next mailbox command at T (numbered like the app)\n"
                        "  --cmd-burst T:N  N mixed commands (status, rotate, wallpaper, theme, backlight, ...)\n"
                        "  --quiet-share    no folder share call trace\n"
                        "  --no-remount     after --load-state the card stays as the guest knew it (no re-announce)\n"
                        "  --adopt-state F  like --load-state, but F may be from another screen or memory size (>= 96 MB):\n"
                        "                   WM 6.x boots again (cold) keeping its storage (the app's settings migration)\n"
                        "  --adopt-warm F   the same as a warm restart, same memory size (WM 5: another screen)\n"
                        "  infrared (UART2, docs/research-irda.md):\n"
                        "  --irda-trace     log every IrLAP frame in and out\n"
                        "  --irda-udp L:R   frames over UDP on 127.0.0.1 (port L here, R the other instance)\n"
                        "  --irda-delay MS  UDP: send each frame MS ms late (network latency tests)\n"
                        "  --irda-wan URL   InfraWAN relay: ws://host:port/infrawan?room=R&user=U&token=T&name=N\n"
                        "  --irda-ask       connect only after the helper's popup (\"irda ask\" on start, needs --box)\n"
                        "  --irda-choose T:ble|wan|cancel  answer the popup at T (as if its button was tapped)\n"
                        "  secure web proxy (with --net, docs/research-tls-proxy.md):\n"
                        "  --tls DIR        HTTPS through the proxy; DIR keeps its private root CA (made on first\n"
                        "                   use; DIR/ca.cer is the root for the guest: helper \"tlsca install\")\n"
                        "  --tls-ports P,.. ports to intercept (default 443)\n"
                        "  --tls-roots DIR  validate servers against the PEM files in DIR instead of Android's store\n"
                        "  --tls-ask        certificate warnings to the helper's dialog (\"tlsask\", needs --box) and\n"
                        "                   its answers back to the proxy, as the app does (SecureProxy.kt)\n"
                        "  --tls-answer yes|no  answer every certificate warning at once, without the helper\n"
                        "  --tls-wait S     how long a request waits for the answer (default 40 s)\n"
                        "  --host-map N=T   DNS: answer name N (\".domain\" = domain and below) as T (IPv4 or a name)\n"
                        "  --escargot       Messenger / Passport names to Escargot (net/revival.h); --host-map rules win\n"
                        "  --mws DIR        Windows Live Mobile gateway (net/mwsgate.h) at 10.0.2.4 for *.mws.mobile.live.com,\n"
                        "                   MSNP to Escargot; every exchange (secrets redacted) captured into DIR\n"
                        "  --mws-fake       the gateway signs in to a fake MSNP server on 127.0.0.1 instead (tests)\n"
                        "  %s --mws-selftest [--probe] [--capture DIR]   the gateway without a guest\n", argv[0], argv[0]);
        return 2;
    }
    MachineConfig cfg;
    cfg.imagePath = argv[1];
    double seconds = 10;
    int fnCode = -1;
    bool nmea = false;
    bool net = false;
    std::string tlsDir;                   // --tls
    std::vector<uint16_t> tlsPorts{443};  // --tls-ports
    bool tlsAsk = false;                  // --tls-ask
    std::string tlsAnswer;                // --tls-answer
    int tlsWait = 0;                      // --tls-wait
    std::vector<HostRule> hostRules;      // --host-map
    bool escargot = false;                // --escargot: escargotHostRules() after the --host-map rules
    std::string mwsDir;                   // --mws: Windows Live Mobile gateway, capture folder
    bool mws = false, mwsFake = false;    // --mws-fake: fake MSNP server instead of Escargot
    struct Tap { double t; int x, y; };
    std::vector<Tap> taps;
    struct Drag { double t; int x0, y0, x1, y1; };
    std::vector<Drag> drags;  // --drag T:x0,y0,x1,y1  pen down at T, moved over half a second, then up
    std::vector<std::pair<double, std::vector<int>>> raws;  // --raw
    std::vector<std::pair<double, int>> keys;          // --key
    std::vector<std::pair<double, std::string>> shots;  // --shot
    std::vector<double> powers;                         // --power
    std::vector<double> resets;                         // --reset
    std::vector<std::pair<double, std::string>> puts;   // --put T:path:text  (e.g. a helper mailbox command)
    std::vector<std::pair<double, std::string>> batteries;  // --battery T:ac,percent  (the DE battery device)
    std::vector<std::pair<double, std::string>> accels;     // --accel T:ax,ay,az  (m/s^2 like Android; motion.h)
    std::vector<std::pair<double, std::string>> regDwords;  // --reg-dword T:name:value
    const char *ownNumber = nullptr;
    double burstT0 = 0, burstT1 = -1, burstDt = 0;      // --burst
    std::string burstPrefix;
    double profileFrom = -1;
    bool compare = false;
    const char *dump = nullptr;
    const char *ramDump = nullptr;
    const char *jitMapPath = nullptr;  // --jit-map
    const char *shareDir = nullptr;
    std::vector<FolderShare::Mount> overlays;
    bool traceIo = false;
    bool noCoalesce = false;
    int umstat0 = -1;
    // Virtual modem (modem.h): --modem-net T:"svc=1 sig=3 roam=0 num=310260 op=Name" stands in for the
    // app's network feed, --modem-call T:id:state for its call progress; --modem-stub answers calls itself;
    // --modem-ring T:number rings an incoming call ("-": withheld); --modem-sms T:hex delivers an SMS-DELIVER
    // PDU; with --modem-stub, messages WM sends count as sent.
    std::vector<std::pair<double, std::string>> modemNets, modemCalls, modemRings, modemSms;
    bool traceModem = false, modemStub = false;
    std::vector<double> modemRefresh;
    const char *saveStatePath = nullptr, *loadStatePath = nullptr;
    Machine::Adopt adoptState = Machine::Adopt::No;  // --adopt-state / --adopt-warm: another screen or memory size
    const char *traceFile = nullptr;
    // Sleep/suspend tests (see the usage text).
    std::vector<std::pair<double, double>> pauses, warps;
    std::vector<std::pair<double, std::string>> saveAts, cmds;
    std::vector<std::pair<double, int>> cmdBursts;
    std::vector<double> cardOuts, cardIns;
    double rtcSkew = 0;
    const char *boxDir = nullptr;
    bool quietShare = false, noRemount = false;
    // Infrared (irda.h): transport for the guest's IrLAP frames.
    bool irdaTrace = false, irdaAsk = false;
    std::string irdaUdp, irdaWan;
    int irdaDelay = 0;
    std::vector<std::pair<double, std::string>> irdaChoices;
    auto timeArg = [](const std::string &v, std::string &rest) {
        size_t c = v.find(':');
        rest = c == std::string::npos ? "" : v.substr(c + 1);
        return atof(v.substr(0, c).c_str());
    };
    uint32_t mipsCap = 0;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--mem") cfg.memMB = (uint32_t)atoi(next());
        else if (a == "--mips") mipsCap = (uint32_t)atoi(next());  // speed limit (Machine::mipsCap)
        else if (a == "--seconds") seconds = atof(next());
        else if (a == "--v4") cfg.core = CORE_ARMV4;
        else if (a == "--profile-from") profileFrom = atof(next());
        else if (a == "--screen") sscanf(next(), "%dx%d", &cfg.screenW, &cfg.screenH);
        else if (a == "--dump") dump = next();
        else if (a == "--fn") fnCode = (int)strtol(next(), nullptr, 0);
        else if (a == "--nmea") nmea = true;
        else if (a == "--net") net = true;
        else if (a == "--tls") tlsDir = next();
        else if (a == "--tls-roots") TlsProxy::setTrustDir(next());
        else if (a == "--tls-ask") tlsAsk = true;
        else if (a == "--tls-answer") tlsAnswer = next();
        else if (a == "--tls-wait") tlsWait = atoi(next());
        else if (a == "--tls-ports") {
            tlsPorts.clear();
            std::string v = next();
            for (size_t i = 0; i < v.size();) {
                size_t c = v.find(',', i);
                if (c == std::string::npos) c = v.size();
                tlsPorts.push_back((uint16_t)atoi(v.substr(i, c - i).c_str()));
                i = c + 1;
            }
        }
        else if (a == "--host-map") {
            std::string v = next();
            size_t e = v.find('=');
            if (e != std::string::npos) hostRules.push_back({v.substr(0, e), v.substr(e + 1)});
        }
        else if (a == "--escargot") escargot = true;  // the app's "Windows Live Messenger via Escargot"
        else if (a == "--mws") { mws = true; mwsDir = next(); }
        else if (a == "--mws-fake") mwsFake = true;
        else if (a == "--tap") {  // T:x,y  pen down at T seconds, up a quarter second later
            std::string v = next();
            size_t c = v.find(':'), k = v.find(',');
            taps.push_back({atof(v.substr(0, c).c_str()), atoi(v.substr(c + 1, k - c - 1).c_str()), atoi(v.substr(k + 1).c_str())});
        }
        else if (a == "--drag") {
            Drag d{};
            if (sscanf(next(), "%lf:%d,%d,%d,%d", &d.t, &d.x0, &d.y0, &d.x1, &d.y1) == 5) drags.push_back(d);
        }
        else if (a == "--raw") {  // T:c1,c2,...  raw keyboard matrix codes at T seconds
            std::string v = next();
            size_t c = v.find(':');
            std::vector<int> codes;
            for (size_t p = c + 1; p < v.size();) {
                size_t e = v.find(',', p);
                if (e == std::string::npos) e = v.size();
                codes.push_back((int)strtol(v.substr(p, e - p).c_str(), nullptr, 16));
                p = e + 1;
            }
            raws.push_back({atof(v.substr(0, c).c_str()), codes});
        }
        else if (a == "--key") {  // T:VK, pressed at T seconds and released a quarter second later
            std::string v = next();
            size_t c = v.find(':');
            keys.push_back({atof(v.substr(0, c).c_str()), (int)strtol(v.substr(c + 1).c_str(), nullptr, 0)});
        } else if (a == "--shot") {  // T:file.ppm, framebuffer at T seconds
            std::string v = next();
            size_t c = v.find(':');
            shots.push_back({atof(v.substr(0, c).c_str()), v.substr(c + 1)});
        }
        else if (a == "--own-number") ownNumber = next();  // FakeRIL's SIM number (codepatch.h)
        else if (a == "--reg-dword") {  // T:name:value  experiment: set a REG_DWORD in the RAM hive cells
            std::string rest;
            double t = timeArg(next(), rest);
            regDwords.push_back({t, rest});
        }
        else if (a == "--accel") {
            std::string v = next();
            size_t c = v.find(':');
            if (c != std::string::npos) accels.push_back({atof(v.substr(0, c).c_str()), v.substr(c + 1)});
        }
        else if (a == "--battery") {
            std::string v = next();
            size_t c = v.find(':');
            if (c != std::string::npos) batteries.push_back({atof(v.substr(0, c).c_str()), v.substr(c + 1)});
        }
        else if (a == "--put") {
            std::string v = next();
            size_t c = v.find(':');
            if (c != std::string::npos) puts.push_back({atof(v.substr(0, c).c_str()), v.substr(c + 1)});
        }
        else if (a == "--pause" || a == "--warp" || a == "--save-at" || a == "--cmd" || a == "--cmd-burst") {
            std::string rest;
            double t = timeArg(next(), rest);
            if (a == "--pause") pauses.push_back({t, atof(rest.c_str())});
            else if (a == "--warp") warps.push_back({t, atof(rest.c_str())});
            else if (a == "--save-at") saveAts.push_back({t, rest});
            else if (a == "--cmd") cmds.push_back({t, rest});
            else cmdBursts.push_back({t, atoi(rest.c_str())});
        }
        else if (a == "--card-out") cardOuts.push_back(atof(next()));
        else if (a == "--card-in") cardIns.push_back(atof(next()));
        else if (a == "--rtc-skew") rtcSkew = atof(next());
        else if (a == "--box") boxDir = next();
        else if (a == "--quiet-share") quietShare = true;
        else if (a == "--no-remount") noRemount = true;
        else if (a == "--irda-trace") irdaTrace = true;
        else if (a == "--irda-udp") irdaUdp = next();
        else if (a == "--irda-wan") irdaWan = next();
        else if (a == "--irda-delay") irdaDelay = atoi(next());
        else if (a == "--irda-ask") irdaAsk = true;
        else if (a == "--irda-choose") { std::string rest; double t = timeArg(next(), rest); irdaChoices.push_back({t, rest}); }
        else if (a == "--reset") resets.push_back(atof(next()));  // T: the app's Restart (warm reboot)
        else if (a == "--power") powers.push_back(atof(next()));  // T: press the power button (suspend/wake)
        else if (a == "--burst") {  // T0:T1:DT:prefix  framebuffer every DT s in [T0,T1] (only frames that changed)
            char pre[400] = "";
            if (sscanf(next(), "%lf:%lf:%lf:%399s", &burstT0, &burstT1, &burstDt, pre) == 4) burstPrefix = pre;
        }
        else if (a == "--dump-ram") ramDump = next();
        else if (a == "--share") shareDir = next();
        else if (a == "--overlay") {  // name=path, repeatable
            std::string v = next();
            size_t eq = v.find('=');
            if (eq != std::string::npos) overlays.push_back({v.substr(0, eq), v.substr(eq + 1)});
        }
        else if (a == "--trace-io") traceIo = true;
        else if (a == "--save-state") saveStatePath = next();
        else if (a == "--load-state") loadStatePath = next();
        else if (a == "--adopt-state") { loadStatePath = next(); adoptState = Machine::Adopt::Cold; }  // other screen / RAM size
        else if (a == "--adopt-warm") { loadStatePath = next(); adoptState = Machine::Adopt::Warm; }  // WM 5: other screen
        else if (a == "--modem-net" || a == "--modem-call" || a == "--modem-ring" || a == "--modem-sms") {
            std::string v = next();
            size_t c = v.find(':');
            (a == "--modem-net" ? modemNets : a == "--modem-call" ? modemCalls : a == "--modem-ring" ? modemRings : modemSms).push_back({atof(v.substr(0, c).c_str()), v.substr(c + 1)});
        }
        else if (a == "--trace-modem") traceModem = true;
        else if (a == "--modem-stub") modemStub = true;
        else if (a == "--modem-refresh") modemRefresh.push_back(atof(next()));  // T: the app's answer to "radio refresh"

        else if (a == "--umstat0") umstat0 = (int)strtol(next(), nullptr, 0);
        else if (a == "--trace") g_traceLeft = strtoull(next(), nullptr, 0);
        else if (a == "--trace-at") g_traceAt = (uint32_t)strtoul(next(), nullptr, 16);
        else if (a == "--trace-pid") g_tracePid = (uint32_t)strtoul(next(), nullptr, 16);  // e.g. 1e000000
        else if (a == "--trace-file") traceFile = next();
        else if (a == "--trace-range") {
            g_rangeLo = (uint32_t)strtoul(next(), nullptr, 16);
            g_rangeHi = (uint32_t)strtoul(next(), nullptr, 16);
        }
        else if (a == "--ring") g_ring.resize(strtoul(next(), nullptr, 0));
        else if (a == "--ring-low-mmu") g_ringLowMmu = true;
        else if (a == "--ring-vec-loop") g_ringVecLoop = true;
        else if (a == "--ring-at-insn") g_ringAtInsn = strtoull(next(), nullptr, 0);
        else if (a == "--log") { std::string l = next(); if (l == "debug") g_logLevel = LOG_DEBUG; }
        else if (a == "--nojit") cfg.jit = false;
        else if (a == "--no-coalesce") noCoalesce = true;
        else if (a == "--det") cfg.deterministic = true;
        else if (a == "--compare") compare = true;
        else if (a == "--jit-max") jitSetMaxBlock(atoi(next()));
        else if (a == "--compare-step") g_compareStep = strtoull(next(), nullptr, 0);
        else if (a == "--helper-hist") jitProfileHelpers(true);  // instructions blocks hand to the interpreter
        else if (a == "--jit-map") jitMapPath = next();  // host code -> guest pc map at exit (profiling)
        // Camera bridge: --camera-ppm F feeds a still picture as the phone camera (default: the
        // test pattern), --camera-rot N turns it clockwise, --camera-mirror flips it, --camera-era.
        else if (a == "--camera-ppm") g_camPpm = next();
        else if (a == "--camera-rot") g_camRot = atoi(next());
        else if (a == "--camera-mirror") g_camMirror = true;
        else if (a == "--camera-era") g_camEra = 100;
        else if (a == "--camera-era-level") g_camEra = atoi(next());
    }
    if (compare) return runCompare(cfg, seconds);
    if (!boxDir)
        for (auto &o : overlays)
            if (o.name == ".wmdroid") boxDir = o.path.c_str();
    Machine m(cfg);
    m.mipsCap = mipsCap;
    std::string err;
    if (!m.load(err)) { LOGE("load failed: %s", err.c_str()); return 1; }
    if (ownNumber) m.codePatch->setOwnNumber(ownNumber);
    if (m.cpu.jit) jitProfileInterp(m.cpu.jit, true);
    if (umstat0 >= 0) m.uart0.umstat = (uint32_t)umstat0;
    if (loadStatePath) {
        auto t = std::chrono::steady_clock::now();
        if (!m.loadState(loadStatePath, err, adoptState)) { LOGE("restore failed: %s", err.c_str()); return 1; }
        LOGI("restored in %.2f s (pen %s, raw %d,%d, adctsc %x)", std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(),
             m.adc->penDown ? "down" : "up", m.adc->rawX, m.adc->rawY, m.adc->adctsc);
        if (rtcSkew != 0) { m.rtc.offsetSec += (int64_t)rtcSkew; LOGI("rtc: %+.0f s", rtcSkew); }
    }
    m.idleCoalescing = !noCoalesce;
    m.modem->trace = traceModem;
    m.modem->stubCalls = modemStub;
    if (traceIo) {
        m.dePage->traceDma = true;
        m.uart0.traceConfig = m.uart1.traceConfig = m.uart2.traceConfig = true;
    }
    if (shareDir) {
        m.folderShare->traceCalls = !quietShare;
        m.emulServ->trace = !quietShare;
        m.folderShare->setOverlays(overlays);
        m.folderShare->setMounts({{"", shareDir}}, !noRemount);
    }
    if (!g_ring.empty()) {
        g_traceFile = traceFile ? fopen(traceFile, "w") : stdout;
        m.cpu.traceHook = ringHook;
    } else if (g_traceLeft) {
        g_traceFile = traceFile ? fopen(traceFile, "w") : stdout;
        g_tracing = g_traceAt == 0xFFFFFFFF;
        m.cpu.traceHook = traceHook;
    }
    static std::string line[3];
    auto serial = [](int idx, uint8_t c) {
        if (c == '\n') { printf("[uart%d] %s\n", idx, line[idx].c_str()); fflush(stdout); line[idx].clear(); }
        else if (c != '\r') line[idx] += (char)c;
    };
    if (fnCode >= 0) m.fnCode = (uint8_t)fnCode;
    if (g_camEra) m.camera->setEra(g_camEra);
    if (g_camPpm) {  // camera bridge: a P6 picture as the phone camera's frames
        FILE *cf = fopen(g_camPpm, "rb");
        int cw = 0, ch = 0, cmax = 0;
        if (cf && fscanf(cf, "P6 %d %d %d", &cw, &ch, &cmax) == 3 && fgetc(cf) != EOF && cw > 0 && ch > 0) {
            std::vector<uint8_t> px((size_t)cw * ch * 3);
            if (fread(px.data(), 1, px.size(), cf) == px.size()) {
                m.camera->setHostCamera(true);
                m.camera->pushRgb(px.data(), cw, ch, g_camRot, g_camMirror);
                LOGI("camera: %dx%d picture from %s", cw, ch, g_camPpm);
            }
        } else LOGW("camera: cannot read %s", g_camPpm);
        if (cf) fclose(cf);
    }
    // --net: NE2000 card in the PCMCIA slot, connected to the user-mode network.
    UserNet userNet;
    std::shared_ptr<TlsProxy> tlsProxy;
    if (net) {
        const uint8_t mac[6] = {0x02, 0x57, 0x4D, 0x44, 0x00, 0x01};
        m.pcmcia->setMac(mac);
        m.pcmcia->onTransmit = [&](const uint8_t *f, size_t n) {
            static int logged = 0;
            if (logged++ < 20) LOGI("net: guest sent %zu bytes, type %02x%02x", n, f[12], f[13]);
            userNet.fromGuest(f, n);
        };
        if (mws) {
            std::vector<HostRule> gw = MwsGateway::hostRules();
            hostRules.insert(hostRules.end(), gw.begin(), gw.end());
            MwsGateway::Config gc;
            gc.captureDir = mwsDir;
            if (mwsFake) {
                gc.msnpHost = "127.0.0.1";
                gc.msnpPort = startFakeMsnpServer(&gc.nexusUrl);
            }
            auto gw2 = std::make_shared<MwsGateway>(gc);
            if (!tlsDir.empty()) gw2->setStateDir(tlsDir);
            setLocalService(gw2);
        }
        if (escargot) {
            std::vector<HostRule> esc = escargotHostRules();
            hostRules.insert(hostRules.end(), esc.begin(), esc.end());
        }
        if (!hostRules.empty()) userNet.setHostRules(hostRules);
        if (!tlsDir.empty()) {
            std::string err;
            auto proxy = TlsProxy::open(tlsDir, &err);
            if (proxy) {
                LOGI("tls: root %s (%s/ca.cer)", proxy->caThumbprint().c_str(), tlsDir.c_str());
                if (tlsWait > 0) proxy->setPromptWait(tlsWait);
                userNet.setTlsProxy(proxy, tlsPorts);
                tlsProxy = proxy;
            } else {
                LOGE("tls: proxy not available: %s", err.c_str());
            }
        }
        userNet.start();
        m.pcmcia->setInserted(true);
    }
    auto pumpNet = [&] {
        std::vector<uint8_t> f;
        while (net && userNet.nextForGuest(f)) {
            if (!m.pcmcia->receive(f.data(), f.size())) break;
            userNet.popForGuest();
        }
    };
    // Audio meter: frames and loudest sample played per second.
    static uint64_t audioFrames = 0;
    static int audioPeak = 0;
    m.dma->onPlay = [](const int16_t *f, uint32_t n) {
        audioFrames += n;
        for (uint32_t i = 0; i < n * 2; i++) audioPeak = std::max(audioPeak, std::abs((int)f[i]));
    };
    m.uart0.onTx = serial;
    m.uart1.onTx = serial;
    // UART2 is the infrared port (irda.h): binary SIR frames, not text.
    m.irda->trace = irdaTrace;
    std::unique_ptr<IrdaLink> irdaLink;
    if (!irdaUdp.empty()) {
        int lp = 0, rp = 0;
        sscanf(irdaUdp.c_str(), "%d:%d", &lp, &rp);
        irdaLink = irdaUdpLink(lp, rp, irdaDelay);
    } else if (!irdaWan.empty()) {
        irdaLink = irdaWanLink(irdaWan);
    }
    bool irdaStarted = false;
    auto irdaConnect = [&] {
        if (!irdaLink || irdaStarted) return;
        std::string e;
        if (irdaLink->start(*m.irda, e)) irdaStarted = true;
        else LOGE("irda: %s", e.c_str());
    };
    if (!irdaAsk) irdaConnect();
    std::set<std::string> irdaSeenMsgs, tlsSeenMsgs;

    std::map<uint32_t, uint32_t> hist;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t lastInsns = 0;
    const uint64_t runInsns0 = m.cpu.insnCount, runIdle0 = m.idleNs, runWakes0 = m.idleWakes;
    for (int s = 0; s < (int)(seconds * 4); s++) {
        double t = s / 4.0;
        for (auto &k : keys) {
            if (k.first == t) {
                m.key(k.second, true);
                LOGI("key %02x down: spcon=%x queued=%zu intmsk=%x eint1 pending=%d", k.second, m.kbd->control(),
                     m.kbd->queued(), m.intc.intmsk, (int)((m.intc.srcpnd >> 1) & 1));
            }
            if (k.first + 0.25 == t) { LOGI("key %02x up", k.second); m.key(k.second, false); }
        }
        for (auto &tp : taps) {
            if (tp.t == t) m.touch(true, tp.x, tp.y);
            if (tp.t + 0.25 == t) m.touch(false, tp.x, tp.y);
        }
        bool dragging = false;
        for (auto &d : drags) {
            if (d.t == t) m.touch(true, d.x0, d.y0);
            if (d.t + 0.5 == t) m.touch(false, d.x1, d.y1);
            if (t >= d.t && t < d.t + 0.5) dragging = true;
        }
        for (auto &r : raws)
            if (r.first == t) {
                for (int c : r.second) m.kbd->push((uint8_t)c);
                LOGI("raw keys pushed (%zu)", r.second.size());
            }
        for (auto &pu : puts)
            if (pu.first == t) {
                size_t c = pu.second.find(':');
                FILE *pf = fopen(pu.second.substr(0, c).c_str(), "w");
                if (pf) { fputs(pu.second.substr(c + 1).c_str(), pf); fclose(pf); }
                LOGI("put %s", pu.second.c_str());
            }
        for (auto &rd : regDwords)
            if (rd.first == t) {
                // Hive value cell as seen in the RAM dumps: u16 type (4), u16 data size (4), u8 name
                // length, u8 flags, the UTF-16 name, then the data.
                size_t c = rd.second.find(':');
                std::string name = rd.second.substr(0, c);
                uint32_t val = (uint32_t)strtoul(rd.second.substr(c + 1).c_str(), nullptr, 0);
                std::vector<uint8_t> pat = {4, 0, 4, 0, (uint8_t)name.size()};
                int n = 0;
                for (size_t p = 0; p + 6 + name.size() * 2 + 4 <= m.ram.size(); p += 2) {
                    if (memcmp(&m.ram[p], pat.data(), pat.size())) continue;
                    bool ok = true;
                    for (size_t k = 0; k < name.size() && ok; k++)
                        ok = tolower(m.ram[p + 6 + k * 2]) == tolower((uint8_t)name[k]) && m.ram[p + 7 + k * 2] == 0;
                    if (!ok) continue;
                    uint32_t old;
                    memcpy(&old, &m.ram[p + 6 + name.size() * 2], 4);
                    memcpy(&m.ram[p + 6 + name.size() * 2], &val, 4);
                    LOGI("reg-dword %s at pa %08zx: %u -> %u", name.c_str(), p + 0x30000000, old, val);
                    n++;
                }
                LOGI("reg-dword %s: %d cells", name.c_str(), n);
            }
        for (auto &ac : accels)
            if (ac.first == t) {
                float ax = 0, ay = 0, az = 9.80665f;
                sscanf(ac.second.c_str(), "%f,%f,%f", &ax, &ay, &az);
                m.motion->setAccel(ax, ay, az);
                LOGI("accel: %.2f %.2f %.2f (guest reads so far %u)", ax, ay, az, m.motion->reads());
            }
        for (auto &b : batteries)
            if (b.first == t) {   // as NativeBridge does: byte 0 = 0 on AC, byte 1 = percent used
                int ac = 0, pct = 100;
                sscanf(b.second.c_str(), "%d,%d", &ac, &pct);
                m.dePage->battery[0] = ac ? 0 : 1;
                m.dePage->battery[1] = (uint8_t)(100 - std::min(100, std::max(0, pct)));
                LOGI("battery: %s, %d %%", ac ? "AC" : "on battery", pct);
            }
        for (auto &mn : modemNets)
            if (mn.first == t) {
                Modem::Network n;
                std::string spec = mn.second;
                size_t at = spec.find("op=");
                if (at != std::string::npos) { n.op = spec.substr(at + 3); spec = spec.substr(0, at); }
                for (size_t p = 0; p < spec.size();) {
                    size_t e = spec.find(' ', p);
                    std::string kv = spec.substr(p, e == std::string::npos ? std::string::npos : e - p);
                    p = e == std::string::npos ? spec.size() : e + 1;
                    size_t q = kv.find('=');
                    if (q == std::string::npos) continue;
                    std::string k = kv.substr(0, q), v = kv.substr(q + 1);
                    if (k == "svc") n.service = v == "1";
                    else if (k == "sig") n.signal = atoi(v.c_str());
                    else if (k == "roam") n.roaming = v == "1";
                    else if (k == "num") n.numeric = v;
                    else if (k == "data") n.data = v;
                    else if (k == "own") n.number = v;
                }
                m.modem->setNetwork(n);
                LOGI("modem: network %s", mn.second.c_str());
            }
        for (double r : modemRefresh)
            if (r == t) m.modem->refresh();
        for (auto &ms : modemSms)
            if (ms.first == t) { LOGI("modem: deliver sms"); m.modem->smsDeliver(ms.second); }
        for (auto &mr : modemRings)
            if (mr.first == t) { LOGI("modem: ring %s", mr.second.c_str()); m.modem->ring(mr.second); }
        for (auto &mc : modemCalls)
            if (mc.first == t) {
                size_t c = mc.second.find(':');
                m.modem->callState(atoi(mc.second.substr(0, c).c_str()), mc.second.substr(c + 1));
            }
        {
            std::string ev = m.modem->takeEvents();
            if (!ev.empty()) LOGI("modem events: %s", ev.c_str());
            for (size_t p = ev.find("smssend "); modemStub && p != std::string::npos; p = ev.find("smssend ", p + 1))
                m.modem->smsResult(atoi(ev.c_str() + p + 8), true);
        }
        for (double p : powers)
            if (p == t) { LOGI("power button (lcd %s)", m.lcd->enabled() ? "on" : "off"); m.gpio.powerButton(); }
        for (double p : resets)
            if (p == t) m.resetRequested = true;
        // Sleep/suspend tests.
        for (auto &sa : saveAts)
            if (sa.first == t) {
                std::string e;
                if (!m.saveState(sa.second, e)) LOGE("save failed: %s", e.c_str());
            }
        for (auto &p : pauses)
            if (p.first == t) {
                LOGI("pause: guest stopped for %.0f s", p.second);
                std::this_thread::sleep_for(std::chrono::milliseconds((int64_t)(p.second * 1000)));
                LOGI("pause: running again");
            }
        for (auto &w : warps)
            if (w.first == t) {
                // As if the host had stopped running the guest that long (Machine::nowNs, RTC).
                m.startHostNs -= (uint64_t)(w.second * 1e9);
                m.rtc.offsetSec += (int64_t)w.second;
                LOGI("warp: host clock %+.0f s", w.second);
            }
        if (shareDir) {
            for (double c : cardOuts)
                if (c == t) { LOGI("card out"); m.folderShare->setMounts({}); }
            for (double c : cardIns)
                if (c == t) {
                    LOGI("card in");
                    m.folderShare->setMounts({{"", shareDir}});
                    m.folderShare->setOverlays(overlays);
                }
        }
        if (boxDir) {
            for (auto &c : cmds)
                if (c.first == t) boxCommand(boxDir, c.second);
            for (auto &b : cmdBursts)
                if (b.first == t) {
                    static const char *kMix[] = {
                        "status svc=1 sig=4 roam=0 data=H wifi=1 wificonn=1 wifisig=3 op=Verizon",
                        "ping", "backlight 27",
                        "status svc=1 sig=4 roam=0 data=H wifi=1 wificonn=1 wifisig=4 op=Verizon",
                        "tz 240 0 EDT", "clip watch 1", "ping", "backlight 60", "rotate 270", "rotate 0",
                        "status svc=1 sig=3 roam=0 data=LTE wifi=0 wificonn=0 wifisig=0 op=Verizon",
                        "ping", "wallpaper", "ping", "theme", "hello", "ping", "clip watch 0", "dpi", "ping",
                    };
                    const int kinds = (int)(sizeof kMix / sizeof kMix[0]);
                    for (int k = 0; k < b.second; k++) boxCommand(boxDir, kMix[k % kinds]);
                    LOGI("box: %d commands queued", b.second);
                }
            if (s % 4 == 3) boxReport(boxDir, t + 0.25);
        }
        {   // Infrared: the app's part (InfraredBridge.kt): popup on "start", transport on the answer.
            std::string ev = m.irda->takeEvents();
            if (ev.find("start") != std::string::npos && irdaAsk && boxDir) {
                LOGI("irda: asking the helper for a transport");
                boxCommand(boxDir, "irda ask");
            }
            for (auto &c : irdaChoices)
                if (c.first == t && boxDir) boxCommand(boxDir, "irda pick " + c.second);
            if (irdaAsk && boxDir && s % 2 == 1)
                for (auto &n : boxList(std::string(boxDir) + "/out", ".msg")) {
                    if (!irdaSeenMsgs.insert(n).second) continue;
                    FILE *mf = fopen((std::string(boxDir) + "/out/" + n).c_str(), "r");
                    char buf[256] = "";
                    if (mf) { size_t k = fread(buf, 1, sizeof buf - 1, mf); buf[k] = 0; fclose(mf); }
                    if (strncmp(buf, "irda use ", 9) == 0) {
                        LOGI("irda: helper chose \"%s\"", buf + 9);
                        if (strncmp(buf + 9, "cancel", 6) != 0) irdaConnect();
                    }
                }
        }
        if (tlsProxy && (!tlsAnswer.empty() || (tlsAsk && boxDir))) {
            // Certificate warnings: the app's part (SecureProxy.kt). Polling also tells the proxy that
            // someone answers; without --tls-ask / --tls-answer it shows the error page at once.
            for (auto &p : tlsProxy->takePrompts()) {
                LOGI("tls ask #%u %s (%d s): %s", p.id, p.host.c_str(), p.seconds, p.text.c_str());
                if (!tlsAnswer.empty()) tlsProxy->answerPrompt(p.id, tlsAnswer == "yes");
                else boxCommand(boxDir, "tlsask " + std::to_string(p.id) + " " + std::to_string(p.seconds) + " " + p.host + "\n" + p.text);
            }
            if (tlsAsk && boxDir && s % 2 == 1)
                for (auto &n : boxList(std::string(boxDir) + "/out", ".msg")) {
                    if (!tlsSeenMsgs.insert(n).second) continue;
                    FILE *mf = fopen((std::string(boxDir) + "/out/" + n).c_str(), "r");
                    char buf[256] = "";
                    if (mf) { size_t k = fread(buf, 1, sizeof buf - 1, mf); buf[k] = 0; fclose(mf); }
                    unsigned id = 0;
                    char ans[16] = "";
                    if (sscanf(buf, "tlsask %u %15s", &id, ans) == 2) {
                        LOGI("tls: helper answered #%u %s", id, ans);
                        tlsProxy->answerPrompt(id, strcmp(ans, "yes") == 0);
                    }
                }
        }
        for (auto &sh : shots)
            if (sh.first == t) {
                LOGI("kbd queue %zu, spcon %x, srcpnd %x intpnd %x", m.kbd->queued(), m.kbd->control(), m.intc.srcpnd, m.intc.intpnd);
                dumpPpm(m, sh.second.c_str());
            }
        if (profileFrom >= 0 && s / 4.0 >= profileFrom) {
            // Sample the PC between short slices for a quarter second.
            for (int k = 0; k < 2500; k++) {
                m.runFor(100000);
                if ((m.cpu.getCpsr() & 0x1F) != 0x10) hist[m.cpu.pc() & ~3u]++;  // privileged only
            }
        } else if (net) {
            for (int k = 0; k < 16; k++) { pumpNet(); m.runFor(15625000); }
        } else if (dragging || (burstDt > 0 && t + 0.25 > burstT0 && t <= burstT1)) {
            // --burst: this quarter second in DT slices, dumping each new frame as <prefix><ms>.ppm.
            // --drag: the pen moves a step per slice.
            static std::vector<uint32_t> prev;
            int n = std::max(1, (int)(0.25 / (burstDt > 0 ? burstDt : 0.02) + 0.5));
            for (int k = 0; k < n; k++) {
                for (auto &d : drags) {
                    double f = (t + 0.25 * k / n - d.t) / 0.5;
                    if (f > 0 && f < 1) m.touch(true, d.x0 + (int)((d.x1 - d.x0) * f), d.y0 + (int)((d.y1 - d.y0) * f));
                }
                m.runFor(250000000ull / n);
                if (burstDt <= 0) continue;
                double tt = t + 0.25 * (k + 1) / n;
                std::vector<uint32_t> px;
                int w, h;
                if (tt < burstT0 || tt > burstT1 || !m.snapshotFramebuffer(px, w, h) || px == prev) continue;
                prev = px;
                char p[512];
                snprintf(p, sizeof p, "%s%06d.ppm", burstPrefix.c_str(), (int)(tt * 1000 + 0.5));
                dumpPpm(m, p);
            }
        } else {
            m.runFor(250000000);
        }
        uint64_t n = m.cpu.insnCount;
        LOGI("t=%.2fs pc=%08x cpsr=%08x insns=%llu (%.1f MIPS)%s", s / 4.0 + 0.25, m.cpu.pc(), m.cpu.getCpsr(),
             (unsigned long long)n, (n - lastInsns) / 250000.0, m.cpu.halted ? " idle" : "");
        {   // Notification LED / vibrator, polled the way the app's NativeBridge.ledState() does (nled.h).
            static uint32_t lastLed = 0;
            uint32_t led = m.dePage->nled.state();
            if (led != lastLed) LOGI("ledState %#x (light %d blink %d vibrate %d)", led, led & 1, (led >> 1) & 1, (led >> 2) & 1);
            lastLed = led;
        }
        static uint64_t lastIdle = 0;
        if (nmea && s % 4 == 3) {
            bool open = m.uart0.rxEnabled() && !(m.intc.intsubmsk & 1u) && !(m.intc.intmsk & (1u << IRQ_UART0));
            static bool wasOpen = false;
            if (open != wasOpen) { LOGI("gps: COM1 %s", open ? "opened" : "closed"); wasOpen = open; }
            if (open)
                for (char c : nmeaBurst(s / 4)) m.uart0.feed((uint8_t)c);
        }
        if (s % 4 == 3) {
            static uint64_t lastWakes = 0, lastFired = 0, lastPabt = 0, lastDabt = 0;
            LOGI("   idle %.0f%%  wakes %llu/s  ticks %llu/s  audio %llu frames, peak %d  aborts p%llu d%llu", (m.idleNs - lastIdle) / 1e7,
                 (unsigned long long)(m.idleWakes - lastWakes), (unsigned long long)(m.pwm.fired - lastFired),
                 (unsigned long long)audioFrames, audioPeak, (unsigned long long)(m.cpu.prefetchAborts - lastPabt),
                 (unsigned long long)(m.cpu.dataAborts - lastDabt));
            lastPabt = m.cpu.prefetchAborts;
            lastDabt = m.cpu.dataAborts;
            lastWakes = m.idleWakes;
            lastFired = m.pwm.fired;
            lastIdle = m.idleNs;
            audioFrames = 0;
            audioPeak = 0;
        }
        lastInsns = n;
        for (int i = 0; i < 3; i++)
            if (!line[i].empty()) { printf("[uart%d] %s\n", i, line[i].c_str()); line[i].clear(); }
    }
    if (irdaLink) irdaLink->stop();
    LOGI("irda: frames out %llu in %llu, bad %llu, answered from cache %llu, dropped %llu",
         (unsigned long long)m.irda->framesOut, (unsigned long long)m.irda->framesIn, (unsigned long long)m.irda->badFrames,
         (unsigned long long)m.irda->spoofed, (unsigned long long)m.irda->dropped);
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("done: %.2fs wall, %llu insns", el, (unsigned long long)m.cpu.insnCount);
    {   // Whole-run summary: guest busy time (wall minus idle) and the MIPS reached while busy.
        double idleS = (m.idleNs - runIdle0) / 1e9, busyS = el > idleS ? el - idleS : 0;
        uint64_t n = m.cpu.insnCount - runInsns0;
        LOGI("summary: %llu insns, busy %.2f s, %.0f active MIPS, %.0f idle wakes/s", (unsigned long long)n, busyS,
             busyS > 0 ? n / busyS / 1e6 : 0.0, (m.idleWakes - runWakes0) / el);
    }
    {
        uint64_t h = 1469598103934665603ull;
        for (int r = 0; r < 16; r++) h = (h ^ m.cpu.reg(r)) * 1099511628211ull;
        h = (h ^ m.cpu.getCpsr()) * 1099511628211ull;
        for (size_t i = 0; i < m.ram.size(); i += 8) { uint64_t v; memcpy(&v, &m.ram[i], 8); h = (h ^ v) * 1099511628211ull; }
        LOGI("state hash %016llx at insn %llu pc=%08x", (unsigned long long)h, (unsigned long long)m.cpu.insnCount, m.cpu.pc());
    }
    if (m.cpu.jit) {
        if (jitMapPath) jitDumpMap(m.cpu.jit, jitMapPath);
        jitDumpInterp(m.cpu.jit);
        JitStats js = jitStats(m.cpu.jit);
        LOGI("jit: blocks %llu run %llu interpExits %llu helpers %llu invalidations %llu flushes %llu code %llu KB",
             (unsigned long long)js.blocksCompiled, (unsigned long long)js.blocksRun, (unsigned long long)js.interpExits,
             (unsigned long long)js.helperCalls, (unsigned long long)js.invalidations, (unsigned long long)js.flushes,
             (unsigned long long)(js.codeBytes >> 10));
    }
    LOGI("motion: %u guest reads", m.motion->reads());
    LOGI("codepatch: status %x, %llu page scans, battery reads %llu", m.codePatch->status(),
         (unsigned long long)m.codePatch->pagesScanned, (unsigned long long)m.dePage->batteryReads);
    if (saveStatePath) {
        auto t = std::chrono::steady_clock::now();
        if (!m.saveState(saveStatePath, err)) LOGE("save failed: %s", err.c_str());
        else LOGI("saved in %.2f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
    }
    if (dump) dumpPpm(m, dump);
    if (ramDump) {
        FILE *f = fopen(ramDump, "wb");
        if (f) { fwrite(m.ram.data(), 1, m.ram.size(), f); fclose(f); LOGI("RAM written to %s", ramDump); }
    }
    if (!hist.empty()) {
        std::vector<std::pair<uint32_t, uint32_t>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        uint64_t total = 0;
        for (auto &e : v) total += e.second;
        for (size_t i = 0; i < v.size() && i < 25; i++) {
            uint32_t insn = 0;
            m.cpu.debugRead32(v[i].first, insn);
            printf("%5.1f%%  %08x  %08x  %s\n", 100.0 * v[i].second / total, v[i].first, insn,
                   disasmArm(v[i].first, insn).c_str());
        }
    }
    if (g_traceFile && g_traceFile != stdout) fclose(g_traceFile);
    return 0;
}
