// Virtual GSM modem on UART0 (guest COM1) for the WM 6 / 6.1 / 6.1.4 images, whose radio driver
// is Microsoft's sample GSM RIL (rilgsm.dll, Ericsson flavour). The guest helper points the RIL
// at COM1: (Software\Microsoft\RIL ComPort) instead of the CAIF mux, so the RIL speaks plain AT
// here. The modem answers the RIL's AT dialect (docs/research-cellular.md section 3) with the
// Android phone's real network state, and hands voice calls dialled in Windows Mobile to the app,
// which places the real call and reports its progress back.
//
// Threads: fromGuest() and service() run on the emulation thread (the UART); the host-side calls
// (setNetwork, callState, takeEvents) come from the app's threads and only queue work under the
// mutex. service() applies it and sends the unsolicited result codes.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace wm {

class StateIO;

class Modem {
public:
    /** Bytes for the guest (the UART's receive side). Emulation thread only. */
    std::function<void(const std::string &)> toGuest;

    // ---- Emulation thread ----
    void fromGuest(uint8_t c);
    /** Applies host updates and sends unsolicited codes; called between slices. */
    void service(uint64_t nowNs);
    /** The guest restarted: its RIL will initialise the modem again. Calls are dropped. */
    void reset();
    void serialize(StateIO &s);

    // ---- Host (any thread) ----
    struct Network {
        bool service = false;      // registered
        int signal = 0;            // 0..4 bars
        bool roaming = false;
        std::string op;            // operator name
        std::string numeric;       // MCC+MNC, e.g. "310260"
        std::string data;          // WM data icon (H, 3G, E, G, EV, 1X) or "" without data
        std::string number;        // the phone's own number, if known
    };
    void setNetwork(const Network &n);
    /** The real call's progress: pending, dialing, active, held, ended. */
    void callState(int id, const std::string &state);
    /** Events for the app, one per line: "dial <id> <number>", "end <id>", "hold <id> <0|1>", "mute <0|1>". */
    std::string takeEvents();
    /** Announce the registration again (after the guest learned a new operator name). */
    void refresh();
    /** The guest's RIL has talked to the modem since the last restart (COM1 is the radio). */
    bool claimed() const { return claimedFlag; }

    bool trace = false;       // log every command and response (CLI --trace-modem)
    // CLI stand-in for the app: a dialled call is answered after 3 s (never a real call).
    bool stubCalls = false;

private:
    // Settings the RIL changes (saved with snapshots: a restored RIL does not initialise again).
    struct Settings {
        bool echo = true, verbose = true, quiet = false;
        int cmee = 0, creg = 0, cgreg = 0, ereg = 0, cmerInd = 0, copsFormat = 0, fun = 1;
        bool crc = false, clip = false, ecam = false, mute = false;
    } st;
    struct Call {
        int id = 0;
        int stat = 2;              // +CLCC: 0 active, 1 held, 2 dialing, 3 alerting
        std::string number;
        uint64_t since = 0;
        uint64_t deadline = 0;     // end the call if the app has not taken it by then (0: none)
    };
    std::vector<Call> calls;

    // Emulation thread state
    std::string line;
    bool inPdu = false;           // +CMGS: collecting the PDU up to Ctrl-Z
    int dialing = 0;              // call whose ATD has no result yet (it comes when answered or failed)
    std::atomic<bool> claimedFlag{false};
    uint64_t now = 0;
    // What the guest was last told (unsolicited codes go out on change).
    int sentSignal = -1, sentReg = -1, sentGprs = -1;
    std::string sentOp;

    // Host side, under mu
    std::mutex mu;
    Network net, applied;
    bool netDirty = false, refreshWanted = false;
    struct HostCall { int id; std::string state; };
    std::vector<HostCall> hostCalls;
    std::vector<std::string> events;

    std::vector<std::string> *afterFinal = nullptr;  // while a command line runs: codes sent after its result

    void execute(const std::string &cmdLine);
    // One command; appends information lines to `out`. Returns the final result: 0 OK, 3 NO
    // CARRIER, 4 ERROR, or -n for "+CME ERROR: n", -1000-n for "+CMS ERROR: n".
    int command(const std::string &cmd, std::vector<std::string> &out, bool &stop);
    int dial(const std::string &number, std::vector<std::string> &out);
    void hangup(int id, bool fromGuest);
    void endCall(size_t i, bool fromGuest);
    void setStat(Call &c, int stat);
    void final(int code, const std::vector<std::string> &info);
    void unsolicited(const std::string &text);
    void event(const std::string &e);
    int regStat() const;
    int signalValue() const;
    std::string regLine(const char *prefix, int mode, int stat) const;
    std::string copsName(int format) const;
    std::string simFile(int command, int file, int length) const;
    std::string ecavLine(const Call &c, int ccstatus, int exitCause) const;
    void log(const char *dir, const std::string &text) const;
};

}  // namespace wm
