// wmcore-cli --mws-selftest [--probe] [--capture DIR]: the Windows Live Mobile gateway (net/mwsgate.h)
// without a guest. Plays the phone's Windows Live client against the gateway: the security policy,
// then a Live ID sign-in (PPCRL RequestMultipleSecurityTokens, shaped like WLMService.dll's) with a
// made-up name and password, which the gateway turns into an MSNP8 + Tweener sign-in to a fake
// server on 127.0.0.1. Nothing reaches a real service; --probe adds a reachability check of
// Escargot (MSNP VER/CVR and the Passport nexus) that signs nobody in.
#include "../src/net/mwsgate.h"
#include "../src/net/mwspdu.h"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/util/log.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace wm;

namespace {

std::string rstRequest(const std::string &user, const std::string &password) {
    // The request WLMService.dll builds (strings in the DLL), with the policy's RPS cluster and template.
    std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><Envelope xmlns=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" xmlns:wsp=\"http://schemas.xmlsoap.org/ws/2004/09/policy\""
        " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\""
        " xmlns:wst=\"http://schemas.xmlsoap.org/ws/2005/02/trust\"><Header>"
        "<ps:AuthInfo xmlns:ps=\"http://schemas.microsoft.com/Passport/SoapServices/PPCRL\" Id=\"PPAuthInfo\">"
        "<ps:HostingApp>{DF60E2DF-88AD-4526-AE21-83D130EF0F68}</ps:HostingApp><ps:BinaryVersion>5</ps:BinaryVersion>"
        "<ps:UIVersion/><ps:Cookies/><ps:RequestParams/></ps:AuthInfo>"
        "<wsse:Security xmlns=\"http://schemas.xmlsoap.org/ws/2004/04/trust\"><wsse:UsernameToken Id=\"user\">"
        "<wsse:Username>" + user + "</wsse:Username><wsse:Password>" + password + "</wsse:Password></wsse:UsernameToken>"
        "</wsse:Security></Header><Body>"
        "<ps:RequestMultipleSecurityTokens xmlns:ps=\"http://schemas.microsoft.com/Passport/SoapServices/PPCRL\" Id=\"RSTS\">"
        "<wst:RequestSecurityToken><wst:RequestType>http://schemas.xmlsoap.org/ws/2005/02/trust/Issue</wst:RequestType>"
        "<wsp:AppliesTo><wsa:EndpointReference><wsa:Address>http://Passport.NET/tb</wsa:Address></wsa:EndpointReference></wsp:AppliesTo>"
        "</wst:RequestSecurityToken><wst:RequestSecurityToken>"
        "<wst:RequestType>http://schemas.xmlsoap.org/ws/2005/02/trust/Issue</wst:RequestType>"
        "<wsp:AppliesTo><wsa:EndpointReference><wsa:Address>mobile.live.com</wsa:Address></wsa:EndpointReference></wsp:AppliesTo>"
        "<wsp:PolicyReference URI=\"MBI_SSL\" /></wst:RequestSecurityToken></ps:RequestMultipleSecurityTokens></Body></Envelope>";
    return "POST /RST.srf HTTP/1.1\r\nContent-Type: application/soap+xml\r\nUser-Agent: Mozilla/4.0 (compatible; MSIE 4.01; Windows CE)\r\n"
           "Host: http.mws.mobile.live.com\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

}  // namespace

int mwsSelfTest(int argc, char **argv) {
    bool probe = false;
    std::string capture;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = true;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc && !strcmp(argv[++i], "debug")) g_logLevel = LOG_DEBUG;
    }
    bool all = true;
    if (probe) {
        MwsGateway real(MwsGateway::Config{});
        std::string report;
        bool ok = real.probe(&report);
        printf("%s", report.c_str());
        all &= check(ok, "Escargot reachable (MSNP8 VER/CVR, Passport nexus with DALogin)");
    }
    MwsGateway::Config cfg;
    cfg.captureDir = capture;
    cfg.msnpHost = "127.0.0.1";
    cfg.msnpPort = startFakeMsnpServer(&cfg.nexusUrl);
    if (!check(cfg.msnpPort != 0, "fake MSNP server started")) return 1;
    MwsGateway gw(cfg);

    std::string policy = gw.selfTestRequest(
        "GET /2006/10/MWP2007_02/SecurityPolicy/Default.aspx HTTP/1.1\r\nHost: http.mws.mobile.live.com\r\n\r\n");
    all &= check(policy.compare(0, 12, "HTTP/1.1 200") == 0 && policy.find("<sp:IssuedToken>") != std::string::npos &&
                 policy.find("/RST.srf</wsa:Address>") != std::string::npos && policy.find("<mws:Expiry>") != std::string::npos,
                 "security policy served");

    // A made-up account for the fake server (it accepts any password).
    std::string rst = gw.selfTestRequest(rstRequest("tester@fake.invalid", "dummy-for-the-fake-server"));
    bool rstOk = rst.compare(0, 12, "HTTP/1.1 200") == 0;
    all &= check(rstOk, "Live ID sign-in turned into an MSNP8/Tweener sign-in and answered");
    all &= check(rst.find("<wst:RequestSecurityTokenResponse>") != std::string::npos &&
                 rst.find("<wst:BinarySecret>") != std::string::npos &&
                 rst.find("urn:passport:compact") != std::string::npos,
                 "token response has compact tokens and proof keys for the client");
    if (!rstOk) printf("response: %.600s\n", rst.c_str());
    // The push channel: the connect PDU the phone's client sends (captured), then a heartbeat.
    {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        gw.serve(sv[1], 50001, "tcp.mws.mobile.live.com", false);
        const uint8_t connect[] = {0x30, 0x01, 0, 0, 0, 0, 0, 0, 0x0c, 0, 0, 0x03, 0x08, 0x02, 0x01, 0, 0, 0x06, 0x01, 0, 0x08, 0x02, 0x0c, 0};
        const uint8_t heartbeat[] = {0x30, 0x04, 0, 0, 0, 0, 0, 0, 0x04, 0, 0, 0x05, 0x00, 0x02, 0x0e, 0x01};
        auto exchange = [&](const uint8_t *p, size_t n, MwsPdu &out) {
            if (send(sv[0], p, n, 0) != (ssize_t)n) return false;
            std::string buf;
            for (int i = 0; i < 50; i++) {
                pollfd pf{sv[0], POLLIN, 0};
                if (poll(&pf, 1, 100) > 0) {
                    char tmp[512];
                    ssize_t r = recv(sv[0], tmp, sizeof tmp, 0);
                    if (r <= 0) return false;
                    buf.append(tmp, (size_t)r);
                }
                if (parseMwsPdu(buf, out) > 0) return true;
            }
            return false;
        };
        MwsPdu a;
        bool ok = exchange(connect, sizeof connect, a) && a.type == 0x82 && a.opt(2) && a.opt(2)->size() >= 5 &&
                  (uint8_t)(*a.opt(2))[2] == 5 && (uint8_t)(*a.opt(2))[3] == a.opt(2)->size() - 4;
        all &= check(ok, "push channel: connect answered with 0x82 and a session id (option 2)");

        // Reproduce exactly what WLMService.dll does with a received 0x82 to decide ConnectionStatus
        // (TCP::_ThdRecv 0x4243c8 reads a u32 from option 4; the platform at 0x40ca18 writes
        // ConnectionStatus = 4 when that is > 0 while the connect state is 1, else 1). The plugins sync
        // only at 4. getOption returns a pointer at the option entry [next_type][len][data...]; the DLL
        // reads the word at entry+4, i.e. data bytes 2..5.
        auto connectionStatus = [](const std::string &wire) -> int {
            MwsPdu q;
            if (parseMwsPdu(wire, q) <= 0 || q.type != 0x82) return -1;
            const std::string *o4 = q.opt(4);
            uint32_t seq = 0;
            if (o4 && o4->size() >= 6)
                seq = (uint8_t)(*o4)[2] | (uint8_t)(*o4)[3] << 8 | (uint8_t)(*o4)[4] << 16 | (uint8_t)(*o4)[5] << 24;
            return seq > 0 ? 4 : 1;  // connect state is 1 after a good handshake
        };
        all &= check(connectionStatus(buildMwsPdu(a)) == 4,
                     "push channel: the 0x82 drives ConnectionStatus to 4 (connected), so plugins sync");
        MwsPdu h;
        all &= check(exchange(heartbeat, sizeof heartbeat, h) && h.type == 0x84 && h.opt(5) && *h.opt(5) == std::string("\x0e\x01", 2),
                     "push channel: heartbeat answered with 0x84 and the same TTL (option 5)");
        close(sv[0]);
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));  // let the session go online and answer the challenge
    printf(all ? "ALL PASSED\n" : "SOME FAILED\n");
    return all ? 0 : 1;
}
