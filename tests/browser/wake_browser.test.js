// SPDX-License-Identifier: MIT
//
// Browser test for the wake callback (vnet_config.wake): Chromium (Playwright)
// runs the Emscripten build of examples/wake_probe.c, an echo host whose wake
// callback is the only thing that can make it poll, against a real relay; a
// native wake_probe (the guest, VNET_BACKEND_RELAY_AUTO over UDP) sends it
// datagrams. For WebSocket and WebTransport:
//
//   frozen:  the page polls until it is in its room and then stops for good (a
//            hidden tab's loop). 20 datagrams arrive. The callback must fire for
//            each of them while vnet_poll() is never called again.
//   driven:  the page has no main loop at all (requestAnimationFrame is stubbed
//            out too). Each wake schedules one poll; the whole exchange (the
//            transport opening, the handshake, the room, the datagrams and their
//            echoes) runs on wake-ups. A 1 Hz timer polls too, as a real page
//            must: vnet's own timers (connect retries, the fallback, handshake
//            timeout, keepalives) are not events and run only inside vnet_poll().
//
//   node wake_browser.test.js --relay-bin PATH --probe-bin PATH --web-dir DIR
'use strict';
const { spawn } = require('child_process');
const readline = require('readline');
const { chromium, arg, check, failureCount, withTimeout, startRelay, serveWeb } = require('./harness');

const PROBE_BIN = arg('probe-bin');
const N = 20;
if (!PROBE_BIN) { console.error('--probe-bin is required'); process.exit(2); }

async function openProbe(browser, origin, params) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  // A page with no animation frames: nothing can drive a loop from rAF.
  await page.addInitScript(() => { window.requestAnimationFrame = () => 0; });
  await page.goto(`${origin}/wake_probe.html?${new URLSearchParams({ verbose: '1', timeout: '60000', ...params })}`);
  return { ctx, page, errors };
}

const room = async (p) => {
  await p.page.waitForFunction(() => window.vnetLog.some((l) => /^ROOM /.test(l)), null, { timeout: 20000, polling: 20 });
  return (await p.page.evaluate(() => window.vnetLog.find((l) => /^ROOM /.test(l)))).split(' ')[1];
};
const counters = (p) => p.page.evaluate(() => window.vnetWake);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The native guest: joins `code`, pings N times, reports how many were echoed.
function guest(udp, code) {
  const child = spawn(PROBE_BIN, ['--relay', udp, '--room', code, '--count', String(N), '--linger', '1500', '--timeout', '30000'],
    { stdio: ['ignore', 'pipe', 'inherit'] });
  const rl = readline.createInterface({ input: child.stdout });
  const result = new Promise((resolve) => rl.on('line', (l) => { const m = /^RESULT (.*)/.exec(l); if (m) resolve(JSON.parse(m[1])); }));
  const exited = new Promise((resolve) => child.on('exit', resolve));
  return withTimeout(Promise.all([result, exited]).then(([r]) => r), 40000, 'the native guest');
}

async function main() {
  const relay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
  const web = await serveWeb();
  const origin = `http://127.0.0.1:${web.address().port}`;
  const browser = await chromium.launch();
  const transports = {
    'WebSocket': { backend: 'ws', relay: `ws://${relay.http}/vnet` },
    'WebTransport': { backend: 'wt', relay: `https://${relay.wt}/vnet`, cert: relay.certHash },
    'auto (WebTransport, then WebSocket)': { backend: 'auto', relay: `ws://${relay.http}/vnet`, wt: `https://${relay.wt}/vnet`, cert: relay.certHash },
  };
  try {
    for (const [name, t] of Object.entries(transports)) {
      console.log(`${name}: the loop is frozen; the callback still fires for every datagram`);
      {
        const p = await openProbe(browser, origin, { ...t, mode: 'freeze' });
        const code = await room(p);
        await sleep(300);
        const before = await counters(p);
        check(before.ready === 1 && before.polls > 0, `the page polled its way into room ${code} (${JSON.stringify(before)})`);
        check(before.wakes > 0, `the callback fired while connecting and entering the room (${before.wakes} wakes)`);
        const r = await guest(relay.udp, code);
        const after = await counters(p);
        check(r.ok === true && r.sent === N, `the native guest sent ${N} datagrams: ${JSON.stringify(r)}`);
        check(after.wakes - before.wakes >= N, `the callback fired for each of them (${after.wakes - before.wakes} wakes >= ${N})`);
        check(after.polls === before.polls, `and vnet_poll() was not called again (${before.polls} polls before, ${after.polls} after)`);
        check(after.received === 0 && r.echoed === 0, 'so nothing was received or echoed: the loop really was frozen');
        check(p.errors.length === 0, 'no page errors ' + p.errors.join('; '));
        await p.ctx.close();
      }
      console.log(`${name}: no loop at all; wake-ups alone drive the connection and the exchange`);
      {
        const p = await openProbe(browser, origin, { ...t, mode: 'drive' });
        const code = await room(p);
        const r = await guest(relay.udp, code);
        const c = await counters(p);
        check(c.ready === 1, 'the page opened its transport, handshook and entered a room with nothing but wake-ups');
        check(r.ok === true && r.sent === N && r.echoed === N, `the native guest got all ${N} echoes: ${JSON.stringify(r)}`);
        check(c.received === N && c.echoed === N, `the page received and echoed ${N} (${JSON.stringify(c)})`);
        check(c.wakes > N && c.polls > 0 && c.polls <= c.wakes + c.timers, `it polled only after wake-ups and its 1 Hz timer (${c.polls} polls for ${c.wakes} wakes and ${c.timers} timer ticks)`);
        check(p.errors.length === 0, 'no page errors ' + p.errors.join('; '));
        await p.ctx.close();
      }
    }
  } finally {
    await browser.close();
    web.close();
    relay.child.kill('SIGTERM');
  }
}

main().then(() => {
  console.log(failureCount() ? `\n${failureCount()} check(s) FAILED` : '\nall wake checks passed');
  process.exit(failureCount() ? 1 : 0);
}, (e) => { console.error('ERROR:', e); process.exit(1); });
