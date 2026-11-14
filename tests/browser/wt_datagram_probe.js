// SPDX-License-Identifier: MIT
//
// A measurement tool (not a test): how does the browser's WebTransport datagram
// writer behave? Per-task bursts, how long a write() stays unsettled,
// writer.desiredSize, the defaults of datagrams.outgoingHighWaterMark and
// outgoingMaxAge, and what happens when the relay stops reading (SIGSTOP).
// It talks to a real relay (-wt -dev-cert) from a Chromium page; no vnet code
// is involved. brief 15 used it to find the cause of "datagrams lost in a burst".
//
//   node wt_datagram_probe.js --relay-bin PATH --web-dir ANY_EXISTING_DIR
'use strict';
const { chromium, startRelay, serveWeb } = require('./harness');

async function main() {
  const relay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
  const web = await serveWeb();
  const browser = await chromium.launch();
  const page = await (await browser.newContext()).newPage();
  await page.goto(`http://127.0.0.1:${web.address().port}/none`);
  const url = `https://${relay.wt}/vnet`;
  const hash = relay.certHash;
  const out = await page.evaluate(async ({ url, hash }) => {
    const R = {};
    const hex = new Uint8Array(hash.match(/../g).map((h) => parseInt(h, 16)));
    const open = async (extra = {}) => {
      const wt = new WebTransport(url, { serverCertificateHashes: [{ algorithm: 'sha-256', value: hex }], ...extra });
      await wt.ready;
      return wt;
    };
    const wt = await open();
    const dg = wt.datagrams;
    R.defaults = { outgoingHighWaterMark: dg.outgoingHighWaterMark, outgoingMaxAge: dg.outgoingMaxAge,
      incomingHighWaterMark: dg.incomingHighWaterMark, incomingMaxAge: dg.incomingMaxAge, maxDatagramSize: dg.maxDatagramSize };
    const w = dg.writable.getWriter();
    const tick = () => new Promise((r) => setTimeout(r, 0));
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const pkt = () => new Uint8Array(100).fill(7);

    // 1. Bursts of N datagrams in one synchronous run: how many writes have
    // settled by the end of the task (microtask), by the next task, and when?
    R.bursts = [];
    for (const n of [16, 64, 256]) {
      const t0 = performance.now();
      const settledAt = [];
      const desired = [];
      let sync = 0;
      const ps = [];
      for (let i = 0; i < n; i++) {
        ps.push(w.write(pkt()).then(() => settledAt.push(performance.now() - t0)));
        desired.push(w.desiredSize);
        sync++;
      }
      let micro = 0;
      await Promise.resolve();
      micro = settledAt.length;
      await tick();
      const nextTask = settledAt.length;
      await Promise.all(ps);
      R.bursts.push({ n, desiredSizeAfterEachWrite: [desired[0], desired[Math.min(15, n - 1)], desired[n - 1]],
        settledAfterMicrotask: micro, settledByNextTask: nextTask, allSettledMs: +(Math.max(...settledAt)).toFixed(2),
        desiredAfter: w.desiredSize });
      await sleep(50);
    }

    // 2. A game-like rhythm: every 25 ms one task writes 10 datagrams; how many
    // are still unsettled when the next task starts?
    const pending = [];
    let inflight = 0;
    for (let step = 0; step < 80; step++) {
      pending.push(inflight);
      for (let i = 0; i < 10; i++) { inflight++; w.write(pkt()).then(() => inflight--, () => inflight--); }
      await sleep(25);
    }
    R.gameLike = { stepsWithUnsettledAtTaskStart: pending.filter((x) => x > 0).length, maxUnsettledAtTaskStart: Math.max(...pending) };

    // 3. Does setting outgoingHighWaterMark / outgoingMaxAge work?
    R.setters = {};
    try { dg.outgoingHighWaterMark = 64; R.setters.hwmAfterSet = dg.outgoingHighWaterMark; } catch (e) { R.setters.hwmError = String(e); }
    try { dg.outgoingMaxAge = 100; R.setters.maxAgeAfterSet = dg.outgoingMaxAge; } catch (e) { R.setters.maxAgeError = String(e); }
    wt.close();
    return R;
  }, { url, hash });
  console.log(JSON.stringify(out, null, 1));

  // 4. The relay stops reading (SIGSTOP): QUIC still acks at the transport
  // level of the relay's kernel UDP socket? No: the process is frozen, so acks
  // stop, flow control and congestion control engage. Writes from the page:
  // how many settle, what is desiredSize, how many stay pending?
  await page.exposeFunction('signalRelay', (sig) => process.kill(relay.child.pid, sig));
  const frozen = async (maxAge) => page.evaluate(async ({ url, hash, maxAge }) => {
    const hex = new Uint8Array(hash.match(/../g).map((h) => parseInt(h, 16)));
    const wt = new WebTransport(url, { serverCertificateHashes: [{ algorithm: 'sha-256', value: hex }] });
    await wt.ready;
    if (maxAge) wt.datagrams.outgoingMaxAge = maxAge;
    const w = wt.datagrams.writable.getWriter();
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    let settled = 0, written = 0, rejected = 0;
    const samples = [];
    await window.signalRelay('SIGSTOP');
    for (let step = 0; step < 100; step++) {
      for (let i = 0; i < 20; i++) { written++; w.write(new Uint8Array(100)).then(() => settled++, () => rejected++); }
      await sleep(25);
      if (step % 20 === 19) samples.push({ step, written, settled, rejected, unsettled: written - settled - rejected, desiredSize: w.desiredSize });
    }
    await window.signalRelay('SIGCONT');
    wt.close();
    return samples;
  }, { url, hash, maxAge });
  console.log('relay frozen:', JSON.stringify(await frozen(0)));
  console.log('relay frozen, outgoingMaxAge = 200 ms:', JSON.stringify(await frozen(200)));
  await browser.close(); web.close(); relay.child.kill('SIGKILL');
}
main().catch((e) => { console.error(e); process.exit(1); });
