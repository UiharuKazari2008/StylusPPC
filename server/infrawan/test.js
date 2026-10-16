// Self-test for the InfraWAN relay: node test.js (Node 22+, which has a WebSocket client built in).
// Starts server.js on a free port with a test secret and checks rooms, relaying, auth and limits.
'use strict';

const { spawn } = require('child_process');
const path = require('path');
const assert = require('assert');

const SECRET = 'test-secret';
process.env.INFRAWAN_SECRET = SECRET;
const { userToken } = require('./server.js');

const port = 20000 + Math.floor(Math.random() * 20000);
const url = `ws://127.0.0.1:${port}/infrawan`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function client(room, user, token, name) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    const c = { ws, texts: [], frames: [], id: 0, closed: false };
    ws.onopen = () => ws.send(JSON.stringify({ type: 'hello', version: 1, room, user, token: token ?? userToken(user), name: name || user }));
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') {
        const m = JSON.parse(e.data);
        c.texts.push(m);
        if (m.type === 'welcome') { c.id = m.id; c.peers = m.peers; resolve(c); }
        if (m.type === 'error') resolve(c);
      } else {
        const b = Buffer.from(e.data);
        c.frames.push({ from: b.readUInt32LE(1), frame: b.subarray(5) });
      }
    };
    ws.onclose = () => { c.closed = true; resolve(c); };
    ws.onerror = (e) => reject(e);
  });
}

function sendFrame(c, bytes) {
  c.ws.send(Buffer.concat([Buffer.from([1]), Buffer.from(bytes)]));
}

async function main() {
  const srv = spawn(process.execPath, [path.join(__dirname, 'server.js')], {
    env: { ...process.env, PORT: String(port), HOST: '127.0.0.1', INFRAWAN_SECRET: SECRET, INFRAWAN_FRAMES_PER_SEC: '100', INFRAWAN_BURST_FRAMES: '200' },
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  srv.stdout.on('data', (d) => process.env.VERBOSE && process.stdout.write('  server: ' + d));
  await sleep(500);
  try {
    // Rooms and presence
    const a = await client('lab', 'alice');
    assert.ok(a.id > 0, 'alice welcomed');
    assert.deepStrictEqual(a.peers, []);
    const b = await client('lab', 'bob', undefined, 'Bob PPC');
    assert.deepStrictEqual(b.peers, [{ id: a.id, name: 'alice' }]);
    const c = await client('elsewhere', 'carol');
    await sleep(100);
    assert.ok(a.texts.some((m) => m.type === 'join' && m.id === b.id && m.name === 'Bob PPC'), 'alice saw bob join');
    console.log('ok  rooms and presence');

    // Multicast within the room, never back to the sender, never to other rooms
    const xid = [0xff, 0x3f, 0x01, 1, 2, 3, 4, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00];
    sendFrame(a, xid);
    await sleep(200);
    assert.strictEqual(b.frames.length, 1, 'bob got the frame');
    assert.strictEqual(b.frames[0].from, a.id);
    assert.deepStrictEqual([...b.frames[0].frame], xid);
    assert.strictEqual(a.frames.length, 0, 'no echo');
    assert.strictEqual(c.frames.length, 0, 'other room untouched');
    const d = await client('lab', 'dave');
    sendFrame(b, [0xfe, 0xbf, 0x01]);
    await sleep(200);
    assert.strictEqual(a.frames.length, 1, 'alice got bob\'s frame');
    assert.strictEqual(d.frames.length, 1, 'dave got bob\'s frame');
    console.log('ok  multicast to the room');

    // Authentication
    const bad = await client('lab', 'mallory', 'not-a-token');
    assert.ok(bad.texts.some((m) => m.type === 'error' && m.code === 'auth'), 'wrong token refused');
    console.log('ok  authentication');

    // Frame rate limit: 100/s with a burst of 200; the rest is dropped
    const before = b.frames.length;
    for (let i = 0; i < 1000; i++) sendFrame(a, [0x10, 0x01, i & 255]);
    await sleep(500);
    const got = b.frames.length - before;
    assert.ok(got >= 200 && got < 400, `rate limited (${got} of 1000 passed)`);
    console.log(`ok  rate limit (${got} of 1000 frames passed)`);

    // Oversized and malformed frames are ignored
    const n = b.frames.length;
    a.ws.send(Buffer.alloc(5000, 1));
    a.ws.send(Buffer.from([2, 1, 2, 3]));
    await sleep(200);
    assert.strictEqual(b.frames.length, n, 'bad frames dropped');
    console.log('ok  malformed frames dropped');

    // Leaving
    b.ws.close();
    await sleep(300);
    assert.ok(a.texts.some((m) => m.type === 'leave' && m.id === b.id), 'alice saw bob leave');
    console.log('ok  leave');

    for (const x of [a, c, d]) x.ws.close();
    console.log('all tests passed');
  } finally {
    srv.kill();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
