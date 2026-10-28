// SPDX-License-Identifier: MIT

package relay

import (
	"bytes"
	"fmt"
	"math/rand"
	"strings"
	"testing"
	"time"
)

// ---------------------------------------------------------------- rooms

func TestCreateGivesHostAddressAndCode(t *testing.T) {
	th := newTestHub(t, func(c *Config) { c.MaxMembers = 5; c.KeepaliveInterval = 7 * time.Second })
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
	th.feed(ap("192.0.2.4", 1), handshakeFrame(4, host.room.code, MinUDPHandshake))
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
		th.feed(src, handshakeFrame(uint64(100+i), code, MinUDPHandshake))
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
	th.feed(ap("192.0.2.3", 1), handshakeFrame(9, host.room.code, MinUDPHandshake))
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
		th.feed(src, handshakeFrame(42, "", MinUDPHandshake))
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
	th.feed(src, handshakeFrame(43, "", MinUDPHandshake))
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
		th.feed(src, handshakeFrame(77, host.room.code, MinUDPHandshake))
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
	th.feed(src, handshakeFrame(5, "", MinUDPHandshake))
	lost := parseRoom(t, th.take(src)[0])
	th.feed(src, handshakeFrame(5, "", MinUDPHandshake))
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
		th.feed(src, frame)
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
	th.feed(src, handshakeFrame(9, "", MinUDPHandshake)) // a reordered duplicate of the CREATE
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
	th.feed(src, handshakeFrame(9, "", MinUDPHandshake))
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
	th.h.replyUnauth(src, roomLen-1, make([]byte, roomLen), th.clock.Now())
	if len(th.drain()) != 0 || th.h.snapshot().RepliesSuppressed != 1 {
		t.Fatal("an oversize reply was sent")
	}
	th.h.replyUnauth(src, roomLen, make([]byte, roomLen), th.clock.Now())
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
		c.MaxRooms = 1000
	})
	src := ap("192.0.2.1", 1)
	for i := 0; i < 50; i++ {
		th.feed(src, handshakeFrame(uint64(i+1), "", MinUDPHandshake))
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
		th.feed(src, handshakeFrame(uint64(100+i), "", MinUDPHandshake))
	}
	if th.h.snapshot().Rooms != 7 {
		t.Fatalf("after 1 s: %d rooms, want 7", th.h.snapshot().Rooms)
	}
	// Another IP is unaffected.
	th.feed(ap("192.0.2.2", 1), handshakeFrame(999, "", MinUDPHandshake))
	if th.h.snapshot().Rooms != 8 {
		t.Fatal("other source was limited")
	}
	// A retry of an existing handshake still counts, but port changes do not
	// give a fresh budget (the key is the IP).
	th.feed(ap("192.0.2.1", 2), handshakeFrame(1000, "", MinUDPHandshake))
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
	th.feed(ap("192.0.2.3", 1), handshakeFrame(3, "", MinUDPHandshake))
	if r := errReason(t, th.take(ap("192.0.2.3", 1))[0]); r != ErrLimit {
		t.Fatalf("third room: reason %d, want LIMIT", r)
	}
	// Joining is not limited by the room cap, only by the session cap.
	th.mustJoin(ap("192.0.2.4", 1), 4, a.room.code)
	th.drain()
	th.feed(ap("192.0.2.5", 1), handshakeFrame(5, b.room.code, MinUDPHandshake))
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

func TestNATRebindingFollowsTheToken(t *testing.T) {
	th := newTestHub(t, nil)
	host := th.mustCreate(ap("192.0.2.1", 1), 1)
	guest := th.mustJoin(ap("192.0.2.2", 1), 2, host.room.code)
	th.drain()
	// The guest's NAT mapping changes: same token, new source port and IP.
	moved := ap("198.51.100.77", 40000)
	th.feed(moved, guest.tokenFrame(TypeKeepalive))
	if pk := th.take(moved); len(pk) != 1 || pk[0][0] != TypeKeepalive {
		t.Fatalf("no keepalive reply at the new address: %v", pk)
	}
	host.send(2, 1, 1, []byte("after-rebind"))
	got := th.drain()
	if len(got) != 1 || got[0].to != moved {
		t.Fatalf("data went to %v, want %v", got, moved)
	}
	// The old address no longer receives anything for the guest.
	if th.h.byToken[guest.room.token].octet != 2 {
		t.Fatal("vaddr changed on rebinding")
	}
	// A retried handshake from the new address rebinds too.
	third := ap("198.51.100.78", 1)
	th.feed(third, handshakeFrame(2, host.room.code, MinUDPHandshake))
	th.take(third)
	host.send(2, 1, 1, []byte("again"))
	if got := th.drain(); len(got) != 1 || got[0].to != third {
		t.Fatalf("handshake retry did not rebind: %v", got)
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
			th.feed(src, handshakeFrame(nonce, host.room.code, MinUDPHandshake))
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
