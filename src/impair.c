// SPDX-License-Identifier: MIT
// Impairment layer: latency, jitter, loss, duplication and reordering,
// driven by a seeded PRNG so that runs are exactly repeatable.
#include "vnet_internal.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_REORDER_DELAY_MS 50u
#define MAX_DELAY_MS 60000u
#define PPM 1000000u

typedef struct imp_slot {
    int used;
    int sock;
    size_t len;
    uint64_t due;
    uint64_t seq;
    vnet_addr to;
    uint8_t data[VNET_MAX_DATAGRAM];
} imp_slot;

struct vnet_imp {
    vnet_impair cfg;
    uint32_t loss_ppm, dup_ppm, reorder_ppm;
    uint32_t reorder_delay_ms;
    uint64_t rng;
    uint64_t seq;
    int cap;
    int count;
    imp_slot *slots;
};

// splitmix64: tiny, well distributed, and identical on every platform.
static uint64_t rng_next(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static uint32_t pct_to_ppm(float pct) {
    return (uint32_t)((double)pct * 10000.0 + 0.5);
}

int vnet__imp_enabled(const vnet_impair *c) {
    return c->latency_ms || c->jitter_ms || c->loss_pct > 0.0f || c->duplicate_pct > 0.0f ||
           c->reorder_pct > 0.0f;
}

int vnet__imp_validate(const vnet_impair *c) {
    // Written so that NaN fails every comparison.
    if (!(c->loss_pct >= 0.0f && c->loss_pct <= 100.0f)) return VNET_EINVAL;
    if (!(c->duplicate_pct >= 0.0f && c->duplicate_pct <= 100.0f)) return VNET_EINVAL;
    if (!(c->reorder_pct >= 0.0f && c->reorder_pct <= 100.0f)) return VNET_EINVAL;
    if (c->latency_ms > MAX_DELAY_MS || c->jitter_ms > MAX_DELAY_MS ||
        c->reorder_delay_ms > MAX_DELAY_MS)
        return VNET_EINVAL;
    return VNET_OK;
}

vnet_imp *vnet__imp_create(vnet *v, const vnet_impair *cfg, int queue_len) {
    (void)v;
    vnet_imp *imp = calloc(1, sizeof *imp);
    if (!imp) return NULL;
    imp->cap = queue_len > 0 ? queue_len : 256;
    imp->slots = calloc((size_t)imp->cap, sizeof *imp->slots);
    if (!imp->slots) {
        free(imp);
        return NULL;
    }
    imp->cfg = *cfg;
    imp->loss_ppm = pct_to_ppm(cfg->loss_pct);
    imp->dup_ppm = pct_to_ppm(cfg->duplicate_pct);
    imp->reorder_ppm = pct_to_ppm(cfg->reorder_pct);
    imp->reorder_delay_ms = cfg->reorder_delay_ms ? cfg->reorder_delay_ms : DEFAULT_REORDER_DELAY_MS;
    imp->rng = cfg->seed;
    return imp;
}

void vnet__imp_destroy(vnet_imp *imp) {
    if (!imp) return;
    free(imp->slots);
    free(imp);
}

static int roll(uint64_t *rng, uint32_t ppm) {
    // Always consume one value so that the draw sequence does not depend on
    // the configured percentages.
    uint32_t r = (uint32_t)(rng_next(rng) >> 32) % PPM;
    return r < ppm;
}

// Extra value in [0, 2*jitter] from one draw.
static uint64_t jitter_draw(uint64_t *rng, uint32_t jit) {
    uint64_t r = rng_next(rng) >> 11;
    return r % ((uint64_t)jit * 2 + 1);
}

static uint64_t delay_for(const vnet_imp *imp, uint64_t jdraw, int reordered) {
    int64_t d = (int64_t)imp->cfg.latency_ms + (int64_t)jdraw - (int64_t)imp->cfg.jitter_ms;
    if (d < 0) d = 0;
    if (reordered) d += imp->reorder_delay_ms;
    return (uint64_t)d;
}

static void enqueue(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to, uint64_t due) {
    vnet_imp *imp = v->imp;
    for (int i = 0; i < imp->cap; i++) {
        imp_slot *sl = &imp->slots[i];
        if (sl->used) continue;
        sl->used = 1;
        sl->sock = s;
        sl->len = len;
        sl->due = due;
        sl->seq = imp->seq++;
        sl->to = *to;
        if (len) memcpy(sl->data, buf, len);
        imp->count++;
        return;
    }
    v->stats.impair_overflow++;
}

void vnet__imp_submit(vnet *v, int s, const void *buf, size_t len, const vnet_addr *to) {
    vnet_imp *imp = v->imp;
    uint64_t now = vnet__now(v);

    // Fixed draw order: loss, dup, reorder, jitter(original), jitter(copy).
    int lost = roll(&imp->rng, imp->loss_ppm);
    int dup = roll(&imp->rng, imp->dup_ppm);
    int reorder = roll(&imp->rng, imp->reorder_ppm);
    uint64_t j1 = jitter_draw(&imp->rng, imp->cfg.jitter_ms);
    uint64_t j2 = jitter_draw(&imp->rng, imp->cfg.jitter_ms);

    if (lost) {
        v->stats.impair_lost++;
    } else {
        if (reorder) v->stats.impair_reordered++;
        enqueue(v, s, buf, len, to, now + delay_for(imp, j1, reorder));
        if (dup) {
            v->stats.impair_duplicated++;
            enqueue(v, s, buf, len, to, now + delay_for(imp, j2, 0));
        }
    }
    vnet__imp_release(v);
}

void vnet__imp_release(vnet *v) {
    vnet_imp *imp = v->imp;
    if (!imp || imp->count == 0) return;
    uint64_t now = vnet__now(v);
    for (;;) {
        imp_slot *best = NULL;
        for (int i = 0; i < imp->cap; i++) {
            imp_slot *sl = &imp->slots[i];
            if (!sl->used || sl->due > now) continue;
            if (!best || sl->due < best->due || (sl->due == best->due && sl->seq < best->seq))
                best = sl;
        }
        if (!best) return;
        best->used = 0;
        imp->count--;
        // Copy out first: the backend may not re-enter, but keep it simple.
        vnet__wire_send(v, best->sock, best->data, best->len, &best->to);
    }
}

void vnet__imp_purge_socket(vnet *v, int s) {
    vnet_imp *imp = v->imp;
    if (!imp) return;
    for (int i = 0; i < imp->cap; i++) {
        if (imp->slots[i].used && imp->slots[i].sock == s) {
            imp->slots[i].used = 0;
            imp->count--;
        }
    }
}

// ---------------------------------------------------------------------
// Spec parsing: "lat=80,jit=20,loss=5,dup=1,reorder=3,rdelay=40,seed=7"
// ---------------------------------------------------------------------

static int parse_u64(const char *s, size_t n, uint64_t *out) {
    if (n == 0) return -1;
    uint64_t v = 0;
    int base = 10;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        n -= 2;
    }
    for (size_t i = 0; i < n; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return -1;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return -1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = v;
    return 0;
}

// Locale-independent "12" / "12.5" / ".5" parser (no sign, no exponent).
static int parse_pct(const char *s, size_t n, float *out) {
    if (n == 0) return -1;
    double v = 0.0, scale = 0.1;
    size_t i = 0;
    int digits = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++, digits++) v = v * 10.0 + (s[i] - '0');
    if (i < n && s[i] == '.') {
        for (i++; i < n && s[i] >= '0' && s[i] <= '9'; i++, digits++) {
            v += (s[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    if (i != n || digits == 0 || v > 100.0) return -1;
    *out = (float)v;
    return 0;
}

int vnet_impair_parse(const char *spec, vnet_impair *out) {
    if (!spec || !out) return VNET_EINVAL;
    vnet_impair r;
    memset(&r, 0, sizeof r);
    int rc = vnet__impair_parse_into(spec, &r);
    if (rc == VNET_OK) *out = r;
    return rc;
}

int vnet__impair_parse_into(const char *spec, vnet_impair *out) {
    vnet_impair r = *out;
    const char *p = spec;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ',' && *end != ' ' && *end != '\t') end++;
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq) return VNET_EINVAL;
        size_t kn = (size_t)(eq - p);
        const char *val = eq + 1;
        size_t vn = (size_t)(end - val);
        uint64_t u;
        if (kn == 3 && !memcmp(p, "lat", 3)) {
            if (parse_u64(val, vn, &u) || u > MAX_DELAY_MS) return VNET_EINVAL;
            r.latency_ms = (uint32_t)u;
        } else if (kn == 3 && !memcmp(p, "jit", 3)) {
            if (parse_u64(val, vn, &u) || u > MAX_DELAY_MS) return VNET_EINVAL;
            r.jitter_ms = (uint32_t)u;
        } else if (kn == 4 && !memcmp(p, "loss", 4)) {
            if (parse_pct(val, vn, &r.loss_pct)) return VNET_EINVAL;
        } else if (kn == 3 && !memcmp(p, "dup", 3)) {
            if (parse_pct(val, vn, &r.duplicate_pct)) return VNET_EINVAL;
        } else if (kn == 7 && !memcmp(p, "reorder", 7)) {
            if (parse_pct(val, vn, &r.reorder_pct)) return VNET_EINVAL;
        } else if (kn == 6 && !memcmp(p, "rdelay", 6)) {
            if (parse_u64(val, vn, &u) || u > MAX_DELAY_MS) return VNET_EINVAL;
            r.reorder_delay_ms = (uint32_t)u;
        } else if (kn == 4 && !memcmp(p, "seed", 4)) {
            if (parse_u64(val, vn, &r.seed)) return VNET_EINVAL;
        } else {
            return VNET_EINVAL;
        }
        p = end;
    }
    *out = r;
    return VNET_OK;
}
