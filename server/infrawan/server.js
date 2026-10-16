#!/usr/bin/env node
// InfraWAN: a small relay that makes the Internet look like one room full of infrared ports.
//
// PocketStylus tunnels Windows Mobile's IrDA traffic (IrLAP frames, see docs/research-irda.md) to
// this server over a WebSocket. Every frame a device sends is passed to everyone else in the same
// room, the way IR light reaches every port facing it; the devices' own IrDA stacks sort out who
// is talking to whom. The server never looks inside the frames.
//
// No dependencies: Node's http/https/crypto and a minimal RFC 6455 WebSocket implementation.
//
//   node server.js                  start (settings from the environment, see README.md)
//   node server.js token <user>     print the token for <user> (needs INFRAWAN_SECRET)
//
// Protocol (version 1), one WebSocket per device, path /infrawan:
//   client -> server  text   {"type":"hello","version":1,"room":R,"user":U,"token":T,"name":N}  (first message)
//                     binary 0x01, IrLAP frame (A, C, I; no FCS), at most 4096 bytes
//   server -> client  text   {"type":"welcome","id":ID,"room":R,"peers":[{"id":..,"name":..}],"limits":{..}}
//                     text   {"type":"join","id":..,"name":..} / {"type":"leave","id":..}
//                     text   {"type":"error","code":"auth"|"proto"|"full"|"rate"|"busy","message":..} then close
//                     binary 0x01, sender id (u32 little-endian), IrLAP frame
'use strict';

const http = require('http');
const https = require('https');
const crypto = require('crypto');
const fs = require('fs');

const env = process.env;
const num = (v, d) => (v === undefined || v === '' ? d : Number(v));
const cfg = {
  host: env.HOST || '0.0.0.0',
  port: num(env.PORT, 7300),
  path: env.INFRAWAN_PATH || '/infrawan',
  secret: env.INFRAWAN_SECRET || '',
  usersFile: env.INFRAWAN_USERS || '',
  open: env.INFRAWAN_OPEN === '1',          // no authentication (a LAN or a test only)
  tlsCert: env.TLS_CERT || '',
  tlsKey: env.TLS_KEY || '',
  maxRoom: num(env.INFRAWAN_MAX_ROOM, 32),           // devices per room
  maxPerUser: num(env.INFRAWAN_MAX_PER_USER, 4),     // connections per user
  maxPerIp: num(env.INFRAWAN_MAX_PER_IP, 16),        // connections per address
  bytesPerSec: num(env.INFRAWAN_BYTES_PER_SEC, 64 * 1024),  // per device; SIR tops out near 11.5 KB/s
  burstBytes: num(env.INFRAWAN_BURST_BYTES, 256 * 1024),
  framesPerSec: num(env.INFRAWAN_FRAMES_PER_SEC, 400),
  burstFrames: num(env.INFRAWAN_BURST_FRAMES, 800),
  maxFrame: 4096,
  helloTimeoutMs: 10000,
  pingMs: 25000,
  trustProxy: env.INFRAWAN_TRUST_PROXY === '1',      // take the client address from X-Forwarded-For
};

// ---- Tokens ----

function userToken(user) {
  return crypto.createHmac('sha256', cfg.secret).update('infrawan:v1:' + user).digest('hex').slice(0, 32);
}

let usersCache = { mtime: 0, users: {} };
function fileUsers() {
  if (!cfg.usersFile) return {};
  try {
    const st = fs.statSync(cfg.usersFile);
    if (st.mtimeMs !== usersCache.mtime) usersCache = { mtime: st.mtimeMs, users: JSON.parse(fs.readFileSync(cfg.usersFile, 'utf8')) };
  } catch (e) {
    log('users file: ' + e.message);
  }
  return usersCache.users;
}

function sameString(a, b) {
  const x = Buffer.from(String(a)), y = Buffer.from(String(b));
  return x.length === y.length && crypto.timingSafeEqual(x, y);
}

function authorized(user, token) {
  if (cfg.open) return true;
  const listed = fileUsers()[user];
  if (listed !== undefined && sameString(listed, token)) return true;
  return cfg.secret !== '' && sameString(userToken(user), token);
}

// ---- Abuse limits ----

const failures = new Map();  // ip -> { count, since, blockedUntil }
function failed(ip) {
  const now = Date.now();
  const f = failures.get(ip) || { count: 0, since: now, blockedUntil: 0 };
  if (now - f.since > 60000) { f.count = 0; f.since = now; }
  if (++f.count >= 5) f.blockedUntil = now + 5 * 60000;
  failures.set(ip, f);
}
function blocked(ip) {
  const f = failures.get(ip);
  return !!f && f.blockedUntil > Date.now();
}
setInterval(() => {
  const now = Date.now();
  for (const [ip, f] of failures) if (f.blockedUntil < now && now - f.since > 60000) failures.delete(ip);
}, 60000).unref();

class Bucket {
  constructor(rate, burst) { this.rate = rate; this.burst = burst; this.level = burst; this.t = Date.now(); }
  take(n) {
    const now = Date.now();
    this.level = Math.min(this.burst, this.level + ((now - this.t) / 1000) * this.rate);
    this.t = now;
    if (this.level < n) return false;
    this.level -= n;
    return true;
  }
}

// ---- WebSocket (RFC 6455, server side) ----

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function frame(op, payload) {
  const n = payload.length;
  let head;
  if (n < 126) head = Buffer.from([0x80 | op, n]);
  else if (n < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | op; head[1] = 126; head.writeUInt16BE(n, 2); }
  else { head = Buffer.alloc(10); head[0] = 0x80 | op; head[1] = 127; head.writeBigUInt64BE(BigInt(n), 2); }
  return Buffer.concat([head, payload]);
}

class Peer {
  constructor(socket, ip) {
    this.socket = socket;
    this.ip = ip;
    this.id = 0;
    this.room = null;
    this.user = '';
    this.name = '';
    this.buf = Buffer.alloc(0);
    this.parts = [];
    this.partOp = 0;
    this.alive = true;
    this.closed = false;
    this.bytes = new Bucket(cfg.bytesPerSec, cfg.burstBytes);
    this.frames = new Bucket(cfg.framesPerSec, cfg.burstFrames);
    this.dropped = 0;
    socket.setNoDelay(true);
    socket.on('data', (d) => this.onData(d));
    socket.on('close', () => this.onClose());
    socket.on('error', () => {});
    this.helloTimer = setTimeout(() => this.fail('proto', 'no hello'), cfg.helloTimeoutMs);
  }

  send(op, payload) {
    if (this.closed) return false;
    // A device that can't keep up loses frames, as IR does; the others are not held back.
    if (op === 2 && this.socket.writableLength > 1 << 20) { this.dropped++; return false; }
    this.socket.write(frame(op, payload));
    return true;
  }
  sendJson(o) { this.send(1, Buffer.from(JSON.stringify(o))); }

  close(code) {
    if (this.closed) return;
    const p = Buffer.alloc(2);
    p.writeUInt16BE(code || 1000, 0);
    this.send(8, p);
    this.closed = true;
    this.socket.end();
    setTimeout(() => this.socket.destroy(), 2000).unref();
  }

  fail(code, message) {
    if (code === 'auth') failed(this.ip);
    this.sendJson({ type: 'error', code, message });
    this.close(code === 'proto' ? 1002 : 1008);
  }

  onData(d) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    for (;;) {
      if (this.buf.length < 2) return;
      const b0 = this.buf[0], b1 = this.buf[1];
      const fin = (b0 & 0x80) !== 0, op = b0 & 15, masked = (b1 & 0x80) !== 0;
      let n = b1 & 127, off = 2;
      if (n === 126) { if (this.buf.length < 4) return; n = this.buf.readUInt16BE(2); off = 4; }
      else if (n === 127) {
        if (this.buf.length < 10) return;
        const big = this.buf.readBigUInt64BE(2);
        if (big > 65536n) return this.fail('proto', 'message too large');
        n = Number(big); off = 10;
      }
      if (!masked) return this.fail('proto', 'client frames must be masked');
      if (n > 65536) return this.fail('proto', 'message too large');
      if (this.buf.length < off + 4 + n) return;
      const mask = this.buf.subarray(off, off + 4);
      const p = Buffer.from(this.buf.subarray(off + 4, off + 4 + n));
      for (let i = 0; i < n; i++) p[i] ^= mask[i & 3];
      this.buf = this.buf.subarray(off + 4 + n);
      if (op === 8) { this.close(1000); return; }
      if (op === 9) { this.send(10, p); continue; }
      if (op === 10) { this.alive = true; continue; }
      if (op === 1 || op === 2) { this.partOp = op; this.parts = [p]; }
      else if (op === 0 && this.partOp) this.parts.push(p);
      else return this.fail('proto', 'bad opcode');
      if (!fin) {
        if (this.parts.reduce((a, x) => a + x.length, 0) > 65536) return this.fail('proto', 'message too large');
        continue;
      }
      const msg = this.parts.length === 1 ? this.parts[0] : Buffer.concat(this.parts);
      const mop = this.partOp;
      this.parts = [];
      this.partOp = 0;
      this.alive = true;
      if (mop === 1) this.onText(msg.toString('utf8'));
      else this.onBinary(msg);
      if (this.closed) return;
    }
  }

  onText(text) {
    let m;
    try { m = JSON.parse(text); } catch (e) { return this.fail('proto', 'bad JSON'); }
    if (m.type === 'ping') return this.sendJson({ type: 'pong' });
    if (m.type !== 'hello' || this.room) return;  // later control messages are ignored
    clearTimeout(this.helloTimer);
    const room = String(m.room || '');
    const user = String(m.user || '');
    if (m.version !== 1) return this.fail('proto', 'protocol version 1 expected');
    if (!/^[\w .\-]{1,64}$/.test(room)) return this.fail('proto', 'room: 1-64 letters, digits, space . _ -');
    if (!/^[\w.\-@]{1,64}$/.test(user)) return this.fail('proto', 'user: 1-64 letters, digits . _ - @');
    if (!authorized(user, String(m.token || ''))) {
      log(`auth failed for "${user}" from ${this.ip}`);
      return this.fail('auth', 'unknown user or wrong token');
    }
    let perUser = 0;
    for (const p of peers) if (p.user === user && p.room) perUser++;
    if (perUser >= cfg.maxPerUser) return this.fail('busy', 'too many connections for this user');
    const members = rooms.get(room) || new Set();
    if (members.size >= cfg.maxRoom) return this.fail('full', 'room is full');
    this.user = user;
    this.name = String(m.name || user).replace(/[\u0000-\u001f]/g, '').slice(0, 64);
    this.id = nextId++;
    if (nextId > 0xFFFFFFF0) nextId = 1;
    this.room = room;
    this.sendJson({
      type: 'welcome', id: this.id, room,
      peers: [...members].map((p) => ({ id: p.id, name: p.name })),
      limits: { bytesPerSec: cfg.bytesPerSec, framesPerSec: cfg.framesPerSec, maxFrame: cfg.maxFrame },
    });
    for (const p of members) p.sendJson({ type: 'join', id: this.id, name: this.name });
    members.add(this);
    rooms.set(room, members);
    log(`join #${this.id} "${this.name}" (${user}) room "${room}" from ${this.ip}; ${members.size} in room`);
  }

  onBinary(msg) {
    if (!this.room) return this.fail('proto', 'hello first');
    if (msg.length < 3 || msg[0] !== 0x01 || msg.length - 1 > cfg.maxFrame) return;  // not an IrLAP frame
    if (!this.frames.take(1) || !this.bytes.take(msg.length)) {
      // Over the limit: the frame is lost (IrLAP retransmits); persistent abuse ends the session.
      if (++this.dropped > 5000) this.fail('rate', 'sending too fast');
      stats.limited++;
      return;
    }
    const out = Buffer.alloc(msg.length + 4);
    out[0] = 0x01;
    out.writeUInt32LE(this.id, 1);
    msg.copy(out, 5, 1);
    const wire = frame(2, out);
    for (const p of rooms.get(this.room) || []) {
      if (p === this || p.closed) continue;
      if (p.socket.writableLength > 1 << 20) { p.dropped++; continue; }
      p.socket.write(wire);
    }
    stats.frames++;
    stats.bytes += msg.length - 1;
  }

  onClose() {
    this.closed = true;
    clearTimeout(this.helloTimer);
    peers.delete(this);
    if (!this.room) return;
    const members = rooms.get(this.room);
    if (!members) return;
    members.delete(this);
    for (const p of members) p.sendJson({ type: 'leave', id: this.id });
    if (members.size === 0) rooms.delete(this.room);
    log(`leave #${this.id} "${this.name}" room "${this.room}"; ${members.size} left`);
  }
}

// ---- Server ----

const peers = new Set();
const rooms = new Map();  // name -> Set<Peer>
const stats = { started: new Date().toISOString(), frames: 0, bytes: 0, limited: 0 };
let nextId = 1;

function log(s) { console.log(new Date().toISOString() + ' ' + s); }

function clientIp(req) {
  if (cfg.trustProxy && req.headers['x-forwarded-for']) return String(req.headers['x-forwarded-for']).split(',')[0].trim();
  return req.socket.remoteAddress || '?';
}

function onRequest(req, res) {
  if (req.url === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: true, rooms: rooms.size, devices: [...peers].filter((p) => p.room).length, ...stats }));
    return;
  }
  res.writeHead(426, { 'content-type': 'text/plain' });
  res.end('InfraWAN relay: connect with a WebSocket to ' + cfg.path + '\n');
}

function onUpgrade(req, socket, head) {
  const ip = clientIp(req);
  const reject = (status, text) => { socket.end(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\n\r\n`); };
  if (req.url.split('?')[0] !== cfg.path) return reject(404, 'Not Found');
  if (blocked(ip)) return reject(429, 'Too Many Requests');
  let perIp = 0;
  for (const p of peers) if (p.ip === ip) perIp++;
  if (perIp >= cfg.maxPerIp) return reject(429, 'Too Many Requests');
  const key = req.headers['sec-websocket-key'];
  if (!key || String(req.headers.upgrade).toLowerCase() !== 'websocket') return reject(400, 'Bad Request');
  const accept = crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  const peer = new Peer(socket, ip);
  peers.add(peer);
  if (head && head.length) peer.onData(head);  // a client that did not wait for the 101
}

function main() {
  if (process.argv[2] === 'token') {
    const user = process.argv[3];
    if (!cfg.secret || !user) { console.error('usage: INFRAWAN_SECRET=... node server.js token <user>'); process.exit(2); }
    console.log(userToken(user));
    return;
  }
  if (!cfg.open && !cfg.secret && !cfg.usersFile) {
    console.error('Set INFRAWAN_SECRET (per-user tokens) and/or INFRAWAN_USERS (a JSON file of user: token), ' +
                  'or INFRAWAN_OPEN=1 for a test relay without authentication.');
    process.exit(2);
  }
  const server = cfg.tlsCert
    ? https.createServer({ cert: fs.readFileSync(cfg.tlsCert), key: fs.readFileSync(cfg.tlsKey) }, onRequest)
    : http.createServer(onRequest);
  server.on('upgrade', onUpgrade);
  server.listen(cfg.port, cfg.host, () =>
    log(`InfraWAN relay on ${cfg.tlsCert ? 'wss' : 'ws'}://${cfg.host}:${cfg.port}${cfg.path}` +
        (cfg.open ? ' (open: no authentication)' : '')));
  // Keepalive: a device that stops answering pings (a dropped mobile link) leaves its room.
  setInterval(() => {
    for (const p of peers) {
      if (!p.alive) { p.socket.destroy(); continue; }
      p.alive = false;
      p.send(9, Buffer.alloc(0));
    }
  }, cfg.pingMs).unref();
  const stop = () => { log('stopping'); server.close(); for (const p of peers) p.close(1001); setTimeout(() => process.exit(0), 500); };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}

if (require.main === module) main();
module.exports = { userToken, cfg };
