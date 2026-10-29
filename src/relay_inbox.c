// SPDX-License-Identifier: MIT
#include "relay_inbox.h"

#include "vnet.h"

#include <stdlib.h>
#include <string.h>

typedef struct data_slot {
    size_t len;
    uint8_t data[RELAY_MAX_FRAME];
} data_slot;

typedef struct ctl_slot {
    size_t len;
    uint8_t data[RELAY_WT_CTL_MAX];
} ctl_slot;

struct relay_inbox {
    int dcap, dhead, dcount;
    int ccap, chead, ccount;
    data_slot *d;
    ctl_slot *c;
};

relay_inbox *relay_inbox_create(int data_frames, int ctl_frames) {
    if (data_frames < 1 || ctl_frames < 1) return NULL;
    relay_inbox *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->dcap = data_frames;
    q->ccap = ctl_frames;
    q->d = calloc((size_t)data_frames, sizeof *q->d);
    q->c = calloc((size_t)ctl_frames, sizeof *q->c);
    if (!q->d || !q->c) {
        relay_inbox_destroy(q);
        return NULL;
    }
    return q;
}

void relay_inbox_destroy(relay_inbox *q) {
    if (!q) return;
    free(q->d);
    free(q->c);
    free(q);
}

int relay_inbox_push(relay_inbox *q, const uint8_t *frame, size_t len) {
    if (len == 0 || len > RELAY_MAX_FRAME) return RELAY_INBOX_DROPPED;
    if (frame[0] == RELAY_T_DATA) {
        if (q->dcount == q->dcap) return RELAY_INBOX_DROPPED;  // overflow: dropped, like a lossy network
        data_slot *s = &q->d[(q->dhead + q->dcount) % q->dcap];
        q->dcount++;
        s->len = len;
        memcpy(s->data, frame, len);
        return RELAY_INBOX_DATA;
    }
    if (len > RELAY_WT_CTL_MAX) return RELAY_INBOX_DROPPED;
    if (q->ccount == q->ccap) return RELAY_INBOX_OVERFLOW;
    ctl_slot *s = &q->c[(q->chead + q->ccount) % q->ccap];
    q->ccount++;
    s->len = len;
    memcpy(s->data, frame, len);
    return RELAY_INBOX_CONTROL;
}

int relay_inbox_pop(relay_inbox *q, uint8_t *buf, size_t cap) {
    const uint8_t *src;
    size_t len;
    if (q->ccount > 0) {
        ctl_slot *s = &q->c[q->chead];
        q->chead = (q->chead + 1) % q->ccap;
        q->ccount--;
        src = s->data;
        len = s->len;
    } else if (q->dcount > 0) {
        data_slot *s = &q->d[q->dhead];
        q->dhead = (q->dhead + 1) % q->dcap;
        q->dcount--;
        src = s->data;
        len = s->len;
    } else {
        return VNET_EAGAIN;
    }
    if (len > cap) return VNET_EMSGSIZE;
    memcpy(buf, src, len);
    return (int)len;
}

int relay_inbox_data_count(const relay_inbox *q) { return q->dcount; }
int relay_inbox_control_count(const relay_inbox *q) { return q->ccount; }
