// SPDX-License-Identifier: MIT

package relay

import (
	"encoding/binary"
	"net/netip"
	"sync"
	"testing"
	"time"
)

// fakeClock is a manually advanced clock, so that timeout and rate-limit
// tests never sleep.
type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func newFakeClock() *fakeClock { return &fakeClock{t: time.Unix(1_700_000_000, 0)} }
func (c *fakeClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}
func (c *fakeClock) Advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

// sentPacket is one UDP datagram the hub "sent".
type sentPacket struct {
	to netip.AddrPort
	b  []byte
}

// testHub drives a Hub synchronously, with no sockets: the hub's UDP output
// is recorded. Every call returns only after the hub has finished with the
// packet, so "no reply" is a plain assertion, not a timing guess.
type testHub struct {
	t       *testing.T
	h       *Hub
	clock   *fakeClock
	mu      sync.Mutex
	sent    []sentPacket
	scratch []byte
}

func newTestHub(t *testing.T, mod func(*Config)) *testHub {
	t.Helper()
	clk := newFakeClock()
	// Per-address caps are exercised by their own tests; everything else
	// comes from a handful of test addresses.
	cfg := Config{Now: clk.Now, MaxSessionsPerIP: 1e6, MaxRoomsPerIP: 1e6, MaxWSPerIP: 1e6, MaxPendingWSPerIP: 1e6}
	if mod != nil {
		mod(&cfg)
	}
	cfg, err := cfg.withDefaults()
	if err != nil {
		t.Fatal(err)
	}
	th := &testHub{t: t, h: newHub(cfg), clock: clk}
	th.h.sendUDP = func(to netip.AddrPort, b []byte) {
		th.mu.Lock()
		th.sent = append(th.sent, sentPacket{to, append([]byte(nil), b...)})
		th.mu.Unlock()
	}
	return th
}

// feed delivers a datagram from src to the hub.
func (th *testHub) feed(src netip.AddrPort, p []byte) {
	th.h.HandleUDP(p, src, &th.scratch)
}

// drain returns and clears everything the hub has sent.
func (th *testHub) drain() []sentPacket {
	th.mu.Lock()
	defer th.mu.Unlock()
	out := th.sent
	th.sent = nil
	return out
}

// take returns the packets sent to addr and discards the rest.
func (th *testHub) take(addr netip.AddrPort) [][]byte {
	var out [][]byte
	for _, p := range th.drain() {
		if p.to == addr {
			out = append(out, p.b)
		}
	}
	return out
}

func ap(ip string, port uint16) netip.AddrPort {
	return netip.AddrPortFrom(netip.MustParseAddr(ip), port)
}

// handshakeFrame builds CREATE_ROOM (code == "") or JOIN_ROOM padded to n
// bytes (at least the frame itself).
func handshakeFrame(nonce uint64, code string, n int) []byte {
	return handshakeFrameC(nonce, code, nil, n)
}

// handshakeFrameC is a handshake carrying a retry cookie (nil: none yet).
func handshakeFrameC(nonce uint64, code string, cookie []byte, n int) []byte {
	l := createLen
	typ := byte(TypeCreate)
	if code != "" {
		typ = TypeJoin
		l = joinLen
	}
	if n < l {
		n = l
	}
	b := make([]byte, n)
	b[0] = typ
	b[1] = ProtocolVersion
	binary.BigEndian.PutUint64(b[2:], nonce)
	copy(b[createLen:], code)
	if cookie != nil {
		at := createLen
		if code != "" {
			at = joinLen
		}
		copy(b[at:], cookie)
	}
	return b
}

func nonceBytes(n uint64) (b [NonceLen]byte) {
	binary.BigEndian.PutUint64(b[:], n)
	return
}

// room is a parsed ROOM reply.
type roomReply struct {
	nonce   [NonceLen]byte
	octet   byte
	token   [TokenLen]byte
	code    string
	max     int
	keepali int
}

func parseRoom(t testing.TB, b []byte) roomReply {
	t.Helper()
	if len(b) != roomLen || b[0] != TypeRoom || b[1] != ProtocolVersion {
		t.Fatalf("not a ROOM frame: % x", b)
	}
	var r roomReply
	copy(r.nonce[:], b[2:])
	if b[10] != 10 || b[11] != 0 || b[12] != 0 {
		t.Fatalf("vaddr not 10.0.0.x: % x", b[10:14])
	}
	r.octet = b[13]
	copy(r.token[:], b[14:])
	r.code = string(b[14+TokenLen : 14+TokenLen+CodeLen])
	r.max = int(b[14+TokenLen+CodeLen])
	r.keepali = int(b[14+TokenLen+CodeLen+1])
	return r
}

// errReason returns the reason of an ERROR frame, failing if b is not one.
func errReason(t testing.TB, b []byte) byte {
	t.Helper()
	if len(b) != errorLen || b[0] != TypeError {
		t.Fatalf("not an ERROR frame: % x", b)
	}
	return b[2+NonceLen]
}

// vclient is a UDP client of a testHub.
type vclient struct {
	th   *testHub
	addr netip.AddrPort
	room roomReply
}

// hsFeed delivers a handshake the way a real client behaves: the first
// request (no cookie) draws a RETRY, which is taken out of the recorded output
// and answered with the same request carrying the cookie. What remains in the
// recorded output are the relay's answers to the second request.
func (th *testHub) hsFeed(src netip.AddrPort, frame []byte) {
	th.t.Helper()
	th.feed(src, frame)
	th.mu.Lock()
	var retry []byte
	kept := th.sent[:0]
	for _, p := range th.sent {
		if retry == nil && p.to == src && len(p.b) == retryLen && p.b[0] == TypeRetry {
			retry = p.b
			continue
		}
		kept = append(kept, p)
	}
	th.sent = kept
	th.mu.Unlock()
	if retry == nil {
		return // refused outright (bad version, ...)
	}
	cookie := retry[2+NonceLen:]
	at := createLen
	if frame[0] == TypeJoin {
		at = joinLen
	}
	again := append([]byte(nil), frame...)
	copy(again[at:], cookie)
	th.feed(src, again)
}

func (th *testHub) mustCreate(addr netip.AddrPort, nonce uint64) *vclient {
	th.t.Helper()
	th.drain()
	th.hsFeed(addr, handshakeFrame(nonce, "", MinUDPHandshake))
	pk := th.take(addr)
	if len(pk) != 1 {
		th.t.Fatalf("CREATE: want 1 reply, got %d", len(pk))
	}
	return &vclient{th: th, addr: addr, room: parseRoom(th.t, pk[0])}
}

func (th *testHub) mustJoin(addr netip.AddrPort, nonce uint64, code string) *vclient {
	th.t.Helper()
	th.drain()
	th.hsFeed(addr, handshakeFrame(nonce, code, MinUDPHandshake))
	pk := th.take(addr)
	if len(pk) != 1 {
		th.t.Fatalf("JOIN: want 1 reply, got %d", len(pk))
	}
	return &vclient{th: th, addr: addr, room: parseRoom(th.t, pk[0])}
}

func (c *vclient) dataFrame(dstOctet byte, dport, sport uint16, payload []byte) []byte {
	b := make([]byte, 0, udpDataHdr+len(payload))
	b = append(b, TypeData)
	b = append(b, c.room.token[:]...)
	b = append(b, 10, 0, 0, dstOctet)
	b = append(b, byte(dport>>8), byte(dport), byte(sport>>8), byte(sport))
	return append(b, payload...)
}

func (c *vclient) send(dstOctet byte, dport, sport uint16, payload []byte) {
	c.th.feed(c.addr, c.dataFrame(dstOctet, dport, sport, payload))
}

func (c *vclient) tokenFrame(typ byte) []byte {
	return append([]byte{typ}, c.room.token[:]...)
}

func (c *vclient) keepalive() { c.th.feed(c.addr, c.tokenFrame(TypeKeepalive)) }
func (c *vclient) leave()     { c.th.feed(c.addr, c.tokenFrame(TypeLeave)) }

// fwd is a parsed relay-to-client DATA frame.
type fwd struct {
	srcOctet byte
	dport    uint16
	sport    uint16
	payload  []byte
}

func parseFwd(t testing.TB, b []byte) fwd {
	t.Helper()
	if len(b) < wsDataHdr || b[0] != TypeData || b[1] != 10 || b[2] != 0 || b[3] != 0 {
		t.Fatalf("not a forwarded DATA frame: % x", b)
	}
	return fwd{srcOctet: b[4], dport: binary.BigEndian.Uint16(b[5:]), sport: binary.BigEndian.Uint16(b[7:]), payload: b[9:]}
}
