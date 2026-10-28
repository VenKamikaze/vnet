// SPDX-License-Identifier: MIT

package relay

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"sync"
	"testing"
	"time"
)

// Hub-level WebTransport tests: the relay's WebTransport code runs against a
// fake session (wt_fake_test.go), so a blocked datagram queue, a control
// stream stuck in Write and so on can be provoked exactly. The real QUIC
// path is covered in wt_e2e_test.go.

func TestWTCreateAndJoinOverTheControlStream(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	if host.octet() != 1 || guest.octet() != 2 {
		t.Fatalf("addresses %d and %d, want 1 and 2", host.octet(), guest.octet())
	}
	if guest.room.code != host.room.code || host.room.max != 8 || host.room.keepali != 5 {
		t.Fatalf("room replies differ: %+v %+v", host.room, guest.room)
	}
	st := th.h.snapshot()
	if st.Sessions != 2 || st.Rooms != 1 || st.WebTransports != 2 || st.WebSockets != 0 {
		t.Fatalf("stats %+v", st)
	}
}

func TestWTHandshakeIsIdempotentAndPerConnection(t *testing.T) {
	th := newTestHub(t, nil)
	a := th.wtClient("192.0.2.1")
	a.nonce = 777
	a.ctlSend(handshakeFrame(777, "", 0))
	first := a.ctlRecv()
	a.ctlSend(handshakeFrame(777, "", 0))
	second := a.ctlRecv()
	if string(first) != string(second) || first == nil {
		t.Fatalf("a repeat of the handshake returned a different reply:\n% x\n% x", first, second)
	}
	room := parseRoom(t, first)
	if th.h.snapshot().Sessions != 1 {
		t.Fatalf("the repeat made a second member")
	}
	// A different request on a connection that already has a session.
	a.ctlSend(handshakeFrame(778, room.code, 0))
	if r := errReason(t, a.ctlRecv()); r != ErrAlreadyInRoom {
		t.Fatalf("second request on a connection in a room: reason %d", r)
	}
	// The same nonce on another connection must not hand out that session.
	b := th.wtClient("192.0.2.2")
	b.ctlSend(handshakeFrame(777, "", 0))
	if r := errReason(t, b.ctlRecv()); r != ErrNonceReused {
		t.Fatalf("nonce reused on another connection: reason %d", r)
	}
	if th.h.snapshot().Sessions != 1 {
		t.Fatal("a session appeared")
	}
}

func TestWTRoutesDataAsDatagramsOnlyWithinTheRoom(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	other := th.wtClient("192.0.2.3").create() // another room, also 10.0.0.1
	if other.octet() != 1 || other.room.code == host.room.code {
		t.Fatal("setup")
	}
	host.dg(host.dataFrame(2, 5000, 5001, []byte("hello")))
	for o := byte(3); o < 20; o++ { // free addresses in the room
		host.dg(host.dataFrame(o, 1, 1, []byte("probe")))
	}
	host.dg(append([]byte{TypeData, 192, 0, 2, 2, 0, 1, 0, 1}, "outside"...)) // not a room address at all
	host.syncData()
	got := guest.syncData()
	if len(got) != 1 {
		t.Fatalf("guest got %d datagrams, want 1", len(got))
	}
	f := parseFwd(t, got[0])
	if f.srcOctet != 1 || f.dport != 5000 || f.sport != 5001 || string(f.payload) != "hello" {
		t.Fatalf("got %+v", f)
	}
	if n := len(other.syncData()); n != 0 {
		t.Fatalf("a member of another room received %d datagrams", n)
	}
	st := th.h.snapshot()
	if st.DataForwarded != 1+1+1+1 || st.Unroutable != 17+1 { // hello, and the three markers' deliveries to self
		t.Fatalf("forwarded %d, unroutable %d", st.DataForwarded, st.Unroutable)
	}
	// No control frames were produced by any of this.
	if c := guest.syncCtl(); len(c) != 0 {
		t.Fatalf("control frames: % x", c)
	}
}

func TestWTDataOnTheControlStreamIsNotForwarded(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	before := th.h.snapshot().Malformed
	host.ctlSend(host.dataFrame(2, 1, 1, []byte("smuggled")))
	host.syncCtl()
	if n := len(guest.syncData()); n != 0 {
		t.Fatalf("DATA sent on the control stream was forwarded (%d)", n)
	}
	if th.h.snapshot().Malformed != before+1 || host.ended() {
		t.Fatalf("DATA on the stream should be counted as malformed and ignored")
	}
}

func TestWTControlFramesInDatagramsDoNothing(t *testing.T) {
	th := newTestHub(t, nil)
	// Before the session: a CREATE_ROOM in a datagram must not create one.
	c := th.wtClient("192.0.2.1")
	c.dg(handshakeFrame(5, "", 0))
	c.dg(handshakeFrame(5, "", 64))
	// (give the datagram reader a moment: it processes them in order and then the marker)
	waitFor(t, "datagrams read", func() bool { return th.h.snapshot().WTEarlyDatagrams == 2 })
	if st := th.h.snapshot(); st.Sessions != 0 || st.Rooms != 0 {
		t.Fatalf("a handshake in a datagram created state: %+v", st)
	}
	// In a room: a KEEPALIVE, LEAVE or CREATE in a datagram is ignored (the member stays, nothing is answered).
	c.create()
	c.dg([]byte{TypeKeepalive})
	c.dg([]byte{TypeLeave})
	c.dg(handshakeFrame(6, "", 0))
	c.dg([]byte{})
	c.syncData()
	if st := th.h.snapshot(); st.Sessions != 1 || st.Malformed < 4 {
		t.Fatalf("stats %+v", st)
	}
	if f := c.syncCtl(); len(f) != 0 {
		t.Fatalf("answers to control frames sent as datagrams: % x", f)
	}
	if c.ended() {
		t.Fatal("a LEAVE in a datagram ended the session")
	}
}

func TestWTDatagramsBeforeTheRoomAreDropped(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2")
	guest.dg(append([]byte{TypeData, 10, 0, 0, 1, 0, 1, 0, 1}, "early"...))
	waitFor(t, "early datagram seen", func() bool { return th.h.snapshot().WTEarlyDatagrams == 1 })
	guest.join(host.room.code)
	if n := len(host.syncData()); n != 0 {
		t.Fatalf("an early datagram was forwarded")
	}
	if th.h.snapshot().DataForwarded != 1 { // only host's own sync marker
		t.Fatalf("forwarded %d", th.h.snapshot().DataForwarded)
	}
}

func TestWTPayloadLimit(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	big := make([]byte, MaxPayload)
	for i := range big {
		big[i] = byte(i)
	}
	host.dg(host.dataFrame(2, 1, 1, big))               // exactly VNET_MAX_DATAGRAM
	host.dg(host.dataFrame(2, 1, 1, append(big, 0xff))) // one byte too many: dropped, not truncated
	host.dg(host.dataFrame(2, 1, 1, nil))               // empty payload is fine
	host.dg([]byte{TypeData, 10, 0, 0, 2, 0, 1, 0})     // header one byte short
	host.syncData()
	got := guest.syncData()
	if len(got) != 2 || len(got[0]) != wsDataHdr+MaxPayload || string(got[0][wsDataHdr:]) != string(big) || len(got[1]) != wsDataHdr {
		t.Fatalf("got %d datagrams", len(got))
	}
	if th.h.snapshot().Malformed != 2 {
		t.Fatalf("malformed = %d, want 2", th.h.snapshot().Malformed)
	}
}

func TestWTMalformedControlStreamsAreRefusedWithoutState(t *testing.T) {
	// One byte over the limit, and a perfectly good CREATE_ROOM followed by
	// padding: only the length makes it wrong.
	long := append([]byte{0, maxCtlFrame + 1}, handshakeFrame(1, "", maxCtlFrame+1)...)
	cases := []struct {
		name     string
		raw      []byte
		wantErr  bool // an ERROR(MALFORMED) comes back
		malform  bool
		endInput bool
	}{
		{"zero length", []byte{0, 0}, true, true, false},
		{"length over the limit", long, true, true, false},
		{"huge length", []byte{0xff, 0xff, 1, 2, 3}, true, true, false},
		{"keepalive before a handshake", ctlFrame([]byte{TypeKeepalive}), true, true, false},
		{"unknown type before a handshake", ctlFrame([]byte{0x55, 1, 2}), true, true, false},
		{"DATA before a handshake", ctlFrame([]byte{TypeData, 10, 0, 0, 1, 0, 1, 0, 1}), true, true, false},
		{"truncated CREATE_ROOM", ctlFrame([]byte{TypeCreate, ProtocolVersion, 1, 2, 3}), true, true, false},
		{"truncated JOIN_ROOM", ctlFrame([]byte{TypeJoin, ProtocolVersion, 1, 2, 3, 4, 5, 6, 7, 8, 'A'}), true, true, false},
		{"LEAVE before a handshake", ctlFrame([]byte{TypeLeave}), false, false, false},
		{"stream ends inside a frame", []byte{0, 10, 1, 1}, false, false, true},
		{"stream ends inside the length", []byte{0}, false, false, true},
		{"stream ends at once", nil, false, false, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			th := newTestHub(t, nil)
			c := th.wtClient("192.0.2.1")
			if tc.raw != nil {
				c.ctlRaw(tc.raw)
			}
			if tc.endInput {
				c.ctl.endInput()
			}
			if tc.wantErr {
				if r := errReason(t, c.ctlRecv()); r != ErrMalformed {
					t.Fatalf("reason %d", r)
				}
			}
			c.waitEnded()
			st := th.h.snapshot()
			if st.Sessions != 0 || st.Rooms != 0 || st.RoomsCreated != 0 {
				t.Fatalf("state was created: %+v", st)
			}
			if (st.Malformed > 0) != tc.malform {
				t.Fatalf("malformed = %d, want >0: %v", st.Malformed, tc.malform)
			}
			th.h.wtGone(c.c)
			if len(th.h.perIP) != 0 || th.h.connsHeld != 0 {
				t.Fatalf("counters leaked: %v, %d", th.h.perIP, th.h.connsHeld)
			}
		})
	}
}

func TestWTDatagramTooLargeForTheConnectionIsDroppedNotFatal(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	guest.f.maxDatagram.Store(wsDataHdr + 100) // the connection can carry only a little
	host.dg(host.dataFrame(2, 1, 1, make([]byte, 500)))
	host.dg(host.dataFrame(2, 1, 1, make([]byte, 50)))
	host.syncData()
	got := guest.syncData()
	if len(got) != 1 || len(got[0]) != wsDataHdr+50 {
		t.Fatalf("guest received %d datagrams", len(got))
	}
	if th.h.snapshot().WTDatagramDrops != 1 || guest.ended() {
		t.Fatalf("a datagram that did not fit should be dropped and counted, not end the session: %+v", th.h.snapshot())
	}
}

func TestWTGarbageInsideARoomIsIgnored(t *testing.T) {
	th := newTestHub(t, nil)
	c := th.wtClient("192.0.2.1").create()
	for _, junk := range [][]byte{{0x55}, {0x81, 1, 2}, {TypePathResponse, 1}, {TypeRetry}, {0x10}, {TypeCreate}} {
		c.ctlSend(junk)
	}
	// {TypeCreate} alone is a (short) handshake: that one closes the session; the rest do not.
	if r := errReason(t, c.ctlRecv()); r != ErrMalformed {
		t.Fatalf("reason %d", r)
	}
	c.waitEnded()
	if th.h.snapshot().Sessions != 0 {
		t.Fatal("session survived")
	}
	d := th.wtClient("192.0.2.2").create()
	for _, junk := range [][]byte{{0x55}, {0x81, 1, 2}, {TypePathResponse, 1}, {TypeRetry}, {0x10}} {
		d.ctlSend(junk)
	}
	d.syncCtl()
	if d.ended() || th.h.snapshot().Malformed < 5 {
		t.Fatalf("junk in a room: ended=%v, malformed=%d", d.ended(), th.h.snapshot().Malformed)
	}
}

func TestWTLeaveFreesTheMember(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	guest.ctlSend([]byte{TypeLeave})
	guest.waitEnded()
	if th.h.snapshot().Sessions != 1 {
		t.Fatal("the member is still there")
	}
	again := th.wtClient("192.0.2.3").join(host.room.code)
	if again.octet() != 2 {
		t.Fatalf("address 2 was not reusable: got %d", again.octet())
	}
}

func TestWTSlowDatagramConsumerLosesDatagramsNotTheRelay(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.WTSendQueue = 8; c.WTSendQueueBytes = 2048 })
	src := th.wtClient("192.0.2.1").create()
	slow := th.wtClient("192.0.2.2").join(src.room.code)
	third := th.wtClient("192.0.2.3").join(src.room.code)
	gate := make(chan struct{})
	slow.f.sendGate.Store(&gate) // the connection's datagram queue is full: SendDatagram blocks

	const n = 300
	for i := 0; i < n; i++ {
		src.dg(src.dataFrame(2, 1, 1, make([]byte, 100)))
	}
	// The sender is never held up by the stuck receiver: its datagrams are
	// all handled (the marker comes back) with the receiver still stuck, and
	// the hub is not locked.
	src.syncData()
	if !lockable(th.h) {
		t.Fatal("the hub is locked while a receiver is stuck")
	}
	// Traffic to everyone else flows.
	src.dg(src.dataFrame(3, 4, 4, []byte("still flowing")))
	src.syncData()
	if got := third.syncData(); len(got) != 1 || string(got[0][wsDataHdr:]) != "still flowing" {
		t.Fatalf("the healthy receiver got %d datagrams while another was stuck", len(got))
	}
	// Memory stays bounded: at most the queue's frames and bytes (plus the one the writer holds).
	if q := len(slow.c.out); q > 8 || slow.c.qbytes.Load() > 2048 {
		t.Fatalf("send queue holds %d frames, %d bytes", q, slow.c.qbytes.Load())
	}
	if d := th.h.snapshot().WTDatagramDrops; d < n-20 {
		t.Fatalf("only %d datagrams dropped of %d", d, n)
	}
	// The control stream is a different path: it still works while datagrams are stuck.
	slow.syncCtl()
	// When the queue drains the receiver gets what was queued, and new traffic flows.
	close(gate)
	slow.f.sendGate.Store(nil)
	waitFor(t, "the queue to drain", func() bool { return len(slow.c.out) == 0 && slow.c.qbytes.Load() == 0 })
	src.dg(src.dataFrame(2, 7, 7, []byte("after")))
	var last []byte
	for i := 0; i < 20; i++ {
		b := slow.dgRecv()
		if b == nil {
			break
		}
		if len(b) > wsDataHdr && string(b[wsDataHdr:]) == "after" {
			last = b
			break
		}
	}
	if last == nil {
		t.Fatal("no traffic after the receiver recovered")
	}
}

// The frame bound and the byte bound are separate: large datagrams reach the
// byte bound long before the frame bound.
func TestWTDatagramQueueIsBoundedInBytes(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.WTSendQueue = 1000; c.WTSendQueueBytes = 2500 })
	src := th.wtClient("192.0.2.1").create()
	slow := th.wtClient("192.0.2.2").join(src.room.code)
	gate := make(chan struct{})
	defer close(gate)
	slow.f.sendGate.Store(&gate)
	for i := 0; i < 50; i++ {
		src.dg(src.dataFrame(2, 1, 1, make([]byte, 500))) // 509 bytes each
	}
	src.syncData()
	if q := len(slow.c.out); q > 2500/509 || slow.c.qbytes.Load() > 2500 {
		t.Fatalf("%d frames, %d bytes queued against a limit of 2500 bytes", q, slow.c.qbytes.Load())
	}
	if d := th.h.snapshot().WTDatagramDrops; d < 50-2500/509-2 {
		t.Fatalf("only %d datagrams dropped", d)
	}
}

func TestWTControlQueueOverflowClosesTheSession(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.WTWriteTimeout = 30 * time.Second })
	c := th.wtClient("192.0.2.1").create()
	gate := make(chan struct{})
	defer close(gate)
	c.ctl.gate.Store(&gate) // the client has stopped reading the control stream
	for i := 0; i < wtCtlQueue+10; i++ {
		c.ctlSend([]byte{TypeKeepalive}) // each draws an answer
	}
	c.waitEnded()
	st := th.h.snapshot()
	if st.WTCtlOverflow < 1 || st.Sessions != 0 {
		t.Fatalf("stats %+v", st)
	}
}

// The WebSocket writer panicked in brief 10 (B1) because a rejection was
// written from the reader goroutine while the writer goroutine was inside a
// write. Here the control writer is held inside Write, as when a client stops
// reading, and a bad frame makes the reader write an ERROR of its own. The
// fake stream fails the test if two writes ever overlap.
func TestWTControlStreamWritesAreSerialised(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.WTWriteTimeout = 300 * time.Millisecond })
	c := th.wtClient("192.0.2.1").create()
	gate := make(chan struct{})
	defer close(gate)
	c.ctl.gate.Store(&gate)
	c.ctlSend([]byte{TypeKeepalive}) // the answer parks the writer inside Write
	waitFor(t, "the writer to be inside Write", func() bool { return c.ctl.inW.Load() == 1 })
	c.ctlRaw([]byte{0, 0}) // a bad frame: the reader now writes ERROR(MALFORMED) itself
	c.waitEnded()          // (fails via t.Error in the fake if the writes overlapped)
}

func TestWTSessionLivenessFollowsTheConnection(t *testing.T) {
	th := newTestHub(t, nil)
	wt := th.wtClient("192.0.2.1").create()
	// An idle member of a live connection is not timed out: the relay's QUIC
	// keep-alive and idle timeout watch the connection (as WebSocket pongs do).
	th.clock.Advance(10 * th.h.cfg.SessionTimeout)
	th.h.Sweep()
	if th.h.snapshot().Sessions != 1 || wt.ended() {
		t.Fatal("a live idle WebTransport member was timed out")
	}
	// A UDP member in the same position is.
	udp := th.mustCreate(ap("198.51.100.1", 1), 4242)
	_ = udp
	th.clock.Advance(10 * th.h.cfg.SessionTimeout)
	th.h.Sweep()
	if th.h.snapshot().Sessions != 1 {
		t.Fatalf("sessions = %d: the idle UDP member should be gone and the WebTransport one still there", th.h.snapshot().Sessions)
	}
	// When the connection dies the member goes at once, without waiting for a sweep.
	wt.f.cancel()
	wt.waitEnded()
	if th.h.snapshot().Sessions != 0 {
		t.Fatal("a dead connection's member stayed")
	}
}

func TestWTHandshakeTimeoutClosesAndReleases(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxPendingWTPerIP = 1 })
	c := th.wtClient("192.0.2.1") // never says anything
	if _, ok := th.h.wtAdmit(netip.MustParseAddr("192.0.2.1")); ok {
		t.Fatal("a second pending connection was admitted")
	}
	th.clock.Advance(th.h.cfg.HandshakeTimeout + time.Second)
	th.h.Sweep()
	c.waitEnded()
	waitFor(t, "the session to be closed", func() bool { return c.f.closeCode.Load() != 0 })
	th.h.wtGone(c.c)
	if len(th.h.perIP) != 0 || th.h.connsHeld != 0 {
		t.Fatalf("counters leaked: %v, %d", th.h.perIP, th.h.connsHeld)
	}
	if _, ok := th.h.wtAdmit(netip.MustParseAddr("192.0.2.1")); !ok {
		t.Fatal("the slot was not given back")
	}
}

func TestWTPerAddressCaps(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxWTPerIP = 3; c.MaxPendingWTPerIP = 2 })
	h := th.h
	a1 := netip.MustParseAddr("192.0.2.1")
	c1, ok1 := h.wtAdmit(a1)
	c2, ok2 := h.wtAdmit(a1)
	_, ok3 := h.wtAdmit(a1)
	if !ok1 || !ok2 || ok3 {
		t.Fatalf("pending cap 2: %v %v %v", ok1, ok2, ok3)
	}
	if h.snapshot().WTRefused != 1 {
		t.Fatal("refusal not counted")
	}
	// Other addresses are separate budgets.
	if _, ok := h.wtAdmit(netip.MustParseAddr("192.0.2.2")); !ok {
		t.Fatal("another address was refused")
	}
	// Entering a room frees the pending slot but not the connection slot.
	if !h.connHandshake(c1, handshakeFrame(1, "", 0), th.clock.Now()) || c1.getSession() == nil {
		t.Fatal("handshake")
	}
	c3, ok := h.wtAdmit(a1)
	if !ok {
		t.Fatal("a pending slot was not freed by entering a room")
	}
	if _, ok := h.wtAdmit(a1); ok {
		t.Fatal("the cap on connections (3) was exceeded")
	}
	// Giving back, once each, restores the budget exactly (a double release must not underflow it).
	for _, c := range []*wtConn{c1, c2, c3} {
		h.wtGone(c)
		h.wtGone(c)
	}
	for i := 0; i < 2; i++ {
		if _, ok := h.wtAdmit(a1); !ok {
			t.Fatalf("budget not restored (%d)", i)
		}
	}
	if _, ok := h.wtAdmit(a1); ok {
		t.Fatal("budget exceeded after a double release")
	}
}

// Releasing a connection gives back exactly what it held: a connection that
// had entered a room holds no pending slot, so its end must not free the
// pending slot of another connection that is still waiting.
func TestWTReleaseGivesBackOnlyWhatWasHeld(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxPendingWTPerIP = 2 })
	h := th.h
	a := netip.MustParseAddr("192.0.2.1")
	c1, _ := h.wtAdmit(a)
	c2, _ := h.wtAdmit(a) // still waiting for its room
	if !h.connHandshake(c1, handshakeFrame(1, "", 0), th.clock.Now()) {
		t.Fatal("handshake")
	}
	h.wtGone(c1)
	if _, ok := h.wtAdmit(a); !ok {
		t.Fatal("one pending slot is free (c1 left it when it entered a room)")
	}
	if _, ok := h.wtAdmit(a); ok {
		t.Fatal("c1's end freed a pending slot that c2 still holds")
	}
	h.wtGone(c2)
}

func TestWTPerAddressCapsKeyIPv6ByPrefix(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxWTPerIP = 2; c.MaxPendingWTPerIP = 2; c.PrefixFactor = 3 })
	h := th.h
	// Two hosts of one /64 share a budget.
	if _, ok := h.wtAdmit(netip.MustParseAddr("2001:db8:1:1::1")); !ok {
		t.Fatal()
	}
	if _, ok := h.wtAdmit(netip.MustParseAddr("2001:db8:1:1:ffff::2")); !ok {
		t.Fatal()
	}
	if _, ok := h.wtAdmit(netip.MustParseAddr("2001:db8:1:1:aaaa::3")); ok {
		t.Fatal("rotating the host part of an IPv6 address bought a fresh budget")
	}
	// Other /64s of the same /48 add up to PrefixFactor times the cap and no more: 6 in all.
	n := 2
	for i := 2; i < 40; i++ {
		if _, ok := h.wtAdmit(netip.MustParseAddr(fmt.Sprintf("2001:db8:1:%x::1", i))); ok {
			n++
		}
	}
	if n != 6 {
		t.Fatalf("one /48 holds %d connections, want 6 (3 x 2)", n)
	}
}

func TestWTAndWebSocketShareTheGlobalConnectionCap(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxSessions = 3 })
	h := th.h
	ip := func(i int) netip.Addr { return netip.MustParseAddr(fmt.Sprintf("192.0.2.%d", i)) }
	if !h.acquireConn(kindWS, ip(1)) || !h.acquireConn(kindWS, ip(2)) {
		t.Fatal()
	}
	if _, ok := h.wtAdmit(ip(3)); !ok {
		t.Fatal()
	}
	if _, ok := h.wtAdmit(ip(4)); ok {
		t.Fatal("WebTransport exceeded the shared cap")
	}
	if h.acquireConn(kindWS, ip(5)) {
		t.Fatal("WebSocket exceeded the shared cap")
	}
	h.releaseConn(kindWS, ip(1), true)
	if _, ok := h.wtAdmit(ip(4)); !ok {
		t.Fatal("a released slot was not reusable")
	}
}

func TestWTTokenIsNoCredentialOverUDP(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	attacker := ap("198.51.100.9", 4000)
	th.drain()
	// The host's token, presented from another address, as a UDP member would.
	frame := append([]byte{TypeData}, host.room.token[:]...)
	frame = append(frame, 10, 0, 0, 2, 0, 1, 0, 1)
	frame = append(frame, "forged"...)
	th.feed(attacker, frame)
	th.feed(attacker, append([]byte{TypeKeepalive}, host.room.token[:]...))
	th.feed(attacker, append([]byte{TypeLeave}, host.room.token[:]...))
	out := th.take(attacker)
	if len(out) != 3 {
		t.Fatalf("%d replies", len(out))
	}
	for _, b := range out {
		if errReason(t, b) != ErrNoSession {
			t.Fatalf("the token of a WebTransport session worked over UDP: % x", b)
		}
	}
	if n := len(guest.syncData()); n != 0 {
		t.Fatalf("a UDP packet with a WebTransport token reached a member")
	}
	st := th.h.snapshot()
	if st.Sessions != 2 || st.PathChallenges != 0 || st.DataForwarded != 1 { // 1 = the guest's own sync marker
		t.Fatalf("stats %+v", st)
	}
}

func TestWTMemberAndUDPMemberShareARoom(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("198.51.100.1", 1), 1)
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	if guest.octet() != 2 {
		t.Fatal("octet")
	}
	th.drain()
	host.send(2, 9, 8, []byte("udp->wt"))
	got := guest.syncData()
	if len(got) != 1 || string(parseFwd(t, got[0]).payload) != "udp->wt" || parseFwd(t, got[0]).srcOctet != 1 {
		t.Fatalf("got %d: %x", len(got), got)
	}
	guest.dg(guest.dataFrame(1, 8, 9, []byte("wt->udp")))
	guest.syncData()
	pk := th.take(host.addr)
	if len(pk) != 1 || parseFwd(t, pk[0]).srcOctet != 2 || string(parseFwd(t, pk[0]).payload) != "wt->udp" {
		t.Fatalf("UDP member got %d packets", len(pk))
	}
}

func TestWTRecoveredPanicLeavesTheHubUsable(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	th.h.panicHook = func(w string) {
		if w == "forward" {
			panic("injected into forward")
		}
	}
	host.dg(host.dataFrame(2, 1, 1, []byte("x")))
	host.waitEnded() // the datagram reader recovered, and the connection was closed
	if th.h.snapshot().Panics != 1 {
		t.Fatalf("panics = %d", th.h.snapshot().Panics)
	}
	if !lockable(th.h) {
		t.Fatal("a recovered panic left the hub locked")
	}
	th.h.panicHook = nil
	if th.h.snapshot().Sessions != 1 { // the host is gone, the guest is not
		t.Fatalf("sessions = %d", th.h.snapshot().Sessions)
	}
	guest.syncCtl()
	again := th.wtClient("192.0.2.3").join(host.room.code)
	if again.octet() != 3 { // 1 is never reissued, 2 is the guest
		t.Fatalf("unexpected octet %d", again.octet())
	}
}

func TestWTRateLimitsApplyToDatagramsAndControlFramesTogether(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.ClientPPS = 10; c.ClientPPSBurst = 20 })
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	// The fake clock stands still, so the bucket never refills: a member has
	// 20 frames in all, whichever way they travel. 12 datagrams and the sync
	// marker use 13; of the next 10 control frames 7 are served, 3 are not.
	for i := 0; i < 12; i++ {
		host.dg(host.dataFrame(2, 1, 1, []byte{byte(i)}))
	}
	host.syncData()
	if th.h.snapshot().RateLimited != 0 {
		t.Fatal("limited too early")
	}
	for i := 0; i < 10; i++ {
		host.ctlSend([]byte{TypeKeepalive})
	}
	waitFor(t, "the surplus control frames to be limited", func() bool { return th.h.snapshot().RateLimited == 3 })
	if got := len(guest.syncData()); got != 12 {
		t.Fatalf("guest received %d of 12 datagrams", got)
	}
}

// Datagrams and control frames of one member are handled by two goroutines;
// they share the member's rate buckets. Run with -race.
func TestWTConcurrentControlAndDataFromOneMember(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.ClientPPS = 1e6
		c.ClientBPS = 1e9
		c.WTSendQueue = 2000
		c.WTSendQueueBytes = 1 << 20
	})
	host := th.wtClient("192.0.2.1").create()
	guest := th.wtClient("192.0.2.2").join(host.room.code)
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		for i := 0; i < 25; i++ { // (each is answered on a queue of wtCtlQueue frames)
			host.ctlSend([]byte{TypeKeepalive})
		}
	}()
	go func() {
		defer wg.Done()
		for i := 0; i < 400; i++ {
			host.dg(host.dataFrame(2, 1, 1, []byte{byte(i)}))
		}
	}()
	wg.Wait()
	host.syncData()
	host.syncCtl()
	if got := len(guest.syncData()); got != 400 {
		t.Fatalf("guest received %d of 400", got)
	}
}

func TestWTSurplusStreamsAreResetAndAbuseIsClosed(t *testing.T) {
	th := newTestHub(t, nil)
	c := th.wtClient("192.0.2.1").create()
	streams := make([]*fakeWTStream, 0, 5)
	for i := 0; i < 5; i++ {
		s := newFakeWTStream(t)
		streams = append(streams, s)
		c.f.bidi <- s
		c.f.uni <- fakeUni{&c.f.uniSeen}
	}
	waitFor(t, "streams refused", func() bool { return th.h.snapshot().WTBadStreams == 10 })
	for i, s := range streams {
		if s.cancel.Load() != 2 {
			t.Fatalf("stream %d: read/write not both cancelled", i)
		}
	}
	if c.f.uniSeen.Load() != 5 {
		t.Fatal("unidirectional streams were not reset")
	}
	if c.ended() {
		t.Fatal("a few stray streams ended the session")
	}
	c.syncCtl() // still works
	for i := 0; i < maxBadStreams; i++ {
		c.f.bidi <- newFakeWTStream(t)
	}
	c.waitEnded()
	if c.f.closeCode.Load() != wtCloseProtocol+1 {
		t.Fatalf("close code %d", c.f.closeCode.Load()-1)
	}
}

func TestWTHTTPHandlerChecks(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.AllowedOrigins = []string{"game.example"} })
	srv := &Server{cfg: th.h.cfg, hub: th.h, stop: make(chan struct{})}
	conn, ok := th.h.wtAdmit(netip.MustParseAddr("192.0.2.1"))
	if !ok {
		t.Fatal()
	}
	do := func(method, path, origin string, withConn bool) int {
		r := httptest.NewRequest(method, path, nil)
		if origin != "" {
			r.Header.Set("Origin", origin)
		}
		if withConn {
			r = r.WithContext(context.WithValue(r.Context(), wtConnKey{}, conn))
		}
		w := httptest.NewRecorder()
		srv.serveWTHTTP(w, r)
		return w.Code
	}
	for _, tc := range []struct {
		name         string
		method, path string
		origin       string
		withConn     bool
		want         int
	}{
		{"wrong path", http.MethodConnect, "/other", "", true, 404},
		{"not CONNECT", http.MethodGet, wtPath, "", true, 405},
		{"no connection context", http.MethodConnect, wtPath, "", false, 500},
		{"origin not allowed", http.MethodConnect, wtPath, "https://evil.example", true, 403},
	} {
		if got := do(tc.method, tc.path, tc.origin, tc.withConn); got != tc.want {
			t.Errorf("%s: status %d, want %d", tc.name, got, tc.want)
		}
	}
	// One session per connection: the second claim is refused.
	if !conn.start() {
		t.Fatal("first claim")
	}
	if got := do(http.MethodConnect, wtPath, "https://game.example", true); got != http.StatusTooManyRequests {
		t.Errorf("second session on one connection: status %d", got)
	}
}

func TestWTStatsReportsBothKinds(t *testing.T) {
	th := newTestHub(t, nil)
	th.wtClient("192.0.2.1")
	th.wtClient("192.0.2.2")
	if !th.h.acquireConn(kindWS, netip.MustParseAddr("192.0.2.3")) {
		t.Fatal()
	}
	// A bare acquire is not a registered connection: only wtAdmit registers.
	st := th.h.snapshot()
	if st.WebTransports != 2 || st.WebSockets != 0 {
		t.Fatalf("%+v", st)
	}
}

func TestConfigRejectsNegativeWebTransportLimits(t *testing.T) {
	for _, c := range []Config{{MaxWTPerIP: -1}, {MaxPendingWTPerIP: -1}, {WTSendQueue: -1}} {
		if _, err := New(c); err == nil {
			t.Fatalf("%+v accepted", c)
		}
	}
}

// FuzzHandleWT feeds arbitrary bytes to a WebTransport session's control
// stream and arbitrary datagrams to its datagram path. The relay must survive
// (no recovered panic), answer only with well-formed ROOM, ERROR and
// KEEPALIVE frames on the stream and DATA in datagrams, end the session when
// the stream ends, and give back everything it held.
//
// Input: byte 0 is the number of datagrams (low 3 bits); each datagram is
// preceded by its length byte; the rest is the control stream.
func FuzzHandleWT(f *testing.F) {
	create := ctlFrame(handshakeFrame(1, "", 0))
	f.Add(append([]byte{0}, create...))
	f.Add(append(append([]byte{1, 9, TypeData, 10, 0, 0, 1, 0, 1, 0, 1}, create...), ctlFrame([]byte{TypeKeepalive})...))
	f.Add(append([]byte{0}, ctlFrame([]byte{TypeLeave})...))
	f.Add([]byte{0, 0, 0})
	f.Add([]byte{2, 3, TypeCreate, 1, 2, 4, TypeKeepalive, 0, 0, 0xff, 0xff, 1})
	f.Add(append(append([]byte{0}, create...), ctlFrame(handshakeFrame(2, "ZZZZZZ", 0))...))
	f.Fuzz(func(t *testing.T, in []byte) {
		if len(in) == 0 {
			return
		}
		th := newTestHub(t, func(c *Config) { c.HandshakePerSec = 1e9; c.HandshakeBurst = 1e9; c.WTWriteTimeout = time.Second })
		// A second member so there is somebody to route to.
		other := th.wtClient("192.0.2.9").create()
		c := th.wtClient("192.0.2.1")
		nd := int(in[0] & 7)
		in = in[1:]
		for i := 0; i < nd && len(in) > 0; i++ {
			n := int(in[0])
			in = in[1:]
			if n > len(in) {
				n = len(in)
			}
			c.dg(in[:n])
			in = in[n:]
		}
		if len(in) > 0 {
			c.ctlRaw(in)
		}
		c.ctl.endInput()
		c.waitEnded()
		// What the relay wrote to the stream is well formed.
		for {
			select {
			case b := <-c.ctl.out:
				if len(b) < 3 || int(b[0])<<8|int(b[1]) != len(b)-2 {
					t.Fatalf("badly framed control message % x", b)
				}
				switch b[2] {
				case TypeRoom:
					if len(b)-2 != roomLen {
						t.Fatalf("ROOM of %d bytes", len(b)-2)
					}
				case TypeError:
					if len(b)-2 != errorLen {
						t.Fatalf("ERROR of %d bytes", len(b)-2)
					}
				case TypeKeepalive:
					if len(b)-2 != 1 {
						t.Fatalf("KEEPALIVE of %d bytes", len(b)-2)
					}
				default:
					t.Fatalf("unexpected frame type %#x on the control stream", b[2])
				}
				continue
			default:
			}
			break
		}
		// What it sent as datagrams is DATA.
		drainDatagrams(c.f)
		drainDatagrams(other.f)
		th.h.wtGone(c.c)
		st := th.h.snapshot()
		if st.Panics != 0 {
			t.Fatalf("recovered panic: %+v", st)
		}
		if st.Sessions > 1 { // only `other` may remain
			t.Fatalf("sessions left behind: %+v", st)
		}
		if !lockable(th.h) {
			t.Fatal("hub locked")
		}
	})
}

// drainDatagrams checks that everything the relay sent as a datagram is a DATA
// frame (an assertion helper for the fuzzer).
func drainDatagrams(f *fakeWT) {
	for {
		select {
		case b := <-f.dgOut:
			if len(b) < wsDataHdr || b[0] != TypeData || b[1] != 10 {
				f.t.Fatalf("a datagram that is not a forwarded DATA frame: % x", b)
			}
		default:
			return
		}
	}
}
