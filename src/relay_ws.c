// SPDX-License-Identifier: MIT
// Relay transport over a WebSocket, for Emscripten (browser) builds. It uses
// Emscripten's <emscripten/websocket.h> (-lwebsocket.js). Browser events
// arrive between frames, run the callbacks below, and queue frames in a
// ring; vnet_poll() moves them on into the sockets' receive queues, so no
// vnet call ever blocks.
#include "relay_proto.h"
#include "vnet_internal.h"

#if !defined(__EMSCRIPTEN__)
int vnet__relay_ws_init(vnet *v, const vnet_config *cfg) {
    (void)v;
    (void)cfg;
    return VNET_ENOTSUP;  // WebSocket transport exists in browser builds only
}
#else

#include <emscripten/websocket.h>
#include <stdlib.h>
#include <string.h>

#define INBOX_FRAMES 256
// A WebSocket whose send buffer is this full is not keeping up: new datagrams
// are dropped rather than queued behind stale ones (vnet is unreliable).
#define MAX_BUFFERED_BYTES 65536

typedef struct inbox_frame {
    size_t len;
    uint8_t data[RELAY_MAX_FRAME];
} inbox_frame;

typedef struct ws_tp {
    EMSCRIPTEN_WEBSOCKET_T ws;
    int state;  // RELAY_TP_*
    int head, count;
    inbox_frame *inbox;
} ws_tp;

static EM_BOOL on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *user) {
    (void)type;
    (void)e;
    ws_tp *t = user;
    if (t->state == RELAY_TP_CONNECTING) t->state = RELAY_TP_OPEN;
    return EM_TRUE;
}

static EM_BOOL on_fail(int type, const void *e, void *user) {  // error and close
    (void)type;
    (void)e;
    ws_tp *t = user;
    t->state = RELAY_TP_FAILED;
    return EM_TRUE;
}

static EM_BOOL on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *user) {
    (void)type;
    ws_tp *t = user;
    if (e->isText || e->numBytes == 0 || e->numBytes > RELAY_MAX_FRAME) return EM_TRUE;  // not a vnet frame
    if (t->count == INBOX_FRAMES) return EM_TRUE;  // overflow: dropped, like a lossy network
    inbox_frame *f = &t->inbox[(t->head + t->count) % INBOX_FRAMES];
    t->count++;
    f->len = e->numBytes;
    memcpy(f->data, e->data, e->numBytes);
    return EM_TRUE;
}

static int tp_state(void *p) { return ((ws_tp *)p)->state; }

static int tp_send(void *p, const uint8_t *frame, size_t len) {
    ws_tp *t = p;
    if (t->state != RELAY_TP_OPEN) return VNET_ENOTCONN;
    uint64_t buffered = 0;
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
    if (t->count == 0) return VNET_EAGAIN;
    inbox_frame *f = &t->inbox[t->head];
    t->head = (t->head + 1) % INBOX_FRAMES;
    t->count--;
    if (f->len > cap) return VNET_EMSGSIZE;
    memcpy(buf, f->data, f->len);
    return (int)f->len;
}

static void tp_destroy(void *p) {
    ws_tp *t = p;
    if (!t) return;
    if (t->ws > 0) {
        // Close, then delete: deleting stops further callbacks into `t`.
        emscripten_websocket_close(t->ws, 1000, "bye");
        emscripten_websocket_delete(t->ws);
    }
    free(t->inbox);
    free(t);
}

static const relay_transport_ops ws_ops = {"relay-ws", 0, tp_state, tp_send, tp_recv, tp_destroy};

int vnet__relay_ws_init(vnet *v, const vnet_config *cfg) {
    if (!cfg->relay || (strncmp(cfg->relay, "ws://", 5) != 0 && strncmp(cfg->relay, "wss://", 6) != 0))
        return VNET_EINVAL;
    if (!emscripten_websocket_is_supported()) return VNET_ENOTSUP;
    ws_tp *t = calloc(1, sizeof *t);
    if (!t) return VNET_ENOMEM;
    t->inbox = calloc(INBOX_FRAMES, sizeof *t->inbox);
    if (!t->inbox) {
        free(t);
        return VNET_ENOMEM;
    }
    t->state = RELAY_TP_CONNECTING;
    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = cfg->relay;
    t->ws = emscripten_websocket_new(&attr);
    if (t->ws <= 0) {
        vnet__logf(v, VNET_LOG_ERROR, "cannot create WebSocket for \"%s\"", cfg->relay);
        t->ws = 0;
        tp_destroy(t);
        return VNET_ECONNREFUSED;
    }
    emscripten_websocket_set_onopen_callback(t->ws, t, on_open);
    emscripten_websocket_set_onerror_callback(t->ws, t, (em_websocket_error_callback_func)on_fail);
    emscripten_websocket_set_onclose_callback(t->ws, t, (em_websocket_close_callback_func)on_fail);
    emscripten_websocket_set_onmessage_callback(t->ws, t, on_message);
    return vnet__relay_init(v, cfg, &ws_ops, t);
}

#endif /* __EMSCRIPTEN__ */
