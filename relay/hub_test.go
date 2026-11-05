// SPDX-License-Identifier: MIT

package relay

import (
	"bytes"
	"fmt"
	"math/rand"
	"net/http/httptest"
	"net/netip"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// ---------------------------------------------------------------- rooms

func TestCreateGivesHostAddressAndCode(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.MaxMembers = 5
		c.KeepaliveInterval = 7 * time.Second
		c.SessionTimeout = 30 * time.Second
	})
	c := th.mustCreate(ap("192.0.2.1", 1000), 1)
	if c.room.octet != 1 {
		t.Fatalf("creator vaddr = 10.0.0.%d, want 10.0.0.1 (the host convention)", c.room.octet)
	}
	if c.room.nonce != nonceBytes(1) {
		t.Fatalf("nonce not echoed")
	}
	if len(c.room.code) != CodeLen {
		t.Fatalf("code %q", c.room.code)
	}
	if c.room.token == ([TokenLen]byte{}) {
		t.Fatalf("zero token")
	}
	if c.room.max != 5 || c.room.keepali != 7 {
		t.Fatalf("max=%d keepalive=%d", c.room.max, c.room.keepali)
	}
}

func TestRoomCodesAreUnambiguousAndUnique(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.MaxRooms = 5000
		c.MaxSessions = 5000
		c.HandshakeBurst = 1e9
		c.HandshakePerSec = 1e9
		c.ReplyBurst = 1e9
		c.GlobalReplyBurst = 1e9
	})
	seen := map[string]bool{}
	for i := 0; i < 3000; i++ {
		c := th.mustCreate(ap("192.0.2.1", 1000), uint64(i+1))
		code := c.room.code
		if seen[code] {
			t.Fatalf("duplicate code %s", code)
		}
		seen[code] = true
		if strings.ContainsAny(code, "0O1Il") || strings.ToUpper(code) != code {
			t.Fatalf("ambiguous or lower-case symbol in %q", code)
		}
		for _, ch := range code {
			if !strings.ContainsRune(codeAlphabet, ch) {
				t.Fatalf("symbol %q of %q outside the alphabet", ch, code)
			}
		}
	}
	// Every symbol of the alphabet is reachable (sanity check of the mapping).
	used := map[rune]bool{}
	for code := range seen {
		for _, ch := range code {
			used[ch] = true
		}
	}
	if len(used) != len(codeAlphabet) {
		t.Fatalf("only %d of %d symbols ever used", len(used), len(codeAlphabet))
	}
}

func TestNormaliseCode(t *testing.T) {
	for _, tc := range []struct {
		in  string
		ok  bool
		out string
	}{
		{"ABCDEF", true, "ABCDEF"}, {"abcdef", true, "ABCDEF"}, {"aBc234", true, "ABC234"},
		{"ABCDE0", false, ""}, {"ABCDEO", false, ""}, {"ABCDE1", false, ""}, {"ABCDEI", false, ""},
		{"ABCDE", false, ""}, {"ABCDEFG", false, ""}, {"ABCDE-", false, ""}, {"ABCDE\x00", false, ""},
	} {
		got, ok := normaliseCode([]byte(tc.in))
		if ok != tc.ok || got != tc.out {
			t.Errorf("normaliseCode(%q) = %q,%v want %q,%v", tc.in, got, ok, tc.out, tc.ok)
		}
	}
}

func TestJoinAssignsAddressesAndReusesLowestFree(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	code := host.room.code
	a := th.mustJoin(ap("192.0.2.2", 1), 2, code)
	b := th.mustJoin(ap("192.0.2.3", 1), 3, code)
	c := th.mustJoin(ap("192.0.2.4", 1), 4, strings.ToLower(code)) // codes are case-insensitive
	if a.room.octet != 2 || b.room.octet != 3 || c.room.octet != 4 {
		t.Fatalf("addresses %d %d %d, want 2 3 4", a.room.octet, b.room.octet, c.room.octet)
	}
	if a.room.code != code {
		t.Fatalf("join reply code %q", a.room.code)
	}
	b.leave()
	d := th.mustJoin(ap("192.0.2.5", 1), 5, code)
	if d.room.octet != 3 {
		t.Fatalf("lowest free address not reused: got %d", d.room.octet)
	}
	// A member keeps its address for as long as it is in the room.
	for i := 0; i < 5; i++ {
		a.keepalive()
	}
	if got := th.h.byToken[a.room.token].octet; got != 2 {
		t.Fatalf("address changed to %d", got)
	}
}

func TestHostAddressIsNeverReissued(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	a := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	host.leave()
	b := th.mustJoin(ap("192.0.2.3", 1), 3, host.room.code)
	if b.room.octet == 1 || b.room.octet == a.room.octet {
		t.Fatalf("new joiner got %d after the host left", b.room.octet)
	}
}

func TestRoomFullAtMemberCap(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxMembers = 3 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	second := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.mustJoin(ap("192.0.2.3", 1), 3, host.room.code)
	th.drain()
	th.hsFeed(ap("192.0.2.4", 1), handshakeFrame(4, host.room.code, MinUDPHandshake))
	pk := th.take(ap("192.0.2.4", 1))
	if len(pk) != 1 || errReason(t, pk[0]) != ErrRoomFull {
		t.Fatalf("want ROOM_FULL, got %v", pk)
	}
	if th.h.snapshot().Sessions != 3 {
		t.Fatalf("rejected joiner was registered")
	}
	// A slot freed by a leaver can be taken again.
	second.leave()
	again := th.mustJoin(ap("192.0.2.4", 1), 5, host.room.code)
	if again.room.octet != 2 {
		t.Fatalf("got %d", again.room.octet)
	}
}

func TestJoinUnknownOrInvalidRoom(t *testing.T) {
	th := newTestHub(t, nil)
	for i, code := range []string{"ABCDEF", "abcdef", "ZZZZZZ", "ABCDE0", "ABCDE1"} {
		th.drain()
		src := ap("192.0.2.9", 5)
		th.hsFeed(src, handshakeFrame(uint64(100+i), code, MinUDPHandshake))
		pk := th.take(src)
		if len(pk) != 1 || errReason(t, pk[0]) != ErrRoomNotFound {
			t.Fatalf("%s: want ROOM_NOT_FOUND, got %v", code, pk)
		}
	}
	if th.h.snapshot().Sessions != 0 || th.h.snapshot().Rooms != 0 {
		t.Fatal("state created by failed joins")
	}
}

func TestRoomDiesWhenEmpty(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	a := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	host.leave()
	if th.h.snapshot().Rooms != 1 {
		t.Fatal("room should survive while a member remains")
	}
	a.leave()
	st := th.h.snapshot()
	if st.Rooms != 0 || st.Sessions != 0 || st.RoomsClosed != 1 {
		t.Fatalf("room not closed when empty: %+v", st)
	}
	// The code is gone for good.
	th.drain()
	th.hsFeed(ap("192.0.2.3", 1), handshakeFrame(9, host.room.code, MinUDPHandshake))
	if r := errReason(t, th.take(ap("192.0.2.3", 1))[0]); r != ErrRoomNotFound {
		t.Fatalf("reason %d", r)
	}
}

// -------------------------------------------------------------- routing

func TestRoutingStaysInsideTheRoom(t *testing.T) {
	th := newTestHub(t, nil)
	// Two rooms whose members have the SAME vaddrs.
	hostA := th.mustCreate(ap("192.0.2.1", 1), 1)
	guestA := th.mustJoin(ap("192.0.2.2", 1), 2, hostA.room.code)
	hostB := th.mustCreate(ap("198.51.100.1", 1), 3)
	guestB := th.mustJoin(ap("198.51.100.2", 1), 4, hostB.room.code)
	if hostA.room.code == hostB.room.code {
		t.Fatal("same code")
	}
	th.drain()

	hostA.send(2, 5000, 6000, []byte("for-A-guest"))
	got := th.drain()
	if len(got) != 1 || got[0].to != guestA.addr {
		t.Fatalf("A's packet went to %v", got)
	}
	f := parseFwd(t, got[0].b)
	if f.srcOctet != 1 || f.dport != 5000 || f.sport != 6000 || string(f.payload) != "for-A-guest" {
		t.Fatalf("forwarded frame %+v", f)
	}

	hostB.send(2, 7000, 8000, []byte("for-B-guest"))
	got = th.drain()
	if len(got) != 1 || got[0].to != guestB.addr {
		t.Fatalf("B's packet went to %v", got)
	}

	// Destinations that are not members of the SENDER's room go nowhere:
	// absent octets, the broadcast and network addresses, other prefixes.
	before := th.h.snapshot().Unroutable
	n := 0
	for _, o := range []byte{0, 3, 9, 200, 254, 255} {
		guestA.send(o, 1, 1, []byte("x"))
		n++
	}
	for _, dst := range [][4]byte{{8, 8, 8, 8}, {127, 0, 0, 1}, {10, 0, 1, 2}, {10, 1, 0, 2}, {192, 0, 2, 2}, {0, 0, 0, 0}} {
		f := guestA.dataFrame(2, 1, 1, []byte("x"))
		copy(f[1+TokenLen:], dst[:])
		th.feed(guestA.addr, f)
		n++
	}
	if got := th.drain(); len(got) != 0 {
		t.Fatalf("%d packets leaked to non-members: %v", len(got), got)
	}
	if after := th.h.snapshot().Unroutable; after-before != uint64(n) {
		t.Fatalf("unroutable counted %d, want %d", after-before, n)
	}
}

func TestSourceAddressComesFromTheRelay(t *testing.T) {
	// A client cannot choose the source vaddr: the forwarded frame carries
	// the sender's real address whatever the payload claims.
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	g1 := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	g2 := th.mustJoin(ap("192.0.2.3", 1), 3, host.room.code)
	th.drain()
	g2.send(1, 1, 1, append([]byte{TypeData, 10, 0, 0, 2}, make([]byte, 20)...)) // payload imitates a header
	got := th.drain()
	if len(got) != 1 || got[0].to != host.addr {
		t.Fatalf("%v", got)
	}
	if f := parseFwd(t, got[0].b); f.srcOctet != g2.room.octet || g2.room.octet == g1.room.octet {
		t.Fatalf("source = %d, want %d", f.srcOctet, g2.room.octet)
	}
}

func TestSelfAddressedDataIsDelivered(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.drain()
	host.send(1, 10, 20, []byte("me"))
	got := th.drain()
	if len(got) != 1 || got[0].to != host.addr || string(parseFwd(t, got[0].b).payload) != "me" {
		t.Fatalf("%v", got)
	}
}

func TestDataNeedsTheRightToken(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	bad := &vclient{th: th, addr: ap("192.0.2.66", 1), room: roomReply{token: host.room.token}}
	bad.room.token[3] ^= 0x40
	bad.send(2, 1, 1, []byte("forged"))
	got := th.drain()
	if len(got) != 1 || got[0].to != bad.addr {
		t.Fatalf("forged packet reached %v", got)
	}
	if errReason(t, got[0].b) != ErrNoSession {
		t.Fatal("expected NO_SESSION")
	}
	// The token is the identity, not the source address: a wrong token from
	// the member's own address is just as unauthenticated.
	spoof := &vclient{th: th, addr: host.addr, room: bad.room}
	spoof.send(2, 1, 1, []byte("same-address-wrong-token"))
	got = th.drain()
	if len(got) != 1 || got[0].to != host.addr || errReason(t, got[0].b) != ErrNoSession {
		t.Fatalf("a wrong token from a member's address was accepted: %v", got)
	}
	_ = guest
	// A token is not valid until it was issued, and not after the session ended.
	host.leave()
	th.drain()
	host.send(2, 1, 1, []byte("after-leave"))
	got = th.drain()
	if len(got) != 1 || got[0].to != host.addr || errReason(t, got[0].b) != ErrNoSession {
		t.Fatalf("data after LEAVE: %v", got)
	}
}

func TestOversizePayloadIsDropped(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	host.send(2, 1, 1, make([]byte, MaxPayload+1))
	if got := th.drain(); len(got) != 0 {
		t.Fatalf("oversize payload forwarded: %d bytes", len(got[0].b))
	}
	host.send(2, 1, 1, make([]byte, MaxPayload)) // the limit itself is fine
	got := th.drain()
	if len(got) != 1 || len(parseFwd(t, got[0].b).payload) != MaxPayload {
		t.Fatalf("max-size payload not forwarded: %v", got)
	}
	host.send(2, 1, 1, nil) // empty payloads are datagrams too
	if got := th.drain(); len(got) != 1 {
		t.Fatalf("empty payload not forwarded")
	}
	// Datagrams longer than any valid frame never get parsed.
	before := th.h.snapshot().Oversize
	th.feed(host.addr, make([]byte, maxUDPFrame+1))
	if th.h.snapshot().Oversize != before+1 {
		t.Fatal("oversize datagram not counted")
	}
}

// ------------------------------------------------------- idempotent retries

func TestCreateRetryIsIdempotent(t *testing.T) {
	th := newTestHub(t, nil)
	src := ap("192.0.2.1", 1000)
	th.drain()
	var first []byte
	for i := 0; i < 5; i++ {
		th.hsFeed(src, handshakeFrame(42, "", MinUDPHandshake))
		pk := th.take(src)
		if len(pk) != 1 {
			t.Fatalf("retry %d: %d replies", i, len(pk))
		}
		if first == nil {
			first = pk[0]
		} else if !bytes.Equal(first, pk[0]) {
			t.Fatalf("retry %d returned a different ROOM:\n% x\n% x", i, first, pk[0])
		}
	}
	st := th.h.snapshot()
	if st.Rooms != 1 || st.Sessions != 1 || st.RoomsCreated != 1 || st.HandshakeRetries != 4 {
		t.Fatalf("retries made state: %+v", st)
	}
	// A different nonce is a different request.
	th.hsFeed(src, handshakeFrame(43, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 2 {
		t.Fatal("new nonce should create a second room")
	}
}

func TestJoinRetryIsIdempotent(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	src := ap("192.0.2.2", 1)
	th.drain()
	var first []byte
	for i := 0; i < 4; i++ {
		th.hsFeed(src, handshakeFrame(77, host.room.code, MinUDPHandshake))
		pk := th.take(src)
		if len(pk) != 1 {
			t.Fatalf("retry %d: %d replies", i, len(pk))
		}
		if first == nil {
			first = pk[0]
		} else if !bytes.Equal(first, pk[0]) {
			t.Fatalf("retry %d differs", i)
		}
	}
	if r := parseRoom(t, first); r.octet != 2 {
		t.Fatalf("joiner got %d", r.octet)
	}
	if th.h.snapshot().Sessions != 2 {
		t.Fatalf("join retries consumed addresses: %+v", th.h.snapshot())
	}
	next := th.mustJoin(ap("192.0.2.3", 1), 78, host.room.code)
	if next.room.octet != 3 {
		t.Fatalf("next joiner got %d, want 3 (retries must not use up addresses)", next.room.octet)
	}
}

func TestRetryAfterReplyLossStillWorks(t *testing.T) {
	// The client never saw the first ROOM (we simply drop it). The retry gets
	// the same token and address and the session is usable.
	th := newTestHub(t, nil)
	src := ap("192.0.2.1", 1)
	th.hsFeed(src, handshakeFrame(5, "", MinUDPHandshake))
	lost := parseRoom(t, th.take(src)[0])
	th.hsFeed(src, handshakeFrame(5, "", MinUDPHandshake))
	got := parseRoom(t, th.take(src)[0])
	if got != lost {
		t.Fatalf("retry returned a different session")
	}
	c := &vclient{th: th, addr: src, room: got}
	c.keepalive()
	if p := th.take(src); len(p) != 1 || p[0][0] != TypeKeepalive {
		t.Fatalf("session from the retried ROOM does not work: %v", p)
	}
}

func TestNonceMismatchIsRejected(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	other := th.mustCreate(ap("192.0.2.5", 1), 2)
	src := ap("192.0.2.2", 1)
	th.mustJoin(src, 10, host.room.code)
	for name, frame := range map[string][]byte{
		"create reusing a join nonce":      handshakeFrame(10, "", MinUDPHandshake),
		"join of another room, same nonce": handshakeFrame(10, other.room.code, MinUDPHandshake),
	} {
		th.drain()
		th.hsFeed(src, frame)
		pk := th.take(src)
		if len(pk) != 1 || errReason(t, pk[0]) != ErrNonceReused {
			t.Errorf("%s: want NONCE_REUSED, got %v", name, pk)
		}
	}
	if th.h.snapshot().Sessions != 3 {
		t.Fatalf("mismatched retries changed state: %+v", th.h.snapshot())
	}
}

func TestLateDuplicateHandshakeAfterLeaveDoesNotResurrect(t *testing.T) {
	th := newTestHub(t, nil)
	src := ap("192.0.2.1", 1)
	c := th.mustCreate(src, 9)
	c.leave()
	th.drain()
	th.hsFeed(src, handshakeFrame(9, "", MinUDPHandshake)) // a reordered duplicate of the CREATE
	pk := th.take(src)
	if len(pk) != 1 || errReason(t, pk[0]) != ErrClosed {
		t.Fatalf("want CLOSED, got %v", pk)
	}
	if th.h.snapshot().Rooms != 0 {
		t.Fatal("duplicate CREATE after LEAVE made a room")
	}
	// The tombstone expires.
	th.clock.Advance(th.h.cfg.NonceTTL + time.Second)
	th.h.Sweep()
	th.hsFeed(src, handshakeFrame(9, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 1 {
		t.Fatal("nonce not reusable after the tombstone expired")
	}
}

// --------------------------------------------------------- malformed frames

func TestMalformedFramesAreRejectedWithoutEffect(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	base := th.h.snapshot()
	src := ap("192.0.2.77", 9)

	tok := host.room.token
	withTok := func(typ byte, tail ...byte) []byte { return append(append([]byte{typ}, tok[:]...), tail...) }
	cases := map[string][]byte{
		"empty":                       {},
		"unknown type":                append([]byte{0x55}, make([]byte, 80)...),
		"client sends ROOM":           append([]byte{TypeRoom}, make([]byte, 80)...),
		"client sends ERROR":          append([]byte{TypeError}, make([]byte, 80)...),
		"CREATE unpadded":             handshakeFrame(5, "", 0),
		"CREATE 63 bytes":             handshakeFrame(5, "", MinUDPHandshake-1),
		"JOIN unpadded":               handshakeFrame(6, host.room.code, 0),
		"JOIN 63 bytes":               handshakeFrame(6, host.room.code, MinUDPHandshake-1),
		"CREATE with 1 byte":          {TypeCreate},
		"JOIN with 1 byte":            {TypeJoin},
		"DATA without token":          {TypeData},
		"DATA with half a token":      append([]byte{TypeData}, tok[:8]...),
		"DATA header truncated":       withTok(TypeData, 10, 0, 0, 2, 0, 1, 0), // 7 of 8 header bytes
		"KEEPALIVE half a token":      append([]byte{TypeKeepalive}, tok[:8]...),
		"LEAVE half a token":          append([]byte{TypeLeave}, tok[:8]...),
		"oversize":                    make([]byte, maxUDPFrame+1),
		"DATA payload over the limit": withTok(TypeData, append([]byte{10, 0, 0, 2, 0, 1, 0, 1}, make([]byte, MaxPayload+1)...)...),
	}
	for name, p := range cases {
		th.drain()
		th.feed(src, p)
		if got := th.drain(); len(got) != 0 {
			t.Errorf("%s: relay answered a malformed frame: % x", name, got[0].b)
		}
	}
	// The same malformed authenticated frames from a real member also have no effect.
	for _, p := range [][]byte{
		withTok(TypeData),
		withTok(TypeData, 10, 0, 0, 2, 0, 1, 0),
		withTok(TypeData, append([]byte{10, 0, 0, 2, 0, 1, 0, 1}, make([]byte, MaxPayload+1)...)...),
	} {
		th.feed(host.addr, p)
		if got := th.drain(); len(got) != 0 {
			t.Errorf("member's malformed frame produced output: % x", got[0].b)
		}
	}
	st := th.h.snapshot()
	if st.Rooms != base.Rooms || st.Sessions != base.Sessions || st.DataForwarded != base.DataForwarded {
		t.Fatalf("malformed frames changed state: %+v -> %+v", base, st)
	}
	if st.Malformed+st.Oversize-base.Malformed-base.Oversize < uint64(len(cases)) {
		t.Fatalf("malformed frames not counted: %+v", st)
	}
	// The relay is still healthy afterwards.
	host.send(2, 1, 1, []byte("ok"))
	if got := th.drain(); len(got) != 1 {
		t.Fatal("relay stopped forwarding after malformed input")
	}
}

func TestBadVersionGetsASmallError(t *testing.T) {
	th := newTestHub(t, nil)
	src := ap("192.0.2.1", 1)
	f := handshakeFrame(3, "", MinUDPHandshake)
	f[1] = ProtocolVersion + 1
	th.feed(src, f)
	pk := th.take(src)
	if len(pk) != 1 || errReason(t, pk[0]) != ErrBadVersion || pk[0][1] != ProtocolVersion {
		t.Fatalf("got %v", pk)
	}
	if pk[0][2+7] != 3 {
		t.Fatal("nonce not echoed")
	}
	if th.h.snapshot().Sessions != 0 {
		t.Fatal("session created for a bad version")
	}
}

// --------------------------------------------------------- anti-reflection

func TestRepliesToUnauthenticatedPacketsAreNeverLarger(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.HandshakeBurst, c.HandshakePerSec = 1e9, 1e9
		c.ReplyBurst, c.GlobalReplyBurst = 1e9, 1e9
		c.MaxRooms, c.MaxSessions = 100000, 100000
	})
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	rng := rand.New(rand.NewSource(1))
	replies := 0
	for i := 0; i < 20000; i++ {
		n := rng.Intn(300)
		p := make([]byte, n)
		rng.Read(p)
		if n > 0 {
			switch rng.Intn(6) { // bias towards the interesting types
			case 0:
				p[0] = TypeCreate
			case 1:
				p[0] = TypeJoin
			case 2:
				p[0] = TypeData
			case 3:
				p[0] = TypeKeepalive
			case 4:
				p[0] = TypeLeave
			}
			if n > 1 && rng.Intn(2) == 0 {
				p[1] = ProtocolVersion
			}
			if p[0] == TypeJoin && n >= joinLen && rng.Intn(2) == 0 {
				copy(p[createLen:], host.room.code)
			}
		}
		src := ap("203.0.113.9", uint16(1000+i%50))
		th.feed(src, p)
		for _, s := range th.drain() {
			replies++
			if len(s.b) > len(p) {
				t.Fatalf("packet of %d bytes (% x) drew a reply of %d bytes", len(p), p[:min(len(p), 20)], len(s.b))
			}
		}
	}
	if replies < 1000 {
		t.Fatalf("test did not exercise replies (%d)", replies)
	}
}

func TestNothingIsSentBackToShortUnauthenticatedPackets(t *testing.T) {
	th := newTestHub(t, nil)
	for n := 0; n < MinUDPHandshake; n++ {
		for _, typ := range []byte{TypeCreate, TypeJoin} {
			p := make([]byte, n)
			if n > 0 {
				p[0] = typ
			}
			if n > 1 {
				p[1] = ProtocolVersion
			}
			th.feed(ap("203.0.113.1", 1), p)
		}
	}
	if got := th.drain(); len(got) != 0 {
		t.Fatalf("%d replies to handshakes shorter than %d bytes", len(got), MinUDPHandshake)
	}
	if st := th.h.snapshot(); st.Rooms != 0 || st.Sessions != 0 {
		t.Fatalf("short handshakes created state: %+v", st)
	}
}

func TestUnauthenticatedRepliesAreRateLimited(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.ReplyPerSec, c.ReplyBurst = 5, 10
		c.HandshakePerSec, c.HandshakeBurst = 1e9, 1e9
		c.GlobalReplyPerSec, c.GlobalReplyBurst = 1e9, 1e9
	})
	victim := ap("203.0.113.50", 4444) // the spoofed source
	bad := handshakeFrame(1, "", MinUDPHandshake)
	bad[1] = 99 // BAD_VERSION: an easy way to draw a reply
	for i := 0; i < 1000; i++ {
		th.feed(victim, bad)
	}
	if n := len(th.drain()); n > 10 {
		t.Fatalf("a flood of 1000 packets drew %d replies from one source (burst is 10)", n)
	}
	if th.h.snapshot().RepliesSuppressed < 990 {
		t.Fatalf("suppressed = %d", th.h.snapshot().RepliesSuppressed)
	}
	// Tokens come back with time: one second later 5 more replies are allowed.
	th.clock.Advance(time.Second)
	for i := 0; i < 1000; i++ {
		th.feed(victim, bad)
	}
	if n := len(th.drain()); n != 5 {
		t.Fatalf("after 1 s, %d replies (want 5)", n)
	}
	// Another source has its own budget.
	th.feed(ap("203.0.113.51", 1), bad)
	if n := len(th.drain()); n != 1 {
		t.Fatalf("unrelated source got %d replies", n)
	}
	// Random-token DATA/KEEPALIVE from a stranger is also rate limited.
	for i := 0; i < 1000; i++ {
		p := append([]byte{TypeKeepalive}, make([]byte, TokenLen)...)
		p[1] = byte(i)
		th.feed(ap("203.0.113.60", 1), p)
	}
	if n := len(th.drain()); n > 10 {
		t.Fatalf("unknown-token flood drew %d replies", n)
	}
}

func TestAReplyLargerThanItsRequestIsNeverSent(t *testing.T) {
	// The padding rule normally guarantees this; the check itself is the
	// backstop, so exercise it directly.
	th := newTestHub(t, nil)
	src := ap("203.0.113.7", 9)
	th.h.replyUnauth(src, roomLen-1, make([]byte, roomLen), th.clock.Now(), false)
	if len(th.drain()) != 0 || th.h.snapshot().RepliesSuppressed != 1 {
		t.Fatal("an oversize reply was sent")
	}
	th.h.replyUnauth(src, roomLen, make([]byte, roomLen), th.clock.Now(), false)
	if len(th.drain()) != 1 {
		t.Fatal("an equal-size reply should be allowed")
	}
}

func TestGlobalReplyBudget(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.GlobalReplyPerSec, c.GlobalReplyBurst = 1, 20
		c.HandshakePerSec, c.HandshakeBurst = 1e9, 1e9
	})
	bad := handshakeFrame(1, "", MinUDPHandshake)
	bad[1] = 99
	for i := 0; i < 500; i++ { // 500 different (spoofed) sources
		th.feed(ap(fmt.Sprintf("198.51.%d.%d", i/250, i%250+1), 1), bad)
	}
	if n := len(th.drain()); n != 20 {
		t.Fatalf("global budget: %d replies, want 20", n)
	}
}

// ------------------------------------------------------------------ limits

func TestHandshakeRateLimitPerSource(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.HandshakePerSec, c.HandshakeBurst = 2, 5
		c.ReplyPerSec, c.ReplyBurst = 1e6, 1e6
		c.MaxRooms = 1000
	})
	src := ap("192.0.2.1", 1)
	for i := 0; i < 50; i++ {
		th.hsFeed(src, handshakeFrame(uint64(i+1), "", MinUDPHandshake))
	}
	st := th.h.snapshot()
	if st.Rooms != 5 {
		t.Fatalf("%d rooms created from one source in a burst, want 5", st.Rooms)
	}
	if st.RateLimited < 45 {
		t.Fatalf("rate limited = %d", st.RateLimited)
	}
	th.clock.Advance(time.Second)
	for i := 0; i < 50; i++ {
		th.hsFeed(src, handshakeFrame(uint64(100+i), "", MinUDPHandshake))
	}
	if th.h.snapshot().Rooms != 7 {
		t.Fatalf("after 1 s: %d rooms, want 7", th.h.snapshot().Rooms)
	}
	// Another IP is unaffected.
	th.hsFeed(ap("192.0.2.2", 1), handshakeFrame(999, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 8 {
		t.Fatal("other source was limited")
	}
	// A retry of an existing handshake still counts, but port changes do not
	// give a fresh budget (the key is the IP).
	th.hsFeed(ap("192.0.2.1", 2), handshakeFrame(1000, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 8 {
		t.Fatal("a new source port bypassed the per-IP limit")
	}
}

func TestRoomAndSessionCaps(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.MaxRooms = 2
		c.MaxSessions = 3
		c.HandshakeBurst, c.HandshakePerSec = 1e9, 1e9
	})
	a := th.mustCreate(ap("192.0.2.1", 1), 1)
	b := th.mustCreate(ap("192.0.2.2", 1), 2)
	th.drain()
	th.hsFeed(ap("192.0.2.3", 1), handshakeFrame(3, "", MinUDPHandshake))
	if r := errReason(t, th.take(ap("192.0.2.3", 1))[0]); r != ErrLimit {
		t.Fatalf("third room: reason %d, want LIMIT", r)
	}
	// Joining is not limited by the room cap, only by the session cap.
	th.mustJoin(ap("192.0.2.4", 1), 4, a.room.code)
	th.drain()
	th.hsFeed(ap("192.0.2.5", 1), handshakeFrame(5, b.room.code, MinUDPHandshake))
	if r := errReason(t, th.take(ap("192.0.2.5", 1))[0]); r != ErrLimit {
		t.Fatalf("fourth session: reason %d, want LIMIT", r)
	}
	if st := th.h.snapshot(); st.Rooms != 2 || st.Sessions != 3 {
		t.Fatalf("%+v", st)
	}
	// Freed capacity can be used again.
	b.leave()
	th.mustCreate(ap("192.0.2.6", 1), 6)
}

func TestPerClientPacketRate(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.ClientPPS, c.ClientPPSBurst = 10, 10 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.clock.Advance(time.Hour) // refill the buckets the validation keepalives used
	th.drain()
	for i := 0; i < 100; i++ {
		host.send(2, 1, 1, []byte{byte(i)})
	}
	got := th.drain()
	if len(got) != 10 {
		t.Fatalf("%d packets forwarded in a burst, want 10", len(got))
	}
	for i, g := range got { // the ones that pass are the first ones, in order
		if p := parseFwd(t, g.b).payload; len(p) != 1 || p[0] != byte(i) {
			t.Fatalf("packet %d payload %v", i, p)
		}
	}
	if th.h.snapshot().RateLimited != 90 {
		t.Fatalf("rate limited = %d", th.h.snapshot().RateLimited)
	}
	th.clock.Advance(500 * time.Millisecond) // 5 more tokens
	for i := 0; i < 100; i++ {
		host.send(2, 1, 1, []byte{byte(i)})
	}
	if n := len(th.drain()); n != 5 {
		t.Fatalf("after 0.5 s: %d packets, want 5", n)
	}
	// Limits are per client: the other member still has its full budget.
	g := &vclient{th: th, addr: ap("192.0.2.2", 1)}
	for tok, s := range th.h.byToken {
		if s.octet == 2 {
			g.room.token = tok
		}
	}
	for i := 0; i < 10; i++ {
		g.send(1, 1, 1, []byte{1})
	}
	if n := len(th.drain()); n != 10 {
		t.Fatalf("guest got %d, want 10", n)
	}
}

func TestPerClientByteRate(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.ClientBPS, c.ClientBPSBurst = 1000, 3000 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.clock.Advance(time.Hour)
	th.drain()
	// 3000 bytes of budget; each frame is udpDataHdr+100 = 125 bytes.
	for i := 0; i < 100; i++ {
		host.send(2, 1, 1, make([]byte, 100))
	}
	if n := len(th.drain()); n != 3000/(udpDataHdr+100) {
		t.Fatalf("%d frames within the byte budget, want %d", n, 3000/(udpDataHdr+100))
	}
	// A frame bigger than the whole budget is never forwarded.
	th.clock.Advance(time.Hour)
	th.h.cfg.ClientBPSBurst = 500
	host.send(2, 1, 1, make([]byte, 1000))
	if n := len(th.drain()); n != 0 {
		t.Fatal("frame above the burst size was forwarded")
	}
}

// ---------------------------------------------------------------- timeouts

func TestSilentMemberTimesOutAndFreesItsAddress(t *testing.T) {
	th := newTestHub(t, nil) // 15 s timeout
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()

	// The host keeps alive, the guest goes quiet.
	for i := 0; i < 4; i++ {
		th.clock.Advance(5 * time.Second)
		host.keepalive()
		th.h.Sweep()
	}
	pk := th.take(guest.addr)
	if len(pk) != 1 || errReason(t, pk[0]) != ErrTimeout {
		t.Fatalf("timed-out guest should get ERROR(TIMEOUT), got %v", pk)
	}
	st := th.h.snapshot()
	if st.Sessions != 1 || st.Rooms != 1 || st.SessionsTimedOut != 1 {
		t.Fatalf("%+v", st)
	}
	// The guest's token is dead, its address free.
	guest.send(1, 1, 1, []byte("zombie"))
	if p := th.drain(); len(p) != 1 || errReason(t, p[0].b) != ErrNoSession {
		t.Fatal("expired token still works")
	}
	if n := th.mustJoin(ap("192.0.2.3", 1), 3, host.room.code); n.room.octet != 2 {
		t.Fatalf("freed address not reused: %d", n.room.octet)
	}
}

func TestKeepalivesKeepAMemberAlive(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.drain()
	for i := 0; i < 20; i++ { // 100 s, far beyond the 15 s timeout
		th.clock.Advance(5 * time.Second)
		host.keepalive()
		th.h.Sweep()
	}
	if th.h.snapshot().Sessions != 1 {
		t.Fatal("member with keepalives was expired")
	}
	// Each keepalive is answered with a 1-byte KEEPALIVE.
	for _, p := range th.drain() {
		if len(p.b) != 1 || p.b[0] != TypeKeepalive {
			t.Fatalf("keepalive reply % x", p.b)
		}
	}
}

func TestRoomDiesWhenAllMembersTimeOut(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.clock.Advance(16 * time.Second)
	th.h.Sweep()
	if st := th.h.snapshot(); st.Rooms != 0 || st.Sessions != 0 {
		t.Fatalf("%+v", st)
	}
}

func TestIdleRoomIsClosedEvenWithKeepalives(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.RoomIdleTimeout = 60 * time.Second })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	// Data keeps a room alive...
	for i := 0; i < 5; i++ {
		th.clock.Advance(30 * time.Second)
		host.keepalive()
		guest.keepalive()
		host.send(2, 1, 1, []byte("x"))
		th.h.Sweep()
	}
	if th.h.snapshot().Rooms != 1 {
		t.Fatal("active room closed")
	}
	th.drain()
	// ...keepalives alone do not.
	for i := 0; i < 4; i++ {
		th.clock.Advance(20 * time.Second)
		host.keepalive()
		guest.keepalive()
		th.h.Sweep()
	}
	st := th.h.snapshot()
	if st.Rooms != 0 || st.Sessions != 0 || st.RoomsIdleClosed != 1 {
		t.Fatalf("idle room not closed: %+v", st)
	}
	closed := 0
	for _, p := range th.drain() {
		if len(p.b) == errorLen && p.b[0] == TypeError && p.b[2+NonceLen] == ErrClosed {
			closed++
		}
	}
	if closed != 2 {
		t.Fatalf("%d members told the room closed, want 2", closed)
	}
}

// pathResponse is a client's PATH_RESPONSE frame.
func (c *vclient) pathResponse(cookie []byte) []byte {
	return append(append([]byte{TypePathResponse}, c.room.token[:]...), cookie...)
}

// challengeTo returns the cookie of the single PATH_CHALLENGE sent to addr in
// out, failing if there is not exactly one frame for it.
func challengeCookie(t *testing.T, out [][]byte) []byte {
	t.Helper()
	if len(out) != 1 || len(out[0]) != challengeLen || out[0][0] != TypePathChallenge {
		t.Fatalf("want one PATH_CHALLENGE, got %v", out)
	}
	return out[0][1:]
}

func TestRealNATRebindingMigratesOnlyAfterTheEcho(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	moved := ap("198.51.100.77", 40000)

	// The guest's NAT mapping changes: same token, another source.
	req := guest.tokenFrame(TypeKeepalive)
	th.feed(moved, req)
	out := th.drain()
	var atMoved [][]byte
	for _, p := range out {
		if p.to == moved {
			atMoved = append(atMoved, p.b)
		}
	}
	cookie := challengeCookie(t, atMoved)
	if len(atMoved[0]) >= len(req) {
		t.Fatal("the challenge must be smaller than the frame that triggered it")
	}
	// Not moved yet: traffic still goes to the old, validated address.
	host.send(2, 1, 1, []byte("before-echo"))
	got := th.drain()
	if len(got) != 1 || got[0].to != guest.addr {
		t.Fatalf("session moved before the echo: %v", got)
	}
	// The echo from the new address moves it.
	th.feed(moved, guest.pathResponse(cookie))
	host.send(2, 1, 1, []byte("after-echo"))
	got = th.drain()
	if len(got) != 1 || got[0].to != moved {
		t.Fatalf("session did not follow the validated path: %v", got)
	}
	if th.h.byToken[guest.room.token].octet != 2 {
		t.Fatal("vaddr changed on migration")
	}
	if th.h.snapshot().PathMigrations != 1 {
		t.Fatalf("%+v", th.h.snapshot())
	}
	// The old address is no longer the session's: its frames now get challenged.
	th.clock.Advance(2 * time.Second)
	th.drain()
	th.feed(guest.addr, guest.tokenFrame(TypeKeepalive))
	old := 0
	for _, b := range th.take(guest.addr) {
		if len(b) == challengeLen {
			old++
		}
	}
	if old != 1 {
		t.Fatal("old address was not challenged")
	}
}

func TestSpoofedSourceCannotRedirectASession(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	victim := ap("203.0.113.9", 53)

	// An attacker who holds the guest's token forges frames "from" the victim.
	for _, typ := range []byte{TypeKeepalive, TypeData} {
		var f []byte
		if typ == TypeKeepalive {
			f = guest.tokenFrame(TypeKeepalive)
		} else {
			f = guest.dataFrame(1, 1, 1, []byte("x"))
		}
		th.feed(victim, f)
	}
	out := th.drain()
	toVictim := 0
	for _, p := range out {
		if p.to == victim {
			toVictim++
			if len(p.b) != challengeLen {
				t.Fatalf("the victim was sent something other than a challenge: % x", p.b)
			}
		}
	}
	if toVictim != 1 {
		t.Fatalf("victim received %d packets from two forged frames, want 1 challenge", toVictim)
	}
	// Relayed traffic still goes to the real guest, never to the victim.
	for i := 0; i < 20; i++ {
		host.send(2, 1, 1, []byte("for-the-guest"))
	}
	for _, p := range th.drain() {
		if p.to == victim {
			t.Fatalf("relay sent data to the victim: % x", p.b)
		}
	}
	// A flood of forged frames, even from many different victims, produces at
	// most one challenge per interval for the whole session.
	for i := 0; i < 500; i++ {
		th.feed(ap(fmt.Sprintf("203.0.%d.%d", i/250, i%250+1), 53), guest.tokenFrame(TypeKeepalive))
	}
	n := 0
	for _, p := range th.drain() {
		if len(p.b) == challengeLen {
			n++
		}
	}
	if n != 0 {
		t.Fatalf("%d challenges inside the interval", n)
	}
	th.clock.Advance(2 * time.Second)
	th.feed(ap("203.0.250.1", 53), guest.tokenFrame(TypeKeepalive))
	n = 0
	for _, p := range th.drain() {
		if len(p.b) == challengeLen {
			n++
		}
	}
	if n != 1 {
		t.Fatalf("%d challenges after the interval, want 1", n)
	}
}

func TestPathResponsesAreChecked(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	n1 := ap("198.51.100.1", 1000)
	other := ap("198.51.100.2", 2000)
	th.feed(n1, guest.tokenFrame(TypeKeepalive))
	cookie := challengeCookie(t, th.take(n1))
	toGuest := func() bool {
		host.send(2, 1, 1, []byte("probe"))
		got := th.drain()
		return len(got) == 1 && got[0].to == n1
	}
	bad := append([]byte(nil), cookie...)
	bad[0] ^= 1
	th.feed(n1, guest.pathResponse(bad))       // wrong cookie
	th.feed(other, guest.pathResponse(cookie)) // right cookie, wrong address
	wrongTok := &vclient{th: th, addr: n1, room: roomReply{token: guest.room.token}}
	wrongTok.room.token[0] ^= 1
	th.feed(n1, wrongTok.pathResponse(cookie))  // right cookie, wrong token
	th.feed(n1, guest.pathResponse(cookie[:7])) // truncated
	if toGuest() {
		t.Fatal("a bad PATH_RESPONSE moved the session")
	}
	if th.h.snapshot().BadPathResponses < 2 {
		t.Fatalf("%+v", th.h.snapshot())
	}
	// Expired.
	th.clock.Advance(th.h.cfg.PathChallengeTimeout + time.Second)
	th.feed(n1, guest.pathResponse(cookie))
	if toGuest() {
		t.Fatal("an expired challenge was accepted")
	}
	// Fresh challenge, right answer: moves; the same answer again is no longer valid.
	th.feed(n1, guest.tokenFrame(TypeKeepalive))
	cookie2 := challengeCookie(t, th.take(n1))
	th.feed(n1, guest.pathResponse(cookie2))
	if !toGuest() {
		t.Fatal("the right answer did not move the session")
	}
	// The same answer again is refused (and counted), not a second migration.
	before := th.h.snapshot()
	th.feed(n1, guest.pathResponse(cookie2))
	if after := th.h.snapshot(); after.PathMigrations != before.PathMigrations || after.BadPathResponses != before.BadPathResponses+1 {
		t.Fatalf("a repeated PATH_RESPONSE was accepted: %+v -> %+v", before, after)
	}
	// A completed validation cannot be replayed: move on to another address,
	// then repeat the old answer from the first one.
	th.clock.Advance(2 * time.Second)
	th.feed(other, guest.tokenFrame(TypeKeepalive))
	cookie3 := challengeCookie(t, th.take(other))
	th.feed(other, guest.pathResponse(cookie3))
	th.feed(n1, guest.pathResponse(cookie2))
	host.send(2, 1, 1, []byte("probe"))
	got := th.drain()
	if len(got) != 1 || got[0].to != other {
		t.Fatalf("a replayed PATH_RESPONSE moved the session: %v", got)
	}
}

func TestChallengesAtOneVictimAreRateLimited(t *testing.T) {
	// Many sessions (each with its own token) all spoofing one victim: the
	// per-session interval is not enough, the per-address reply limit is.
	th := newTestHub(t, func(c *Config) { c.ReplyPerSec, c.ReplyBurst = 5, 10; c.MaxMembers = 200 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	var members []*vclient
	for i := 0; i < 100; i++ {
		members = append(members, th.mustJoin(ap(fmt.Sprintf("192.0.3.%d", i+1), 1000), uint64(100+i), host.room.code))
	}
	th.drain()
	victim := ap("203.0.113.9", 53)
	for _, m := range members {
		th.feed(victim, m.tokenFrame(TypeKeepalive))
	}
	toVictim := 0
	for _, p := range th.drain() {
		if p.to == victim {
			toVictim++
		}
	}
	if toVictim > 10 {
		t.Fatalf("the victim received %d challenges from 100 sessions (burst limit 10)", toVictim)
	}
}

// retryCookie takes the cookie out of the RETRY the relay sent to addr.
func retryCookie(t *testing.T, pk [][]byte) []byte {
	t.Helper()
	if len(pk) != 1 || len(pk[0]) != retryLen || pk[0][0] != TypeRetry {
		t.Fatalf("want one RETRY, got %v", pk)
	}
	return pk[0][2+NonceLen:]
}

// A repeat of a handshake from another address must not learn the session's
// ROOM or token, even with a valid cookie of its own.
func TestHandshakeRepeatFromAnotherAddressLearnsNothing(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	elsewhere := ap("203.0.113.40", 4000)
	th.hsFeed(elsewhere, handshakeFrame(2, host.room.code, MinUDPHandshake)) // the guest's JOIN nonce
	pk := th.take(elsewhere)
	if len(pk) != 1 || errReason(t, pk[0]) != ErrNonceReused {
		t.Fatalf("a foreign repeat got %v", pk)
	}
	host.send(2, 1, 1, []byte("x"))
	if got := th.drain(); len(got) != 1 || got[0].to != guest.addr {
		t.Fatalf("a handshake repeat moved the session: %v", got)
	}
	// The guest's own repeat is still answered (idempotent), from its address.
	th.hsFeed(guest.addr, handshakeFrame(2, host.room.code, MinUDPHandshake))
	if pk := th.take(guest.addr); len(pk) != 1 || parseRoom(t, pk[0]).token != guest.room.token {
		t.Fatalf("the owner's repeat: %v", pk)
	}
}

// The reviewer's probe: forge a JOIN from the victim to create a session that
// points at the victim, learn its token by repeating the nonce from the
// attacker's own address, forge a KEEPALIVE from the victim to "validate" it,
// and have another member send data to it. Every step must fail.
func TestForgedHandshakeCannotAimASessionAtAVictim(t *testing.T) {
	th := newTestHub(t, nil)
	attacker := th.mustCreate(ap("198.51.100.66", 6000), 1) // a legitimate room of the attacker's
	victim := ap("203.0.113.9", 53)
	th.drain()

	// (a) JOIN forged from the victim, no cookie (the attacker never sees the RETRY).
	th.feed(victim, handshakeFrame(77, attacker.room.code, MinUDPHandshake))
	forged := th.take(victim)
	_ = retryCookie(t, forged) // the victim gets one small RETRY and the relay forgets it
	if len(forged[0]) >= MinUDPHandshake {
		t.Fatal("RETRY must be smaller than the request")
	}
	if st := th.h.snapshot(); st.Sessions != 1 || st.Rooms != 1 {
		t.Fatalf("a forged request created state: %+v", st)
	}
	// (b) The same nonce from the attacker's own address, with a cookie that is
	// valid for the attacker's own address: that is a request for a NEW session
	// of the attacker's own (this nonce was never used), pointing at the attacker.
	own := ap("198.51.100.67", 6001)
	th.hsFeed(own, handshakeFrame(77, attacker.room.code, MinUDPHandshake))
	pk := th.take(own)
	if len(pk) != 1 || pk[0][0] != TypeRoom {
		t.Fatalf("setup: %v", pk)
	}
	second := &vclient{th: th, addr: own, room: parseRoom(t, pk[0])}
	// (c) A KEEPALIVE bearing that token, forged to come from the victim.
	th.feed(victim, second.tokenFrame(TypeKeepalive))
	// (d) Traffic to the second member's vaddr.
	for i := 0; i < 100; i++ {
		attacker.send(second.room.octet, 1, 1, []byte("aimed-at-the-victim"))
	}
	data := 0
	for _, p := range th.drain() {
		if p.to == victim && p.b[0] == TypeData {
			data++
		}
	}
	if data != 0 {
		t.Fatalf("%d datagrams reached the victim", data)
	}
	// What the victim did get is bounded: RETRY and at most one PATH_CHALLENGE, both tiny.
	// (They were drained above; check the counters instead.)
	if th.h.snapshot().PathMigrations != 0 {
		t.Fatal("the session moved")
	}
}

func TestForgedHandshakesCreateNoStateAndUseNoBudget(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.MaxRooms = 5
		c.MaxRoomsPerIP = 1
		c.MaxSessionsPerIP = 1
		c.HandshakePerSec, c.HandshakeBurst = 1, 3 // forged requests at 8/s would drain this if they counted
	})
	victim := ap("203.0.113.9", 53)
	// A flood of forged CREATEs and JOINs from the victim's address and from
	// many other addresses, at a rate that would exhaust every budget if they
	// counted.
	for sec := 0; sec < 120; sec++ {
		for i := 0; i < 4; i++ {
			th.feed(victim, handshakeFrame(uint64(sec*10+i), "", MinUDPHandshake))
			th.feed(ap(fmt.Sprintf("192.0.2.%d", i+1), 5000), handshakeFrame(uint64(5000+sec*10+i), "ABCDEF", MinUDPHandshake))
		}
		th.clock.Advance(time.Second)
		th.h.Sweep()
		for _, p := range th.drain() {
			if len(p.b) != retryLen || len(p.b) >= MinUDPHandshake {
				t.Fatalf("forged request drew % x", p.b)
			}
		}
	}
	if st := th.h.snapshot(); st.Sessions != 0 || st.Rooms != 0 || st.RoomsCreated != 0 || len(th.h.perIP) != 0 || len(th.h.byNonce) != 0 {
		t.Fatalf("forged handshakes left state: %+v perIP=%d nonces=%d", st, len(th.h.perIP), len(th.h.byNonce))
	}
	// The victim's real client, from the same address, still gets its room, and
	// the relay-wide room budget is untouched.
	c := th.mustCreate(victim, 999999)
	if c.room.octet != 1 {
		t.Fatal("the victim could not create a room after the forged flood")
	}
	for i := 0; i < 4; i++ {
		th.mustCreate(ap(fmt.Sprintf("192.0.2.%d", 100+i), 1), uint64(2000000+i))
	}
	th.drain()
	th.hsFeed(ap("192.0.2.200", 1), handshakeFrame(3000000, "", MinUDPHandshake))
	if pk := th.take(ap("192.0.2.200", 1)); len(pk) != 1 || errReason(t, pk[0]) != ErrLimit {
		t.Fatalf("MaxRooms=5 should now be reached by real clients only: %v", pk)
	}
}

func TestHandshakeCookieIsBoundToSourceNonceAndTime(t *testing.T) {
	th := newTestHub(t, nil)
	a, b := ap("192.0.2.1", 1000), ap("192.0.2.2", 1000)
	th.drain()
	th.feed(a, handshakeFrame(10, "", MinUDPHandshake))
	cookie := retryCookie(t, th.take(a))
	// Another source, another port, another nonce: each just gets a RETRY.
	for name, c := range map[string]struct {
		src   netip.AddrPort
		nonce uint64
	}{"other address": {b, 10}, "other port": {ap("192.0.2.1", 1001), 10}, "other nonce": {a, 11}} {
		th.feed(c.src, handshakeFrameC(c.nonce, "", cookie, MinUDPHandshake))
		pk := th.take(c.src)
		if len(pk) != 1 || pk[0][0] != TypeRetry {
			t.Errorf("%s: a cookie for someone else was accepted: %v", name, pk)
		}
	}
	// A cookie with a flipped bit.
	bad := append([]byte(nil), cookie...)
	bad[3] ^= 1
	th.feed(a, handshakeFrameC(10, "", bad, MinUDPHandshake))
	if pk := th.take(a); len(pk) != 1 || pk[0][0] != TypeRetry {
		t.Errorf("forged cookie accepted: %v", pk)
	}
	if th.h.snapshot().Sessions != 0 {
		t.Fatal("state from rejected cookies")
	}
	// The previous time bucket still works (a request in flight across a boundary)...
	th.clock.Advance(th.h.cfg.RetryCookieBucket)
	th.feed(a, handshakeFrameC(10, "", cookie, MinUDPHandshake))
	if pk := th.take(a); len(pk) != 1 || pk[0][0] != TypeRoom {
		t.Fatalf("a cookie one bucket old was refused: %v", pk)
	}
	// ...but a cookie is good for at most two buckets. The fake clock starts at a
	// bucket boundary, so a cookie issued there lives to the end of the next
	// bucket: usable just before two buckets have passed, expired at exactly
	// two (a third bucket must not be accepted).
	bucket := th.h.cfg.RetryCookieBucket
	for _, tc := range []struct {
		name  string
		issue time.Duration // offset into the bucket at which the cookie is issued
		wait  time.Duration
		ok    bool
	}{
		{"issued at the start, just under two buckets", 0, 2*bucket - time.Millisecond, true},
		{"issued at the start, exactly two buckets", 0, 2 * bucket, false},
		{"issued at the end of a bucket, one bucket later", bucket - time.Millisecond, bucket, true},
		{"issued at the end of a bucket, two buckets later", bucket - time.Millisecond, 2 * bucket, false},
		{"three buckets", 0, 3 * bucket, false},
	} {
		x := newTestHub(t, nil)
		x.clock.Advance(tc.issue)
		x.feed(a, handshakeFrame(10, "", MinUDPHandshake))
		ck := retryCookie(t, x.take(a))
		x.clock.Advance(tc.wait)
		x.feed(a, handshakeFrameC(10, "", ck, MinUDPHandshake))
		pk := x.take(a)
		got := len(pk) == 1 && pk[0][0] == TypeRoom
		if got != tc.ok {
			t.Errorf("%s: accepted=%v, want %v", tc.name, got, tc.ok)
		}
	}
	// Another relay (another secret) does not honour it either.
	th3 := newTestHub(t, nil)
	th3.feed(a, handshakeFrameC(10, "", cookie, MinUDPHandshake))
	if pk := th3.take(a); len(pk) != 1 || pk[0][0] != TypeRetry {
		t.Fatalf("a cookie from another relay was accepted: %v", pk)
	}
}

func TestRetryDoesNotNeedAClientToChangeAnythingButTheCookie(t *testing.T) {
	// The retried request is the same bytes with the cookie filled in, so a
	// client can do it without reparsing anything else.
	th := newTestHub(t, nil)
	src := ap("192.0.2.1", 1)
	first := handshakeFrame(5, "", MinUDPHandshake)
	th.feed(src, first)
	cookie := retryCookie(t, th.take(src))
	second := append([]byte(nil), first...)
	copy(second[createLen:], cookie)
	th.feed(src, second)
	if pk := th.take(src); len(pk) != 1 || pk[0][0] != TypeRoom {
		t.Fatalf("%v", pk)
	}
}

func TestSessionsArePathValidatedFromBirth(t *testing.T) {
	// The cookie proved the address, so data flows at once, without any
	// further "I am here" from the client.
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	host.send(2, 1, 1, []byte("first"))
	if got := th.drain(); len(got) != 1 || got[0].to != ap("192.0.2.2", 1) {
		t.Fatalf("%v", got)
	}
}

func TestIPv6PrefixCapsBoundWhatOneSubscriberCanHold(t *testing.T) {
	// 256 /64s of one /56 (or /48) each stay under their own per-address caps,
	// but together they must not take more than PrefixFactor times one address.
	th := newTestHub(t, func(c *Config) {
		c.MaxRoomsPerIP = 2
		c.PrefixFactor = 3 // 6 rooms for the whole /48
		c.MaxRooms = 1000
	})
	created, refused := 0, 0
	for i := 0; i < 40; i++ {
		src := netip.MustParseAddrPort(fmt.Sprintf("[2001:db8:1:%x::1]:1000", i)) // 40 different /64s in one /48
		th.drain()
		th.hsFeed(src, handshakeFrame(uint64(i+1), "", MinUDPHandshake))
		pk := th.take(src)
		if len(pk) == 1 && pk[0][0] == TypeRoom {
			created++
		} else if len(pk) == 1 && errReason(t, pk[0]) == ErrLimit {
			refused++
		} else {
			t.Fatalf("unexpected %v", pk)
		}
	}
	if created != 6 || refused != 34 {
		t.Fatalf("created %d rooms from one /48, want 6 (refused %d)", created, refused)
	}
	// Another /48 has its own budget.
	th.hsFeed(netip.MustParseAddrPort("[2001:db8:2::1]:1000"), handshakeFrame(999, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 7 {
		t.Fatal("a different /48 was limited")
	}
	// Counters return to zero when everything is gone.
	th.clock.Advance(time.Hour)
	th.h.Sweep()
	if len(th.h.perIP) != 0 {
		t.Fatalf("counters leaked: %v", th.h.perIP)
	}
}

func TestLeaveRemovesTheMember(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	guest.leave()
	th.drain()
	host.send(2, 1, 1, []byte("x"))
	if th.h.snapshot().Unroutable != 1 || len(th.drain()) != 0 {
		t.Fatal("departed member still routable")
	}
}

// ------------------------------------------------------------- invariants

func TestAddressesNeverExceedTheMemberCap(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.MaxMembers = 6
		c.HandshakeBurst, c.HandshakePerSec = 1e9, 1e9
		c.ReplyBurst, c.GlobalReplyBurst = 1e9, 1e9
	})
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	rng := rand.New(rand.NewSource(7))
	var live []*vclient
	nonce := uint64(100)
	for i := 0; i < 2000; i++ {
		if rng.Intn(2) == 0 || len(live) == 0 {
			nonce++
			src := ap("192.0.2.2", uint16(nonce))
			th.drain()
			th.hsFeed(src, handshakeFrame(nonce, host.room.code, MinUDPHandshake))
			pk := th.take(src)
			if len(pk) != 1 {
				t.Fatal("no reply")
			}
			if pk[0][0] == TypeRoom {
				c := &vclient{th: th, addr: src, room: parseRoom(t, pk[0])}
				if c.room.octet < 2 || c.room.octet > 6 {
					t.Fatalf("address %d outside 2..6", c.room.octet)
				}
				for _, o := range live {
					if o.room.octet == c.room.octet {
						t.Fatalf("address %d handed out twice", c.room.octet)
					}
				}
				live = append(live, c)
			} else if errReason(t, pk[0]) != ErrRoomFull || len(live) != 5 {
				t.Fatalf("unexpected rejection with %d live", len(live))
			}
		} else {
			k := rng.Intn(len(live))
			live[k].leave()
			live = append(live[:k], live[k+1:]...)
		}
	}
}

// FuzzHandleUDP feeds arbitrary datagrams to a hub that has a room in it.
// Invariants: no panic; the hub only ever sends to the packet's source or to
// a member of the room (never anywhere else); and a reply to a source that
// holds no session is never larger than the packet that drew it.
func FuzzHandleUDP(f *testing.F) {
	f.Add([]byte{})
	f.Add(handshakeFrame(1, "", MinUDPHandshake))
	f.Add(handshakeFrame(2, "ABCDEF", MinUDPHandshake))
	f.Add(append([]byte{TypeData}, make([]byte, 40)...))
	f.Add(append([]byte{TypeKeepalive}, make([]byte, 16)...))
	f.Add(append([]byte{TypeLeave}, make([]byte, 16)...))
	f.Fuzz(func(t *testing.T, p []byte) {
		th := newTestHub(t, func(c *Config) {
			c.HandshakeBurst, c.HandshakePerSec = 1e9, 1e9
			c.ReplyBurst, c.GlobalReplyBurst = 1e9, 1e9
		})
		host := th.mustCreate(ap("192.0.2.1", 1), 1)
		guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
		th.drain()
		stranger := ap("203.0.113.5", 5)
		th.feed(stranger, p)
		for _, s := range th.drain() {
			if s.to != stranger && s.to != host.addr && s.to != guest.addr {
				t.Fatalf("sent to %v, which is neither the source nor a member", s.to)
			}
			if s.to == stranger && len(s.b) > len(p) {
				t.Fatalf("reply of %d bytes to a stranger's packet of %d", len(s.b), len(p))
			}
		}
	})
}

// ------------------------------------------------- per-address capacity

func TestIPKeyGroupsIPv6By64(t *testing.T) {
	same := func(a, b string) bool {
		return ipKey(netip.MustParseAddr(a)) == ipKey(netip.MustParseAddr(b))
	}
	if !same("2001:db8:1:2::1", "2001:db8:1:2:ffff:ffff:ffff:ffff") {
		t.Error("two hosts of one /64 must share a key")
	}
	if same("2001:db8:1:2::1", "2001:db8:1:3::1") {
		t.Error("different /64s must not share a key")
	}
	if !same("::ffff:192.0.2.7", "192.0.2.7") {
		t.Error("an IPv4-mapped address is the IPv4 address")
	}
	if same("192.0.2.7", "192.0.2.8") {
		t.Error("IPv4 addresses are keyed whole")
	}
}

func TestHandshakeBudgetIsPerIPv6Prefix(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.HandshakePerSec, c.HandshakeBurst = 1, 3 })
	// Five hosts of one /64 share the budget of three...
	for i := 1; i <= 5; i++ {
		th.hsFeed(netip.MustParseAddrPort(fmt.Sprintf("[2001:db8:1:2::%x]:1000", i)), handshakeFrame(uint64(i), "", MinUDPHandshake))
	}
	if th.h.snapshot().Rooms != 3 {
		t.Fatalf("%d rooms from one /64 in a burst, want 3 (rotating the host part must not help)", th.h.snapshot().Rooms)
	}
	// ...another /64 has its own.
	th.hsFeed(netip.MustParseAddrPort("[2001:db8:1:3::1]:1000"), handshakeFrame(99, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 4 {
		t.Fatal("a different /64 was limited")
	}
}

func TestPerAddressSessionCap(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxSessionsPerIP = 3; c.MaxMembers = 8 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustJoin(ap("192.0.2.1", 2), 2, host.room.code)
	g3 := th.mustJoin(ap("192.0.2.1", 3), 3, host.room.code)
	th.drain()
	th.hsFeed(ap("192.0.2.1", 4), handshakeFrame(4, host.room.code, MinUDPHandshake))
	if pk := th.take(ap("192.0.2.1", 4)); len(pk) != 1 || errReason(t, pk[0]) != ErrLimit {
		t.Fatalf("a fourth session from one address: %v", pk)
	}
	if th.h.snapshot().Sessions != 3 {
		t.Fatalf("%+v", th.h.snapshot())
	}
	// Other addresses are unaffected; leaving frees capacity.
	th.mustJoin(ap("192.0.2.2", 1), 5, host.room.code)
	g3.leave()
	th.mustJoin(ap("192.0.2.1", 6), 6, host.room.code)
	// The accounting returns to zero, no entry is left behind.
	th2 := newTestHub(t, func(c *Config) { c.MaxSessionsPerIP = 1 })
	c := th2.mustCreate(ap("192.0.2.9", 1), 1)
	c.leave()
	if len(th2.h.perIP) != 0 {
		t.Fatalf("per-address counters leaked: %v", th2.h.perIP)
	}
	th2.hsFeed(ap("192.0.2.9", 1), handshakeFrame(7, "ZZZZZZ", MinUDPHandshake)) // refused join
	if len(th2.h.perIP) != 0 {
		t.Fatalf("a refused request left a counter: %v", th2.h.perIP)
	}
}

func TestPerAddressRoomCap(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxRoomsPerIP = 2 })
	a := th.mustCreate(ap("192.0.2.1", 1), 1)
	th.mustCreate(ap("192.0.2.1", 2), 2)
	th.drain()
	th.hsFeed(ap("192.0.2.1", 3), handshakeFrame(3, "", MinUDPHandshake))
	if pk := th.take(ap("192.0.2.1", 3)); len(pk) != 1 || errReason(t, pk[0]) != ErrLimit {
		t.Fatalf("a third room from one address: %v", pk)
	}
	// Joining is not creating.
	th.mustJoin(ap("192.0.2.1", 4), 4, a.room.code)
	// The creator leaving does not release the room while others keep it alive
	// (that was the way to hold rooms open)...
	a.leave()
	th.drain()
	th.hsFeed(ap("192.0.2.1", 5), handshakeFrame(5, "", MinUDPHandshake))
	if pk := th.take(ap("192.0.2.1", 5)); len(pk) != 1 || errReason(t, pk[0]) != ErrLimit {
		t.Fatalf("room cap released while the room lived on: %v", pk)
	}
	// ...but when it dies, it does.
	th.clock.Advance(time.Hour)
	th.h.Sweep()
	th.mustCreate(ap("192.0.2.1", 6), 6)
}

func TestPanicsAreRecoveredAndCounted(t *testing.T) {
	th := newTestHub(t, nil)
	func() {
		defer th.h.recoverPanic("test")
		panic("boom")
	}()
	if th.h.snapshot().Panics != 1 {
		t.Fatal("panic not counted")
	}
}

func TestConfigRejectsKeepaliveThatWouldTimeOutHealthyClients(t *testing.T) {
	if _, err := New(Config{KeepaliveInterval: 10 * time.Second, SessionTimeout: 15 * time.Second}); err == nil {
		t.Fatal("a keepalive interval above a third of the session timeout was accepted")
	}
	if _, err := New(Config{KeepaliveInterval: 5 * time.Second, SessionTimeout: 15 * time.Second}); err != nil {
		t.Fatal(err)
	}
}

// 10.0.0.254 and 10.0.0.255 are reserved (doco/relay-protocol.md): a room can
// never be big enough to hand either to a player.
func TestMaxMembersStopsBeforeTheReservedAddresses(t *testing.T) {
	if _, err := New(Config{MaxMembers: 253}); err != nil {
		t.Fatalf("253 members (.1 to .253) must be accepted: %v", err)
	}
	for _, n := range []int{254, 255, 1000} {
		if _, err := New(Config{MaxMembers: n}); err == nil {
			t.Fatalf("MaxMembers %d would allow 10.0.0.254 as a player", n)
		}
	}
}

func TestForwardedForUsesTheEntryOurProxyAdded(t *testing.T) {
	srv, err := New(Config{TrustForwardedFor: true})
	if err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("GET", "/vnet", nil)
	r.RemoteAddr = "127.0.0.1:5555"
	r.Header.Set("X-Forwarded-For", "6.6.6.6, 198.51.100.4")
	if got := srv.clientIP(r); got != netip.MustParseAddr("198.51.100.4") {
		t.Fatalf("client IP %v: the leftmost entry is written by the client", got)
	}
	r.Header.Set("X-Forwarded-For", "garbage")
	if got := srv.clientIP(r); got != netip.MustParseAddr("127.0.0.1") {
		t.Fatalf("unparsable header: %v", got)
	}
	srv2, _ := New(Config{})
	r.Header.Set("X-Forwarded-For", "6.6.6.6")
	if got := srv2.clientIP(r); got != netip.MustParseAddr("127.0.0.1") {
		t.Fatalf("header trusted without the flag: %v", got)
	}
}

// ------------------------------------------------------ panics and locks

// lockable reports whether h.mu can be taken for writing soon: a recovered
// panic that left it locked would make every later packet hang.
func lockable(h *Hub) bool {
	done := make(chan struct{})
	go func() {
		h.mu.Lock()
		h.mu.Unlock()
		close(done)
	}()
	select {
	case <-done:
		return true
	case <-time.After(2 * time.Second):
		return false
	}
}

func TestARecoveredPanicNeverLeavesTheHubLocked(t *testing.T) {
	for _, where := range []string{"forward", "removeLocked"} {
		th := newTestHub(t, nil)
		srv := &Server{cfg: th.h.cfg, hub: th.h, stop: make(chan struct{})}
		host := th.mustCreate(ap("192.0.2.1", 1), 1)
		guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
		th.drain()
		th.h.panicHook = func(w string) {
			if w == where {
				panic("injected into " + w)
			}
		}
		switch where {
		case "forward": // under the read lock, from the packet path
			srv.handleUDP(host.dataFrame(2, 1, 1, []byte("x")), host.addr, &th.scratch)
		case "removeLocked": // under the write lock, from LEAVE...
			srv.handleUDP(guest.tokenFrame(TypeLeave), guest.addr, &th.scratch)
			if !lockable(th.h) {
				t.Fatalf("%s: a recovered panic in LEAVE left the hub locked", where)
			}
			// ...and from the sweeper.
			th.clock.Advance(time.Hour)
			srv.sweep()
		}
		if th.h.snapshot().Panics == 0 {
			t.Fatalf("%s: the injected panic did not happen", where)
		}
		if !lockable(th.h) {
			t.Fatalf("%s: a recovered panic left the hub locked", where)
		}
		// And the relay still works.
		th.h.panicHook = nil
		th.clock.Advance(0)
		again := th.mustCreate(ap("192.0.2.9", 1), 99)
		if again.room.octet != 1 {
			t.Fatalf("%s: relay broken after the panic", where)
		}
	}
}

func TestWebSocketAdmissionIsAtomic(t *testing.T) {
	// Many upgrades arriving at once must not together exceed the limit: the
	// check and the reservation are one step.
	th := newTestHub(t, func(c *Config) { c.MaxSessions = 5 })
	var wg sync.WaitGroup
	var admitted atomic.Int64
	for i := 0; i < 200; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if th.h.acquireConn(kindWS, netip.MustParseAddr(fmt.Sprintf("192.0.2.%d", i%250+1))) {
				admitted.Add(1)
			}
		}(i)
	}
	wg.Wait()
	if admitted.Load() != 5 {
		t.Fatalf("%d upgrades admitted with MaxSessions=5", admitted.Load())
	}
	th.h.releaseConn(kindWS, netip.MustParseAddr("192.0.2.1"), true)
	if !th.h.acquireConn(kindWS, netip.MustParseAddr("192.0.2.77")) {
		t.Fatal("a released slot was not reusable")
	}
}

func TestConfigRequiresCookiesToExpireBeforeNoncesAreForgotten(t *testing.T) {
	// A cookie valid for up to two buckets must be dead before the nonce of a
	// finished session is forgotten, or a replayed handshake could resurrect a room.
	if _, err := New(Config{RetryCookieBucket: 30 * time.Second, NonceTTL: 60 * time.Second}); err == nil {
		t.Fatal("2 x bucket == NonceTTL was accepted")
	}
	if _, err := New(Config{RetryCookieBucket: 40 * time.Second, NonceTTL: 60 * time.Second}); err == nil {
		t.Fatal("2 x bucket > NonceTTL was accepted")
	}
	if _, err := New(Config{RetryCookieBucket: 29 * time.Second, NonceTTL: 60 * time.Second}); err != nil {
		t.Fatal(err)
	}
	if _, err := New(Config{}); err != nil {
		t.Fatalf("defaults: %v", err)
	}
}

// ---------------------------------------- relay-wide floods of forged packets

// A flood of forged, random-source packets above the old global reply budget
// must not stop a real client from completing its handshake: RETRY and
// PATH_CHALLENGE have their own allowance, and the ROOM/ERROR replies to a
// source that echoed a cookie are not charged to the unauthenticated budgets
// at all.
func TestForgedFloodAboveTheGlobalBudgetDoesNotStopRealHandshakes(t *testing.T) {
	th := newTestHub(t, nil) // GlobalReplyPerSec 5000
	rng := rand.New(rand.NewSource(3))
	forged := func(n int) {
		for i := 0; i < n; i++ {
			src := netip.AddrPortFrom(netip.AddrFrom4([4]byte{byte(1 + rng.Intn(200)), byte(rng.Intn(256)), byte(rng.Intn(256)), byte(rng.Intn(256))}), uint16(1024+rng.Intn(60000)))
			if i%2 == 0 {
				th.feed(src, handshakeFrame(rng.Uint64(), "", MinUDPHandshake)) // draws a RETRY
			} else {
				f := handshakeFrame(rng.Uint64(), "", MinUDPHandshake)
				f[1] = 9 // wrong version: draws an ERROR from the other budget
				th.feed(src, f)
			}
		}
		th.drain()
	}
	for sec := 0; sec < 4; sec++ {
		forged(6000) // above the 5000/s global budget, every second
		real := ap(fmt.Sprintf("192.0.2.%d", sec+1), 4000)
		c := th.mustCreate(real, uint64(100+sec))
		if c.room.octet != 1 {
			t.Fatalf("second %d: the real client's handshake failed under the flood", sec)
		}
		th.clock.Advance(time.Second)
	}
	// The flood drew no state.
	if st := th.h.snapshot(); st.Rooms != 4 || st.Sessions != 4 {
		t.Fatalf("%+v", st)
	}
}

func TestRepliesToCookieProvenSourcesAreNotChargedToTheUnprovenBudgets(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.GlobalReplyPerSec, c.GlobalReplyBurst = 1, 1; c.ReplyPerSec, c.ReplyBurst = 1, 1 })
	src := ap("192.0.2.1", 1000)
	// Get a cookie first (this uses the sender's tiny reply budget), then
	// use up every unauthenticated budget there is.
	th.feed(src, handshakeFrame(5, "", MinUDPHandshake))
	cookie := retryCookie(t, th.take(src))
	for i := 0; i < 50; i++ {
		f := handshakeFrame(uint64(i), "", MinUDPHandshake)
		f[1] = 9
		th.feed(src, f) // bad version from the same address: drains its reply budget and the global one
		th.feed(ap(fmt.Sprintf("198.51.100.%d", i+1), 1), f)
	}
	th.drain()
	// A request that echoes the cookie still gets its answer.
	th.feed(src, handshakeFrameC(5, "", cookie, MinUDPHandshake))
	if pk := th.take(src); len(pk) != 1 || pk[0][0] != TypeRoom {
		t.Fatalf("a cookie-proven source got no ROOM: %v", pk)
	}
	// So does a proven source asking for a room that does not exist.
	th.feed(src, handshakeFrame(6, "ABCDEF", MinUDPHandshake))
	th.drain() // (the RETRY may or may not fit the exhausted budget)
	ck2 := th.h.cookieFor(src, nonceBytes(6), th.h.bucketOf(th.clock.Now()))
	th.feed(src, handshakeFrameC(6, "ABCDEF", ck2[:], MinUDPHandshake))
	if pk := th.take(src); len(pk) != 1 || errReason(t, pk[0]) != ErrRoomNotFound {
		t.Fatalf("a proven source got no ERROR: %v", pk)
	}
}

// exhaustUnprovenBudgets uses up the ordinary global reply budget (and the
// per-address budgets of 50 throwaway addresses) with wrong-version packets.
func exhaustUnprovenBudgets(th *testHub) {
	for i := 0; i < 50; i++ {
		f := handshakeFrame(uint64(1000+i), "", MinUDPHandshake)
		f[1] = 9
		th.feed(ap(fmt.Sprintf("198.51.100.%d", i+1), 1), f)
	}
	th.drain()
}

// Every other answer to a cookie-proven source is also free of the
// unauthenticated budgets: the rate-limit ERROR and the malformed-code ERROR.
func TestErrorsToCookieProvenSourcesAreNotChargedToTheUnprovenBudgets(t *testing.T) {
	th := newTestHub(t, func(c *Config) {
		c.GlobalReplyPerSec, c.GlobalReplyBurst = 1, 1
		c.ReplyPerSec, c.ReplyBurst = 1, 1
		c.HandshakePerSec, c.HandshakeBurst = 1, 1
	})
	exhaustUnprovenBudgets(th)
	ck := func(src netip.AddrPort, nonce uint64) []byte {
		c := th.h.cookieFor(src, nonceBytes(nonce), th.h.bucketOf(th.clock.Now()))
		return c[:]
	}
	// Malformed room code.
	a := ap("192.0.2.1", 1000)
	th.feed(a, handshakeFrameC(1, "!!!!!!", ck(a, 1), MinUDPHandshake))
	if pk := th.take(a); len(pk) != 1 || errReason(t, pk[0]) != ErrRoomNotFound {
		t.Fatalf("malformed code: want ERROR(ROOM_NOT_FOUND), got %v", pk)
	}
	// Second proven handshake from one address inside the 1/s limit.
	b := ap("192.0.2.2", 1000)
	th.feed(b, handshakeFrameC(2, "", ck(b, 2), MinUDPHandshake))
	if pk := th.take(b); len(pk) != 1 || pk[0][0] != TypeRoom {
		t.Fatalf("first handshake: %v", pk)
	}
	th.feed(b, handshakeFrameC(3, "", ck(b, 3), MinUDPHandshake))
	if pk := th.take(b); len(pk) != 1 || errReason(t, pk[0]) != ErrRate {
		t.Fatalf("second handshake: want ERROR(RATE), got %v", pk)
	}
}

// PATH_CHALLENGE is not starved by a flood of ordinary unauthenticated
// replies either: a client behind a rebinding NAT can still migrate.
func TestPathChallengeSurvivesAnExhaustedOrdinaryReplyBudget(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.GlobalReplyPerSec, c.GlobalReplyBurst = 1, 1 })
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	exhaustUnprovenBudgets(th)
	moved := ap("203.0.113.9", 40000)
	th.feed(moved, guest.tokenFrame(TypeKeepalive))
	var atMoved [][]byte
	for _, p := range th.drain() {
		if p.to == moved {
			atMoved = append(atMoved, p.b)
		}
	}
	challengeCookie(t, atMoved)
}
