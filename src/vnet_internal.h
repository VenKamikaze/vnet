// SPDX-License-Identifier: MIT
// Internal declarations shared between vnet's source files. Not installed.
#ifndef VNET_INTERNAL_H
#define VNET_INTERNAL_H

#include "vnet.h"

#include <stdarg.h>

typedef struct vnet_backend {
    const char *name;
    int (*open)(vnet *v, uint16_t port);
    void (*close)(vnet *v, int s);
    int (*local_addr)(vnet *v, int s, vnet_addr *out);
    // Would a send from `s` to `to` be accepted? (Used before queueing an
    // impaired datagram so errors are reported synchronously.)
    int (*check_send)(vnet *v, int s, const vnet_addr *to);
    // Hand one already-validated datagram to the "wire". Must not block.
    int (*send)(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to);
    int (*recv)(vnet *v, int s, void *buf, size_t cap, vnet_addr *from);
    void (*poll)(vnet *v);
    void (*destroy)(vnet *v);
    // Optional (NULL = no setup phase: always READY, no extra address forms).
    // Fill in everything but the common fields; return the VNET_STATUS_* state.
    int (*status)(vnet *v, vnet_status_info *out);
    // Optional extra address forms for vnet_resolve(). Return 0 (done), a
    // negative VNET_E* code (it was this backend's form but invalid), or 1
    // (not one of this backend's forms: use the built-in parser).
    int (*resolve)(vnet *v, const char *host_port, vnet_addr *out);
} vnet_backend;

typedef struct vnet_imp vnet_imp;

struct vnet {
    const vnet_backend *be;
    void *bs;  // backend state
    int max_sockets;
    size_t max_dgram;  // largest datagram the backend accepts: VNET_MAX_DATAGRAM,
                       // except for the relay's inner UDP socket, which also
                       // carries the relay framing
    vnet_clock_fn clock;
    void *clock_user;
    vnet_log_fn log;
    void *log_user;
    vnet_imp *imp;  // NULL when impairment is off
    vnet_stats stats;
};

// vnet.c
void vnet__logf(vnet *v, int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
uint64_t vnet__now(vnet *v);
uint64_t vnet__system_clock(void *user);
// Impaired datagrams due now go to the backend (also done by vnet_poll()).
void vnet__release(vnet *v);
int vnet__wire_send(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to);

// addr.c
// Numeric-only parsers. Return 0 on success, -1 on failure. `n` is the length.
int vnet__parse_ipv4(const char *s, size_t n, uint8_t out[4]);
int vnet__parse_ipv6(const char *s, size_t n, uint8_t out[16]);
// Collapse an IPv4-mapped IPv6 address into an IPv4 vnet_addr.
void vnet__addr_canonicalise(vnet_addr *a);
void vnet__addr_set_ipv4(vnet_addr *a, const uint8_t ip[4], uint16_t port);

// impair.c
vnet_imp *vnet__imp_create(vnet *v, const vnet_impair *cfg, int queue_len);
void vnet__imp_destroy(vnet_imp *imp);
// Like vnet_impair_parse() but updates `inout` in place (keys not mentioned keep their value).
int vnet__impair_parse_into(const char *spec, vnet_impair *inout);
int vnet__imp_enabled(const vnet_impair *cfg);
int vnet__imp_validate(const vnet_impair *cfg);
// Takes ownership of the datagram's fate (lose/duplicate/delay/forward).
void vnet__imp_submit(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to);
void vnet__imp_release(vnet *v);
void vnet__imp_purge_socket(vnet *v, int s);

// random.c
// Fill buf (n <= 256) with bytes from the OS cryptographic random source.
// Returns 0, or -1 if there is none: callers must fail, never fall back to
// something guessable. The relay's handshake nonce is effectively a
// credential (whoever repeats it gets the session's ROOM and token), so it
// must be unpredictable.
int vnet__random(void *buf, size_t n);

// Relay client (relay_client.c). A relay backend is a framing/state-machine
// core plus a transport that moves whole frames: UDP datagrams (relay_udp.c)
// or WebSocket messages (relay_ws.c).
#define RELAY_TP_CONNECTING 0
#define RELAY_TP_OPEN 1
#define RELAY_TP_FAILED 2
#define RELAY_TPF_UDP_STYLE 1u  // frames carry the token, handshake padded and retransmitted

typedef struct relay_transport_ops {
    const char *name;  // vnet_status_info.transport
    unsigned flags;
    int (*state)(void *tp);                                  // RELAY_TP_*
    int (*send)(void *tp, const uint8_t *frame, size_t len); // 0, or a VNET_E* (EAGAIN = dropped)
    int (*recv)(void *tp, uint8_t *buf, size_t cap);         // frame length or VNET_EAGAIN
    void (*destroy)(void *tp);
    // Optional. The largest DATA frame (type byte included) the transport
    // promises to carry, once it is open. If that is less than
    // RELAY_WT_DATA_MAX the core checks what really fits before it reports
    // READY (a datagram-size probe). NULL: no such limit.
    size_t (*max_data_frame)(void *tp);
} relay_transport_ops;

// A way to open the transport to fall back to. `arg` is the copy of the bytes
// given to vnet__relay_init_fallback() (it outlives vnet_create()'s strings).
// Returns 0 and fills ops/tp, or a VNET_E* code.
typedef int (*relay_fallback_fn)(vnet *v, const void *arg, const relay_transport_ops **ops, void **tp);

// Takes ownership of `tp` (destroyed on failure too). Sets v->be / v->bs.
int vnet__relay_init(vnet *v, const vnet_config *cfg, const relay_transport_ops *ops, void *tp);
// The same, with a fallback: if the transport fails before the room is
// entered (or does not come up within primary_timeout_ms, or turns out not to
// carry a full-size datagram), `fb` opens another and the handshake starts over
// on it. Used by VNET_BACKEND_RELAY_AUTO.
int vnet__relay_init_fallback(vnet *v, const vnet_config *cfg, const relay_transport_ops *ops, void *tp,
                              unsigned primary_timeout_ms, relay_fallback_fn fb, const void *fb_arg,
                              size_t fb_arg_len);
int vnet__relay_udp_init(vnet *v, const vnet_config *cfg);
int vnet__relay_ws_init(vnet *v, const vnet_config *cfg);
int vnet__relay_wt_init(vnet *v, const vnet_config *cfg);
int vnet__relay_auto_init(vnet *v, const vnet_config *cfg);
// Transport constructors (the WebSocket and WebTransport ones also exist in
// native builds, where they return VNET_ENOTSUP).
int vnet__relay_wt_check_args(const char *url, const char *cert_sha256, uint8_t hash[32], int *hashlen);
int vnet__wt_parse_hash(const char *hex, uint8_t out[32]);
// "wss://host[:port]/path" -> "https://host[:port]/path" in out[cap]; -1 if
// ws has no WebTransport form.
int vnet__relay_auto_derive_wt_url(const char *ws, char *out, size_t cap);
int vnet__relay_ws_make(vnet *v, const char *url, const relay_transport_ops **ops, void **tp);
int vnet__relay_wt_make(vnet *v, const char *url, const char *cert_sha256, const relay_transport_ops **ops, void **tp);
// Like vnet_create() with a relay backend over a caller-supplied transport.
// For tests (scripted relays, WebSocket-style framing on a native build).
vnet *vnet__create_with_relay_transport(const vnet_config *cfg, const relay_transport_ops *ops,
                                        void *tp, int *err);
// The same with a fallback (see vnet__relay_init_fallback).
vnet *vnet__create_with_relay_fallback(const vnet_config *cfg, const relay_transport_ops *ops, void *tp,
                                       unsigned primary_timeout_ms, relay_fallback_fn fb, const void *fb_arg,
                                       size_t fb_arg_len, int *err);

// Backends. Each fills v->be / v->bs (and v->clock if cfg->clock is NULL
// and the backend has its own default). Return 0 or a VNET_E* code.
int vnet__udp_init(vnet *v, const vnet_config *cfg);
int vnet__loopback_init(vnet *v, const vnet_config *cfg);

#endif
