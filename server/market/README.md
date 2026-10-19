# Windows Marketplace server

Microsoft closed Windows Marketplace for Mobile in 2012. This process hosts the cabinets and answers
the catalog the Windows Mobile client still calls (`/appcatalog/02/2009`, the download URL, and the
HTML storefront). PocketStylus keeps the old `*.marketplace.windowsmobile.com` names on the phone and
forwards each request here.

One file, `server.js`, needs Node.js 18 or newer and nothing from npm. The phone's **Marketplace
Server** setting is the public hostname. The default is `market.stylus.737.jp.net`.

## Run it

```sh
MARKET_DIR=/var/lib/wmdroid-market node server.js    # listens on 0.0.0.0:8080
```

Copy a `.cab` into that folder. Optional notes go in `Name.cab.ini` beside it (`title`, `publisher`,
`version`, `summary`). `Name.png` is the icon. `node test.js` checks the catalog and a cabinet download.

## On market.stylus.737.jp.net

The phone opens this host with HTTPS and no port. Put TLS in front and leave Node on plain HTTP:

```
market.stylus.737.jp.net {
    reverse_proxy 127.0.0.1:8080
}
```

Caddy sends `X-Forwarded-Proto`, which is how a cabinet URL stays `https://market.stylus.737.jp.net/...`.
Set `HOST=127.0.0.1`. `TLS_CERT` and `TLS_KEY` serve HTTPS from Node itself when there is no proxy.

A systemd unit (`/etc/systemd/system/wmdroid-market.service`):

```ini
[Unit]
Description=Windows Marketplace cabinets for PocketStylus
After=network.target

[Service]
ExecStart=/usr/bin/node /opt/wmdroid-market/server.js
Environment=HOST=127.0.0.1 PORT=8080 MARKET_DIR=/var/lib/wmdroid-market
DynamicUser=yes
Restart=always

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now wmdroid-market`.

## Settings

| Variable | Default | Meaning |
|---|---|---|
| `PORT`, `HOST` | `8080`, `0.0.0.0` | Where to listen |
| `MARKET_DIR` | `./cabinets` | Folder of `.cab` files, read again on each request |
| `TLS_CERT`, `TLS_KEY` | | PEM files: serve HTTPS directly |

An empty Marketplace Server setting on the phone uses only the cabinets in `\Storage Card\Marketplace`.
If this server does not answer, the phone uses that folder too.
