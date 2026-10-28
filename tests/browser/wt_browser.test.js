// SPDX-License-Identifier: MIT
//
// Browser integration test for the WebTransport transport: Chromium
// (Playwright) running the Emscripten build of examples/relay_peer.c with
// VNET_BACKEND_RELAY_AUTO / _WT against a real relay with a development
// certificate (-dev-cert), whose hash the page passes as
// serverCertificateHashes. Native relay_peer processes are the other players.
//
//   node wt_browser.test.js --relay-bin PATH --peer-bin PATH --web-dir DIR [--count N]
//
// Chromium needs no flags for this: WebTransport to 127.0.0.1 with
// serverCertificateHashes works in a default Playwright Chromium.
'use strict';
const dgram = require('dgram');
const { chromium, COUNT, check, failureCount, withTimeout, startRelay, getJSON, startPeer, serveWeb,
  openPeerPage, pageResult, pageRoom } = require('./harness');

const roomLine = async (p) => (await pageRoom(p)).line;
const transportOf = (line) => line.split(' ')[3];
const logOf = (p) => p.page.evaluate(() => window.vnetLog.slice());
const wsOpened = (p) => p.page.evaluate(() => window.__wsOpened);
const has = (log, re) => log.some((l) => re.test(l));

// A page whose WebTransport behaves as the W3C text allows: a datagram longer
// than datagrams.maxDatagramSize (1024 here, as Chromium reports) is silently
// dropped. Chromium in practice delivers longer ones (see the first scenario).
const STRICT_DATAGRAMS = () => {
  const Orig = window.WebTransport;
  window.WebTransport = function (...a) {
    const real = new Orig(...a);
    const dg = real.datagrams;
    const fake = {
      get maxDatagramSize() { return 1024; },
      get readable() { return dg.readable; },
      get writable() {
        return {
          getWriter() {
            const w = dg.writable.getWriter();
            return { write: (c) => (c.length > 1024 ? Promise.resolve() : w.write(c)) };
          },
        };
      },
    };
    return new Proxy(real, {
      get(t, k) {
        if (k === 'datagrams') return fake;
        const v = t[k];
        return typeof v === 'function' ? v.bind(t) : v;
      },
    });
  };
};

// A UDP port that swallows everything: a firewall that drops QUIC.
function blackhole() {
  return new Promise((resolve) => {
    const s = dgram.createSocket('udp4');
    s.on('message', () => {});
    s.bind(0, '127.0.0.1', () => resolve(s));
  });
}

async function main() {
  const relay = await startRelay(['-wt', '127.0.0.1:0', '-dev-cert']);
  const noWT = await startRelay([]); // a relay that does not serve WebTransport
  const web = await serveWeb();
  const webOrigin = `http://127.0.0.1:${web.address().port}`;
  const wsUrl = `ws://${relay.http}/vnet`;
  const wtUrl = `https://${relay.wt}/vnet`;
  const auto = { backend: 'auto', wt: wtUrl, cert: relay.certHash, verbose: '1' };
  const browser = await chromium.launch();
  console.log(`relay udp=${relay.udp} ws=${wsUrl} wt=${wtUrl}; chromium ${browser.version()}; ${COUNT} datagrams each way`);
  const hole = await blackhole();
  const holePort = hole.address().port;
  let maxDatagramSize = null;

  try {
    console.log('auto: WebTransport is chosen, datagrams of every size flow');
    {
      // Messages up to 1100 bytes (VNET_MAX_DATAGRAM): longer than the 1024
      // Chromium reports as datagrams.maxDatagramSize.
      const host = startPeer(relay.udp, null, ['--maxlen', '1093']);
      await withTimeout(host.entered, 15000, 'the native host to enter its room');
      const guest = await openPeerPage(browser, webOrigin, wsUrl, host.code, { ...auto, maxlen: '1093' });
      const r = await pageResult(guest);
      const log = await logOf(guest);
      const line = await roomLine(guest);
      check(transportOf(line) === 'relay-wt', `the browser guest is on relay-wt (${line})`);
      check(r.ok === true && r.sent === COUNT && r.received === COUNT, `exchange, up to 1100 bytes a datagram, both ways: ${JSON.stringify(r)}`);
      const code = await withTimeout(host.exited, 20000, 'the native host to finish');
      check(code === 0 && host.result && host.result.ok && host.result.received === COUNT, 'native host result ' + JSON.stringify(host.result));
      check((await wsOpened(guest)) === 0, 'no WebSocket was opened');
      const m = log.map((l) => /maxDatagramSize (\d+)/.exec(l)).find(Boolean);
      maxDatagramSize = m ? parseInt(m[1], 10) : null;
      check(maxDatagramSize > 0, `Chromium reported datagrams.maxDatagramSize ${maxDatagramSize} (a full vnet datagram needs 1109)`);
      if (maxDatagramSize < 1109) {
        check(has(log, /a 1100 byte datagram fits over relay-wt/), 'the datagram-size check found that a full-size datagram fits anyway');
      }
      check(guest.errors.length === 0, 'no page errors ' + guest.errors.join('; '));
      await guest.ctx.close();
    }

    console.log('auto: a browser creates the room (derived WebTransport URL), a native client joins');
    {
      // relay is "wss://" at the WebTransport address: the WebTransport URL is
      // derived from it ("https://" with the same host, port and path). Nothing
      // listens for TLS WebSocket there, so only WebTransport can have worked.
      const derived = { backend: 'auto', cert: relay.certHash, verbose: '1' };
      const host = await openPeerPage(browser, webOrigin, `wss://${relay.wt}/vnet`, null, derived);
      const line = await roomLine(host);
      check(transportOf(line) === 'relay-wt' && / 10\.0\.0\.1:0 /.test(line), `derived URL: the browser host is 10.0.0.1 on relay-wt (${line})`);
      const code = line.split(' ')[1];
      const guest = startPeer(relay.udp, code);
      const gr = await withTimeout(guest.entered.then(() => guest.exited), 45000, 'the native guest to finish');
      check(gr === 0 && guest.result && guest.result.ok && guest.result.received === COUNT, 'native guest result ' + JSON.stringify(guest.result));
      const r = await pageResult(host);
      check(r.ok === true && r.sent === COUNT && r.received === COUNT, 'browser host result ' + JSON.stringify(r));
      check(has(await logOf(host), new RegExp(`trying WebTransport at https://${relay.wt.replace('.', '\\.')}/vnet`)), 'the log names the derived URL');
      await host.ctx.close();
    }

    console.log('a WebTransport browser and a WebSocket-only browser share a room');
    {
      const host = await openPeerPage(browser, webOrigin, wsUrl, null, auto);
      const { code, line } = await pageRoom(host);
      check(transportOf(line) === 'relay-wt', `host on relay-wt (${line})`);
      const guest = await openPeerPage(browser, webOrigin, wsUrl, code, { backend: 'ws' });
      const [rh, rg] = await Promise.all([pageResult(host), pageResult(guest)]);
      check(rh.ok && rg.ok && rh.received === COUNT && rg.received === COUNT, `host ${JSON.stringify(rh)} guest ${JSON.stringify(rg)}`);
      check(transportOf(await roomLine(guest)) === 'relay-ws', 'the other browser is on relay-ws');
      await host.ctx.close(); await guest.ctx.close();
    }

    console.log('two WebTransport browsers');
    {
      const host = await openPeerPage(browser, webOrigin, wsUrl, null, auto);
      const { code } = await pageRoom(host);
      const guest = await openPeerPage(browser, webOrigin, wsUrl, code, auto);
      const [rh, rg] = await Promise.all([pageResult(host), pageResult(guest)]);
      check(rh.ok && rg.ok && rh.received === COUNT && rg.received === COUNT, `host ${JSON.stringify(rh)} guest ${JSON.stringify(rg)}`);
      check(transportOf(await roomLine(guest)) === 'relay-wt' && (await wsOpened(host)) + (await wsOpened(guest)) === 0, 'both on relay-wt, no WebSocket');
      await host.ctx.close(); await guest.ctx.close();
    }

    // Each of these makes WebTransport fail in a different way. The page must
    // end up on WebSocket, with no change to the game, and still exchange everything.
    const fallbacks = [
      ['nothing listens on the WebTransport port', { wt: 'https://127.0.0.1:1/vnet' }, null, /falling back/],
      ['a relay that does not serve WebTransport', { wt: `https://${noWT.udp}/vnet` }, null, /falling back/],
      ['a firewall that drops QUIC (timeout)', { wt: `https://127.0.0.1:${holePort}/vnet`, wttimeout: '1500' }, null, /timed out.*falling back/],
      ['the wrong certificate hash', { cert: '00'.repeat(32) }, null, /falling back/],
      ['a browser without WebTransport', {}, () => { delete window.WebTransport; }, /WebTransport not available/],
      ['a browser that drops datagrams over maxDatagramSize', { maxlen: '1093' }, STRICT_DATAGRAMS, /full size do not fit/],
    ];
    for (const [what, extra, init, logRe] of fallbacks) {
      console.log(`auto falls back to WebSocket: ${what}`);
      const t0 = Date.now();
      const host = startPeer(relay.udp, null, ['--maxlen', '1093']);
      await withTimeout(host.entered, 15000, 'the native host to enter its room');
      const guest = await openPeerPage(browser, webOrigin, wsUrl, host.code, { ...auto, maxlen: '1093', ...extra }, init);
      const r = await pageResult(guest);
      const line = await roomLine(guest);
      const log = await logOf(guest);
      check(transportOf(line) === 'relay-ws', `the page is on relay-ws (${line}) after ${Date.now() - t0} ms`);
      check(r.ok === true && r.sent === COUNT && r.received === COUNT, `the exchange works over the fallback: ${JSON.stringify(r)}`);
      const code = await withTimeout(host.exited, 20000, 'the native host to finish');
      check(code === 0 && host.result && host.result.ok && host.result.received === COUNT, 'native host result ' + JSON.stringify(host.result));
      check(has(log, logRe), `the log says why: ${log.filter((l) => /fall|WebTransport|fit/.test(l)).join(' | ')}`);
      check((await wsOpened(guest)) === 1, 'exactly one WebSocket was opened');
      check(guest.errors.length === 0, 'no page errors ' + guest.errors.join('; '));
      await guest.ctx.close();
    }

    console.log('auto with a ws:// relay and no WebTransport URL goes straight to WebSocket');
    {
      const host = startPeer(relay.udp, null);
      await withTimeout(host.entered, 15000, 'the native host to enter its room');
      const guest = await openPeerPage(browser, webOrigin, wsUrl, host.code, { backend: 'auto', verbose: '1' });
      const r = await pageResult(guest);
      check(r.ok === true && transportOf(await roomLine(guest)) === 'relay-ws', 'on relay-ws: ' + JSON.stringify(r));
      check(has(await logOf(guest), /WebTransport not available/), 'the log says there was no WebTransport URL');
      await withTimeout(host.exited, 20000, 'the native host to finish');
      await guest.ctx.close();
    }

    console.log('the WebTransport-only backend does not fall back: it fails with a reason');
    {
      const closed = await openPeerPage(browser, webOrigin, wsUrl, null, { backend: 'wt', relay: 'https://127.0.0.1:1/vnet', cert: relay.certHash, timeout: '4000' });
      const r = await pageResult(closed);
      check(r.ok === false && /relay unreachable/.test(r.error), 'no endpoint: ' + JSON.stringify(r));
      check((await wsOpened(closed)) === 0, 'and no WebSocket was tried');
      await closed.ctx.close();
      const strict = await openPeerPage(browser, webOrigin, wsUrl, null, { backend: 'wt', relay: wtUrl, cert: relay.certHash, verbose: '1' }, STRICT_DATAGRAMS);
      const r2 = await pageResult(strict);
      check(r2.ok === false && /datagram too large/.test(r2.error), 'datagrams that do not fit: ' + JSON.stringify(r2));
      await strict.ctx.close();
      const nope = await openPeerPage(browser, webOrigin, wsUrl, 'ZZZZZZ', { backend: 'wt', relay: wtUrl, cert: relay.certHash });
      const r3 = await pageResult(nope);
      check(r3.ok === false && /no such room/.test(r3.error), 'an unknown room is an answer, not a reason to fall back: ' + JSON.stringify(r3));
      await nope.ctx.close();
      const nope2 = await openPeerPage(browser, webOrigin, wsUrl, 'ZZZZZZ', auto);
      const r4 = await pageResult(nope2);
      check(r4.ok === false && /no such room/.test(r4.error) && (await wsOpened(nope2)) === 0, 'auto: the same, and no WebSocket: ' + JSON.stringify(r4));
      await nope2.ctx.close();
    }

    // Everything has been closed by now: the relay holds nothing, and saw nothing odd.
    let st = await getJSON(relay.stats, '/stats');
    for (let i = 0; i < 1000 && (st.sessions !== 0 || st.webtransports !== 0 || st.websockets !== 0); i++) {
      await new Promise((r) => setTimeout(r, 20));
      st = await getJSON(relay.stats, '/stats');
    }
    check(st.sessions === 0 && st.webtransports === 0 && st.websockets === 0 && st.rooms === 0,
      `all sessions left, nothing is held (${st.sessions} sessions, ${st.webtransports} WebTransport, ${st.websockets} WebSocket, ${st.rooms} rooms)`);
    check(st.malformed === 0 && st.rate_limited === 0 && st.panics === 0 && st.wt_ctl_overflow === 0 && st.wt_bad_streams === 0,
      `no malformed frames, rate limiting, panics or control overflow (${JSON.stringify({ m: st.malformed, r: st.rate_limited, p: st.panics, c: st.wt_ctl_overflow, b: st.wt_bad_streams })})`);
    console.log(`(Chromium ${browser.version()} reports datagrams.maxDatagramSize = ${maxDatagramSize}; wt_datagram_drops = ${st.wt_datagram_drops}, wt_early_datagrams = ${st.wt_early_datagrams})`);
  } finally {
    hole.close();
    await browser.close();
    web.close();
    relay.child.kill('SIGTERM');
    noWT.child.kill('SIGTERM');
  }
}

main().then(() => {
  console.log(failureCount() ? `\n${failureCount()} check(s) FAILED` : '\nall WebTransport browser checks passed');
  process.exit(failureCount() ? 1 : 0);
}, (e) => { console.error('ERROR:', e); process.exit(1); });
