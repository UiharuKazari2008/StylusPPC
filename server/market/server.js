#!/usr/bin/env node
// Windows Marketplace for Mobile, hosted so a PocketStylus phone can install cabinets.
//
// Microsoft closed the store in 2012. The phone still calls *.marketplace.windowsmobile.com;
// PocketStylus keeps those names on the phone and forwards each request here (core/src/net/market.h).
// This process answers the 02/2009 catalog and serves the .cab files in MARKET_DIR.
//
// No dependencies. Node 18 or newer.
//
//   MARKET_DIR=/var/lib/wmdroid-market node server.js
//
// Put a TLS proxy in front (see README.md) and point the phone's Marketplace Server setting at
// the public hostname. The default in the app is market.stylus.737.jp.net.
'use strict';

const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');

const NS = 'Microsoft.WindowsMobile.Service.Marketplace';
const env = process.env;
const num = (v, d) => (v === undefined || v === '' ? d : Number(v));

function lower(s) { return String(s || '').toLowerCase(); }
function trim(s) { return String(s || '').replace(/^\s+|\s+$/g, ''); }

function xmlEscape(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]));
}

function appId(rel) {
  const mix = (s, h) => {
    let x = BigInt(h);
    const prime = 0x100000001B3n;
    const mask = (1n << 64n) - 1n;
    for (let i = 0; i < s.length; i++) {
      x ^= BigInt(s.charCodeAt(i) & 0xff);
      x = (x * prime) & mask;
    }
    return x;
  };
  const text = lower(rel);
  const a = mix(text, 0xcbf29ce484222325n);
  const b = mix(text, 0x84222325cbf29ce4n);
  const hex = (n, w) => n.toString(16).padStart(w, '0');
  return [hex(a >> 32n, 8), hex((a >> 16n) & 0xffffn, 4), hex(a & 0xffffn, 4), hex((b >> 48n) & 0xffffn, 4), hex(b & 0xffffffffffffn, 12)].join('-');
}

function readIni(file) {
  let text;
  try { text = fs.readFileSync(file, 'utf8'); } catch { return []; }
  const out = [];
  let cur = '*';
  let keys = [];
  const flush = () => {
    if (keys.length || cur !== '*') out.push([cur, keys]);
    keys = [];
  };
  for (const raw of text.split(/\r?\n/)) {
    const s = trim(raw);
    if (!s || s[0] === ';' || s[0] === '#') continue;
    if (s[0] === '[' && s[s.length - 1] === ']') {
      flush();
      cur = lower(trim(s.slice(1, -1)));
      continue;
    }
    const eq = s.indexOf('=');
    if (eq < 0) continue;
    keys.push([lower(trim(s.slice(0, eq))), trim(s.slice(eq + 1))]);
  }
  flush();
  return out;
}

function iniSection(ini, name) {
  const n = lower(name);
  for (const [sec, keys] of ini) if (sec === n) return keys;
  return null;
}

function iniGet(sec, key) {
  if (!sec) return '';
  for (const [k, v] of sec) if (k === key) return v;
  return '';
}

function applySection(app, sec) {
  if (!sec) return;
  const title = iniGet(sec, 'title');
  const publisher = iniGet(sec, 'publisher');
  const version = iniGet(sec, 'version');
  const summary = iniGet(sec, 'summary');
  if (title) app.title = title;
  if (publisher) app.publisher = publisher;
  if (version) app.version = version;
  if (summary) app.summary = summary;
}

function stemOf(fileName) {
  let s = fileName;
  if (lower(s).endsWith('.cab') && s.length > 4) s = s.slice(0, -4);
  return s.replace(/[_-]/g, ' ');
}

function isoTime(ms) {
  if (!ms) return '2012-01-01T00:00:00Z';
  return new Date(ms).toISOString().replace(/\.\d{3}Z$/, 'Z');
}

function inside(rootReal, file) {
  let real;
  try { real = fs.realpathSync(file); } catch { return null; }
  const sep = path.sep;
  const root = rootReal.endsWith(sep) ? rootReal : rootReal + sep;
  const cmp = (a, b) => (process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b);
  const starts = (a, b) => (process.platform === 'win32' ? a.toLowerCase().startsWith(b.toLowerCase()) : a.startsWith(b));
  if (!cmp(real, root.slice(0, -1)) && !starts(real, root)) return null;
  return real;
}

function scanDir(root) {
  const apps = [];
  let rootReal;
  try { rootReal = fs.realpathSync(root); } catch { return apps; }
  if (!fs.statSync(rootReal).isDirectory()) return apps;
  const catalog = readIni(path.join(rootReal, 'catalog.ini'));
  const stack = [[rootReal, '']];
  while (stack.length && apps.length < 500) {
    const [dir, relDir] = stack.pop();
    if ((relDir.match(/\//g) || []).length > 5) continue;
    let names;
    try { names = fs.readdirSync(dir); } catch { continue; }
    for (const name of names) {
      if (name[0] === '.') continue;
      const full = path.join(dir, name);
      const rel = relDir ? relDir + '/' + name : name;
      let st;
      try { st = fs.statSync(full); } catch { continue; }
      if (st.isDirectory()) { stack.push([full, rel]); continue; }
      if (!lower(name).endsWith('.cab')) continue;
      const real = inside(rootReal, full);
      if (!real) continue;
      const stem = name.length > 4 ? name.slice(0, -4) : name;
      const app = {
        rel, fileName: name, cabPath: real, id: appId(rel),
        title: stemOf(name), publisher: 'Marketplace', version: '1.0', summary: '',
        bytes: st.size, added: isoTime(st.mtimeMs), iconPath: '',
      };
      const icon1 = path.join(dir, stem + '.png');
      const icon2 = full + '.png';
      app.iconPath = inside(rootReal, icon1) || inside(rootReal, icon2) || '';
      applySection(app, iniSection(catalog, rel));
      applySection(app, iniSection(catalog, name));
      applySection(app, iniSection(catalog, stem));
      const side = readIni(full + '.ini');
      const star = iniSection(side, '*');
      if (star) applySection(app, star);
      else if (side.length) applySection(app, side[0][1]);
      if (!app.title) app.title = stemOf(name);
      apps.push(app);
    }
  }
  apps.sort((x, y) => lower(x.title) < lower(y.title) ? -1 : lower(x.title) > lower(y.title) ? 1 : 0);
  apps.forEach((a, i) => { a.num = String(i + 1); });
  return apps;
}

function xel(name, value) { return '<' + name + '>' + xmlEscape(value) + '</' + name + '>'; }
function xnil(name) { return '<' + name + ' i:nil="true"/>'; }
function xbool(name, v) { return '<' + name + '>' + (v ? 'true' : 'false') + '</' + name + '>'; }
function xdoc(root, inner) {
  return '<?xml version="1.0" encoding="utf-8"?>\n<' + root + ' xmlns="' + NS +
    '" xmlns:i="http://www.w3.org/2001/XMLSchema-instance">' + inner + '</' + root + '>';
}
function xok() { return xel('ResponseCode', 'Success') + xnil('ResponseMessage'); }

function queryParam(query, key) {
  const want = lower(key);
  for (const part of String(query || '').split('&')) {
    const eq = part.indexOf('=');
    const k = decodeURIComponent(eq < 0 ? part : part.slice(0, eq));
    if (lower(k) === want) return decodeURIComponent(eq < 0 ? '' : part.slice(eq + 1).replace(/\+/g, ' '));
  }
  return '';
}

function queryUInt(query, key, fallback) {
  const v = queryParam(query, key);
  if (!v) return fallback;
  const n = parseInt(v, 10);
  return Number.isFinite(n) && n >= 0 ? n : fallback;
}

function segsOf(p) {
  return p.split('/').filter(Boolean);
}

function findGuid(text) {
  const m = String(text).match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i);
  return m ? m[0].toLowerCase() : '';
}

function findAny(apps, id) {
  if (!id) return null;
  const want = lower(id);
  return apps.find((a) => a.id === want || a.num === id || lower(a.fileName) === want || lower(a.rel) === want) || null;
}

function publicBase(req) {
  const first = (v) => String(v || '').split(',')[0].trim();
  const xfProto = first(req.headers['x-forwarded-proto']);
  const xfHost = first(req.headers['x-forwarded-host']);
  const host = xfHost || req.headers.host || 'localhost';
  let scheme = xfProto || (req.socket.encrypted ? 'https' : '');
  if (!scheme) {
    const h = host.toLowerCase();
    const local = h.startsWith('127.') || h.startsWith('localhost') || h.startsWith('[::1]');
    scheme = (local || /:\d+$/.test(h)) ? 'http' : 'https';
  }
  return scheme + '://' + host;
}

function packageUrl(req, app) { return publicBase(req) + '/pkg/' + app.id; }

function iconUrl(req, app) {
  return app.iconPath ? publicBase(req) + '/' + app.id + '/LargeWebIcon.png' : '';
}

function appSummaryXml(req, app) {
  const icon = iconUrl(req, app);
  return '<AppSummary>' + xel('CompanyName', app.publisher) + xel('Id', app.num) + xel('InstallName', app.title) +
    xbool('IsFeatured', app.num === '1') + xel('Name', app.title) + xel('Price', '0') + xel('Rating', '0') +
    (icon ? xel('SmallImageUrl', icon) : xnil('SmallImageUrl')) + xbool('dotNet35', false) + '</AppSummary>';
}

function appListXml(req, apps, before, chunk) {
  if (!chunk || chunk > 100) chunk = 25;
  let inner = '';
  let n = 0;
  for (const a of apps) {
    if (parseInt(a.num, 10) <= before) continue;
    inner += appSummaryXml(req, a);
    if (++n >= chunk) break;
  }
  return xdoc('ListOfAppSummary', inner);
}

function categoryXml(id, name, count) {
  return '<Category>' + xbool('HasSubCategories', false) + xel('Id', id) + xnil('ImageUrl') + xel('Name', name) +
    xel('NumItems', String(count)) + '</Category>';
}

function appEntityXml(req, app) {
  const icon = iconUrl(req, app);
  const features = app.summary ? '<Features><string>' + xmlEscape(app.summary) + '</string></Features>' : '<Features/>';
  const shots = icon ? '<Screenshots><string>' + xmlEscape(icon) + '</string></Screenshots>' : '<Screenshots/>';
  const hw = ['Accelerometer', 'Audio', 'Bluetooth', 'Dpad', 'FmRadio', 'GPS', 'Infrared', 'Keyboard',
    'MemoryExtension', 'Photo', 'TouchScreen', 'TvOut', 'Video', 'Wifi'];
  const flags = hw.map((name) => xbool(name, false)).join('');
  return xok() + '<Entity>' + xel('ApplicationId', app.id) + xel('CompanyName', app.publisher) +
    xel('DateAdded', app.added || '2012-01-01T00:00:00Z') + xnil('DotNetVersion') + features +
    xel('Id', app.num) + xel('InstallName', app.title) + xel('LongDescription', app.summary || app.title) +
    (icon ? xel('MediumImageUrl', icon) : xnil('MediumImageUrl')) + xel('Name', app.title) +
    xel('NumOfReviews', '0') + xbool('OverAirDownloadAllowed', true) + xel('Price', '0') + xel('PriceWithTax', '0') +
    xel('Rating', '0') + shots + xel('Size', String(app.bytes)) + xnil('SupportUrl') +
    xbool('SupportsCCBilling', false) + xbool('SupportsMOBilling', false) + xnil('TermsOfUseURL') +
    xel('Version', app.version) + xbool('dotNet35', false) + flags + '</Entity>';
}

function matching(apps, q) {
  const want = lower(trim(q));
  if (!want) return apps;
  return apps.filter((a) => lower(a.title + ' ' + a.publisher + ' ' + a.summary + ' ' + a.fileName).includes(want));
}

function pageHtml(apps, query, one) {
  let h = '<!DOCTYPE html><html><head><title>Windows Marketplace</title></head><body>' +
    '<h1>Windows Marketplace</h1>' +
    '<p>Microsoft\'s Marketplace service ended in 2012. Installers on this server are what the phone offers. ' +
    'Install opens the cabinet.</p>';
  if (!one) {
    h += '<form action="/" method="get"><input type="text" name="q" value="' + xmlEscape(query) +
      '"><input type="submit" value="Search"></form>';
    if (!apps.length) h += query ? '<p>No cabinets match.</p>' : '<p>No cabinets yet.</p>';
  }
  h += '<table>';
  for (const a of apps) {
    h += '<tr><td><b>' + xmlEscape(a.title) + '</b><br>' + xmlEscape(a.publisher);
    if (a.version) h += ' ' + xmlEscape(a.version);
    if (a.summary) h += '<br>' + xmlEscape(a.summary);
    h += '<br><a href="/pkg/' + a.id + '">Install</a>';
    if (!one) h += ' <a href="/app/' + a.id + '">Details</a>';
    h += '</td></tr>';
  }
  return h + '</table></body></html>';
}

function loginHtml() {
  return '<!DOCTYPE html><html><head><title>Windows Marketplace</title></head><body>' +
    '<h1>Signed in on this phone</h1>' +
    '<p>This Marketplace does not use a Microsoft account. ' +
    '<a href="/">Browse installers</a>.</p></body></html>';
}

function xmlReply(body, code) {
  return { code: code || 200, type: 'text/xml; charset=utf-8', body };
}
function htmlReply(body, code) {
  return { code: code || 200, type: 'text/html; charset=utf-8', body };
}

function marketplaceApi(req, pathL, query, apps) {
  const apiPath = pathL.includes('/appcatalog/') || pathL.includes('/devices/') || pathL.includes('/deviceinstance/') ||
    pathL.includes('updateproducts') || pathL.includes('/update/') || pathL.includes('/commerce/');
  if (!apiPath) return null;
  if (pathL.includes('login') || pathL.includes('passport') || pathL.includes('expirecookie')) return null;
  const segs = segsOf(pathL);
  let at = -1;
  let instance = false;
  for (let i = 0; i < segs.length; i++) {
    if (segs[i] === 'devices' || segs[i] === 'deviceinstance') {
      at = i;
      instance = segs[i] === 'deviceinstance';
      break;
    }
  }
  const fail = (root, msg) => xmlReply(xdoc(root, xel('ResponseCode', 'NotFound') + xel('ResponseMessage', msg)), 404);
  if (pathL.includes('updateproducts')) {
    return xmlReply(xdoc('UpdateResponse', xok() + '<RemoveProducts/><AvailableUpdates/><AvailableProducts/><RevokeLicenses/>'));
  }
  if (at < 0 || at + 1 >= segs.length) return fail('ResponseOfApplication', 'Unknown catalog request');
  const rest = segs.slice(at + 2);
  const head = rest[0] || '';
  const id = rest[1] || '';
  const before = queryUInt(query, 'beforemarker', 0);
  const chunk = queryUInt(query, 'chunksize', 25);
  let q = queryParam(query, 'q') || queryParam(query, 'applicationid');
  if (!instance && head === 'markets') {
    return xmlReply(xdoc('ListResponseOfGeoMarket', xok() + '<List><GeoMarket>' + xel('Code', 'US') +
      xel('DisplayName', 'United States') + xel('GeoFilter', 'US') + xbool('IsHome', true) + '</GeoMarket></List>'));
  }
  if (!instance && head === 'categories' && rest.length <= 2) {
    const cats = !id || id === '1' ? categoryXml('1', 'Marketplace', apps.length) : '';
    return xmlReply(xdoc('ListResponseOfCategory', xok() + cats));
  }
  if (!instance && head === 'charts' && rest.length <= 1) {
    const cats = categoryXml('1', "What's new", apps.length) + categoryXml('2', 'Top free', apps.length) +
      categoryXml('3', 'Top paid', apps.length);
    return xmlReply(xdoc('ListResponseOfCategory', xok() + cats));
  }
  if (!instance && head === 'customizations') {
    return xmlReply(xdoc('ListResponseOfMoCustomizationProperty', xok() + '<List/>'));
  }
  if (!instance && (head === 'account' || head === 'charge')) {
    return xmlReply(xdoc(head === 'charge' ? 'OneTimePaymentResponse' : 'ResponseOfAccount', xok() + xel('AccountId', 'local')));
  }
  if (!instance && (head === 'search' || (head === 'categories' && rest[2] === 'apps') || (head === 'charts' && rest[2] === 'apps'))) {
    return xmlReply(appListXml(req, head === 'search' ? matching(apps, q) : apps, before, chunk));
  }
  if (head === 'apps' && rest[2] === 'reviews') return xmlReply(xdoc('ListOfReview', ''));
  const details = !instance && head === 'apps' && id && rest.length === 2;
  const byApp = !instance && head === 'catalogs' && id;
  const download = instance && head === 'apps' && rest[2] === 'urlandlicense';
  let app = (details || byApp || download) ? findAny(apps, id) : null;
  if (!app && q && (details || byApp)) app = findAny(apps, q);
  if ((details || byApp) && app) return xmlReply(xdoc('ResponseOfApplication', appEntityXml(req, app)));
  if (download && app) {
    const url = packageUrl(req, app);
    return xmlReply(xdoc('ResponseOfDownloadUrl', xok() + '<License>' + xel('Id', app.id) + xel('Url', url) + '</License>' + xel('Url', url)));
  }
  if (details || byApp || download) return fail(download ? 'ResponseOfDownloadUrl' : 'ResponseOfApplication', 'No installer with that id');
  return fail('ResponseOfApplication', 'Unknown catalog request');
}

function clientVersion(s) {
  const m = String(s).match(/\d+\.\d[\d.]*/);
  return m ? m[0].slice(0, 32) : '0.0.0.0';
}

function respond(req, root) {
  const url = new URL(req.url || '/', 'http://localhost');
  let pathName = url.pathname;
  try { pathName = decodeURIComponent(pathName); } catch { /* keep the raw path */ }
  if (pathName.includes('..')) return htmlReply('Bad request\n', 400);
  const pathL = lower(pathName);
  const query = url.search.slice(1);
  const action = lower(req.headers.soapaction || '');
  const apps = scanDir(root);
  const api = marketplaceApi(req, pathL, query, apps);
  if (api) return api;
  const guid = findGuid(pathL);
  const app = guid ? apps.find((a) => a.id === guid) : null;
  const wantIcon = pathL.includes('largewebicon') || pathL.includes('screenshot') || pathL.endsWith('.png');
  const pkgId = pathL.startsWith('/pkg/') ? pathL.slice(5).split('/')[0] : '';
  const pkgApp = pkgId ? findAny(apps, pkgId) : null;
  const wantCab = pathL.includes('/pkg/') || pathL.includes('/download/') || pathL.includes('/package/') || pathL.endsWith('.cab');
  if ((app && wantIcon && !wantCab) || (pkgApp && wantIcon)) {
    const icon = (app || pkgApp).iconPath;
    if (!icon) return { code: 404, type: 'text/plain; charset=utf-8', body: 'No icon\n' };
    return { code: 200, type: 'image/png', file: icon };
  }
  const cabApp = (app && wantCab) ? app : pkgApp;
  if (cabApp && wantCab) {
    return { code: 200, type: 'application/vnd.ms-cab-compressed', file: cabApp.cabPath, downloadName: cabApp.fileName };
  }
  if (pathL.startsWith('/app/') && app) return htmlReply(pageHtml([app], '', app));
  const bootstrap = pathL.includes('bootstrap') || action.includes('bootstrap') || action.includes('federation') ||
    action.includes('clientconfig') || action.includes('clientupdate');
  const catalog = pathL.includes('catalog') || action.includes('catalog') || action.includes('search') || action.includes('application');
  if (bootstrap && !catalog) {
    const version = clientVersion((req.bodyText || '') + ' ' + query);
    const inner = '<BootstrapResponse xmlns="http://marketplace.windowsmobile.com/2009/02">' +
      '<ClientUpdate available="false"/>' +
      '<LatestClientVersion>' + xmlEscape(version) + '</LatestClientVersion>' +
      '<Catalog>' + xmlEscape(publicBase(req) + '/') + '</Catalog>' +
      '<Files>' + xmlEscape(publicBase(req) + '/') + '</Files>' +
      '<Store>' + xmlEscape(publicBase(req) + '/') + '</Store></BootstrapResponse>';
    const soap = action || (req.headers['content-type'] || '').includes('soap');
    const body = soap
      ? '<?xml version="1.0" encoding="utf-8"?>\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>' + inner + '</s:Body></s:Envelope>'
      : '<?xml version="1.0" encoding="utf-8"?>\n' + inner;
    return xmlReply(body);
  }
  if (pathL.includes('login') || pathL.includes('passport')) return htmlReply(loginHtml());
  const q = queryParam(query, 'q') || queryParam(query, 'search');
  const storefront = pathL === '/' || pathL === '/index.html' || pathL === '/default.aspx' || pathL === '/search';
  if (storefront || pathL === '/apps') return htmlReply(pageHtml(matching(apps, q), q, null));
  return htmlReply(pageHtml([], '', null), 404);
}

function safeFileName(name) {
  const o = String(name || '').replace(/[\u0000-\u001f"\\/]/g, '_');
  return o || 'installer.cab';
}

function send(res, out) {
  const headers = { 'Cache-Control': 'no-cache', Connection: 'close' };
  if (out.file) {
    const st = fs.statSync(out.file);
    headers['Content-Type'] = out.type;
    headers['Content-Length'] = st.size;
    if (out.downloadName) headers['Content-Disposition'] = 'attachment; filename="' + safeFileName(out.downloadName) + '"';
    res.writeHead(out.code, headers);
    fs.createReadStream(out.file).pipe(res);
    return;
  }
  const buf = Buffer.from(out.body || '', 'utf8');
  headers['Content-Type'] = out.type || 'text/plain; charset=utf-8';
  headers['Content-Length'] = buf.length;
  res.writeHead(out.code || 200, headers);
  res.end(buf);
}

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let n = 0;
    req.on('data', (c) => {
      n += c.length;
      if (n > limit) { reject(new Error('large')); req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

function start(opts = {}) {
  const root = path.resolve(opts.dir || env.MARKET_DIR || path.join(process.cwd(), 'cabinets'));
  const host = opts.host || env.HOST || '0.0.0.0';
  const port = opts.port !== undefined ? opts.port : num(env.PORT, 8080);
  const cert = opts.tlsCert || env.TLS_CERT || '';
  const key = opts.tlsKey || env.TLS_KEY || '';
  const onRequest = async (req, res) => {
    try {
      const body = await readBody(req, 1024 * 1024);
      req.bodyText = body.toString('utf8');
      send(res, respond(req, root));
    } catch (e) {
      if (!res.headersSent) {
        const msg = e.message === 'large' ? 'Request too large\n' : 'Bad request\n';
        res.writeHead(e.message === 'large' ? 413 : 400, { 'Content-Type': 'text/plain; charset=utf-8', Connection: 'close' });
        res.end(msg);
      }
    }
  };
  const server = cert && key
    ? https.createServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key) }, onRequest)
    : http.createServer(onRequest);
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => resolve(server));
  });
}

if (require.main === module) {
  start().then((server) => {
    const a = server.address();
    console.log('marketplace cabinets on http://' + a.address + ':' + a.port);
  }).catch((e) => {
    console.error(e.message);
    process.exit(1);
  });
}

module.exports = { start, scanDir, appId };
