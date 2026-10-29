// SPDX-License-Identifier: MIT
//
// Loss behaviour of the browser transports (a measurement, not a pass/fail
// test; see doco/11-vnet-webtransport.md). Two Chromium pages talk through a
// relay: A sends numbered datagrams at a fixed rate, B records when each one
// arrives. The pages speak the relay protocol directly, in plain JavaScript,
// so the numbers belong to the browser transport and the relay, not to the
// vnet client library.
//
//   WebTransport  B reaches the relay through a UDP proxy that drops a share
//                 of the QUIC packets in both directions, so the loss is
//                 real, in the network: QUIC retransmits stream data, the
//                 datagrams are simply lost.
//   WebSocket     TCP cannot be degraded from user space, and `tc netem`
//                 (needs NET_ADMIN and a kernel with CONFIG_NET_SCH_NETEM) is
//                 not available where this was written. So A drops the same
//                 share of its sends itself ("application-level" loss, which
//                 is what vnet's impairment layer does): that shows loss, not
//                 the head-of-line stalls real packet loss causes on TCP.
//
//   node transport_loss.js --relay-bin PATH [--loss 10] [--rate 100] [--seconds 10] [--size 200]
'use strict';
const dgram = require('dgram');
const http = require('http');
const { chromium, startRelay, getJSON, arg, withTimeout } = require('./harness');

const LOSS = parseFloat(arg('loss', '10'));
const RATE = parseInt(arg('rate', '100'), 10);
const SECONDS = parseInt(arg('seconds', '10'), 10);
const SIZE = parseInt(arg('size', '200'), 10);

// A UDP proxy that drops each packet with probability p, in both directions.
function lossyProxy(upstreamPort, p, seed = 12345) {
  let x = seed >>> 0;
  const rnd = () => { x = (x * 1664525 + 1013904223) >>> 0; return x / 4294967296; };
  const stats = { toRelay: 0, toClient: 0, droppedToRelay: 0, droppedToClient: 0 };
  const front = dgram.createSocket('udp4');
  const clients = new Map(); // "addr:port" -> upstream socket
  front.on('message', (msg, rinfo) => {
    const key = rinfo.address + ':' + rinfo.port;
    let up = clients.get(key);
    if (!up) {
      up = dgram.createSocket('udp4');
      up.on('message', (m) => {
        stats.toClient++;
        if (rnd() < p / 100) { stats.droppedToClient++; return; }
        front.send(m, rinfo.port, rinfo.address);
      });
      clients.set(key, up);
    }
    stats.toRelay++;
    if (rnd() < p / 100) { stats.droppedToRelay++; return; }
    up.send(msg, upstreamPort, '127.0.0.1');
  });
  return new Promise((resolve) => front.bind(0, '127.0.0.1', () =>
    resolve({ port: front.address().port, stats, close: () => { front.close(); clients.forEach((s) => s.close()); } })));
}

// The page: plain JS clients of the relay protocol (doco/relay-protocol.md).
const PAGE = `<!doctype html><meta charset=utf-8><script>
const hex = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
function frameCreate(nonce) { const f = new Uint8Array(10); f[0] = 1; f[1] = 1; f.set(nonce, 2); return f; }
function frameJoin(nonce, code) { const f = new Uint8Array(16); f[0] = 2; f[1] = 1; f.set(nonce, 2); f.set(new TextEncoder().encode(code), 10); return f; }
const ctl = (f) => { const m = new Uint8Array(2 + f.length); m[1] = f.length; m.set(f, 2); return m; };

// A session over WebTransport or WebSocket, with the same small interface.
async function open(kind, url, hash, code) {
  const nonce = crypto.getRandomValues(new Uint8Array(8));
  const s = { kind, onData: null, rttSamples: [], pendingKA: [], room: null };
  const onFrame = (f) => {
    if (f[0] === 0x81) { s.room = { code: new TextDecoder().decode(f.subarray(14 + 16, 14 + 16 + 6)), octet: f[13] }; s.entered(); }
    else if (f[0] === 0x04) { const t0 = s.pendingKA.shift(); if (t0 !== undefined) s.rttSamples.push(performance.now() - t0); }
    else if (f[0] === 0x10 && s.onData) s.onData(f);
  };
  const entered = new Promise((r) => (s.entered = r));
  const hs = code ? frameJoin(nonce, code) : frameCreate(nonce);
  if (kind === 'wt') {
    const wt = new WebTransport(url, { serverCertificateHashes: [{ algorithm: 'sha-256', value: hex(hash) }] });
    await wt.ready;
    const bs = await wt.createBidirectionalStream();
    const w = bs.writable.getWriter();
    const dw = wt.datagrams.writable.getWriter();
    s.sendData = (f) => dw.write(f);
    s.sendKeepalive = () => { s.pendingKA.push(performance.now()); return w.write(ctl(new Uint8Array([4]))); };
    s.close = () => wt.close();
    let acc = new Uint8Array(0);
    const cr = bs.readable.getReader();
    (async () => { for (;;) { const r = await cr.read(); if (r.done) return;
      const j = new Uint8Array(acc.length + r.value.length); j.set(acc); j.set(r.value, acc.length); acc = j;
      while (acc.length >= 2 && acc.length >= 2 + ((acc[0] << 8) | acc[1])) { const n = (acc[0] << 8) | acc[1]; onFrame(acc.slice(2, 2 + n)); acc = acc.subarray(2 + n); } } })();
    const dr = wt.datagrams.readable.getReader();
    (async () => { for (;;) { const r = await dr.read(); if (r.done) return; onFrame(r.value); } })();
    await w.write(ctl(hs));
  } else {
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('ws failed')); });
    s.sendData = (f) => ws.send(f);
    s.sendKeepalive = () => { s.pendingKA.push(performance.now()); ws.send(new Uint8Array([4])); };
    s.close = () => ws.close();
    ws.onmessage = (e) => onFrame(new Uint8Array(e.data));
    ws.send(hs);
  }
  await entered;
  return s;
}

window.runReceiver = async (kind, url, hash, code) => {
  const s = await open(kind, url, hash, code);
  window.__s = s;
  window.__rx = [];
  s.onData = (f) => {
    const dv = new DataView(f.buffer, f.byteOffset, f.byteLength);
    window.__rx.push([dv.getUint32(9), dv.getFloat64(13), performance.timeOrigin + performance.now()]);
  };
  return s.room;
};
window.startKeepalives = (everyMs) => { window.__ka = setInterval(() => window.__s.sendKeepalive(), everyMs); };
window.runSender = async (kind, url, hash, code, rate, seconds, size, dropPct) => {
  const s = code ? await open(kind, url, hash, code) : await open(kind, url, hash, null);
  window.__tx = s;
  return s.room;
};
window.send = async (dstOctet, rate, seconds, size, dropPct) => {
  const s = window.__tx;
  const n = rate * seconds;
  const t0 = performance.now();
  let sent = 0, skipped = 0;
  for (let i = 0; i < n; i++) {
    const due = t0 + (i * 1000) / rate;
    while (performance.now() < due) await new Promise((r) => setTimeout(r, 1));
    if (dropPct > 0 && Math.random() * 100 < dropPct) { skipped++; continue; } // application-level loss
    const f = new Uint8Array(size);
    f[0] = 0x10; f[1] = 10; f[4] = dstOctet; f[6] = 1; f[8] = 1;
    const dv = new DataView(f.buffer);
    dv.setUint32(9, i);
    dv.setFloat64(13, performance.timeOrigin + performance.now());
    s.sendData(f);
    sent++;
  }
  return { n, sent, skipped };
};
window.collect = () => { clearInterval(window.__ka); return { rx: window.__rx, rtt: window.__s.rttSamples }; };
window.finish = () => { try { window.__s.close(); } catch (e) {} try { window.__tx.close(); } catch (e) {} };
</script>`;

const pct = (a, q) => { if (!a.length) return NaN; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(q * s.length))]; };
const r1 = (x) => (Number.isFinite(x) ? x.toFixed(1) : 'n/a');

async function run(browser, web, kind, urlA, urlB, hash, dropAtSender) {
  const ctxA = await browser.newContext(), ctxB = await browser.newContext();
  const A = await ctxA.newPage(), Bp = await ctxB.newPage();
  await A.goto(web); await Bp.goto(web);
  const roomA = await A.evaluate(([k, u, h]) => window.runSender(k, u, h, null), [kind, urlA, hash]);
  const roomB = await Bp.evaluate(([k, u, h, c]) => window.runReceiver(k, u, h, c), [kind, urlB, hash, roomA.code]);
  await Bp.evaluate(() => window.startKeepalives(100));
  const t0 = Date.now();
  const tx = await A.evaluate(([o, r, s, z, d]) => window.send(o, r, s, z, d), [roomB.octet, RATE, SECONDS, SIZE, dropAtSender]);
  await new Promise((r) => setTimeout(r, 1500)); // what is still in flight, and the keep-alive answers
  const { rx, rtt } = await Bp.evaluate(() => window.collect());
  await A.evaluate(() => window.finish()); await Bp.evaluate(() => window.finish());
  await ctxA.close(); await ctxB.close();
  // rx entries: [seq, sentAtMs, arrivedAtMs]
  const seen = new Set(), lat = [], arrivals = [];
  let dup = 0, reordered = 0, maxSeq = -1;
  for (const [seq, sent, arr] of rx) {
    if (seen.has(seq)) { dup++; continue; }
    seen.add(seq);
    lat.push(arr - sent);
    arrivals.push(arr);
    if (seq < maxSeq) reordered++; else maxSeq = seq;
  }
  const gaps = arrivals.slice(1).map((t, i) => t - arrivals[i]);
  return { kind, offered: tx.n, sent: tx.sent, appSkipped: tx.skipped, received: seen.size, dup, reordered,
    lossPct: 100 * (1 - seen.size / tx.n), latMs: { p50: pct(lat, 0.5), p99: pct(lat, 0.99), max: Math.max(...lat) },
    gapMs: { p50: pct(gaps, 0.5), p99: pct(gaps, 0.99), max: Math.max(...gaps) },
    controlRttMs: rtt.length ? { n: rtt.length, p50: pct(rtt, 0.5), p99: pct(rtt, 0.99), max: Math.max(...rtt) } : null,
    seconds: (Date.now() - t0) / 1000 };
}

async function main() {
  const relay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
  const web = http.createServer((q, r) => { r.writeHead(200, { 'content-type': 'text/html' }); r.end(PAGE); });
  await new Promise((r) => web.listen(0, '127.0.0.1', r));
  const webUrl = `http://127.0.0.1:${web.address().port}/`;
  const wtPort = parseInt(relay.wt.split(':').pop(), 10);
  const proxy = await lossyProxy(wtPort, LOSS);
  const browser = await chromium.launch();
  console.log(`Chromium ${browser.version()}, ${RATE} datagrams/s of ${SIZE} bytes for ${SECONDS} s, loss ${LOSS}%`);
  const wtDirect = `https://${relay.wt}/vnet`, wtLossy = `https://127.0.0.1:${proxy.port}/vnet`, ws = `ws://${relay.http}/vnet`;
  const results = [];
  try {
    results.push({ name: 'WebTransport, no loss', ...(await run(browser, webUrl, 'wt', wtDirect, wtDirect, relay.certHash, 0)) });
    const before = { ...proxy.stats };
    results.push({ name: `WebTransport, ${LOSS}% of QUIC packets lost on B's path`, ...(await run(browser, webUrl, 'wt', wtDirect, wtLossy, relay.certHash, 0)) });
    results.at(-1).proxy = { packetsToRelay: proxy.stats.toRelay - before.toRelay, dropToRelay: proxy.stats.droppedToRelay - before.droppedToRelay,
      packetsToClient: proxy.stats.toClient - before.toClient, dropToClient: proxy.stats.droppedToClient - before.droppedToClient };
    results.push({ name: 'WebSocket, no loss', ...(await run(browser, webUrl, 'ws', ws, ws, relay.certHash, 0)) });
    results.push({ name: `WebSocket, ${LOSS}% dropped by the sender (application level)`, ...(await run(browser, webUrl, 'ws', ws, ws, relay.certHash, LOSS)) });
  } finally {
    proxy.close();
    await browser.close();
    web.close();
  }
  const st = await getJSON(relay.stats, '/stats').catch(() => null);
  relay.child.kill('SIGTERM');
  for (const r of results) {
    console.log(`\n${r.name}`);
    console.log(`  datagrams offered ${r.offered}, sent ${r.sent}, received ${r.received} (${r1(r.lossPct)}% lost), duplicates ${r.dup}, reordered ${r.reordered}`);
    console.log(`  one-way latency ms: p50 ${r1(r.latMs.p50)}, p99 ${r1(r.latMs.p99)}, max ${r1(r.latMs.max)}`);
    console.log(`  gap between delivered datagrams ms (sent every ${(1000 / RATE).toFixed(0)}): p50 ${r1(r.gapMs.p50)}, p99 ${r1(r.gapMs.p99)}, max ${r1(r.gapMs.max)}`);
    if (r.controlRttMs) console.log(`  keepalive round trip on the reliable control path ms (${r.controlRttMs.n}): p50 ${r1(r.controlRttMs.p50)}, p99 ${r1(r.controlRttMs.p99)}, max ${r1(r.controlRttMs.max)}`);
    if (r.proxy) console.log(`  proxy: ${r.proxy.dropToClient}/${r.proxy.packetsToClient} packets dropped toward the browser, ${r.proxy.dropToRelay}/${r.proxy.packetsToRelay} toward the relay`);
  }
  if (st) console.log(`\nrelay: panics ${st.panics}, wt_datagram_drops ${st.wt_datagram_drops}, ws_queue_drops ${st.ws_queue_drops}, malformed ${st.malformed}`);
  console.log('\n' + JSON.stringify({ chromium: results.length && 'see above', results }, null, 0));
}

main().then(() => process.exit(0), (e) => { console.error('ERROR:', e); process.exit(1); });
