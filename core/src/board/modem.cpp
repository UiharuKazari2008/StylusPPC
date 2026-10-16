// Virtual GSM modem for rilgsm.dll (modem.h). The AT dialect follows docs/research-cellular.md
// section 3: numeric result codes after ATV0, signal only as +CIEV indicator 2, registration as
// +CREG / *EREG, call progress as *ECAV (after AT*ECAM=1) and the +CLCC call list.
#include "modem.h"
#include "state.h"
#include "../util/log.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wm {

namespace {

const uint64_t kSecond = 1000000000ull;
const uint64_t kDialTimeout = 10 * kSecond;      // the app has not taken a dialled call: end it
const uint64_t kPendingTimeout = 60 * kSecond;   // the app is asking for a permission
const int kPending = 100;                        // command(): the result comes later (ATD)

std::string upper(std::string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// Splits "a,b,"c,d"" at commas outside quotes; quotes are kept.
std::vector<std::string> splitArgs(const std::string &s) {
    std::vector<std::string> out(1);
    bool q = false;
    for (char c : s) {
        if (c == '"') q = !q;
        if (c == ',' && !q) out.emplace_back();
        else out.back() += c;
    }
    return out;
}

int argInt(const std::vector<std::string> &a, size_t i, int def) {
    if (i >= a.size() || a[i].empty()) return def;
    return atoi(a[i].c_str());
}

std::string visible(const std::string &s) {
    std::string v;
    for (unsigned char c : s) {
        if (c == '\r') v += "<cr>";
        else if (c == '\n') v += "<lf>";
        else if (c < 0x20 || c >= 0x7F) { char b[8]; snprintf(b, sizeof b, "<%02x>", c); v += b; }
        else v += (char)c;
    }
    return v;
}

const char *kWords[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "", "NO DIALTONE", "BUSY", "NO ANSWER"};

// Fixed cell identity: the RIL only shows it in diagnostics.
const char *kLac = "1A2B", *kCi = "00003C4D";

}  // namespace

void Modem::log(const char *dir, const std::string &text) const {
    if (trace) LOGI("modem %s %s", dir, visible(text).c_str());
}

// ---------------------------------------------------------------------------------------------
// Guest side

void Modem::fromGuest(uint8_t c) {
    if (inPdu) {
        // AT+CMGS: hex PDU, ended by Ctrl-Z (send) or Esc (cancel).
        if (c == 0x1A) {
            inPdu = false;
            log("<-", "<sms pdu " + std::to_string(line.size()) + " chars>");
            line.clear();
            // Sending SMS from Windows Mobile is not bridged (yet): report a failure, never a fake success.
            final(-1000 - 500, {});
        } else if (c == 0x1B) {
            inPdu = false;
            line.clear();
            final(0, {});
        } else if (line.size() < 2048) {
            line += (char)c;
        }
        return;
    }
    if (dialing && c != '\r' && c != '\n') {
        // V.250: any character while a call is being set up aborts it (the RIL hanging up before the
        // other side answered). The character still starts the next command line.
        for (size_t i = 0; i < calls.size(); i++)
            if (calls[i].id == dialing) { endCall(i, true); break; }
    }
    // No echo before a command has come: other users of COM1 (a GPS receiver's driver) must not
    // read their own bytes back.
    if (st.echo && claimedFlag && toGuest) toGuest(std::string(1, (char)c));
    if (c == '\r') {
        std::string l;
        l.swap(line);
        execute(l);
    } else if (c == 8) {
        if (!line.empty()) line.pop_back();
    } else if (c != '\n' && c != 0 && line.size() < 1024) {
        line += (char)c;
    }
}

void Modem::execute(const std::string &raw) {
    size_t p = raw.find_first_not_of(" \t");
    if (p == std::string::npos || raw.size() < p + 2 || toupper((unsigned char)raw[p]) != 'A' ||
        toupper((unsigned char)raw[p + 1]) != 'T')
        return;  // not a command (line noise)
    if (!claimedFlag) LOGI("modem: the guest's radio driver opened COM1 (virtual GSM modem)");
    claimedFlag = true;
    std::string body = raw.substr(p + 2);
    log("<-", "AT" + body);
    std::vector<std::string> info;
    std::vector<std::string> after;  // unsolicited codes that must follow the final result
    afterFinal = &after;
    int result = 0;
    bool stop = false;
    size_t i = 0;
    while (i < body.size() && result == 0 && !stop) {
        char c = body[i];
        if (c == ' ' || c == ';') { i++; continue; }
        std::string cmd;
        if (c == '+' || c == '*' || c == '$' || c == '%' || c == '^') {
            // Extended command: up to the next ';' outside quotes.
            bool q = false;
            size_t j = i;
            for (; j < body.size(); j++) {
                if (body[j] == '"') q = !q;
                if (body[j] == ';' && !q) break;
            }
            cmd = body.substr(i, j - i);
            i = j;
        } else if (toupper((unsigned char)c) == 'D') {
            cmd = body.substr(i);  // dial: the rest of the line, ';' included
            i = body.size();
        } else if (c == '&') {
            size_t j = i + 2;
            while (j < body.size() && isdigit((unsigned char)body[j])) j++;
            cmd = body.substr(i, j - i);
            i = j;
        } else if (toupper((unsigned char)c) == 'S' && i + 1 < body.size() && isdigit((unsigned char)body[i + 1])) {
            size_t j = i + 1;
            while (j < body.size() && isdigit((unsigned char)body[j])) j++;
            if (j < body.size() && body[j] == '?') j++;
            else if (j < body.size() && body[j] == '=') {
                j++;
                while (j < body.size() && isdigit((unsigned char)body[j])) j++;
            }
            cmd = body.substr(i, j - i);
            i = j;
        } else if (isalpha((unsigned char)c)) {
            size_t j = i + 1;
            while (j < body.size() && isdigit((unsigned char)body[j])) j++;
            cmd = body.substr(i, j - i);
            i = j;
        } else {
            result = 4;
            break;
        }
        result = command(cmd, info, stop);
    }
    afterFinal = nullptr;
    if (!stop && result != kPending) final(result, info);
    for (auto &u : after) unsolicited(u);
}

int Modem::command(const std::string &cmd, std::vector<std::string> &out, bool &stop) {
    char first = (char)toupper((unsigned char)cmd[0]);
    // ---- Basic commands ----
    if (first != '+' && first != '*' && first != '$' && first != '%' && first != '^') {
        if (first == 'D') return dial(cmd.substr(1), out);
        int n = cmd.size() > 1 ? atoi(cmd.c_str() + 1) : 0;
        switch (first) {
            case 'E': st.echo = n != 0; return 0;
            case 'V': st.verbose = n != 0; return 0;
            case 'Q': st.quiet = n != 0; return 0;
            case 'Z': st = Settings(); return 0;
            case 'H': hangup(0, true); return 0;
            case 'A': return 3;  // nothing is ringing (incoming calls are not bridged)
            case 'O': return 3;
            case 'I': out.push_back("PocketStylus virtual GSM modem"); return 0;
            default: return 0;  // X, S, &C, &D, &F, &W, L, M, ...: accepted
        }
    }
    // ---- Extended commands: NAME, NAME?, NAME=?, NAME=args ----
    size_t eq = cmd.find('=');
    size_t qm = cmd.find('?');
    size_t nameEnd = std::min(eq, qm);
    std::string name = upper(cmd.substr(0, nameEnd));
    bool test = eq != std::string::npos && cmd.compare(eq, 2, "=?") == 0;
    bool read = !test && qm != std::string::npos && (eq == std::string::npos || qm < eq);
    bool set = !test && eq != std::string::npos;
    std::vector<std::string> a = set ? splitArgs(cmd.substr(eq + 1)) : std::vector<std::string>();
    int reg = regStat();
    bool registered = reg == 1 || reg == 5;

    if (name == "+CGMI") { out.push_back("PocketStylus"); return 0; }
    if (name == "+CGMM") { out.push_back("Virtual GSM modem"); return 0; }
    if (name == "+CGMR") { out.push_back("1.0"); return 0; }
    if (name == "+CGSN") { out.push_back("357000000000001"); return 0; }
    if (name == "+CIMI") {
        std::string mccmnc = applied.numeric.size() >= 5 ? applied.numeric : "00101";
        std::string imsi = mccmnc + "0000000001";
        out.push_back(imsi.substr(0, 15));
        return 0;
    }
    if (name == "+CNUM") {
        if (!applied.number.empty())
            out.push_back("+CNUM: \"\",\"" + applied.number + "\"," + (applied.number[0] == '+' ? "145" : "129"));
        return 0;
    }
    if (name == "+CPIN") {
        if (read) out.push_back("+CPIN: READY");
        return 0;
    }
    if (name == "+CFUN") {
        if (test) { out.push_back("+CFUN: (0,1,4),(0)"); return 0; }
        if (read) { out.push_back("+CFUN: " + std::to_string(st.fun)); return 0; }
        if (set) {
            st.fun = argInt(a, 0, 1);
            if (st.fun != 1) hangup(0, true);  // flight mode ends calls
        }
        return 0;
    }
    if (name == "+CMEE") { if (set) st.cmee = argInt(a, 0, 0); if (read) out.push_back("+CMEE: " + std::to_string(st.cmee)); return 0; }
    if (name == "+CRC") { if (set) st.crc = argInt(a, 0, 0) != 0; return 0; }
    if (name == "*ECAM") {
        if (set) st.ecam = argInt(a, 0, 0) != 0;
        if (read) out.push_back(std::string("*ECAM: ") + (st.ecam ? "1" : "0"));
        if (test) out.push_back("*ECAM: (0,1)");
        return 0;
    }
    if (name == "+CMER") {
        if (set) st.cmerInd = argInt(a, 3, 0);
        if (read) out.push_back("+CMER: 3,0,0," + std::to_string(st.cmerInd));
        return 0;
    }
    if (name == "+CIND") {
        if (test) {
            out.push_back("+CIND: (\"battchg\",(0-5)),(\"signal\",(0-5)),(\"batterywarning\",(0-1)),"
                          "(\"chargerconnected\",(0-1)),(\"service\",(0-1)),(\"sounder\",(0-1)),(\"message\",(0-1)),"
                          "(\"call\",(0-1)),(\"roam\",(0-1)),(\"smsfull\",(0-1)),(\"callsetup\",(0-3)),(\"callheld\",(0-1))");
        } else if (read) {
            bool inCall = !calls.empty();
            out.push_back("+CIND: 5," + std::to_string(signalValue()) + ",0,0," + (registered ? "1" : "0") + ",0,0," +
                          (inCall ? "1" : "0") + "," + (reg == 5 ? "1" : "0") + ",0,0,0");
        }
        return 0;
    }
    if (name == "+CSQ") {
        if (test) { out.push_back("+CSQ: (0-31,99),(0-7,99)"); return 0; }
        static const int rssi[] = {99, 2, 10, 18, 26, 31};
        out.push_back("+CSQ: " + std::to_string(rssi[signalValue()]) + ",99");
        return 0;
    }
    if (name == "+CREG" || name == "+CGREG" || name == "*EREG") {
        int &mode = name == "+CREG" ? st.creg : name == "+CGREG" ? st.cgreg : st.ereg;
        if (test) { out.push_back(name + ": (0-2)"); return 0; }
        if (set) mode = argInt(a, 0, 0);
        if (read) {
            std::string l = name + ": " + std::to_string(mode) + "," + std::to_string(reg);
            if (mode == 2 && registered) l += std::string(",\"") + kLac + "\",\"" + kCi + "\"";
            out.push_back(l);
        }
        return 0;
    }
    if (name == "+COPS") {
        if (test) {
            if (registered) {
                std::string op = applied.op.empty() ? applied.numeric : applied.op;
                out.push_back("+COPS: (2,\"" + op + "\",\"" + op.substr(0, 16) + "\",\"" + applied.numeric +
                              "\"),,(0-4),(0-2)");
            } else {
                out.push_back("+COPS: ,,(0-4),(0-2)");
            }
            return 0;
        }
        if (read) {
            if (registered) out.push_back("+COPS: 0," + std::to_string(st.copsFormat) + ",\"" + copsName(st.copsFormat) + "\"");
            else out.push_back("+COPS: 0");
            return 0;
        }
        if (set) {
            int mode = argInt(a, 0, 0);
            if (mode == 3) st.copsFormat = argInt(a, 1, 0);
            else if (mode == 2) st.fun = st.fun;  // deregister: ignored, the phone's network is the network
        }
        return 0;
    }
    if (name == "+CLCC") {
        for (const Call &c : calls) {
            bool intl = !c.number.empty() && c.number[0] == '+';
            // rilgsm drops an entry whose number is not followed by ",<alpha>,<priority>"; the call list
            // then looks empty and WM's phone stack ends the call as unreachable.
            out.push_back("+CLCC: " + std::to_string(c.id) + ",0," + std::to_string(c.stat) + ",0,0,\"" + c.number +
                          "\"," + (intl ? "145" : "129") + ",\"\",0");
        }
        return 0;
    }
    if (name == "+CPAS") { out.push_back(calls.empty() ? "+CPAS: 0" : "+CPAS: 4"); return 0; }
    if (name == "*EVD") {
        std::string num = set && !a.empty() ? a[0] : "";
        num.erase(std::remove(num.begin(), num.end(), '"'), num.end());
        return dial(num + ";", out);
    }
    if (name == "+CHUP" || name == "*EVH") { hangup(0, true); return 0; }
    if (name == "*EVA") return 3;
    if (name == "+CHLD") {
        if (test) { out.push_back("+CHLD: (0,1,1x,2,2x,3,4)"); return 0; }
        if (!set || a.empty() || a[0].empty()) return st.cmee ? -3 : 4;
        int n = a[0][0] - '0';
        int x = a[0].size() > 1 ? atoi(a[0].c_str() + 1) : 0;
        if (n == 0) {
            for (size_t i = calls.size(); i-- > 0;) if (calls[i].stat == 1) endCall(i, true);
        } else if (n == 1) {
            if (x) hangup(x, true);
            else {
                for (size_t i = calls.size(); i-- > 0;) if (calls[i].stat != 1) endCall(i, true);
                for (Call &c : calls) if (c.stat == 1) { setStat(c, 0); event("hold " + std::to_string(c.id) + " 0"); }
            }
        } else if (n == 2) {
            for (Call &c : calls) {
                if (x && c.id != x && c.stat == 0) continue;
                if (c.stat == 0) { setStat(c, 1); event("hold " + std::to_string(c.id) + " 1"); }
                else if (c.stat == 1) { setStat(c, 0); event("hold " + std::to_string(c.id) + " 0"); }
            }
        } else if (n == 4) {
            return st.cmee ? -3 : 4;
        }
        return 0;
    }
    if (name == "+CMUT") {
        if (test) { out.push_back("+CMUT: (0,1)"); return 0; }
        if (read) { out.push_back(std::string("+CMUT: ") + (st.mute ? "1" : "0")); return 0; }
        if (set) {
            bool m = argInt(a, 0, 0) != 0;
            if (m != st.mute && !calls.empty()) event(std::string("mute ") + (m ? "1" : "0"));
            st.mute = m;
        }
        return 0;
    }
    if (name == "+CLIP") {
        if (set) st.clip = argInt(a, 0, 0) != 0;
        if (read) out.push_back(std::string("+CLIP: ") + (st.clip ? "1" : "0") + ",1");
        return 0;
    }
    if (name == "+CLIR") { if (read) out.push_back("+CLIR: 0,2"); return 0; }
    if (name == "+COLP") { if (read) out.push_back("+COLP: 0,2"); return 0; }
    if (name == "+CCWA") {
        if (set && argInt(a, 1, 0) == 2) out.push_back("+CCWA: 0,1");
        return 0;
    }
    if (name == "+CCFC") {
        if (test) { out.push_back("+CCFC: (0-5)"); return 0; }
        if (set && argInt(a, 1, 0) == 2) out.push_back("+CCFC: 0,7");
        return 0;
    }
    if (name == "+CLCK") {
        if (test) { out.push_back("+CLCK: (\"SC\",\"AO\",\"OI\",\"OX\",\"AI\",\"IR\",\"AB\",\"AG\",\"AC\",\"FD\")"); return 0; }
        if (set && argInt(a, 1, 0) == 2) out.push_back("+CLCK: 0");
        return 0;
    }
    if (name == "+CPWD") {
        if (test) out.push_back("+CPWD: (\"SC\",8),(\"P2\",8),(\"AO\",4),(\"OI\",4),(\"OX\",4),(\"AI\",4),(\"IR\",4),(\"AB\",4),(\"AG\",4),(\"AC\",4)");
        return 0;
    }
    if (name == "+CBST") { if (test) out.push_back("+CBST: (0-7,12,14,65,66,68,70,71,75),(0),(0-3)"); return 0; }
    if (name == "+CRLP") { if (test) out.push_back("+CRLP: (0-61),(0-61),(39-255),(1-255)"); return 0; }
    if (name == "+DS") { if (test) out.push_back("+DS: (0-3),(0,1),(512-4096),(6-250)"); return 0; }
    if (name == "+CPBS") {
        if (test) out.push_back("+CPBS: (\"SM\")");
        if (read) out.push_back("+CPBS: \"SM\",0,250");
        return 0;
    }
    if (name == "+CPBR") { if (test) out.push_back("+CPBR: (1-250),40,18"); return 0; }
    if (name == "+CPBF") { if (test) out.push_back("+CPBF: 40,18"); return 0; }
    if (name == "+CPBW") return st.cmee ? -3 : 4;
    if (name == "+CSMS") {
        if (test) out.push_back("+CSMS: (0)");
        else if (read) out.push_back("+CSMS: 0,1,1,1");
        else if (set) out.push_back("+CSMS: 1,1,1");
        return 0;
    }
    if (name == "+CPMS") {
        if (test) out.push_back("+CPMS: (\"SM\"),(\"SM\"),(\"SM\")");
        else if (read) out.push_back("+CPMS: \"SM\",0,20,\"SM\",0,20,\"SM\",0,20");
        else if (set) out.push_back("+CPMS: 0,20,0,20,0,20");
        return 0;
    }
    if (name == "+CMGL") return 0;  // no stored messages
    if (name == "+CMGR" || name == "+CMGW" || name == "+CMSS") return -1000 - 321;  // invalid memory index
    if (name == "+CMGD") return 0;
    if (name == "+CMGS") {
        if (test) return 0;
        // The PDU follows after the prompt; the result comes at Ctrl-Z (fromGuest).
        inPdu = true;
        line.clear();
        if (toGuest) toGuest("\r\n> ");
        log("->", "\r\n> ");
        stop = true;
        return 0;
    }
    if (name == "+CSCA") { if (read) out.push_back("+CSCA: \"\",129"); return 0; }
    if (name == "+CRSM") { out.push_back(simFile(argInt(a, 0, 0), argInt(a, 1, 0), argInt(a, 4, 0))); return 0; }
    if (name == "+CSIM") return st.cmee ? -4 : 4;
    if (name == "+CGATT") {
        if (read) out.push_back(std::string("+CGATT: ") + (registered ? "1" : "0"));
        if (test) out.push_back("+CGATT: (0,1)");
        return 0;
    }
    if (name == "+CGDCONT") { if (test) out.push_back("+CGDCONT: (1-3),\"IP\",,,(0-1),(0-1)"); return 0; }
    if (name == "+CGSMS") { if (test) out.push_back("+CGSMS: (0-3)"); return 0; }
    if (name == "+CPOL") { if (test) out.push_back("+CPOL: (1-10),(0-2)"); return 0; }
    if (name == "+CGDATA" || name == "*EIAD" || name == "+CGACT") {
        // Packet data goes over the emulated network card, not the modem.
        return set ? (st.cmee ? -30 : 4) : 0;
    }
    if (name == "*ELIN") { if (read) out.push_back("*ELIN: 1"); return 0; }
    if (name == "+CBC") { out.push_back("+CBC: 0,100"); return 0; }
    // Everything else (Ericsson *E..., *STK..., +CR, +CSSN, +CGEREP, +CNMI, +CUSD, +VTS, ...): accepted.
    return 0;
}

// AT+CRSM (restricted SIM access, 27.007 8.18): the RIL reads SIM files for names and numbers. The
// only files with content are CPHS "operator name" (6F14 long, 6F18 short), which is how the RIL
// names the network when the numeric PLMN is not in its own table: the phone's operator name.
// Everything else is "file not found" (SW 94 04).
std::string Modem::simFile(int command, int file, int length) const {
    const std::string notFound = "+CRSM: 148,4";
    if (file != 0x6F14 && file != 0x6F18) return notFound;
    int reg = regStat();
    if ((reg != 1 && reg != 5) || applied.op.empty()) return notFound;
    size_t size = file == 0x6F14 ? 20 : 10;
    static const char hex[] = "0123456789ABCDEF";
    auto toHex = [](const std::vector<uint8_t> &b) {
        std::string s;
        for (uint8_t v : b) { s += hex[v >> 4]; s += hex[v & 15]; }
        return s;
    };
    if (command == 192) {  // GET RESPONSE: transparent EF of `size` bytes, readable always
        std::vector<uint8_t> r = {0, 0, 0, (uint8_t)size, (uint8_t)(file >> 8), (uint8_t)file, 0x04, 0, 0x0F, 0xFF, 0xFF,
                                  0x01, 0x02, 0x00, 0x00};
        return "+CRSM: 144,0,\"" + toHex(r) + "\"";
    }
    if (command == 176) {  // READ BINARY: the name in the GSM default alphabet, padded with FF
        std::vector<uint8_t> b(size, 0xFF);
        for (size_t i = 0; i < applied.op.size() && i < size; i++) {
            char c = applied.op[i];
            uint8_t g = c == '@' ? 0x00 : c == '$' ? 0x02 : c == '_' ? 0x11 : (c >= 0x20 && c < 0x7F) ? (uint8_t)c : '?';
            if (c == '[' || c == ']' || c == '{' || c == '}' || c == '\\' || c == '^' || c == '~' || c == '|' || c == '`') g = '?';
            b[i] = g;
        }
        if (length > 0 && (size_t)length < b.size()) b.resize((size_t)length);
        return "+CRSM: 144,0,\"" + toHex(b) + "\"";
    }
    return notFound;
}

// ATD<number>; (voice). rilgsm treats the final result of the dial command as the call's outcome:
// OK means the other side answered (it sends RIL_NOTIFY_CONNECT right away), NO CARRIER / BUSY that
// the call failed. So the command stays open while the app places the real call; *ECAV reports the
// progress meanwhile (1 calling, 3 active, 0 ended), and service() sends the result.
int Modem::dial(const std::string &arg, std::vector<std::string> &out) {
    std::string number;
    bool voice = false;
    for (char c : arg) {
        if (c == ';') { voice = true; break; }
        if (isdigit((unsigned char)c) || c == '+' || c == '*' || c == '#' || c == 'p' || c == 'P' || c == 'w' || c == 'W')
            number += c;
    }
    if (!voice) return 3;  // data calls (CSD / dial-up) are not bridged
    if (number.empty()) return st.cmee ? -21 : 4;
    int reg = regStat();
    if (st.fun != 1 || (reg != 1 && reg != 5)) return st.cmee ? -30 : 3;  // no network service
    int id = 1;
    for (bool used = true; used && id < 8;) {
        used = false;
        for (const Call &c : calls) if (c.id == id) { used = true; id++; break; }
    }
    if (id >= 8) return st.cmee ? -3 : 4;
    Call c;
    c.id = id;
    c.stat = 2;
    c.number = number;
    c.since = now;
    c.deadline = stubCalls ? 0 : now + kDialTimeout;
    calls.push_back(c);
    dialing = id;
    LOGI("modem: Windows Mobile dials %s (call %d)", number.c_str(), id);
    event("dial " + std::to_string(id) + " " + number);
    if (st.ecam) unsolicited(ecavLine(c, 1, 0));  // calling
    return kPending;
}

// *ECAV: <ccid>,<ccstatus>,<calltype>[,<processid>,<exitcause>]. rilgsm turns ccstatus 1/2 into
// "new outgoing", 3 into "connected" and 0 into "disconnected" (exit cause 17 = busy, else normal).
std::string Modem::ecavLine(const Call &c, int ccstatus, int exitCause) const {
    std::string u = "*ECAV: " + std::to_string(c.id) + "," + std::to_string(ccstatus) + ",1";
    if (ccstatus == 0) u += ",8," + std::to_string(exitCause);
    return u;
}

void Modem::hangup(int id, bool fromGuest) {
    for (size_t i = calls.size(); i-- > 0;)
        if (id == 0 || calls[i].id == id) endCall(i, fromGuest);
}

void Modem::endCall(size_t i, bool fromGuest) {
    Call c = calls[i];
    calls.erase(calls.begin() + (long)i);
    bool wasDialing = dialing == c.id;
    if (wasDialing) dialing = 0;
    LOGI("modem: call %d ended (%s)", c.id, fromGuest ? "Windows Mobile" : "phone");
    if (fromGuest) event("end " + std::to_string(c.id));
    std::string ecav = ecavLine(c, 0, 16);
    if (st.ecam) {
        if (afterFinal) afterFinal->push_back(ecav);
        else unsolicited(ecav);
    }
    // The open dial command ends with NO CARRIER; a connected call's end is reported unsolicited
    // (unless WM itself hung up, which its ATH / AT+CHUP answers).
    if (wasDialing) final(3, {});
    else if (!fromGuest) unsolicited(st.verbose ? "NO CARRIER" : "3");
}

void Modem::setStat(Call &c, int stat) {
    if (c.stat == stat) return;
    c.stat = stat;
    // +CLCC stat -> *ECAV ccstatus: active 3, held 4, dialing 1 (calling), alerting 2 (connecting).
    static const int cc[] = {3, 4, 1, 2};
    bool answer = stat == 0 && dialing == c.id;
    if (st.ecam) {
        std::string u = ecavLine(c, cc[stat & 3], 0);
        if (afterFinal) afterFinal->push_back(u);
        else unsolicited(u);
    }
    if (answer) {  // answered: the dial command succeeds
        dialing = 0;
        final(0, {});
    }
}

// ---------------------------------------------------------------------------------------------
// Output

void Modem::final(int code, const std::vector<std::string> &info) {
    std::string s;
    for (const auto &l : info) s += st.verbose ? "\r\n" + l + "\r\n" : l + "\r\n";
    if (code <= -1000) {
        std::string e = "+CMS ERROR: " + std::to_string(-1000 - code);
        s += st.verbose ? "\r\n" + e + "\r\n" : e + "\r\n";
    } else if (code < 0) {
        std::string e = "+CME ERROR: " + std::to_string(-code);
        s += st.verbose ? "\r\n" + e + "\r\n" : e + "\r\n";
    } else if (!st.quiet) {
        s += st.verbose ? std::string("\r\n") + kWords[code] + "\r\n" : std::to_string(code) + "\r";
    }
    log("->", s);
    if (toGuest) toGuest(s);
}

void Modem::unsolicited(const std::string &text) {
    // Numeric mode sends result codes as digits (NO CARRIER = "3<cr>"); everything else is a line.
    bool numeric = !text.empty() && isdigit((unsigned char)text[0]) && text.size() == 1;
    std::string s = numeric ? text + "\r" : st.verbose ? "\r\n" + text + "\r\n" : text + "\r\n";
    log("->", s);
    if (toGuest) toGuest(s);
}

void Modem::event(const std::string &e) {
    std::lock_guard<std::mutex> lk(mu);
    if (events.size() < 64) events.push_back(e);
}

int Modem::regStat() const {
    if (st.fun != 1 || !applied.service) return 0;
    return applied.roaming ? 5 : 1;
}

int Modem::signalValue() const {
    if (regStat() == 0) return 0;
    int s = applied.signal < 0 ? 0 : applied.signal > 4 ? 4 : applied.signal;
    return s + 1;
}

std::string Modem::regLine(const char *prefix, int mode, int stat) const {
    std::string l = std::string(prefix) + ": " + std::to_string(stat);
    if (mode == 2 && (stat == 1 || stat == 5)) l += std::string(",\"") + kLac + "\",\"" + kCi + "\"";
    return l;
}

std::string Modem::copsName(int format) const {
    std::string numeric = applied.numeric.empty() ? "00101" : applied.numeric;
    if (format == 2 || applied.op.empty()) return numeric;
    if (format == 1) return applied.op.substr(0, 16);
    return applied.op;
}

// ---------------------------------------------------------------------------------------------
// Host side

void Modem::setNetwork(const Network &n) {
    std::lock_guard<std::mutex> lk(mu);
    net = n;
    netDirty = true;
}

void Modem::callState(int id, const std::string &state) {
    std::lock_guard<std::mutex> lk(mu);
    hostCalls.push_back({id, state});
}

void Modem::refresh() {
    std::lock_guard<std::mutex> lk(mu);
    refreshWanted = true;
}

std::string Modem::takeEvents() {
    std::lock_guard<std::mutex> lk(mu);
    std::string s;
    for (auto &e : events) s += e + "\n";
    events.clear();
    return s;
}

void Modem::service(uint64_t nowNs) {
    now = nowNs;
    std::vector<HostCall> todo;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (netDirty) { applied = net; netDirty = false; }
        if (refreshWanted) { sentReg = -1; refreshWanted = false; }
        todo.swap(hostCalls);
    }
    for (const HostCall &h : todo) {
        size_t i = 0;
        while (i < calls.size() && calls[i].id != h.id) i++;
        if (i == calls.size()) continue;
        Call &c = calls[i];
        if (trace) LOGI("modem: app says call %d %s", h.id, h.state.c_str());
        if (h.state == "pending") c.deadline = now + kPendingTimeout;
        else if (h.state == "dialing") c.deadline = 0;
        else if (h.state == "active") { c.deadline = 0; setStat(c, 0); }
        else if (h.state == "held") { c.deadline = 0; setStat(c, 1); }
        else if (h.state == "ended") endCall(i, false);
    }
    for (size_t i = calls.size(); i-- > 0;) {
        Call &c = calls[i];
        if (stubCalls && c.stat == 2 && now - c.since > 3 * kSecond) {
            LOGI("modem: (stub) call %d answered", c.id);
            setStat(c, 0);
        } else if (c.deadline && now > c.deadline) {
            LOGI("modem: the app did not take call %d; ending it", c.id);
            int id = c.id;
            endCall(i, false);
            event("end " + std::to_string(id));
        }
    }
    if (!claimedFlag || inPdu || !line.empty()) return;  // not between commands: report later
    int sig = signalValue();
    if (sig != sentSignal) {
        sentSignal = sig;
        if (st.cmerInd) unsolicited("+CIEV: 2," + std::to_string(sig));
    }
    int reg = regStat();
    std::string op = applied.op + "/" + applied.numeric;
    if (reg != sentReg || (reg != 0 && op != sentOp)) {
        sentReg = reg;
        sentOp = op;
        if (st.creg) unsolicited(regLine("+CREG", st.creg, reg));
        if (st.ereg) unsolicited(regLine("*EREG", st.ereg, reg));
    }
    if (reg != sentGprs) {
        sentGprs = reg;
        if (st.cgreg) unsolicited(regLine("+CGREG", st.cgreg, reg));
    }
}

void Modem::reset() {
    for (const Call &c : calls) event("end " + std::to_string(c.id));
    calls.clear();
    dialing = 0;
    st = Settings();
    line.clear();
    inPdu = false;
    claimedFlag = false;
    sentSignal = sentReg = sentGprs = -1;
    sentOp.clear();
}

void Modem::serialize(StateIO &s) {
    s.tag("MDM0");
    s.io(st.echo); s.io(st.verbose); s.io(st.quiet); s.io(st.cmee); s.io(st.creg); s.io(st.cgreg); s.io(st.ereg);
    s.io(st.cmerInd); s.io(st.copsFormat); s.io(st.fun); s.io(st.crc); s.io(st.clip); s.io(st.ecam); s.io(st.mute);
    bool claimed = claimedFlag;
    s.io(claimed);
    claimedFlag = claimed;
    // The last network the app reported, so a restored guest keeps it until the app's next update.
    s.io(applied.service); s.io(applied.signal); s.io(applied.roaming);
    s.io(applied.op); s.io(applied.numeric); s.io(applied.data); s.io(applied.number);
    if (!s.writing) {
        // Calls are not saved (the app's side of them is gone): the RIL's next +CLCC finds none.
        // The current network state is reported again.
        calls.clear();
        dialing = 0;
        line.clear();
        inPdu = false;
        sentSignal = sentReg = sentGprs = -1;
        sentOp.clear();
    }
}

}  // namespace wm
