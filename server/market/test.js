// Self-test for the Marketplace cabinet server: node test.js
'use strict';

const assert = require('assert');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { start } = require('./server.js');

function get(port, urlPath, headers) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path: urlPath, method: 'GET', headers }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, type: res.headers['content-type'] || '', body: Buffer.concat(chunks) }));
    });
    req.on('error', reject);
    req.end();
  });
}

function post(port, urlPath, body) {
  return new Promise((resolve, reject) => {
    const req = http.request({
      host: '127.0.0.1', port, path: urlPath, method: 'POST',
      headers: { 'Content-Type': 'text/xml', 'Content-Length': Buffer.byteLength(body) },
    }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, body: Buffer.concat(chunks).toString('utf8') }));
    });
    req.on('error', reject);
    req.end(body);
  });
}

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'wmdroid-market-'));
  const cab = Buffer.from('MSCF-test-cabinet');
  fs.writeFileSync(path.join(dir, 'Hello_App.cab'), cab);
  fs.writeFileSync(path.join(dir, 'Hello_App.cab.ini'), 'title=Hello\npublisher=Test\nsummary=A greeting\n');
  const server = await start({ host: '127.0.0.1', port: 0, dir });
  const port = server.address().port;
  const host = '127.0.0.1:' + port;
  try {
    const page = await get(port, '/', { Host: host });
    assert.strictEqual(page.status, 200);
    const html = page.body.toString('utf8');
    assert.ok(html.includes('Hello'), 'storefront lists the cabinet');
    assert.ok(html.includes('/pkg/'), 'install link stays on this host');

    const cats = await get(port, '/appcatalog/02/2009/devices/phone/categories', { Host: host });
    const catXml = cats.body.toString('utf8');
    assert.ok(catXml.includes('ListResponseOfCategory'), 'category list');
    assert.ok(catXml.includes('Marketplace'), 'category name');

    const list = await get(port, '/appcatalog/02/2009/devices/phone/categories/1/apps?chunkSize=25', { Host: host });
    const listXml = list.body.toString('utf8');
    assert.ok(listXml.includes('ListOfAppSummary'), 'app list');
    assert.ok(listXml.includes('Hello'), 'app title');
    assert.ok(listXml.includes('<Price>0</Price>'), 'free');

    const miss = await get(port, '/appcatalog/02/2009/devices/phone/search?q=zzzz-no-such', { Host: host });
    assert.ok(!miss.body.toString('utf8').includes('Hello'), 'search miss');

    const details = await get(port, '/appcatalog/02/2009/devices/phone/apps/1', { Host: host });
    assert.ok(details.body.toString('utf8').includes('ResponseOfApplication'), 'details');
    assert.ok(details.body.toString('utf8').includes('OverAirDownloadAllowed'), 'over the air');

    const lic = await get(port, '/appcatalog/02/2009/deviceInstance/phone/apps/1/urlandlicense', { Host: host });
    const licXml = lic.body.toString('utf8');
    assert.ok(licXml.includes('ResponseOfDownloadUrl'), 'download url');
    const url = licXml.match(/<Url>([^<]+)<\/Url>/);
    assert.ok(url, 'url element');
    assert.ok(url[1].startsWith('http://' + host + '/pkg/'), 'package url is on this server: ' + url[1]);

    const pub = await get(port, '/appcatalog/02/2009/deviceInstance/phone/apps/1/urlandlicense', {
      Host: 'market.stylus.737.jp.net', 'X-Forwarded-Proto': 'https',
    });
    const pubUrl = pub.body.toString('utf8').match(/<Url>([^<]+)<\/Url>/);
    assert.strictEqual(pubUrl[1].startsWith('https://market.stylus.737.jp.net/pkg/'), true, 'public package url');

    const file = await get(port, url[1].slice(('http://' + host).length));
    assert.strictEqual(file.status, 200);
    assert.ok(file.type.includes('vnd.ms-cab-compressed'), 'cabinet type');
    assert.deepStrictEqual(file.body, cab, 'cabinet bytes');

    const upd = await post(port, '/update/02/2009/updateproducts', '<UpdateRequest/>');
    assert.ok(upd.body.includes('UpdateResponse'), 'update response');
    assert.ok(!upd.body.includes('<Mandatory>true</Mandatory>'), 'no forced client update');

    const gone = await get(port, '/appcatalog/02/2009/devices/phone/apps/999');
    assert.strictEqual(gone.status, 404, 'unknown app');

    const boot = await get(port, '/bootstrap/02/2009');
    assert.ok(boot.body.toString('utf8').includes('available="false"'), 'no client update');
    console.log('market server self-test: ok');
  } finally {
    server.close();
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
