# Windows Marketplace for Mobile

Microsoft ran Windows Marketplace for Mobile for Windows Mobile 6.x from October 2009 until 22 May 2012.
The client on the phone, and Internet Explorer, still call its names. Nothing answers them. PocketStylus
stands in for those names and offers cabinets from a Marketplace server (default `market.stylus.737.jp.net`,
`server/market`). Cabinets on the storage card are the fallback when that server is empty or down.

Tags as in the other research docs: **[SRC]** = a cited page, **[ARCHIVE]** = a Wayback Machine capture,
**[ROM]** = strings in `WMMarketplaceFullClient.exe` 1.2.1422.0000 from the HD2 image,
**[INFERRED]** = not seen from a running client.

## What the client called [SRC] [ARCHIVE] [ROM]

From the Marketplace client's registry (`HKCU\Security\Software\Microsoft\Marketplace`), posted on XDA
when the client was current:

```
WebServiceUpdateServer = https://client.marketplace.windowsmobile.com/bootstrap/02/2009
LiveIdAuthEndpoint     = https://login.live.com/liveidSTS.srf
LiveIdDeviceRegisterEndpoint = https://login.live.com/ppsecure/DeviceAddCredential.srf
```

Wayback still has the service page the client was built against:

```
http://client.marketplace.windowsmobile.com/Services/BootstrapService/BootstrapService.svc
```

That page is a WCF service help page (2009-10-15 and 2010-01-11). It names `BootstrapServiceClient` and
links a WSDL. The WSDL was not archived. The full client on the HD2 image does not call that `.svc`;
it calls the catalog below. [ROM]

Other names Wayback recorded under the same parent:

| Name | What showed up |
|---|---|
| `client.marketplace.windowsmobile.com` | The bootstrap service, and a small web page |
| `cdnclient.marketplace.windowsmobile.com` | Client cabinet downloads (`Marketplacebootstrap.cab`); the captures are 404s |
| `files.marketplace.windowsmobile.com` | `/{guid}/LargeWebIcon.png` and `/{guid}/Screenshot/N.png` |
| `marketplace.windowsmobile.com` | The public site. Before launch it redirected to the older Windows Mobile catalog |
| `api.marketplace.windowsmobile.com` | The catalog, update and commerce bases, and ` /passport/login.aspx?ppud=4&wa=wsignin1.0` [ROM] [SRC] |

The full client hardcodes these bases, all version `02/2009` [ROM]:

| Base | Used for |
|---|---|
| `http://api.marketplace.windowsmobile.com/appcatalog/02/2009` and the `https` twin | categories, charts, search, app details, icons |
| `https://api.marketplace.windowsmobile.com/appcatalog/02/2009` | `deviceInstance/.../urlandlicense` (the cabinet URL) |
| `https://api.marketplace.windowsmobile.com/update/02/2009` | `POST /updateproducts` |
| `https://api.marketplace.windowsmobile.com/commerce/02/2009` | account and one-time charge |

Paths the program formats, with `%1` the base [ROM]:

```
%1/devices/%2/markets?loc=%3
%1/devices/%2/categories?loc=%3
%1/devices/%2/categories/%3/apps?beforeMarker=%4&chunkSize=%5&orderby=%6&loc=%7
%1/devices/%2/charts?ver=%3&loc=%4
%1/devices/%2/charts/%3/apps?beforeMarker=%4&chunkSize=%5&orderby=%6&loc=%7
%1/devices/%2/search?q=%3&beforeMarker=%4&chunkSize=%5&loc=%6
%1/devices/%2/apps/%3?loc=%4
%1/devices/%2/catalogs/%3?loc=%4
%1/deviceInstance/%2/apps/%3/urlandlicense?loc=%4
%1/devices/%2/apps/%3/reviews?beforeMarker=%4&chunkSize=%5&loc=%6
%1/updateproducts
```

`orderby` is one of `toppaid`, `topfree`, `whatsnew`, `all`. XML is DataContract-style,
namespace `Microsoft.WindowsMobile.Service.Marketplace`, with roots the program looks up by name:
`ListResponseOfGeoMarket`, `ListResponseOfCategory`, `ListOfAppSummary`, `ResponseOfApplication`,
`ResponseOfDownloadUrl`, `UpdateResponse`. An application carries `Id`, `ApplicationId`, `Name`,
`Price`, `OverAirDownloadAllowed` and the rest of the fields in that program's parser. [ROM]

`POST /updateproducts` sends an `UpdateRequest` whose product `Marketplace` is the client version.
A mandatory `<ProductUpdate>` is how the program decides it must replace itself. [ROM]

Sign-in is Windows Live. The program loads
`https://api.marketplace.windowsmobile.com/passport/login.aspx?ppud=4&wa=wsignin1.0`, reads federation
metadata from `nexus.passport.com`, and posts a SOAP token request to `login.live.com`. Download is
gated on that sign-in inside the program (`StateDownloadAndInstall: Sign-in required`). [ROM]

Cabinets were what got installed. The client handed them to `wceload`. [SRC: XDA, the 6.5 update notes]

Windows Phone 7's store (`catalog.zune.net`, XAP packages) is a different service. This stand-in does not speak it.

## What PocketStylus does

With **Settings → Network → Windows Marketplace** on (the default):

- DNS for `marketplace.windowsmobile.com` and every name under it answers `10.0.2.4`, the phone's local service.
- HTTPS needs **Secure Web Proxy**, same as any other site the guest opens with its old TLS. The proxy decrypts and hands the request to the stand-in. Plain HTTP on port 80 works without it, which is how Internet Explorer can open `http://marketplace.windowsmobile.com/`.
- **Settings → Network → Marketplace Server** is the host that has the cabinets (default `market.stylus.737.jp.net`, HTTPS, no port). The stand-in forwards each request there. `host:port` with a port other than 443 is plain HTTP. An empty host uses only the storage card.
- That host is `server/market`: the same catalog and the `.cab` files. A TLS proxy in front of it is what the default hostname expects. If the host does not answer, the cabinets in `\Storage Card\Marketplace` are used instead. That folder is read again on each request.
- `Name.cab.ini` beside a cabinet, or a `[Name.cab]` section in `catalog.ini`, can set `title`, `publisher`, `version` and `summary`. `Name.png` is the icon.
- The storefront is an HTML page. **Install** downloads that cabinet (`Content-Type: application/vnd.ms-cab-compressed`) for `wceload`. This path does not ask for a Microsoft account.
- The built-in program's catalog calls are answered with those cabinets: one category, the same list for every chart and for search, price 0, and `OverAirDownloadAllowed` true. On the server the category is "Marketplace" and `urlandlicense` points at `https://<Marketplace Server>/pkg/<id>`. On the storage card the category is "This phone" and the package is `https://files.marketplace.windowsmobile.com/{id}/Package/{file}.cab`. Icons are `LargeWebIcon.png` beside that, when `Name.png` exists. The HTML Install link is `/pkg/<id>`, so Internet Explorer stays on the Marketplace name and the stand-in forwards it.
- `POST /updateproducts` returns empty update, product, and revoke lists, so the program is not told to replace itself.
- `GET /bootstrap/02/2009` and a SOAP POST to `BootstrapService.svc` still answer that no client update is available. Those fields are **[INFERRED]**; the 1.2.1422 program does not use them.
- Sign-in URLs on these hosts answer that this phone is signed in. The body is not stored. `login.live.com` is left alone, so this does not interfere with Windows Live or Escargot. The Marketplace program still asks for that Live ID before it requests a cabinet. Until that sign-in exists, install from Internet Explorer.
- A request the stand-in does not recognise is a 404, and (when the secure web proxy has a folder) a redacted copy is written under `market-capture`, the same way the Windows Live gateway captures.

Windows Live and Marketplace can both be on. They share `10.0.2.4`; `LocalDispatch` picks the service from the host.

## Trying it

1. Leave Windows Marketplace, Marketplace Server (`market.stylus.737.jp.net`) and Secure Web Proxy on.
2. Cabinets come from that server. To offer one only on this phone, clear Marketplace Server and copy the cabinet into `\Storage Card\Marketplace`.
3. In Internet Explorer, open `http://marketplace.windowsmobile.com/` and tap Install. The Marketplace program lists the same cabinets.

`wmcore-cli --market-selftest` checks the storefront, the catalog the built-in program calls, a cabinet download, a forwarded request and the "no update" replies without a guest. `node server/market/test.js` checks the server that hosts the cabinets.
