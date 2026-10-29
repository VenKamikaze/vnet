// SPDX-License-Identifier: MIT
//
// Browser integration test for the vnet relay: Chromium (Playwright) running
// the Emscripten build of examples/relay_peer.c shares rooms with a native
// relay_peer process, through a real relay started on loopback ports (UDP and
// WebSocket; the WebTransport transport is tested in wt_browser.test.js).
//
//   node relay_browser.test.js --relay-bin PATH --peer-bin PATH --web-dir DIR [--count N]
'use strict';
const { chromium, COUNT, check, failureCount, withTimeout, startRelay, getJSON, startPeer, serveWeb,
  openPeerPage, pageResult, pageRoom } = require('./harness');

async function main() {
  const relay = await startRelay();
  const web = await serveWeb();
  const webOrigin = `http://127.0.0.1:${web.address().port}`;
  const wsUrl = `ws://${relay.http}/vnet`;
  const browser = await chromium.launch();
  console.log(`relay udp=${relay.udp} ws=${wsUrl}; ${COUNT} datagrams each way`);

  try {
    // 1. A native client creates the room, a browser joins it.
    console.log('native host, browser guest');
    {
      const host = startPeer(relay.udp, null);
      await withTimeout(host.entered, 15000, 'the native host to enter its room');
      check(host.code && /^[A-HJ-NP-Z2-9]{6}$/.test(host.code), `room code ${host.code}`);
      check(/ 10\.0\.0\.1:0 /.test(host.roomLine), 'the creator is 10.0.0.1 (' + host.roomLine + ')');
      const guest = await openPeerPage(browser, webOrigin, wsUrl, host.code);
      const r = await pageResult(guest);
      check(r.ok === true, 'browser guest result ' + JSON.stringify(r));
      check(r.sent === COUNT && r.received === COUNT, `browser sent ${r.sent}, received ${r.received} in order`);
      const code = await withTimeout(host.exited, 20000, 'the native host to finish');
      check(code === 0 && host.result && host.result.ok && host.result.received === COUNT && host.result.sent === COUNT,
        'native host result ' + JSON.stringify(host.result));
      check(guest.errors.length === 0, 'no page errors ' + guest.errors.join('; '));
      const line = await guest.page.evaluate(() => window.vnetLog.find((l) => /^ROOM /.test(l)));
      check(/ 10\.0\.0\.2:0 relay-ws$/.test(line), 'the browser guest is 10.0.0.2 on relay-ws (' + line + ')');
      await guest.ctx.close();
    }

    // 2. A browser creates the room, a native client joins it.
    console.log('browser host, native guest');
    {
      const host = await openPeerPage(browser, webOrigin, wsUrl, null);
      const { code, line } = await pageRoom(host);
      check(/ 10\.0\.0\.1:0 relay-ws$/.test(line), `browser host created ${code} as 10.0.0.1`);
      const guest = startPeer(relay.udp, code);
      const gr = await withTimeout(guest.entered.then(() => guest.exited), 45000, 'the native guest to finish');
      check(gr === 0 && guest.result && guest.result.ok && guest.result.received === COUNT, 'native guest result ' + JSON.stringify(guest.result));
      const r = await pageResult(host);
      check(r.ok === true && r.sent === COUNT && r.received === COUNT, 'browser host result ' + JSON.stringify(r));
      check(host.errors.length === 0, 'no page errors ' + host.errors.join('; '));
      await host.ctx.close();
    }

    // 3. Two browsers, one native bystander in another room (must see nothing).
    console.log('two browsers (WebSocket to WebSocket)');
    {
      const host = await openPeerPage(browser, webOrigin, wsUrl, null);
      const { code } = await pageRoom(host);
      const guest = await openPeerPage(browser, webOrigin, wsUrl, code);
      const [rh, rg] = await Promise.all([pageResult(host), pageResult(guest)]);
      check(rh.ok && rg.ok && rh.received === COUNT && rg.received === COUNT, `host ${JSON.stringify(rh)} guest ${JSON.stringify(rg)}`);
      await host.ctx.close(); await guest.ctx.close();
    }

    // 4. Failures reach the page as a status, not a hang.
    console.log('failure paths');
    {
      const nope = await openPeerPage(browser, webOrigin, wsUrl, 'ZZZZZZ');
      const r = await pageResult(nope);
      check(r.ok === false && /no such room/.test(r.error), 'unknown room: ' + JSON.stringify(r));
      await nope.ctx.close();
      const bad = await openPeerPage(browser, webOrigin, 'ws://127.0.0.1:1/vnet', null);
      const r2 = await pageResult(bad);
      check(r2.ok === false && /relay unreachable/.test(r2.error), 'relay down: ' + JSON.stringify(r2));
      await bad.ctx.close();
    }

    // 5. A relay that answers 503 because this address has too many sockets
    // pending (many browsers behind one NAT): the page retries and gets in.
    console.log('503 on connect is retried');
    {
      const busy = await startRelay(['-max-pending-ws-per-ip', '1']);
      try {
        const busyWs = `ws://${busy.http}/vnet`;
        // Hold the only pending slot with a socket that never says anything.
        const hold = new WebSocket(busyWs);
        await new Promise((res, rej) => { hold.onopen = res; hold.onerror = () => rej(new Error('hold socket failed')); });
        const page = await openPeerPage(browser, webOrigin, busyWs, null);
        let refused = 0;
        for (let i = 0; i < 300 && refused < 1; i++) {
          await new Promise((r) => setTimeout(r, 10));
          refused = (await getJSON(busy.stats, '/stats')).ws_refused;
        }
        check(refused >= 1, `the relay refused the page's first connection (${refused} refusals)`);
        hold.close();  // the slot frees; the page's retry gets in
        const { code, line } = await pageRoom(page);
        check(/ 10\.0\.0\.1:0 relay-ws$/.test(line), `the page got into room ${code} after being refused`);
        check(page.errors.length === 0, 'no page errors');
        await page.ctx.close();
      } finally {
        busy.child.kill('SIGTERM');
      }
    }

    // Closing a page closes its WebSocket; the relay notices asynchronously.
    let st = await getJSON(relay.stats, '/stats');
    for (let i = 0; i < 500 && st.sessions !== 0; i++) {
      await new Promise((r) => setTimeout(r, 10));
      st = await getJSON(relay.stats, '/stats');
    }
    check(st.rooms_created === 3, `3 rooms were created (${st.rooms_created})`);
    check(st.malformed === 0 && st.rate_limited === 0, `relay saw no malformed frames and no rate limiting (${st.malformed}, ${st.rate_limited})`);
    check(st.sessions === 0 && st.rooms === 0, `all sessions left and every room closed (${st.sessions} sessions, ${st.rooms} rooms)`);
  } finally {
    await browser.close();
    web.close();
    relay.child.kill('SIGTERM');
  }
}

main().then(() => {
  console.log(failureCount() ? `\n${failureCount()} check(s) FAILED` : '\nall browser checks passed');
  process.exit(failureCount() ? 1 : 0);
}, (e) => { console.error('ERROR:', e); process.exit(1); });
