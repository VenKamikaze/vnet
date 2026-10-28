/* SPDX-License-Identifier: MIT */
/*
 * vnet - virtual UDP for games.
 *
 * A small, dependency-free, non-blocking datagram API with pluggable
 * backends:
 *
 *   VNET_BACKEND_UDP       native UDP sockets (BSD sockets / Winsock)
 *   VNET_BACKEND_LOOPBACK  in-process, deterministic, for tests
 *
 * Any backend can be wrapped by an impairment layer (latency, jitter,
 * loss, duplication, reordering) driven by a seeded PRNG.
 *
 * vnet carries UNRELIABLE, UNORDERED datagrams only. Reliability and
 * ordering are the game protocol's job.
 *
 * ---------------------------------------------------------------------
 * Guarantees
 * ---------------------------------------------------------------------
 *  - Nothing in this API blocks, sleeps or spawns threads.
 *  - There is no global state. Every vnet_create() gives an independent
 *    context; create/destroy can be repeated freely.
 *  - After setup, steady-state send/receive/poll performs no heap
 *    allocation (queues are sized and allocated in vnet_create() and
 *    vnet_open()).
 *  - Threading: a vnet context, and every loopback network and context
 *    attached to it, must be used from one thread at a time. Separate
 *    UDP contexts may be used from separate threads.
 *  - A datagram larger than VNET_MAX_DATAGRAM is rejected, never
 *    truncated, in both directions.
 *
 * Return values: functions returning int return >= 0 on success and a
 * negative VNET_E* code on failure. See vnet_strerror().
 *
 * Time: all times are milliseconds. A clock is a function returning a
 * monotonic millisecond count. The default clock of the UDP backend is
 * the system monotonic clock; the default clock of the loopback backend
 * is the loopback network's virtual clock, which only moves when you
 * call vnet_loopback_network_advance() or _step() (so tests are exactly
 * repeatable and never sleep).
 */
#ifndef VNET_H
#define VNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VNET_VERSION_MAJOR 0
#define VNET_VERSION_MINOR 1
#define VNET_VERSION_PATCH 0

/*
 * Maximum datagram payload in bytes. It is chosen so that a vnet datagram
 * plus relay framing, WebTransport and QUIC overhead fits in a minimum-MTU
 * path. This is part of the ABI of every game that uses vnet: it will not
 * be raised.
 */
#define VNET_MAX_DATAGRAM 1100

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

#define VNET_OK            0
#define VNET_EAGAIN       (-1)  /* nothing to receive (or OS send buffer full) */
#define VNET_EINVAL       (-2)  /* bad argument, or unparsable address/option */
#define VNET_EMSGSIZE     (-3)  /* datagram > VNET_MAX_DATAGRAM (send), or
                                   larger than the caller's buffer (recv;
                                   the datagram is consumed, not truncated) */
#define VNET_EBADF        (-4)  /* not an open socket handle of this context */
#define VNET_EADDRINUSE   (-5)  /* port or loopback host address already taken */
#define VNET_ENOMEM       (-6)  /* out of memory */
#define VNET_ENOSOCK      (-7)  /* no free socket slot (see max_sockets) */
#define VNET_EAFNOSUPPORT (-8)  /* address family not usable on this
                                   backend/socket */
#define VNET_EIO          (-9)  /* unexpected OS error; details via the log */
#define VNET_ENOTSUP     (-10)  /* backend not available in this build */
#define VNET_ENOBUFS     (-11)  /* output buffer too small */

/* Human-readable text for a VNET_* code (never NULL; static storage). */
const char *vnet_strerror(int err);

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

#define VNET_AF_NONE 0
#define VNET_AF_IPV4 4
#define VNET_AF_IPV6 6

/*
 * A transport address. IPv4 uses bytes[0..3] (network order), the rest
 * must be zero. IPv6 uses all 16 bytes. port is in host byte order.
 * Always zero-initialise (vnet_addr a = {0};) and compare with
 * vnet_addr_equal(), not memcmp().
 *
 * Relay backends (later) may hand out addresses that are not real IPs but
 * still fit this struct.
 */
typedef struct vnet_addr {
    uint8_t  family;     /* VNET_AF_* */
    uint8_t  bytes[16];
    uint16_t port;
} vnet_addr;

/* Context ("host"). Opaque. */
typedef struct vnet vnet;

/*
 * Parse "1.2.3.4:5000", "[::1]:5000" or "localhost:5000" (-> 127.0.0.1).
 * Numeric only: no DNS lookup is ever done, so this never blocks.
 * IPv4-mapped IPv6 literals ("[::ffff:1.2.3.4]:5") are canonicalised to
 * IPv4. The port is required (0 is accepted so that it can express
 * "any port", but vnet_sendto() rejects port 0).
 * Returns 0, or VNET_EINVAL. `v` is only used so that backends may add
 * their own address forms; it may be NULL for the built-in forms.
 */
int vnet_resolve(vnet *v, const char *host_port, vnet_addr *out);

/*
 * Format as "1.2.3.4:5000" / "[::1]:5000" (IPv6 in RFC 5952 compressed
 * form). Returns the length written, excluding the terminating NUL, or
 * VNET_EINVAL (bad address) / VNET_ENOBUFS (cap too small; buf is left
 * as an empty or truncated NUL-terminated string when cap > 0).
 */
int vnet_addr_to_string(const vnet_addr *a, char *buf, size_t cap);

/* 1 if the two addresses (family, address bytes and port) are equal. */
int vnet_addr_equal(const vnet_addr *a, const vnet_addr *b);

/* ------------------------------------------------------------------ */
/* Impairment                                                          */
/* ------------------------------------------------------------------ */

/*
 * Network impairment applied to every datagram a context SENDS. It is
 * all-zero (= off) by default. Each packet gets:
 *
 *   loss        dropped with probability loss_pct
 *   duplication a second copy is sent with probability duplicate_pct;
 *               the copy has its own jitter, so it may arrive out of order
 *   latency     delay = latency_ms + uniform[-jitter_ms, +jitter_ms],
 *               clamped to >= 0
 *   reordering  with probability reorder_pct the packet is held back by an
 *               extra reorder_delay_ms (0 = 50 ms) so later packets
 *               overtake it
 *
 * Randomness comes from a private seeded PRNG: the same seed, the same
 * settings, the same sends and the same clock give exactly the same
 * outcome. Contexts that should behave independently need different seeds.
 *
 * Delayed datagrams are released to the backend by vnet_poll() (and also
 * on each vnet_sendto()), so a game must keep calling vnet_poll().
 * Percentages are 0..100 and have 0.0001% resolution.
 */
typedef struct vnet_impair {
    uint32_t latency_ms;
    uint32_t jitter_ms;
    float    loss_pct;
    float    duplicate_pct;
    float    reorder_pct;
    uint32_t reorder_delay_ms;
    uint64_t seed;
} vnet_impair;

/*
 * Parse an impairment spec like "lat=80,jit=20,loss=5,dup=1,reorder=3,
 * rdelay=40,seed=7" (keys: lat, jit, loss, dup, reorder, rdelay, seed;
 * numbers may be decimal for the percentages). The empty string means no
 * impairment. Keys not mentioned are zero; `out` is written only on success.
 * Returns 0 or VNET_EINVAL.
 */
int vnet_impair_parse(const char *spec, vnet_impair *out);

/* ------------------------------------------------------------------ */
/* Creation                                                            */
/* ------------------------------------------------------------------ */

#define VNET_BACKEND_UDP      0   /* the default: a zeroed config is UDP */
#define VNET_BACKEND_LOOPBACK 1

#define VNET_FAMILY_ANY 0   /* UDP: dual-stack IPv6 socket if available,
                               else IPv4 */
#define VNET_FAMILY_IPV4 4
#define VNET_FAMILY_IPV6 6

#define VNET_LOG_ERROR 0
#define VNET_LOG_WARN  1
#define VNET_LOG_INFO  2
#define VNET_LOG_DEBUG 3

/* Log sink. Called synchronously from inside vnet calls; msg is a
 * NUL-terminated line without newline, valid only during the call. */
typedef void (*vnet_log_fn)(void *user, int level, const char *msg);

/* Monotonic millisecond clock. */
typedef uint64_t (*vnet_clock_fn)(void *user);

/* A set of loopback hosts that can reach one another. Opaque. */
typedef struct vnet_loopback_network vnet_loopback_network;

typedef struct vnet_config {
    int backend;                  /* VNET_BACKEND_* */
    int max_sockets;              /* 0 = 16 */

    /* UDP only */
    int family;                   /* VNET_FAMILY_*; 0 = ANY */

    /* Loopback only */
    vnet_loopback_network *loopback_network; /* NULL = private network, only
                                                  this context in it */
    const char *loopback_host;    /* this host's IPv4 address, e.g.
                                     "127.1.0.7"; NULL = next free 127.1.x.y
                                     (127.1.0.1, 127.1.0.2, ...) */
    int rx_queue_len;             /* per-socket receive queue, datagrams;
                                     0 = 128. Overflow drops, like UDP. */

    /* Impairment (any backend) */
    vnet_impair impair;
    int impair_queue_len;         /* max delayed datagrams held; 0 = 256.
                                     Overflow drops. */
    int ignore_env;               /* 0: the VNET_IMPAIR environment variable,
                                     if set, REPLACES `impair` (its seed
                                     falls back to impair.seed). A malformed
                                     value makes vnet_create() fail with
                                     VNET_EINVAL. 1: never read it. */

    /* Optional hooks */
    vnet_clock_fn clock;          /* NULL = backend default (see top) */
    void *clock_user;
    vnet_log_fn log;              /* NULL = silent */
    void *log_user;
} vnet_config;

/* Set `cfg` to defaults (equivalent to zeroing it). */
void vnet_config_init(vnet_config *cfg);

/*
 * Create a context. Returns NULL on failure and, if err != NULL, stores a
 * VNET_E* code there. cfg may be NULL (= UDP defaults). A context made on
 * a loopback network must be destroyed before the network is.
 */
vnet *vnet_create(const vnet_config *cfg, int *err);
void  vnet_destroy(vnet *v);   /* closes all its sockets; NULL is a no-op */

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

/*
 * Open a datagram socket bound to `port` on all local addresses (UDP) or
 * on this host's address (loopback). port 0 picks an ephemeral port.
 * Returns a socket handle >= 0 (small integer, local to this context),
 * or VNET_EADDRINUSE / VNET_ENOSOCK / VNET_EAFNOSUPPORT / VNET_EIO ...
 */
int  vnet_open(vnet *v, uint16_t port);
void vnet_close(vnet *v, int s);   /* also discards its delayed datagrams */

/* The address `s` is bound to. For a UDP socket bound to all addresses the
 * IP part is the wildcard (0.0.0.0 or ::); give peers 127.0.0.1:port or
 * the real host address. */
int  vnet_local_addr(vnet *v, int s, vnet_addr *out);

/*
 * Send one datagram. Never blocks. Returns len (>= 0) when the datagram
 * was accepted. Acceptance is NOT delivery: like UDP, a datagram may be
 * lost, duplicated, delayed or reordered, and a datagram to an unknown
 * destination silently vanishes.
 *   VNET_EMSGSIZE  len > VNET_MAX_DATAGRAM (nothing is sent)
 *   VNET_EINVAL    NULL/invalid destination, port 0, or buf NULL with len>0
 *   VNET_EBADF     bad socket handle
 *   VNET_EAFNOSUPPORT  destination family not usable from this socket
 *   VNET_EAGAIN    UDP only: the OS send buffer is full; treat as a drop
 * len may be 0.
 */
int vnet_sendto(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to);

/*
 * Receive one datagram. Never blocks. Returns the datagram length (>= 0)
 * and fills `from` (may be NULL), or:
 *   VNET_EAGAIN    no datagram is waiting
 *   VNET_EMSGSIZE  the next datagram is longer than `cap`; it is
 *                  consumed and discarded, never truncated
 *   VNET_EBADF     bad socket handle
 * Datagrams longer than VNET_MAX_DATAGRAM arriving from foreign senders
 * on a UDP socket are dropped and never reported.
 */
int vnet_recvfrom(vnet *v, int s, void *buf, size_t cap, vnet_addr *from);

/*
 * Pump the context; call once per frame (and as often as you like).
 * Releases impaired datagrams whose delay has elapsed and, on the
 * loopback backend, makes datagrams sent to this context since the last
 * poll receivable. Async backends (relay, browser) do their I/O here.
 */
void vnet_poll(vnet *v);

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

typedef struct vnet_stats {
    uint64_t tx_datagrams;       /* accepted by vnet_sendto() */
    uint64_t tx_bytes;
    uint64_t rx_datagrams;       /* returned by vnet_recvfrom() */
    uint64_t rx_bytes;
    uint64_t wire_datagrams;     /* handed to the backend after impairment */
    uint64_t impair_lost;        /* dropped by loss */
    uint64_t impair_duplicated;  /* extra copies created */
    uint64_t impair_reordered;   /* packets given the reorder delay */
    uint64_t impair_overflow;    /* dropped: delay queue full */
    uint64_t rx_overflow;        /* dropped: receive queue full (loopback) */
    uint64_t rx_oversize;        /* dropped: > VNET_MAX_DATAGRAM (UDP) */
    uint64_t unroutable;         /* loopback: no such host/port */
} vnet_stats;

void vnet_get_stats(const vnet *v, vnet_stats *out);

/* ------------------------------------------------------------------ */
/* Loopback network                                                    */
/* ------------------------------------------------------------------ */

/*
 * Delivery model: vnet_sendto() on one loopback context stages the
 * datagram in the destination socket's queue (after impairment); it
 * becomes receivable when the DESTINATION context next calls vnet_poll().
 * Datagrams are delivered in global send order. With impairment on, the
 * sender must also poll once the delay has elapsed (virtual time).
 *
 * Sending to 127.0.0.1 reaches the sender's own host. Sending to an
 * address that is not a host of the network is silently dropped.
 */
vnet_loopback_network *vnet_loopback_network_create(void);
void vnet_loopback_network_destroy(vnet_loopback_network *net);

/* The virtual clock, in milliseconds, starting at 0. */
uint64_t vnet_loopback_network_time(const vnet_loopback_network *net);
void     vnet_loopback_network_advance(vnet_loopback_network *net, uint64_t ms);

/*
 * Test convenience: advance the virtual clock by `ms`, then release due
 * impaired datagrams from every context, then poll every context (in
 * creation order). After this, everything that is due is receivable.
 */
void vnet_loopback_network_step(vnet_loopback_network *net, uint64_t ms);

#ifdef __cplusplus
}
#endif

#endif /* VNET_H */
