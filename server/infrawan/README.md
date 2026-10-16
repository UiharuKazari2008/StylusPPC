# InfraWAN relay

InfraWAN carries Windows Mobile's infrared beams between PocketStylus phones over the Internet. Each
phone connects to the relay and joins a **room**. Every infrared frame one device sends goes to
everyone else in that room, just as IR light reaches every port facing it. The devices' own IrDA
stacks work out who is talking to whom. The relay never looks inside the frames.

One file, `server.js`, needs Node.js 18 or newer and nothing from npm.

## Run it

```sh
export INFRAWAN_SECRET="$(openssl rand -hex 32)"   # keep this private and stable
node server.js                                     # listens on 0.0.0.0:7300, path /infrawan
node server.js token alice                         # prints alice's token
```

Give each user their user name and token. In PocketStylus they enter them under
**Settings > Infrared > InfraWAN relay**, together with the server address
(`wss://your.host/infrawan`) and a room name. Phones that use the same room hear each other.

`node test.js` starts a private relay and checks rooms, relaying, authentication and the limits
(it needs Node 22 or newer, for the built-in WebSocket client).

## Settings (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `PORT`, `HOST` | `7300`, `0.0.0.0` | Where to listen |
| `INFRAWAN_SECRET` | | Server secret. A user's token is `node server.js token <user>`: an HMAC of the user name, so no user list has to be stored. |
| `INFRAWAN_USERS` | | Optional JSON file `{"alice": "token", ...}` of explicit tokens, re-read when it changes. Use it alone, or with the secret. |
| `INFRAWAN_OPEN` | | `1`: no authentication. Only for a LAN or a test. |
| `TLS_CERT`, `TLS_KEY` | | PEM files: serve `wss://` directly. Otherwise put the relay behind a TLS proxy (below). |
| `INFRAWAN_TRUST_PROXY` | | `1`: take the client address from `X-Forwarded-For` (behind a proxy) for the per-address limits |
| `INFRAWAN_MAX_ROOM` | 32 | Devices per room |
| `INFRAWAN_MAX_PER_USER` | 4 | Simultaneous connections per user |
| `INFRAWAN_MAX_PER_IP` | 16 | Simultaneous connections per address |
| `INFRAWAN_BYTES_PER_SEC`, `INFRAWAN_BURST_BYTES` | 65536, 262144 | Per-device byte rate. IrDA SIR tops out near 11.5 KB/s, so this is generous. |
| `INFRAWAN_FRAMES_PER_SEC`, `INFRAWAN_BURST_FRAMES` | 400, 800 | Per-device frame rate |

Abuse handling:
- Over the rate, frames are dropped and IrLAP retransmits them. More than 5000 drops ends the session.
- Five failed logins from one address in a minute block that address for five minutes.
- A connection must say hello within 10 s.
- Messages are capped at 64 KB and frames at 4096 bytes.
- A device that stops answering the 25 s pings is dropped.
- A device too slow to receive loses frames instead of holding the room back.

`GET /health` returns room and device counts. It never shows names or tokens.

## On a server

Behind Caddy, which handles TLS and certificates:

```
infrawan.example.com {
    reverse_proxy /infrawan* 127.0.0.1:7300
}
```

With nginx, add `proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header
Connection "upgrade"; proxy_read_timeout 120s;` to the location. In both cases set `HOST=127.0.0.1`
and `INFRAWAN_TRUST_PROXY=1`.

A systemd unit (`/etc/systemd/system/infrawan.service`):

```ini
[Unit]
Description=InfraWAN relay for PocketStylus
After=network.target

[Service]
ExecStart=/usr/bin/node /opt/infrawan/server.js
Environment=HOST=127.0.0.1 PORT=7300 INFRAWAN_TRUST_PROXY=1
EnvironmentFile=/etc/infrawan.env
DynamicUser=yes
Restart=always

[Install]
WantedBy=multi-user.target
```

`/etc/infrawan.env` holds `INFRAWAN_SECRET=...`. Then run `systemctl enable --now infrawan`.

## Protocol (version 1)

Each device opens one WebSocket to `/infrawan`.

| Direction | Kind | Content |
|---|---|---|
| device → relay | text, first message | `{"type":"hello","version":1,"room":R,"user":U,"token":T,"name":N}`. Room: 1-64 characters from letters, digits, space, `. _ -`. |
| device → relay | binary | `0x01`, then one IrLAP frame: address, control, information, with no FCS (at most 4096 bytes) |
| relay → device | text | `{"type":"welcome","id":ID,"room":R,"peers":[{"id","name"}],"limits":{...}}` |
| relay → device | text | `{"type":"join","id","name"}` and `{"type":"leave","id"}` |
| relay → device | text | `{"type":"error","code":"auth|proto|full|busy|rate","message"}`, then the connection closes |
| relay → device | binary | `0x01`, the sender's id (u32, little-endian), the frame |

Frames from a sender reach the others in order. The relay adds latency but no retransmission. IrLAP
tolerates a round trip of up to about 0.8 s through PocketStylus (`docs/research-irda.md`, §5).
