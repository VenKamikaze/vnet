// SPDX-License-Identifier: MIT
// Receive queue of a browser relay transport that gets its frames from
// callbacks (the WebSocket one): two rings, one for DATA frames and one for
// control frames, so that a burst of datagrams in a page nobody is polling can
// push out other datagrams (like a lossy network) but never a KEEPALIVE answer
// or an ERROR. relay_wt.c keeps the same split in JavaScript. This file is
// plain C and is built everywhere so that the native tests can drive it.
#ifndef VNET_RELAY_INBOX_H
#define VNET_RELAY_INBOX_H

#include "relay_proto.h"

#include <stddef.h>
#include <stdint.h>

typedef struct relay_inbox relay_inbox;

// The result of relay_inbox_push().
#define RELAY_INBOX_DROPPED 0   // a DATA frame the full data ring had no room for, or a frame that is no vnet frame
#define RELAY_INBOX_DATA 1      // queued as DATA
#define RELAY_INBOX_CONTROL 2   // queued as control
#define RELAY_INBOX_OVERFLOW (-1) // the control ring is full: nobody has polled for a very long time

// data_frames / ctl_frames: ring sizes. NULL if out of memory.
relay_inbox *relay_inbox_create(int data_frames, int ctl_frames);
void relay_inbox_destroy(relay_inbox *q);
// Queue one frame (copied). Frames over RELAY_MAX_FRAME, or control frames over
// RELAY_WT_CTL_MAX bytes, are not vnet frames and are dropped.
int relay_inbox_push(relay_inbox *q, const uint8_t *frame, size_t len);
// The next frame, control before data: its length, VNET_EAGAIN if none, or
// VNET_EMSGSIZE if it did not fit in `cap` (it is consumed, never truncated).
int relay_inbox_pop(relay_inbox *q, uint8_t *buf, size_t cap);
// Frames waiting.
int relay_inbox_data_count(const relay_inbox *q);
int relay_inbox_control_count(const relay_inbox *q);

#endif
