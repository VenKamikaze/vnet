// SPDX-License-Identifier: MIT
//
// Shared pieces of the browser tests: a relay started on loopback ports,
// native relay_peer processes, the Emscripten page served over HTTP, and
// Chromium pages (Playwright) running the same relay_peer program.
//
//   --relay-bin PATH --peer-bin PATH --web-dir DIR [--count N]
//
// Needs `playwright` (require() must find it; set NODE_PATH if it is
// installed elsewhere) and a Chromium that Playwright can launch.
'use strict';
const { spawn } = require('child_process');
const http = require('http');
const fs = require('fs');
const path = require('path');
const readline = require('readline');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 ? process.argv[i + 1] : def;
}
const RELAY_BIN = arg('relay-bin');
const PEER_BIN = arg('peer-bin');
const WEB_DIR = arg('web-dir');
const COUNT = parseInt(arg('count', '150'), 10);
if (!RELAY_BIN) {
  console.error('usage: <test>.js --relay-bin PATH [--peer-bin PATH --web-dir DIR] [--count N]');
  process.exit(2);
}
// (--peer-bin and --web-dir are needed only by the tests that run relay_peer.)

let failures = 0;
function check(cond, what) {
  if (cond) console.log('  ok    ' + what);
  else { console.log('  FAIL  ' + what); failures++; }
}
const failureCount = () => failures;
const withTimeout = (p, ms, what) => Promise.race([
  p, new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for ' + what)), ms))]);

// ---- child processes

function startRelay(extraArgs = []) {
  const child = spawn(RELAY_BIN, ['-udp', '127.0.0.1:0', '-http', '127.0.0.1:0', '-stats', '127.0.0.1:0', ...extraArgs],
    { stdio: ['ignore', 'pipe', 'inherit'], env: { PATH: process.env.PATH } });
  const rl = readline.createInterface({ input: child.stdout });
  const ready = new Promise((resolve, reject) => {
    rl.on('line', (line) => {
      const m = /^LISTEN udp=(\S+) http=(\S+) stats=(\S+)(?: wt=(\S*) wt_cert_sha256=(\S*))?/.exec(line);
      if (m) resolve({ child, udp: m[1], http: m[2], stats: m[3], wt: m[4] || '', certHash: m[5] || '' });
    });
    child.on('exit', (c) => reject(new Error('relay exited early: ' + c)));
  });
  return withTimeout(ready, 15000, 'the relay to start');
}

function getJSON(hostport, p) {
  return new Promise((resolve, reject) => {
    http.get({ host: hostport.split(':')[0], port: hostport.split(':')[1], path: p }, (res) => {
      let b = '';
      res.on('data', (d) => (b += d));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}

// A native relay_peer. Resolves {code, result()} once it has entered its room.
function startPeer(relayUdp, room, extraArgs = []) {
  const args = ['--relay', relayUdp, '--count', String(COUNT), '--timeout', '40000', ...extraArgs];
  if (room) args.push('--room', room);
  const child = spawn(PEER_BIN, args, { stdio: ['ignore', 'pipe', 'inherit'] });
  const rl = readline.createInterface({ input: child.stdout });
  const peer = { child, code: null, roomLine: null, result: null };
  peer.exited = new Promise((resolve) => child.on('exit', (c) => resolve(c)));
  peer.entered = new Promise((resolve, reject) => {
    rl.on('line', (line) => {
      let m;
      if ((m = /^ROOM (\S+) (\S+) (\S+)/.exec(line))) { peer.code = m[1]; peer.roomLine = line; resolve(peer); }
      if ((m = /^RESULT (.*)/.exec(line))) { peer.result = JSON.parse(m[1]); resolve(peer); }
    });
    child.on('exit', () => resolve(peer));
  });
  return peer;
}

// ---- static files for the Emscripten output

function serveWeb() {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const f = path.join(WEB_DIR, path.normalize(u.pathname).replace(/^(\.\.[/\\])+/, ''));
    if (!f.startsWith(path.resolve(WEB_DIR)) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) {
      res.writeHead(404); res.end('not found'); return;
    }
    res.writeHead(200, { 'Content-Type': types[path.extname(f)] || 'application/octet-stream' });
    fs.createReadStream(f).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

// ---- browser pages

// extra: more query parameters for the page (backend, wt, cert, wttimeout,
// maxlen, verbose ...); initScript: JavaScript run in the page before its own.
async function openPeerPage(browser, webOrigin, wsUrl, room, extra = {}, initScript = null) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  // Count the WebSockets the page opens: a page that got WebTransport must open none.
  await page.addInitScript(() => {
    window.__wsOpened = 0;
    const Orig = window.WebSocket;
    window.WebSocket = function (...a) { window.__wsOpened++; return new Orig(...a); };
    window.WebSocket.prototype = Orig.prototype;
    for (const k of ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED']) window.WebSocket[k] = Orig[k];
  });
  if (initScript) await page.addInitScript(initScript);
  const q = new URLSearchParams({ relay: wsUrl, count: String(COUNT), timeout: '40000', ...extra });
  if (room) q.set('room', room);
  await page.goto(`${webOrigin}/relay_peer.html?${q}`);
  return { ctx, page, errors };
}
const pageResult = (p) =>
  withTimeout(p.page.waitForFunction(() => window.vnetResult, null, { timeout: 45000, polling: 20 }).then((h) => h.jsonValue()), 50000, 'the page result');
const pageRoom = async (p) => {
  await p.page.waitForFunction(() => window.vnetLog.some((l) => /^ROOM /.test(l)), null, { timeout: 20000, polling: 20 });
  const line = await p.page.evaluate(() => window.vnetLog.find((l) => /^ROOM /.test(l)));
  return { code: line.split(' ')[1], line };
};


module.exports = {
  chromium: require('playwright').chromium,
  arg, RELAY_BIN, PEER_BIN, WEB_DIR, COUNT,
  check, failureCount, withTimeout,
  startRelay, getJSON, startPeer, serveWeb, openPeerPage, pageResult, pageRoom,
};
