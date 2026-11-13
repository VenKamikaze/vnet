// SPDX-License-Identifier: MIT
//
// Browser test for datagrams sent in a burst (brief 15). Chromium (Playwright)
// runs the Emscripten build of examples/burst_probe.c as the SENDER; native
// burst_probe processes are the receivers and count what arrives, through a
// real relay on loopback with no impairment.
//
//   1. A burst of more than 16 datagrams in one task (one JavaScript task: the
//      whole of a step runs inside one call of C code) loses nothing, over
//      WebTransport and over WebSocket.
//   2. A game-like load (one datagram to each of 3 or 7 peers every 25 ms, plus
//      a few acks and resends, also with several tics in one task) loses
//      nothing and reports no drops. The numbers are printed.
//   3. Back-pressure is real and bounded: with a datagram writer whose writes
//      never settle (the browser stops sending), and with a relay that is
//      frozen (SIGSTOP), the queue stays bounded and the datagrams past the
//      bound are dropped and counted (VNET_EAGAIN, vnet_stats.tx_dropped, and
//      Module.vnetWT.s[id].drops); and when the relay then goes away the
//      session still fails.
//
//   node wt_burst.test.js --relay-bin PATH --probe-bin PATH --web-dir DIR
'use strict';
const { spawn } = require('child_process');
const readline = require('readline');
const { chromium, arg, check, failureCount, withTimeout, startRelay, serveWeb } = require('./harness');

const PROBE_BIN = arg('probe-bin');
if (!PROBE_BIN) { console.error('--probe-bin is required'); process.exit(2); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The native receiver. Resolves with its RESULT line once it exits.
function receiver(udp, code, extra = []) {
  const child = spawn(PROBE_BIN, ['--relay', udp, '--room', code, '--timeout', '60000', ...extra],
    { stdio: ['ignore', 'pipe', 'inherit'] });
  const rl = readline.createInterface({ input: child.stdout });
  let result = null;
  rl.on('line', (l) => { const m = /^RESULT (.*)/.exec(l); if (m) result = JSON.parse(m[1]); });
  // 'close', not 'exit': all of the output has been read by then.
  const exited = new Promise((resolve) => child.on('close', resolve));
  return { child, done: exited.then(() => result) };
}

// The browser sender: a page running burst_probe.html. `params` become the
// program's options. Resolves once it has printed its ROOM line.
async function openSender(browser, origin, t, params, initScript = null) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  if (initScript) await page.addInitScript(initScript);
  const q = new URLSearchParams({ ...t, timeout: '60000' });
  for (const a of params) q.append('arg', a);
  await page.goto(`${origin}/burst_probe.html?${q}`);
  await page.waitForFunction(() => window.vnetLog.some((l) => /^ROOM /.test(l)), null, { timeout: 20000, polling: 20 });
  const room = (await page.evaluate(() => window.vnetLog.find((l) => /^ROOM /.test(l)))).split(' ');
  return { ctx, page, errors, code: room[1], transport: room[3] };
}
const senderResult = (s, ms = 60000) =>
  withTimeout(s.page.waitForFunction(() => window.vnetResult, null, { timeout: ms, polling: 20 }).then((h) => h.jsonValue()), ms + 5000, 'the sender result');
// What the page's WebTransport session says about itself (0 drops for WebSocket).
const jsState = (s) => s.page.evaluate(() => {
  const T = window.Module && Module.vnetWT;
  const x = T && Object.values(T.s)[0];
  return x ? { drops: x.drops, queued: x.dgT ? x.dgT.length : null, state: x.state } : null;
});

// One run: a sender with `params`, K native receivers. Returns everything measured.
async function run(browser, origin, relay, t, K, params, initScript = null) {
  const sender = await openSender(browser, origin, t, ['--peers', String(K), ...params], initScript);
  const rs = [];
  for (let i = 0; i < K; i++) rs.push(receiver(relay.udp, sender.code));
  const res = await senderResult(sender);
  const recv = await withTimeout(Promise.all(rs.map((r) => r.done)), 30000, 'the receivers');
  const js = await jsState(sender);
  await sender.ctx.close();
  return { sender: res, recv, js, transport: sender.transport, errors: sender.errors };
}
const lostOf = (r) => r.recv.reduce((a, x) => a + (x ? x.expect - x.distinct : 1e9), 0);
const summary = (r) => `attempted ${r.sender.attempted}, accepted ${r.sender.accepted}, dropped ${r.sender.eagain} (tx_dropped ${r.sender.tx_dropped}), ` +
  `received ${r.recv.reduce((a, x) => a + (x ? x.distinct : 0), 0)}, lost ${lostOf(r)}, most in one task ${r.sender.max_per_step}`;
function allArrived(r, what) {
  const ok = r.sender.ok && r.recv.every((x) => x && x.ok && x.distinct === x.expect && x.expect > 0);
  check(ok && r.sender.eagain === 0 && r.sender.tx_dropped === 0 && r.sender.accepted === r.sender.attempted && lostOf(r) === 0,
    `${what}: ${summary(r)}`);
  check(r.errors.length === 0, 'no page errors ' + r.errors.join('; '));
}

async function main() {
  const relay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
  const web = await serveWeb();
  const origin = `http://127.0.0.1:${web.address().port}`;
  const wt = { backend: 'wt', relay: `https://${relay.wt}/vnet`, cert: relay.certHash, verbose: '1' };
  const ws = { backend: 'ws', relay: `ws://${relay.http}/vnet`, verbose: '1' };
  const browser = await chromium.launch();
  console.log(`relay udp=${relay.udp} wt=${wt.relay}; chromium ${browser.version()}`);
  const extraRelays = [];

  try {
    // ---- 1. bursts
    for (const [name, t] of [['WebTransport', wt], ['WebSocket', ws]]) {
      console.log(`${name}: bursts of datagrams in one task all arrive`);
      // 22 datagrams to each of 3 receivers in one task: 66, more than 16 (the old
      // limit of datagrams in flight) and than 64. Three rounds, 200 ms apart.
      const r = await run(browser, origin, relay, t, 3, ['--mode', 'burst', '--burst', '22', '--rounds', '3', '--payload', '100']);
      check(r.transport === (name === 'WebTransport' ? 'relay-wt' : 'relay-ws'), `the sender is on ${r.transport}`);
      check(r.sender.max_per_step === 66, `${r.sender.max_per_step} datagrams were sent in one step`);
      allArrived(r, `${name}, 3 x 22 in one task, 3 times`);
      check(r.recv.every((x) => x && x.distinct === 66), `each receiver got all 66: ${JSON.stringify(r.recv)}`);
      if (r.js) check(r.js.drops === 0, 'the page counted no drops');
    }
    console.log('WebTransport: one big burst, 3 x 85 = 255 datagrams in one task');
    {
      const r = await run(browser, origin, relay, wt, 3, ['--mode', 'burst', '--burst', '85', '--rounds', '1', '--payload', '100']);
      allArrived(r, 'one burst of 255');
    }

    // ---- 2. game-like load
    const games = [
      ['3 peers, 3 extra, one tic per task', 3, 3, 1],
      ['7 peers, 3 extra, one tic per task', 7, 3, 1],
      ['3 peers, 3 extra, 3 tics in one task (a frame that fell behind)', 3, 3, 3],
      ['7 peers, 3 extra, 3 tics in one task', 7, 3, 3],
    ];
    for (const [what, K, extra, catchup] of games) {
      console.log(`WebTransport, game load every 25 ms: ${what}`);
      const r = await run(browser, origin, relay, wt, K,
        ['--mode', 'game', '--ticks', '160', '--interval', '25', '--extra', String(extra), '--catchup', String(catchup), '--payload', '60']);
      allArrived(r, what);
    }

    // ---- 3. back-pressure
    console.log('WebTransport: writes that never settle (the browser stops sending): bounded, dropped, counted');
    {
      // The datagram writer passes writes through until window.__stall is set; then
      // write() returns a promise that never settles, as a stalled connection's does.
      const STALLABLE = () => {
        const Orig = window.WebTransport;
        window.__writes = 0;
        window.WebTransport = function (...a) {
          const real = new Orig(...a);
          const dg = real.datagrams;
          const fake = {
            get maxDatagramSize() { return dg.maxDatagramSize; },
            get readable() { return dg.readable; },
            get writable() {
              return { getWriter() {
                const w = dg.writable.getWriter();
                return { write: (c) => { window.__writes++; return window.__stall ? new Promise(() => {}) : w.write(c); },
                  get desiredSize() { return w.desiredSize; } };
              } };
            },
          };
          return new Proxy(real, { get(t, k) { if (k === 'datagrams') return fake; const v = t[k]; return typeof v === 'function' ? v.bind(t) : v; } });
        };
      };
      const sender = await openSender(browser, origin, wt, ['--peers', '1', '--mode', 'burst', '--burst', '64', '--rounds', '40', '--gap', '30', '--payload', '100'], STALLABLE);
      await sender.page.evaluate(() => { window.__stall = true; window.__writes = 0; });
      const rcv = receiver(relay.udp, sender.code, ['--timeout', '15000']);
      const r = await senderResult(sender);
      const js = await jsState(sender);
      const writes = await sender.page.evaluate(() => window.__writes);
      check(r.ok && r.attempted === 64 * 40, `the sender made ${r.attempted} send calls (2560 asked)`);
      check(writes >= r.accepted && writes <= 1100, `the page was given ${writes} writes in all (${r.accepted} datagrams accepted, and a few HELLO answers)`);
      check(r.accepted > 0 && r.accepted <= 1024, `at most 1024 datagrams were queued behind a stalled writer (${r.accepted})`);
      check(r.eagain === r.attempted - r.accepted && r.eagain > 1500, `the rest were refused with VNET_EAGAIN (${r.eagain})`);
      check(r.tx_dropped === r.eagain, `vnet_stats.tx_dropped counts them (${r.tx_dropped})`);
      check(js && js.drops === r.eagain && js.queued <= 1024, `the page's own count agrees: ${JSON.stringify(js)}`);
      rcv.child.kill('SIGKILL');
      await sender.ctx.close();
    }

    console.log('WebTransport: the relay stops reading (frozen), then goes away');
    {
      const frozenRelay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
      extraRelays.push(frozenRelay);
      const t = { backend: 'wt', relay: `https://${frozenRelay.wt}/vnet`, cert: frozenRelay.certHash, verbose: '1' };
      // A game-like sender that keeps going; the relay is frozen once it runs.
      const sender = await openSender(browser, origin, t,
        ['--peers', '1', '--mode', 'game', '--ticks', '100000', '--interval', '25', '--extra', '9', '--payload', '100']);
      const rcv = receiver(frozenRelay.udp, sender.code, ['--timeout', '90000']);
      await sender.page.waitForFunction(() => { const x = Object.values(Module.vnetWT.s)[0]; return x && x.state === 1; }, null, { timeout: 10000, polling: 20 });
      await sleep(500);
      const before = await jsState(sender);
      check(before.drops === 0, `no drops while the relay reads (${before.drops})`);
      frozenRelay.child.kill('SIGSTOP');
      let js = null;
      for (let i = 0; i < 400; i++) {
        await sleep(50);
        js = await jsState(sender);
        if (js && js.drops > 200) break;
      }
      check(js && js.drops > 200, `the page drops datagrams once the relay stops reading (${js && js.drops} so far)`);
      await sleep(2000);
      const later = await jsState(sender);
      check(later.queued <= 1024 && later.queued > 0, `the queue stays bounded: ${later.queued} unsettled writes after ${later.drops} drops`);
      check(later.state === 1, 'the session is still up while the relay is merely frozen (as before)');
      const t0 = Date.now();
      frozenRelay.child.kill('SIGKILL');
      rcv.child.kill('SIGKILL');
      const r = await senderResult(sender, 80000);
      check(r.ok === false && /closed|relay/.test(r.error), `the session fails when the relay is gone: ${JSON.stringify(r)} after ${Date.now() - t0} ms`);
      check(r.tx_dropped > 200, `vnet_stats.tx_dropped saw the drops (${r.tx_dropped})`);
      await sender.ctx.close();
    }
  } finally {
    await browser.close();
    web.close();
    relay.child.kill('SIGTERM');
    for (const r of extraRelays) { try { r.child.kill('SIGKILL'); } catch (e) { /* gone */ } }
  }
}

main().then(() => {
  console.log(failureCount() ? `\n${failureCount()} check(s) FAILED` : '\nall datagram burst checks passed');
  process.exit(failureCount() ? 1 : 0);
}, (e) => { console.error('ERROR:', e); process.exit(1); });
