// Infrared transports for the headless runner (the app has its own in Kotlin: InfraredBridge.kt).
//   UDP pipe:  two CLI instances on one host, each frame one datagram (--irda-udp LPORT:RPORT).
//   InfraWAN:  the WebSocket relay in server/infrawan (--irda-wan ws://host:port/room?user=..&token=..),
//              plain ws:// only (the app adds TLS); a local relay is reached over `adb reverse`.
#pragma once
#include <memory>
#include <string>

namespace wm {

class Irda;

class IrdaLink {
public:
    virtual ~IrdaLink() = default;
    /** Starts the transport threads; false when it cannot connect. */
    virtual bool start(Irda &irda, std::string &err) = 0;
    virtual void stop() = 0;
};

/** delayMs: extra one-way delay for every frame sent (latency tests, --irda-delay). */
std::unique_ptr<IrdaLink> irdaUdpLink(int localPort, int remotePort, int delayMs = 0);
/** url: ws://host:port/path?room=R&user=U&token=T&name=N (room/user/token/name may also be path-free). */
std::unique_ptr<IrdaLink> irdaWanLink(const std::string &url);

}  // namespace wm
