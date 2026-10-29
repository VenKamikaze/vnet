// SPDX-License-Identifier: MIT
// Relay transport over WebTransport, for Emscripten (browser) builds.
//
// WebTransport is HTTP/3 over QUIC. The transport opens one session to the
// relay and one bidirectional stream on it. Control frames (CREATE_ROOM,
// JOIN_ROOM, LEAVE, KEEPALIVE, and the relay's ROOM, ERROR, KEEPALIVE) travel
// on that stream, each preceded by a 2-byte big-endian length; DATA frames
// travel as datagrams, one frame per datagram, in the WebSocket layout. See
// "WebTransport" in doco/relay-protocol.md.
//
// The browser API is promise based. The EM_JS functions below keep a small
// state object per session in JavaScript and queue incoming frames there; C
// polls (vnet_poll() -> tp_state / tp_recv), so nothing blocks. The one way
// back into C is the optional wake callback (vnet_config.wake): when a frame is
// queued or the session opens or fails, JavaScript calls wt_wake() through the
// function table, and that calls the game's hook, which may only schedule a
// poll.
#include "relay_proto.h"
#include "vnet_internal.h"

#include <string.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 64 hex digits -> 32 bytes.
int vnet__wt_parse_hash(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}


// The arguments a WebTransport transport needs, checked the same way in every
// build: an https:// URL and, if given, a 64-digit hex certificate hash.
// Returns 0 or VNET_EINVAL; fills hash/hashlen.
int vnet__relay_wt_check_args(const char *url, const char *cert_sha256, uint8_t hash[32], int *hashlen) {
    *hashlen = 0;
    if (!url || strncmp(url, "https://", 8) != 0 || url[8] == 0) return VNET_EINVAL;
    if (cert_sha256 && cert_sha256[0]) {
        if (vnet__wt_parse_hash(cert_sha256, hash) != 0) return VNET_EINVAL;
        *hashlen = 32;
    }
    return VNET_OK;
}

#if !defined(__EMSCRIPTEN__)
int vnet__relay_wt_init(vnet *v, const vnet_config *cfg) {
    (void)v;
    uint8_t hash[32];
    int hashlen;
    int rc = vnet__relay_wt_check_args(cfg->relay, cfg->relay_cert_sha256, hash, &hashlen);
    return rc != VNET_OK ? rc : VNET_ENOTSUP;  // WebTransport exists in browser builds only
}

int vnet__relay_wt_make(vnet *v, const char *url, const char *cert_sha256, const relay_transport_ops **ops,
                        void **tp) {
    (void)v;
    (void)ops;
    (void)tp;
    uint8_t hash[32];
    int hashlen;
    int rc = vnet__relay_wt_check_args(url, cert_sha256, hash, &hashlen);
    return rc != VNET_OK ? rc : VNET_ENOTSUP;
}
#else

#include <emscripten.h>
#include <stdlib.h>

// Datagrams waiting in JavaScript for C; more are dropped (a lossy network).
// Control frames wait in a queue of their own and are never dropped: a
// datagram burst must not cost a ROOM or an ERROR. They are few (answers to
// requests the client made), so a control queue this long means C has not
// polled for a very long time, and the session is declared failed.
#define JS_INBOX_FRAMES 256
#define JS_CONTROL_FRAMES 256
// Datagram writes the browser has not finished with; more are dropped rather
// than queued behind stale ones (vnet is unreliable). Control writes that
// stay unfinished this long mean the connection is stuck.
#define JS_MAX_DATAGRAMS_IN_FLIGHT 16
#define JS_MAX_CONTROL_IN_FLIGHT 64

// Returns a session id (> 0), or 0 if there is no WebTransport here (the
// object is missing, as in an old browser or an insecure page) or the browser
// refused the arguments. hash (32 bytes, or hashlen == 0) is a SHA-256
// certificate hash for serverCertificateHashes.
// wake_fn is a C function pointer (void (*)(void *)), or 0; it is called with
// wake_arg whenever a frame is queued or the state changes, never after the
// session was closed with vnet_wt_close.
EM_JS_DEPS(vnet_wt_deps, "$getWasmTableEntry")
EM_JS(int, vnet_wt_open, (const char *url, const uint8_t *hash, int hashlen, int inbox_frames, int max_frame, int ctl_max, int ctl_frames, int wake_fn, int wake_arg), {
  var T = Module.vnetWT || (Module.vnetWT = { next: 1, s: {} });
  // Queue a frame for C. Datagrams: dropped when the queue is full or the
  // frame is not acceptable. Control frames: never dropped; too many waiting
  // means the session is stuck.
  if (!T.enqueue) {
    T.enqueue = function (s, control, u8) {
      if (u8.length === 0 || u8.length > s.maxFrame) { s.drops++; return; }
      if (control) {
        if (s.qc.length >= s.ctlFrames) { if (s.state !== 3) { s.state = 2; s.reason = "control queue overflow"; s.wake(); } return; }
        s.qc.push(u8);
      } else {
        if (s.q.length >= s.inbox) { s.drops++; return; }
        s.q.push(u8);
      }
      s.wake();
    };
  }
  if (typeof WebTransport === "undefined") return 0;
  var opts = {};
  if (hashlen === 32) {
    opts.serverCertificateHashes = [{ algorithm: "sha-256", value: HEAPU8.slice(hash, hash + 32) }];
  }
  var s = { id: T.next++, state: 0, q: [], qc: [], inbox: inbox_frames, maxFrame: max_frame, ctlFrames: ctl_frames,
            maxdg: 0, dgInFlight: 0, ctlInFlight: 0, wt: null, dw: null, cw: null, drops: 0, reason: "" };
  // Tell C there is something to poll for. Not once the session is closed or
  // gone (T.s no longer has it): C may have freed what the hook points at.
  s.wake = function () { if (wake_fn && T.s[s.id] === s && s.state !== 3) getWasmTableEntry(wake_fn)(wake_arg); };
  try { s.wt = new WebTransport(UTF8ToString(url), opts); } catch (e) { return 0; }
  T.s[s.id] = s;
  var fail = function (why) {
    if (s.state === 3) return;
    var was = s.state;
    s.state = 2;
    if (!s.reason) s.reason = String(why);
    if (was !== 2) s.wake();
  };
  // After the session is up, any end of it is a failure; before, too.
  s.wt.closed.then(function () { fail("closed"); }, function (e) { fail(e); });
  s.wt.ready.then(function () {
    if (s.wt.reliability === "reliable-only") { fail("only reliable transports (no datagrams)"); return null; }
    s.maxdg = s.wt.datagrams.maxDatagramSize;
    return s.wt.createBidirectionalStream();
  }).then(function (bs) {
    if (!bs || s.state === 3) return;
    s.cw = bs.writable.getWriter();
    s.dw = s.wt.datagrams.writable.getWriter();
    // Control stream: 2-byte length, then the frame. A frame can arrive in
    // pieces, and several in one chunk.
    var acc = new Uint8Array(0);
    var cr = bs.readable.getReader();
    (function pump() {
      cr.read().then(function (r) {
        if (r.done) { fail("control stream ended"); return; }
        var joined = new Uint8Array(acc.length + r.value.length);
        joined.set(acc, 0);
        joined.set(r.value, acc.length);
        acc = joined;
        while (acc.length >= 2) {
          var n = (acc[0] << 8) | acc[1];
          if (n === 0 || n > ctl_max) { fail("bad control frame length " + n); return; }
          if (acc.length < 2 + n) break;
          T.enqueue(s, true, acc.slice(2, 2 + n));
          acc = acc.subarray(2 + n);
        }
        if (acc.length > 2 + ctl_max) { fail("control stream overrun"); return; }
        pump();
      }, function (e) { fail(e); });
    })();
    var dr = s.wt.datagrams.readable.getReader();
    (function pumpD() {
      dr.read().then(function (r) {
        if (r.done) return;
        T.enqueue(s, false, r.value);
        pumpD();
      }, function (e) { fail(e); });
    })();
    if (s.state === 0) { s.state = 1; s.wake(); }
  }).catch(function (e) { fail(e); });
  return s.id;
})

// 0 connecting, 1 open (session and control stream up), 2 failed.
EM_JS(int, vnet_wt_state, (int id), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  return s ? s.state : 2;
})

// maxDatagramSize as the browser reports it (0 before the session is up).
EM_JS(int, vnet_wt_max_datagram, (int id), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  var m = s ? s.maxdg : 0;
  return m > 0x7fffffff ? 0x7fffffff : m;
})

// Copy the next waiting frame into buf (cap bytes). Returns its length, -1 if
// none, -2 if it did not fit (it is consumed, never truncated).
EM_JS(int, vnet_wt_recv, (int id, uint8_t *buf, int cap), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  if (!s || (s.qc.length === 0 && s.q.length === 0)) return -1;
  var f = s.qc.length ? s.qc.shift() : s.q.shift();
  if (f.length > cap) return -2;
  HEAPU8.set(f, buf);
  return f.length;
})

// A control frame on the stream. 0, or -1 when the connection is unusable.
EM_JS(int, vnet_wt_send_control, (int id, const uint8_t *p, int n, int in_flight_max), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  if (!s || s.state !== 1 || !s.cw) return -1;
  // (No wake here: this runs inside a vnet call, and the hook is for browser events.)
  if (s.ctlInFlight >= in_flight_max) { s.state = 2; s.reason = "control stream stuck"; return -1; }
  var m = new Uint8Array(2 + n);
  m[0] = n >> 8;
  m[1] = n & 255;
  m.set(HEAPU8.subarray(p, p + n), 2);
  s.ctlInFlight++;
  s.cw.write(m).then(function () { s.ctlInFlight--; }, function (e) { s.ctlInFlight--; if (s.state !== 3 && s.state !== 2) { s.state = 2; s.reason = String(e); s.wake(); } });
  return 0;
})

// A DATA frame as a datagram. 0, 1 if dropped because the browser is not
// keeping up (a lossy network, as far as the game can tell), -1 when unusable.
EM_JS(int, vnet_wt_send_datagram, (int id, const uint8_t *p, int n, int in_flight_max), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  if (!s || s.state !== 1 || !s.dw) return -1;
  if (s.dgInFlight >= in_flight_max) { s.drops++; return 1; }
  s.dgInFlight++;
  s.dw.write(HEAPU8.slice(p, p + n)).then(function () { s.dgInFlight--; }, function () { s.dgInFlight--; });
  return 0;
})

EM_JS(void, vnet_wt_close, (int id), {
  var T = Module.vnetWT;
  var s = T && T.s[id];
  if (!s) return;
  s.state = 3;
  delete T.s[id];
  try { s.wt.close({ closeCode: 0, reason: "" }); } catch (e) {}
  // The session's promises settle on their own; their handlers ignore state 3.
})

// Why the session failed, for the log (copied into out, NUL terminated).
EM_JS(void, vnet_wt_reason, (int id, char *out, int cap), {
  var s = Module.vnetWT && Module.vnetWT.s[id];
  stringToUTF8(s ? (s.reason || "") : "gone", out, cap);
})

typedef struct wt_tp {
  vnet *v;
  int id;
  int logged;  // the open event has been logged
  int failed_logged;
  size_t max_dgram;  // browser-reported maxDatagramSize, once open
} wt_tp;

// Called by JavaScript (see vnet_wt_open) from a promise handler.
static void wt_wake(void *arg) {
    wt_tp *t = arg;
    vnet__wake(t->v);
}

static int tp_state(void *p) {
    wt_tp *t = p;
    int s = vnet_wt_state(t->id);
    if (s == 1) {
        if (!t->logged) {
            t->logged = 1;
            int m = vnet_wt_max_datagram(t->id);
            t->max_dgram = m > 0 ? (size_t)m : 0;
            vnet__logf(t->v, VNET_LOG_INFO, "WebTransport open, maxDatagramSize %d (a full vnet datagram needs %d)", m,
                       (int)RELAY_WT_DATA_MAX);
        }
        return RELAY_TP_OPEN;
    }
    if (s == 2) {
        if (!t->failed_logged) {
            t->failed_logged = 1;
            char why[160];
            vnet_wt_reason(t->id, why, sizeof why);
            vnet__logf(t->v, VNET_LOG_INFO, "WebTransport failed: %s", why);
        }
        return RELAY_TP_FAILED;
    }
    return RELAY_TP_CONNECTING;
}

static int tp_send(void *p, const uint8_t *frame, size_t len) {
    wt_tp *t = p;
    if (len == 0 || len > 0xffff) return VNET_EINVAL;
    if (frame[0] == RELAY_T_DATA) {
        int rc = vnet_wt_send_datagram(t->id, frame, (int)len, JS_MAX_DATAGRAMS_IN_FLIGHT);
        if (rc == 0) return VNET_OK;
        return rc > 0 ? VNET_EAGAIN : VNET_ENOTCONN;
    }
    if (len > RELAY_WT_CTL_MAX) return VNET_EINVAL;
    return vnet_wt_send_control(t->id, frame, (int)len, JS_MAX_CONTROL_IN_FLIGHT) == 0 ? VNET_OK : VNET_ENOTCONN;
}

static int tp_recv(void *p, uint8_t *buf, size_t cap) {
    wt_tp *t = p;
    int n = vnet_wt_recv(t->id, buf, (int)cap);
    if (n == -1) return VNET_EAGAIN;
    if (n == -2) return VNET_EMSGSIZE;
    return n;
}

static size_t tp_max_data_frame(void *p) {
    wt_tp *t = p;
    return t->max_dgram;
}

static void tp_destroy(void *p) {
    wt_tp *t = p;
    if (!t) return;
    if (t->id > 0) vnet_wt_close(t->id);
    free(t);
}

static const relay_transport_ops wt_ops = {"relay-wt", 0, tp_state, tp_send, tp_recv, tp_destroy, tp_max_data_frame, NULL};

int vnet__relay_wt_make(vnet *v, const char *url, const char *cert_sha256, const relay_transport_ops **ops,
                        void **tp) {
    uint8_t hash[32];
    int hashlen;
    int rc = vnet__relay_wt_check_args(url, cert_sha256, hash, &hashlen);
    if (rc != VNET_OK) return rc;
    wt_tp *t = calloc(1, sizeof *t);
    if (!t) return VNET_ENOMEM;
    t->v = v;
    t->id = vnet_wt_open(url, hash, hashlen, JS_INBOX_FRAMES, RELAY_MAX_FRAME, RELAY_WT_CTL_MAX, JS_CONTROL_FRAMES,
                         v->wake ? (int)(uintptr_t)&wt_wake : 0, (int)(uintptr_t)t);
    if (t->id <= 0) {
        free(t);
        return VNET_ENOTSUP;  // no WebTransport in this browser or page
    }
    *ops = &wt_ops;
    *tp = t;
    return VNET_OK;
}

int vnet__relay_wt_init(vnet *v, const vnet_config *cfg) {
    const relay_transport_ops *ops = NULL;
    void *tp = NULL;
    int rc = vnet__relay_wt_make(v, cfg->relay, cfg->relay_cert_sha256, &ops, &tp);
    if (rc != VNET_OK) return rc;
    return vnet__relay_init(v, cfg, ops, tp);
}

#endif /* __EMSCRIPTEN__ */
