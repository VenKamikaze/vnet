// SPDX-License-Identifier: MIT
// Relay transport over a WebSocket, for Emscripten (browser) builds. It uses
// Emscripten's <emscripten/websocket.h> (-lwebsocket.js). Browser events
// arrive between frames, run the callbacks below, and queue frames in a
// ring; vnet_poll() moves them on into the sockets' receive queues, so no
// vnet call ever blocks.
#include "relay_inbox.h"
#include "relay_proto.h"
#include "vnet_internal.h"

#if !defined(__EMSCRIPTEN__)
int vnet__relay_ws_init(vnet *v, const vnet_config *cfg) {
    (void)v;
    (void)cfg;
    return VNET_ENOTSUP;  // WebSocket transport exists in browser builds only
}

int vnet__relay_ws_make(vnet *v, const char *url, const relay_transport_ops **ops, void **tp) {
    (void)v;
    (void)url;
    (void)ops;
    (void)tp;
    return VNET_ENOTSUP;
}
#else

#include <emscripten/websocket.h>
#include <stdlib.h>
#include <string.h>

// DATA frames waiting for vnet_poll(); more are dropped (a lossy network).
// Control frames (ROOM, ERROR, KEEPALIVE ...) wait in a ring of their own and
// are never pushed out by a burst of datagrams: an unpolled page that fills the
// DATA ring still hears its KEEPALIVE answer or an ERROR(CLOSED). So many
// control frames waiting means vnet_poll() has not run for a very long time:
// the transport is declared failed, as relay_wt.c does.
#define INBOX_FRAMES 256
#define INBOX_CONTROL_FRAMES 64
// A WebSocket whose send buffer is this full is not keeping up: new datagrams
// are dropped rather than queued behind stale ones (vnet is unreliable).
#define MAX_BUFFERED_BYTES 65536

// A browser cannot tell why a WebSocket failed to open: a relay that is down
// and a relay that answered HTTP 503 (this address has too many sockets
// pending right now, which clears within milliseconds) look the same. So a
// failure before the socket ever opened is retried a few times with backoff,
// ~3.75 s in all, before it is reported as "relay unreachable".
#define CONNECT_RETRIES 5
#define CONNECT_BACKOFF_FIRST_MS 250.0
#define CONNECT_BACKOFF_MAX_MS 1000.0

typedef struct ws_tp {
    vnet *v;  // for the wake callback
    EMSCRIPTEN_WEBSOCKET_T ws;
    int state;  // RELAY_TP_*
    relay_inbox *inbox;
    char url[512];
    int attempts;        // failed attempts to open so far
    int retry_pending;   // a reconnect is scheduled
    double next_try_ms;  // emscripten_get_now() time
    double backoff_ms;
} ws_tp;

static EM_BOOL on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user);
static EM_BOOL on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user);
static EM_BOOL on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user);
static EM_BOOL on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user);

static int open_socket(ws_tp *t) {
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = t->url;
    t->ws = emscripten_websocket_new(&attr);
    if (t->ws <= 0) {
        t->ws = 0;
        return -1;
    }
    emscripten_websocket_set_onopen_callback(t->ws, t, on_open);
    emscripten_websocket_set_onerror_callback(t->ws, t, on_error);
    emscripten_websocket_set_onclose_callback(t->ws, t, on_close);
    emscripten_websocket_set_onmessage_callback(t->ws, t, on_message);
    return 0;
}

// The socket failed or closed. Before it ever opened that may be retried.
static void failed(ws_tp *t) {
    if (t->state == RELAY_TP_FAILED || t->retry_pending) return;
    if (t->state == RELAY_TP_CONNECTING && t->attempts < CONNECT_RETRIES) {
        t->attempts++;
        t->retry_pending = 1;
        t->next_try_ms = emscripten_get_now() + t->backoff_ms;
        t->backoff_ms = t->backoff_ms * 2 > CONNECT_BACKOFF_MAX_MS ? CONNECT_BACKOFF_MAX_MS : t->backoff_ms * 2;
        return;
    }
    t->state = RELAY_TP_FAILED;
    vnet__wake(t->v);
}

static EM_BOOL on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void)type;
    (void)e;
    ws_tp *t = user;
    if (t->state == RELAY_TP_CONNECTING) {
        t->state = RELAY_TP_OPEN;
        vnet__wake(t->v);
    }
    return EM_TRUE;
}

static EM_BOOL on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *user) {
    (void)type;
    (void)e;
    failed(user);
    return EM_TRUE;
}

static EM_BOOL on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *user) {
    (void)type;
    (void)e;
    failed(user);
    return EM_TRUE;
}

static EM_BOOL on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void)type;
    ws_tp *t = user;
    if (e->isText || e->numBytes == 0 || e->numBytes > RELAY_MAX_FRAME) return EM_TRUE;  // not a vnet frame
    int rc = relay_inbox_push(t->inbox, e->data, e->numBytes);
    if (rc == RELAY_INBOX_OVERFLOW) {
        if (t->state != RELAY_TP_FAILED) t->state = RELAY_TP_FAILED;
        vnet__wake(t->v);
    } else if (rc != RELAY_INBOX_DROPPED) {
        vnet__wake(t->v);  // a frame is waiting for vnet_poll()
    }
    return EM_TRUE;
}

static int tp_state(void *p) {
    ws_tp *t = p;
    if (t->retry_pending && emscripten_get_now() >= t->next_try_ms) {
        // Deleting here (not in the callback) is safe, and stops late events
        // from the failed socket.
        emscripten_websocket_delete(t->ws);
        t->retry_pending = 0;
        if (open_socket(t) != 0) t->state = RELAY_TP_FAILED;
    }
    return t->state;
}

static int tp_send(void *p, const uint8_t *frame, size_t len) {
    ws_tp *t = p;
    if (t->state != RELAY_TP_OPEN) return VNET_ENOTCONN;
    size_t buffered = 0;
    if (emscripten_websocket_get_buffered_amount(t->ws, &buffered) == EMSCRIPTEN_RESULT_SUCCESS &&
        buffered > MAX_BUFFERED_BYTES)
        return VNET_EAGAIN;  // a drop
    // The API takes a non-const pointer but does not modify the data.
    if (emscripten_websocket_send_binary(t->ws, (void *)(uintptr_t)frame, (uint32_t)len) != EMSCRIPTEN_RESULT_SUCCESS)
        return VNET_EIO;
    return VNET_OK;
}

static int tp_recv(void *p, uint8_t *buf, size_t cap) {
    ws_tp *t = p;
    return relay_inbox_pop(t->inbox, buf, cap);
}

static void tp_destroy(void *p) {
    ws_tp *t = p;
    if (!t) return;
    if (t->ws > 0) {
        // Close, then delete: deleting stops further callbacks into `t`.
        emscripten_websocket_close(t->ws, 1000, "bye");
        emscripten_websocket_delete(t->ws);
    }
    relay_inbox_destroy(t->inbox);
    free(t);
}

static const relay_transport_ops ws_ops = {"relay-ws", 0, tp_state, tp_send, tp_recv, tp_destroy, NULL, NULL};

// Open a WebSocket to `url` (ws:// or wss://). On success *ops and *tp are
// set; the caller owns the transport.
int vnet__relay_ws_make(vnet *v, const char *url, const relay_transport_ops **ops, void **tp) {
    if (!url || (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0)) return VNET_EINVAL;
    if (!emscripten_websocket_is_supported()) return VNET_ENOTSUP;
    ws_tp *t = calloc(1, sizeof *t);
    if (!t) return VNET_ENOMEM;
    t->v = v;
    t->inbox = relay_inbox_create(INBOX_FRAMES, INBOX_CONTROL_FRAMES);
    if (!t->inbox) {
        free(t);
        return VNET_ENOMEM;
    }
    t->state = RELAY_TP_CONNECTING;
    t->backoff_ms = CONNECT_BACKOFF_FIRST_MS;
    size_t urllen = strlen(url);
    if (urllen >= sizeof t->url) {
        tp_destroy(t);
        return VNET_EINVAL;
    }
    memcpy(t->url, url, urllen + 1);
    if (open_socket(t) != 0) {
        vnet__logf(v, VNET_LOG_ERROR, "cannot create WebSocket for \"%s\"", url);
        tp_destroy(t);
        return VNET_ECONNREFUSED;
    }
    *ops = &ws_ops;
    *tp = t;
    return VNET_OK;
}

int vnet__relay_ws_init(vnet *v, const vnet_config *cfg) {
    const relay_transport_ops *ops = NULL;
    void *tp = NULL;
    int rc = vnet__relay_ws_make(v, cfg->relay, &ops, &tp);
    if (rc != VNET_OK) return rc;
    return vnet__relay_init(v, cfg, ops, tp);
}

#endif /* __EMSCRIPTEN__ */
