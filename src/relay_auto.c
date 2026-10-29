// SPDX-License-Identifier: MIT
// VNET_BACKEND_RELAY_AUTO: WebTransport if it works, WebSocket if not. The
// protocol core does the switching (relay_client.c); this file only picks the
// two URLs and opens the first transport.
#include "relay_proto.h"
#include "vnet_internal.h"

#include <stdlib.h>
#include <string.h>

#define URL_MAX 512

// "wss://host[:port]/path" -> "https://host[:port]/path". Anything else has no
// WebTransport form (WebTransport needs TLS).
int vnet__relay_auto_derive_wt_url(const char *ws, char *out, size_t cap) {
    static const char from[] = "wss://";
    if (strncmp(ws, from, sizeof from - 1) != 0 || ws[sizeof from - 1] == 0) return -1;
    size_t rest = strlen(ws) - (sizeof from - 1);
    if (rest + 8 + 1 > cap) return -1;
    memcpy(out, "https://", 8);
    memcpy(out + 8, ws + sizeof from - 1, rest + 1);
    return 0;
}

static int ws_fallback(vnet *v, const void *arg, const relay_transport_ops **ops, void **tp) {
    return vnet__relay_ws_make(v, (const char *)arg, ops, tp);
}

int vnet__relay_auto_init(vnet *v, const vnet_config *cfg) {
    const char *ws_url = cfg->relay;
    if (!ws_url || strlen(ws_url) >= URL_MAX || (strncmp(ws_url, "ws://", 5) != 0 && strncmp(ws_url, "wss://", 6) != 0))
        return VNET_EINVAL;

    char derived[URL_MAX];
    const char *wt_url = NULL;
    if (cfg->relay_wt && cfg->relay_wt[0]) {
        if (strlen(cfg->relay_wt) >= URL_MAX) return VNET_EINVAL;
        wt_url = cfg->relay_wt;
    } else if (vnet__relay_auto_derive_wt_url(ws_url, derived, sizeof derived) == 0) {
        wt_url = derived;
    }

    const relay_transport_ops *ops = NULL;
    void *tp = NULL;
    if (wt_url) vnet__logf(v, VNET_LOG_INFO, "trying WebTransport at %s, WebSocket at %s as the fallback", wt_url, ws_url);
    int rc = wt_url ? vnet__relay_wt_make(v, wt_url, cfg->relay_cert_sha256, &ops, &tp) : VNET_ENOTSUP;
    if (rc == VNET_EINVAL) return rc;  // a malformed URL or hash is the game's bug, not a reason to fall back
    if (rc != VNET_OK) {
        // No WebTransport here (an old browser, an insecure page, a ws:// relay):
        // the WebSocket path from the start.
        vnet__logf(v, VNET_LOG_INFO, "WebTransport not available (%s): using WebSocket",
                   wt_url ? "no WebTransport in this browser or page" : "no wss:// URL to derive one from and no relay_wt");
        return vnet__relay_ws_init(v, cfg);
    }
    return vnet__relay_init_fallback(v, cfg, ops, tp, cfg->wt_timeout_ms > 0 ? (unsigned)cfg->wt_timeout_ms : 3000u,
                                     ws_fallback, ws_url, strlen(ws_url) + 1);
}
