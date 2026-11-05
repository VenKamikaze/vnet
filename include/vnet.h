/* SPDX-License-Identifier: MIT */
/*
 * vnet - virtual UDP for games.
 *
 * A small, dependency-free, non-blocking datagram API with pluggable
 * backends:
 *
 *   VNET_BACKEND_UDP        native UDP sockets (BSD sockets / Winsock)
 *   VNET_BACKEND_LOOPBACK   in-process, deterministic, for tests
 *   VNET_BACKEND_RELAY_UDP  native client of the vnet relay (rooms, virtual
 *                           addresses 10.0.0.N); see doco/relay-protocol.md
 *   VNET_BACKEND_RELAY_WS   browser (Emscripten) client of the vnet relay
 *                           over a WebSocket
 *   VNET_BACKEND_RELAY_WT   browser client of the relay over WebTransport
 *                           (unreliable datagrams)
 *   VNET_BACKEND_RELAY_AUTO the one constant for every build: in a browser,
 *                           WebTransport if it works, else WebSocket; natively,
 *                           the same as RELAY_UDP (see vnet_config.relay_udp)
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
 *  - Nothing in this API blocks, sleeps or spawns threads, except by name:
 *    vnet_wait() (sleeps until a datagram arrives or a timeout) and
 *    vnet_resolve_host() (a DNS lookup). vnet_create() with
 *    VNET_BACKEND_RELAY_UDP (or _AUTO, natively) also resolves a relay HOST
 *    NAME with getaddrinfo(), which can block; a numeric address never does.
 *  - There is no global state. Every vnet_create() gives an independent
 *    context; create/destroy can be repeated freely.
 *  - After setup, steady-state send/receive/poll performs no heap
 *    allocation (queues are sized and allocated in vnet_create() and
 *    vnet_open()). That is vnet's own code: in a browser build, Emscripten's
 *    WebSocket glue, vnet's WebTransport glue (a copy of every frame in each
 *    direction, in JavaScript) and the browser allocate for every message or
 *    datagram that arrives or is sent.
 *  - Threading: a vnet context, and every loopback network and context
 *    attached to it, must be used from one thread at a time. Separate
 *    UDP contexts may be used from separate threads.
 *  - A datagram larger than VNET_MAX_DATAGRAM is rejected, never
 *    truncated, in both directions.
 *
 * Return values: functions returning int return >= 0 on success and a
 * negative VNET_E* code on failure. See vnet_strerror(). Future versions
 * and backends may add further negative codes: treat any negative value you
 * do not recognise as an error (and VNET_EAGAIN / VNET_EIO as transient).
 *
 * API stability (see doco/integration.md): from 0.3.0 on, 0.x releases only
 * add. New fields go at the END of vnet_config, vnet_impair and
 * vnet_status_info (so source written for an older version, with its structs
 * zeroed or vnet_config_init()ed, keeps its meaning when rebuilt), and new
 * functions, error codes and backends may appear; nothing is removed, renamed
 * or reinterpreted. This is source compatibility, not binary: vnet is a static
 * library whose structs grow, so rebuild your code against the header of the
 * vnet you link (never mix a new library with objects compiled against an old
 * header). 1.0 follows once Bit Riot's native and
 * browser netplay have used it. CMake's SameMajorVersion treats every 0.x as
 * compatible, which is more generous than that promise: pin an exact release
 * tag.
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
#define VNET_VERSION_MINOR 4
#define VNET_VERSION_PATCH 0

/* The version of the library that is linked in, e.g. "0.4.0": for logs and bug
 * reports. (The macros above are the version of the header you compiled
 * against; the two differ only if the header and the library do.) Static
 * storage, never NULL. */
const char *vnet_version(void);

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
#define VNET_EIO          (-9)  /* OS error; details via the log. From
                                   vnet_sendto()/vnet_recvfrom() it is
                                   transient (e.g. network unreachable):
                                   treat it like a dropped datagram, not as
                                   a dead socket */
#define VNET_ENOTSUP     (-10)  /* backend not available in this build */
#define VNET_ENOBUFS     (-11)  /* output buffer too small */
/* Relay backends. They are reported by vnet_status() (FAILED), and ENOTCONN
 * also by vnet_sendto() / vnet_local_addr(). */
#define VNET_ETIMEDOUT   (-12)  /* the relay did not answer, or went silent */
#define VNET_ENOTCONN    (-13)  /* not in a room: still connecting, or the
                                   session has failed or ended */
#define VNET_ENOROOM     (-14)  /* no room with that code */
#define VNET_EROOMFULL   (-15)  /* the room has no free address */
#define VNET_ELIMIT      (-16)  /* the relay refused: a server limit (rooms,
                                   members) or a rate limit */
#define VNET_EPROTO      (-17)  /* relay protocol error or version mismatch */
#define VNET_ECLOSED     (-18)  /* the session ended: the relay dropped us
                                   (timeout, room closed, restart) */
#define VNET_ECONNREFUSED (-19) /* the relay could not be reached */

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
 * their own address forms; it may be NULL for the built-in forms. Relay
 * contexts add "room-host:PORT" = 10.0.0.1:PORT, the room creator (this
 * works before the room has been entered). Their addresses are plain
 * 10.0.0.N:PORT IPv4 addresses, so everything else here applies to them.
 */
int vnet_resolve(vnet *v, const char *host_port, vnet_addr *out);

/*
 * Resolve "host:port" like vnet_resolve(), but also look up a host NAME with
 * the system resolver (getaddrinfo). THIS BLOCKS, for as long as DNS takes
 * (seconds on a bad network), so call it from a worker thread or at settings
 * load time, never from a frame; then hand the numeric result to vnet_create()
 * (vnet_addr_to_string() gives a "host:port" string for vnet_config.relay).
 * `family` is VNET_FAMILY_ANY (IPv4 preferred when the name has both),
 * VNET_FAMILY_IPV4 or VNET_FAMILY_IPV6; an address of another family is
 * VNET_EAFNOSUPPORT. Numeric addresses and "localhost" are handled without
 * a lookup. It needs no context and is thread safe.
 * Returns 0; VNET_EINVAL (malformed, or port missing); VNET_EAFNOSUPPORT;
 * VNET_EIO (the lookup failed: unknown name, no network); VNET_ENOTSUP in
 * browser builds, which have no resolver.
 */
int vnet_resolve_host(const char *host_port, int family, vnet_addr *out);

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
 *   burst loss  a two-state (Gilbert-Elliott) model on top of loss_pct: a
 *               packet sent in the "good" state starts a burst with
 *               probability burst_pct; every packet of a burst is dropped, and
 *               after each dropped packet the burst ends with probability
 *               1/burst_len. So bursts last burst_len packets on average
 *               (geometric, at least 1). It counts packets, not milliseconds:
 *               a burst of N packets is a blackout of N / send-rate.
 *               burst_pct = 0 is off. It draws from a PRNG stream of its own,
 *               so switching it on or off never changes the other draws.
 *
 * Randomness comes from a private seeded PRNG: the same seed, the same
 * settings, the same sends and the same clock give exactly the same
 * outcome. Contexts that should behave independently need different seeds.
 * NOTE: the default seed is 0, so if you set VNET_IMPAIR (or
 * vnet_config.impair) without a seed, EVERY process and context gets the
 * same seed and therefore the same loss pattern. Give each its own
 * (e.g. "seed=<player number>").
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
    /* Added in 0.4.0, at the end like every later addition. */
    float    burst_pct;      /* 0..100; 0 = no burst loss */
    float    burst_len;      /* mean burst length in packets, 1..100000; ignored
                                when burst_pct is 0, required when it is not */
} vnet_impair;

/*
 * Parse an impairment spec like "lat=80,jit=20,loss=5,dup=1,reorder=3,
 * rdelay=40,burst=2,blen=5,seed=7" (keys: lat, jit, loss, dup, reorder,
 * rdelay, burst = burst_pct, blen = burst_len, seed; numbers may be decimal
 * for the percentages and blen). The empty string means no
 * impairment. Keys not mentioned are zero; `out` is written only on success.
 * Returns 0 or VNET_EINVAL.
 */
int vnet_impair_parse(const char *spec, vnet_impair *out);

/* ------------------------------------------------------------------ */
/* Creation                                                            */
/* ------------------------------------------------------------------ */

#define VNET_BACKEND_UDP      0   /* the default: a zeroed config is UDP */
#define VNET_BACKEND_LOOPBACK 1
#define VNET_BACKEND_RELAY_UDP 2  /* native client of the relay */
#define VNET_BACKEND_RELAY_WS  3  /* browser client of the relay (Emscripten
                                     builds only; VNET_ENOTSUP elsewhere) */
#define VNET_BACKEND_RELAY_WT  4  /* browser client of the relay over
                                     WebTransport (Emscripten builds only).
                                     `relay` is an "https://host:port/vnet" URL.
                                     Datagrams are really unreliable and a lost
                                     one stalls nothing. Fails (FAILED) where the
                                     browser or the network has no WebTransport,
                                     or (VNET_EMSGSIZE) if the check that a full
                                     VNET_MAX_DATAGRAM datagram fits is lost to
                                     very heavy packet loss: RELAY_AUTO would just
                                     use the WebSocket then */
#define VNET_BACKEND_RELAY_AUTO 5 /* the same constant in every build.
                                     Browser: try WebTransport, fall back to
                                     WebSocket; `relay` is the WebSocket URL
                                     ("wss://host/vnet"), see relay_wt below.
                                     Native: RELAY_UDP, to the relay's UDP
                                     address `relay_udp`, or `relay` if that
                                     is NULL and has no URL scheme; anything
                                     else is VNET_EINVAL. vnet_status() says
                                     which transport was chosen (`transport`) */

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

/* Monotonic millisecond clock (the epoch is arbitrary). */
typedef uint64_t (*vnet_clock_fn)(void *user);

/*
 * Wake callback (browser relay transports only; see vnet_config.wake). It is
 * called from a browser event: when a frame has been queued for vnet_poll()
 * and when the transport opens, fails or closes. It must do nothing but
 * arrange for vnet_poll() to run soon (set a flag, post a message, call
 * emscripten_async_call()): it must NOT call any vnet function, and it must
 * not block. It is called from browser events only (never from inside a vnet
 * call), at any time between the start of vnet_create() and vnet_destroy(), and
 * never after vnet_destroy() returns. It does not replace polling on a timer:
 * vnet's own timers (a WebSocket connect retry, the WebTransport-to-WebSocket
 * fallback, the handshake timeout, keepalives) run only inside vnet_poll(), so
 * keep a timer-driven poll going as well (once a second is plenty) and use the
 * wake to poll sooner.
 */
typedef void (*vnet_wake_fn)(void *user);

/* A set of loopback hosts that can reach one another. Opaque. */
typedef struct vnet_loopback_network vnet_loopback_network;

typedef struct vnet_config {
    int backend;                  /* VNET_BACKEND_* */
    int max_sockets;              /* 0 = 16 */

    /* UDP only */
    int family;                   /* VNET_FAMILY_*; 0 = ANY:
                                     ANY   - a dual-stack IPv6 socket that also
                                             reaches IPv4 peers (IPv4 peers
                                             appear as plain IPv4 addresses);
                                             falls back to an IPv4 socket if
                                             the OS has no IPv6.
                                     IPV4  - IPv4 only; IPv6 destinations are
                                             VNET_EAFNOSUPPORT.
                                     IPV6  - IPv6 only (IPV6_V6ONLY); IPv4
                                             destinations are
                                             VNET_EAFNOSUPPORT. Opening fails if
                                             the OS has no IPv6. */

    /* Loopback only */
    vnet_loopback_network *loopback_network; /* NULL = private network, only
                                                  this context in it */
    const char *loopback_host;    /* this host's IPv4 address, e.g.
                                     "127.1.0.7"; NULL = next free 127.1.x.y
                                     (127.1.0.1, 127.1.0.2, ...) */
    int rx_queue_len;             /* per-socket receive queue, datagrams
                                     (loopback and relay backends); 0 = 128.
                                     Overflow drops, like UDP. */

    /* Impairment (any backend) */
    vnet_impair impair;
    int impair_queue_len;         /* max delayed datagrams held; 0 = 256.
                                     Overflow drops. */
    int ignore_env;               /* 0: the VNET_IMPAIR environment variable,
                                     if set, REPLACES `impair` (its seed
                                     falls back to impair.seed). A malformed
                                     value makes vnet_create() fail with
                                     VNET_EINVAL. 1: never read it. */

    /* Relay backends (VNET_BACKEND_RELAY_UDP / _WS). Strings must stay valid
     * only for the duration of vnet_create(). */
    const char *relay;            /* RELAY_UDP: "host:port" or "[v6]:port" of
                                     the relay's UDP port (a host name is
                                     resolved once, inside vnet_create(); see
                                     vnet_resolve_host() to do it earlier).
                                     RELAY_WS / RELAY_AUTO in a browser:
                                     "ws://host:port/vnet" or "wss://host/vnet".
                                     RELAY_AUTO natively: used as the UDP
                                     address only if `relay_udp` is NULL and
                                     this has no "scheme://". */
    const char *room;             /* NULL or "": create a new room (you become
                                     10.0.0.1, "room-host"). Otherwise join the
                                     room with this 6-symbol code (case
                                     insensitive). */
    int relay_timeout_ms;         /* give up (VNET_ETIMEDOUT) if no room has
                                     been entered after this long; 0 = 10000 */
    int keepalive_ms;             /* keepalive interval once in a room; 0 = the
                                     relay's suggestion (5000). Otherwise
                                     100..5000, else VNET_EINVAL: the relay drops
                                     a silent member after 15 s by default, so
                                     keep it at or below a third of that. The
                                     session is declared lost (VNET_ETIMEDOUT)
                                     after three intervals without any frame
                                     from the relay. */

    /* Optional hooks */
    vnet_clock_fn clock;          /* NULL = backend default (see top) */
    void *clock_user;
    vnet_log_fn log;              /* NULL = silent */
    void *log_user;

    /* Relay backends over WebTransport (VNET_BACKEND_RELAY_WT / _AUTO).
     * Strings must stay valid only for the duration of vnet_create().
     * (Added at the end of the struct, after the hooks.) */
    const char *relay_wt;         /* AUTO: the WebTransport URL ("https://host:port/vnet").
                                     NULL or "": derived from `relay` when that is
                                     "wss://host[:port]/path" (-> "https://..."
                                     with the same host, port and path); a plain
                                     "ws://" `relay` has no WebTransport form, so
                                     AUTO then uses WebSocket only */
    const char *relay_cert_sha256; /* NULL: the browser verifies the relay's
                                     certificate normally (production). Otherwise
                                     64 hex digits, the SHA-256 of a relay
                                     development certificate (-dev-cert), passed
                                     to the browser as serverCertificateHashes */
    int wt_timeout_ms;            /* AUTO: give WebTransport this long (connect,
                                     room, datagram-size check) before falling
                                     back to WebSocket; 0 = 3000. The WebSocket
                                     attempt then gets relay_timeout_ms in full */

    /* Added in 0.4.0 (again at the end). */
    const char *relay_udp;        /* RELAY_AUTO, native builds: the relay's UDP
                                     address "host:port" or "[v6]:port", as for
                                     RELAY_UDP. Browser builds ignore it, and
                                     native builds ignore `relay` when it is
                                     set, so one config can carry both the UDP
                                     address and the WebSocket URL of a relay.
                                     Needs to live only during vnet_create() */
    vnet_wake_fn wake;            /* NULL = none. Browser relay transports (WS,
                                     WT, and AUTO's choice of them) call it from
                                     the browser event that queued a frame or
                                     changed the transport's state, so a page
                                     whose main loop is throttled or stopped
                                     (a hidden tab) can run vnet_poll() from the
                                     events instead. It may only schedule that
                                     poll; see vnet_wake_fn. Native backends
                                     (UDP, loopback, relay-UDP) never call it. */
    void *wake_user;              /* passed to `wake` */
} vnet_config;

/* Set `cfg` to defaults (equivalent to zeroing it). */
void vnet_config_init(vnet_config *cfg);

/*
 * Replace the impairment of a live context (e.g. 500 ms of total loss and then
 * recovery in a test, or an in-game network debug menu).
 *   imp NULL or all-zero (every field 0): impairment off.
 *   otherwise: the new settings, validated like vnet_config.impair, with the
 *   PRNG reseeded from imp->seed so that runs stay reproducible.
 * Datagrams already held back for delay stay held and are released on schedule,
 * whatever the new settings say (also after impairment was turned off); only
 * datagrams sent from now on are subject to the new settings. The call is
 * explicit and wins over the VNET_IMPAIR environment variable, which only
 * vnet_create() reads. Turning impairment on for the first time allocates the
 * delay queue (impair_queue_len as given to vnet_create(); 0 = 256).
 * Returns 0; VNET_EINVAL (a bad value; nothing changes) or VNET_ENOMEM (nothing
 * changes); VNET_ENOTSUP on a backend with no impairment layer. Every backend
 * of this version (UDP, loopback, relay-UDP/WS/WT) has one, so ENOTSUP is
 * reserved for backends added later.
 */
int vnet_set_impair(vnet *v, const vnet_impair *imp);

/*
 * Create a context. Returns NULL on failure and, if err != NULL, stores a
 * VNET_E* code there (VNET_OK on success):
 *   VNET_EINVAL    bad config: impairment values out of range, malformed
 *                  VNET_IMPAIR, bad `family` or `loopback_host`
 *   VNET_ENOTSUP   unknown/unavailable `backend`
 *   VNET_EADDRINUSE  `loopback_host` already used on that network
 *   VNET_ENOMEM    out of memory
 *   VNET_EIO       OS networking could not be initialised (Winsock), or a
 *                  relay backend found no secure random source for its
 *                  handshake nonce
 * cfg may be NULL (= UDP defaults). A context made on a loopback network
 * must be destroyed before the network is.
 */
vnet *vnet_create(const vnet_config *cfg, int *err);
void  vnet_destroy(vnet *v);   /* closes all its sockets; NULL is a no-op. A
                                  relay context leaves its room (best effort,
                                  one LEAVE frame) */

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

/* Close a socket and discard its queued and delayed datagrams. A handle that
 * is not open (never opened, already closed, out of range) or a NULL `v` is
 * silently ignored. Handles may be reused by later vnet_open() calls. */
void vnet_close(vnet *v, int s);

/* The address `s` is bound to. For a UDP socket bound to all addresses the
 * IP part is the wildcard (0.0.0.0 or ::); give peers 127.0.0.1:port or
 * the real host address.
 * Relay contexts: the virtual address 10.0.0.N and the port; before the room
 * has been entered (see vnet_status()) VNET_ENOTCONN, because the address is
 * not known yet.
 * Returns 0, VNET_EINVAL (NULL argument), VNET_EBADF or VNET_EIO. */
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
 *   VNET_EAGAIN    any backend: the OS send buffer is full (UDP), or the
 *                  browser is not keeping up (WebSocket backpressure, too
 *                  many datagram writes in flight over WebTransport). Treat
 *                  it as a drop, never as a reason to end the session
 *   VNET_EIO       transient OS error (network unreachable ...); a drop too
 *   VNET_ENOTCONN  relay only: not in a room (see vnet_status()); nothing
 *                  is sent. A destination that is not in your room is not an
 *                  error: it is dropped by the relay, like any lost datagram.
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
 *   VNET_EIO       UDP only: unexpected OS error; transient, retry later
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

/*
 * Sleep until a datagram may be waiting, or `timeout_ms` has passed: for a
 * headless process or a dedicated server that would otherwise spin or nap in
 * fixed steps. It is the one call that blocks, by name. It does not receive
 * anything: afterwards call vnet_poll() and drain vnet_recvfrom() as usual.
 *   Returns 1  something may be ready (it can be spurious: poll anyway),
 *           0  nothing yet: the timeout passed, or a deadline of the context
 *              (an impaired datagram due for release, a relay keepalive or
 *              handshake retransmission) is here: call vnet_poll() either way,
 *           VNET_EINVAL  NULL context or negative timeout (0 = just look),
 *           VNET_ENOTSUP the backend cannot wait: loopback (it is driven by
 *              vnet_loopback_network_step()) and the browser backends, which
 *              never block,
 *           VNET_EIO an OS error.
 * UDP and relay-UDP wait with poll() / WSAPoll() on the context's sockets;
 * deadlines are judged by the context's clock, which is assumed to run in real
 * time (do not combine vnet_wait() with a virtual clock). A datagram already
 * queued in the context makes it return 1 at once.
 */
int vnet_wait(vnet *v, int timeout_ms);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

/*
 * Some backends need time to become usable: the relay backends connect and
 * enter a room asynchronously, driven by vnet_poll(). vnet_status() tells a
 * game where it is, so it can show "connecting..." and give up with a
 * message. It is generic: every backend answers it.
 */
#define VNET_STATUS_CONNECTING 1  /* setting up; datagrams cannot be sent yet */
#define VNET_STATUS_READY      2  /* usable. UDP and loopback: always. Relay:
                                     in a room, with a virtual address */
#define VNET_STATUS_FAILED     3  /* unusable; `error` says why. Final: create
                                     a new context to try again */

typedef struct vnet_status_info {
    int state;               /* VNET_STATUS_* */
    int error;               /* FAILED: the VNET_E* reason (VNET_ETIMEDOUT,
                                VNET_ENOROOM, VNET_EROOMFULL, VNET_ECONNREFUSED,
                                VNET_ECLOSED ...; VNET_EMSGSIZE when a
                                WebTransport connection cannot carry a
                                VNET_MAX_DATAGRAM datagram). Otherwise 0 */
    const char *transport;   /* static name: "udp", "loopback", "relay-udp",
                                "relay-ws", "relay-wt". With VNET_BACKEND_RELAY_AUTO
                                it names the transport in use, so it changes
                                from "relay-wt" to "relay-ws" if WebTransport
                                failed; once READY it is final */
    char room[8];            /* relay: the room code (NUL-terminated) once
                                READY, else "" */
    vnet_addr self;          /* relay: this context's virtual address
                                (10.0.0.N, port 0) once READY; family
                                VNET_AF_NONE otherwise */
    /* New fields are only ever added at the end of this struct. */
    /* Added in 0.4.0: */
    int relay_rtt_ms;        /* relay: the latest KEEPALIVE round trip to the
                                relay, in ms, measured at vnet_poll() time (so
                                with the resolution of your poll interval);
                                -1 until the first answer, which comes one
                                keepalive interval after READY (or at once
                                after a long gap between polls). UDP and
                                loopback: 0. This is the client-to-relay leg
                                only: add the other peer's for a peer RTT */
    int max_members;         /* relay: the room's member limit (from ROOM),
                                once READY; 0 otherwise */
} vnet_status_info;

/*
 * Returns the state (VNET_STATUS_*) and, if `out` is not NULL, fills it in.
 * Returns VNET_EINVAL for a NULL context. Cheap; call it as often as you
 * like, after vnet_poll(). While the state is CONNECTING:
 *   vnet_open() works (ports are local),
 *   vnet_sendto() returns VNET_ENOTCONN (nothing is queued),
 *   vnet_recvfrom() returns VNET_EAGAIN.
 * After FAILED the same applies, with the reason in `error`.
 *
 * Pauses. A relay context does not take a long gap between two vnet_poll()
 * calls (a hidden browser tab, a laptop that slept, a debugger) for the relay
 * going silent: after a gap of more than one keepalive interval it restarts
 * its silence and handshake clocks and sends a KEEPALIVE at once. Over
 * WebSocket and WebTransport the relay keeps the session as long as the
 * connection is alive, so the context stays READY. Over UDP the relay forgets a
 * member that sent nothing for its session timeout (15 s by default), so a
 * longer pause ends with FAILED / VNET_ECLOSED within a round trip of the next
 * poll (instead of after three more keepalive intervals).
 */
int vnet_status(vnet *v, vnet_status_info *out);

/*
 * The context's clock in milliseconds (arbitrary epoch, monotonic): the
 * loopback network's virtual clock on the loopback backend, the system
 * monotonic clock on UDP, or vnet_config.clock when one was supplied. Use it
 * for RTT and timeout logic so that the same code runs on virtual time in
 * tests. Returns 0 for a NULL context.
 */
uint64_t vnet_time(vnet *v);

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/*
 * Counters are cumulative since vnet_create() and are never reset (take a
 * snapshot and subtract to measure an interval). They are plain counters and
 * wrap only after 2^64 events.
 */
typedef struct vnet_stats {
    uint64_t tx_datagrams;       /* accepted by vnet_sendto() */
    uint64_t tx_bytes;
    uint64_t rx_datagrams;       /* returned by vnet_recvfrom() */
    uint64_t rx_bytes;
    uint64_t wire_datagrams;     /* accepted by the backend after impairment
                                    (a send the backend rejected, e.g. with
                                    VNET_EIO, is not counted) */
    uint64_t impair_lost;        /* dropped by loss */
    uint64_t impair_duplicated;  /* extra copies created */
    uint64_t impair_reordered;   /* packets given the reorder delay */
    uint64_t impair_overflow;    /* dropped: delay queue full */
    uint64_t rx_overflow;        /* dropped: receive queue full (loopback,
                                    relay) */
    uint64_t rx_oversize;        /* dropped: > VNET_MAX_DATAGRAM (UDP, relay) */
    uint64_t unroutable;         /* loopback: no such host/port; relay: a
                                    datagram for a port you have not opened */
} vnet_stats;

/* Copy the counters out. A NULL `v` gives zeros. */
void vnet_get_stats(const vnet *v, vnet_stats *out);

/* ------------------------------------------------------------------ */
/* Loopback network                                                    */
/* ------------------------------------------------------------------ */

/*
 * Delivery model: vnet_sendto() on one loopback context stages the
 * datagram in the destination socket's queue (after impairment); it
 * becomes receivable when the DESTINATION context next calls vnet_poll().
 * Datagrams become receivable in the order they reached the wire: send
 * order when impairment is off, release order (due time, then send order)
 * when it is on. With impairment on, the sender must also poll once the
 * delay has elapsed (virtual time).
 *
 * Sending to 127.0.0.1 reaches the sender's own host. Sending to an
 * address that is not a host of the network is silently dropped.
 */
/* Returns NULL if out of memory. */
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
