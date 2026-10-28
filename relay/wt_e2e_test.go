// SPDX-License-Identifier: MIT

package relay

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/webtransport-go"
)

// Real QUIC: the relay's WebTransport listener on 127.0.0.1 with a
// development certificate, and clients built on webtransport-go's dialer that
// accept the certificate only by its hash, as a browser does with
// serverCertificateHashes.

// listenWT starts the environment's WebTransport listener.
func (e *env) listenWT() {
	e.t.Helper()
	cert, hash, err := NewDevCert(time.Now())
	if err != nil {
		e.t.Fatal(err)
	}
	addr, err := e.srv.ListenWT("127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{cert}})
	if err != nil {
		e.t.Fatal(err)
	}
	e.wt = addr.(*net.UDPAddr)
	e.certHash = hash
}

func newEnvWT(t *testing.T, mod func(*Config)) *env {
	t.Helper()
	e := newEnv(t, mod)
	e.listenWT()
	return e
}

func pinnedTLS(hexHash string) *tls.Config {
	want, _ := hex.DecodeString(hexHash)
	return &tls.Config{
		InsecureSkipVerify: true, // trust is the hash check below, as with serverCertificateHashes
		MinVersion:         tls.VersionTLS13,
		VerifyPeerCertificate: func(raw [][]byte, _ [][]*x509.Certificate) error {
			if len(raw) == 0 {
				return errors.New("no certificate")
			}
			if sum := sha256.Sum256(raw[0]); !bytes.Equal(sum[:], want) {
				return errors.New("certificate hash mismatch")
			}
			return nil
		},
	}
}

func quicClientConfig() *quic.Config {
	return &quic.Config{EnableDatagrams: true, EnableStreamResetPartialDelivery: true,
		HandshakeIdleTimeout: waitLimit, MaxIdleTimeout: 30 * time.Second}
}

// wtC is a WebTransport client of the relay; it is a member like udpC and wsC.
type wtC struct {
	t     *testing.T
	sess  *webtransport.Session
	ctl   *webtransport.Stream
	rx    chan []byte
	room  roomReply
	nonce uint64
	ctx   context.Context
	stop  context.CancelFunc
}

func dialWTRaw(t *testing.T, addr net.Addr, hash string, hdr http.Header) (*http.Response, *webtransport.Session, error) {
	d := &webtransport.Dialer{TLSClientConfig: pinnedTLS(hash), QUICConfig: quicClientConfig()}
	ctx, cancel := context.WithTimeout(context.Background(), waitLimit)
	defer cancel()
	return d.Dial(ctx, "https://"+addr.String()+wtPath, hdr)
}

func dialWT(t *testing.T, addr net.Addr, hash string) *wtC {
	t.Helper()
	_, sess, err := dialWTRaw(t, addr, hash, nil)
	if err != nil {
		t.Fatalf("dial WebTransport: %v", err)
	}
	return newWTC(t, sess)
}

func newWTC(t *testing.T, sess *webtransport.Session) *wtC {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	c := &wtC{t: t, sess: sess, rx: make(chan []byte, 8192), ctx: ctx, stop: cancel}
	st, err := sess.OpenStreamSync(ctx)
	if err != nil {
		t.Fatalf("open the control stream: %v", err)
	}
	c.ctl = st
	t.Cleanup(func() { cancel(); _ = sess.CloseWithError(0, "") })
	go func() { // control frames, length prefixed
		var hdr [2]byte
		for {
			if _, err := io.ReadFull(st, hdr[:]); err != nil {
				return
			}
			b := make([]byte, binary.BigEndian.Uint16(hdr[:]))
			if _, err := io.ReadFull(st, b); err != nil {
				return
			}
			select {
			case c.rx <- b:
			case <-ctx.Done():
				return
			}
		}
	}()
	go func() { // datagrams
		for {
			b, err := sess.ReceiveDatagram(ctx)
			if err != nil {
				return
			}
			select {
			case c.rx <- b:
			case <-ctx.Done():
				return
			}
		}
	}()
	return c
}

// write sends a DATA frame as a datagram and anything else on the control stream.
func (c *wtC) write(b []byte) {
	if len(b) > 0 && b[0] == TypeData {
		if err := c.sess.SendDatagram(b); err != nil {
			c.t.Fatalf("SendDatagram: %v", err)
		}
		return
	}
	if _, err := c.ctl.Write(ctlFrame(b)); err != nil {
		c.t.Fatalf("control write: %v", err)
	}
}

func (c *wtC) read(d time.Duration) []byte {
	select {
	case b := <-c.rx:
		return b
	case <-time.After(d):
		return nil
	}
}

func (c *wtC) octet() byte   { return c.room.octet }
func (c *wtC) vaddr() []byte { return []byte{10, 0, 0, c.room.octet} }

func (c *wtC) handshake(code string) *wtC {
	c.t.Helper()
	c.nonce = nonceCounter.Add(1) + 1<<43
	c.write(handshakeFrame(c.nonce, code, 0))
	b := c.read(waitLimit)
	if b == nil {
		c.t.Fatal("no reply to the handshake")
	}
	c.room = parseRoom(c.t, b)
	return c
}
func (c *wtC) create() *wtC          { return c.handshake("") }
func (c *wtC) join(code string) *wtC { return c.handshake(code) }

func (c *wtC) dataFrame(dst []byte, dport, sport uint16, payload []byte) []byte {
	b := []byte{TypeData}
	b = append(b, dst...)
	b = append(b, byte(dport>>8), byte(dport), byte(sport>>8), byte(sport))
	return append(b, payload...)
}

// syncWT is pending() for a WebTransport member: datagrams and the control
// stream are different paths, so it waits on a marker datagram to itself (sent
// after the datagrams to be covered, handled by the same goroutine in order).
func (c *wtC) sync(t *testing.T) [][]byte {
	t.Helper()
	marker := []byte(fmt.Sprintf("marker-%d", time.Now().UnixNano()))
	c.write(c.dataFrame(c.vaddr(), 1, 1, marker))
	var out [][]byte
	for {
		b := c.read(waitLimit)
		if b == nil {
			t.Fatal("the marker never came back")
		}
		if len(b) >= wsDataHdr && b[0] == TypeData && string(b[wsDataHdr:]) == string(marker) {
			return out
		}
		out = append(out, b)
	}
}

// ---------------------------------------------------------------- tests

func TestWebTransportEndToEndWithUDPAndWebSocketMembers(t *testing.T) {
	e := newEnvWT(t, nil)
	host := dialWT(t, e.wt, e.certHash).create()
	udp := dialUDP(t, e.udp).join(host.room.code)
	ws := dialWS(t, e.wsURL).join(host.room.code)
	wt2 := dialWT(t, e.wt, e.certHash).join(host.room.code)
	if host.octet() != 1 || udp.octet() != 2 || ws.octet() != 3 || wt2.octet() != 4 {
		t.Fatalf("addresses %d %d %d %d", host.octet(), udp.octet(), ws.octet(), wt2.octet())
	}
	members := []member{host, udp, ws, wt2}
	// Everyone sends one datagram to everyone (including themselves): content,
	// ports and the relay-filled sender address all arrive intact.
	for _, from := range members {
		for _, to := range members {
			send(from, to, 1000+uint16(to.octet()), 2000+uint16(from.octet()), fmt.Sprintf("from %d to %d", from.octet(), to.octet()))
		}
	}
	for _, to := range members {
		got := map[byte]string{}
		for range members {
			b := to.read(waitLimit)
			if b == nil {
				t.Fatalf("member %d: timed out", to.octet())
			}
			f := parseFwd(t, b)
			if f.dport != 1000+uint16(to.octet()) || f.sport != 2000+uint16(f.srcOctet) {
				t.Fatalf("member %d: ports %d/%d from %d", to.octet(), f.dport, f.sport, f.srcOctet)
			}
			got[f.srcOctet] = string(f.payload)
		}
		for _, from := range members {
			if got[from.octet()] != fmt.Sprintf("from %d to %d", from.octet(), to.octet()) {
				t.Fatalf("member %d got %q from %d", to.octet(), got[from.octet()], from.octet())
			}
		}
	}
	st := e.srv.Stats()
	if st.WebTransports != 2 || st.WebSockets != 1 || st.Sessions != 4 {
		t.Fatalf("stats %+v", st)
	}
}

func TestWebTransportCarriesMaxSizeDatagramsBothWays(t *testing.T) {
	e := newEnvWT(t, nil)
	host := dialWT(t, e.wt, e.certHash).create()
	udp := dialUDP(t, e.udp).join(host.room.code)
	payload := make([]byte, MaxPayload)
	for i := range payload {
		payload[i] = byte(i * 7)
	}
	send(host, udp, 1, 2, string(payload))
	expectData(t, udp, host, 1, 2, string(payload))
	send(udp, host, 3, 4, string(payload))
	expectData(t, host, udp, 3, 4, string(payload))
	send(host, host, 5, 6, string(payload)) // WebTransport to WebTransport: both directions of one connection
	expectData(t, host, host, 5, 6, string(payload))
}

func TestWebTransportRoutesOnlyWithinTheRoomAcrossTransports(t *testing.T) {
	e := newEnvWT(t, nil)
	a1 := dialWT(t, e.wt, e.certHash).create()
	a2 := dialUDP(t, e.udp).join(a1.room.code)
	a3 := dialWS(t, e.wsURL).join(a1.room.code)
	b1 := dialUDP(t, e.udp).create()
	b2 := dialWT(t, e.wt, e.certHash).join(b1.room.code)
	b3 := dialWS(t, e.wsURL).join(b1.room.code)
	// Both rooms use 10.0.0.1..3. a1 (WebTransport) and b2 (WebTransport)
	// probe every address.
	for o := byte(1); o < 12; o++ {
		a1.write(a1.dataFrame([]byte{10, 0, 0, o}, 3, 3, []byte("probe-a")))
		b2.write(b2.dataFrame([]byte{10, 0, 0, o}, 3, 3, []byte("probe-b")))
	}
	a1.write(a1.dataFrame([]byte{10, 0, 1, 2}, 3, 3, []byte("other-subnet")))
	// Handled in order per sender: after the markers every probe is routed.
	a1.sync(t)
	b2.sync(t)
	// Room A holds 1..3: three probes of a1 are deliverable (to itself, a2, a3), the rest are not.
	want := func(m member, from string, n int) {
		t.Helper()
		got := 0
		for {
			b := m.read(300 * time.Millisecond)
			if b == nil {
				break
			}
			if len(b) == 1 { // keepalive answers etc.
				continue
			}
			if p := string(parseFwd(t, b).payload); p != from {
				t.Fatalf("member %d (room %c) got %q, only %q may reach it", m.octet(), from[len(from)-1], p, from)
			}
			got++
		}
		if got != n {
			t.Fatalf("member %d got %d datagrams, want %d", m.octet(), got, n)
		}
	}
	want(a2, "probe-a", 1)
	want(a3, "probe-a", 1)
	want(b1, "probe-b", 1)
	want(b3, "probe-b", 1)
	// a1 and b2 each see their own probe-to-self and nothing else.
	for _, c := range []struct {
		m    *wtC
		from string
	}{{a1, "probe-a"}, {b2, "probe-b"}} {
		n := 0
		for _, b := range c.m.sync(t) {
			if string(parseFwd(t, b).payload) != c.from {
				t.Fatalf("WebTransport member %d received %q", c.m.octet(), parseFwd(t, b).payload)
			}
			n++
		}
		_ = n
	}
	st := e.srv.Stats()
	// a1: addresses 1, 2, 3 are deliverable, 8 of the 11 probes and the other
	// subnet are not; b2 likewise (8). Of the markers, two per sender go to itself.
	if st.Unroutable != 8+8+1 || st.DataForwarded != 3+3+2+2 {
		t.Fatalf("unroutable %d (want 17), forwarded %d (want 10)", st.Unroutable, st.DataForwarded)
	}
}

func TestWebTransportTokenIsNoCredentialOverUDP(t *testing.T) {
	e := newEnvWT(t, nil)
	wt := dialWT(t, e.wt, e.certHash).create()
	victim := dialUDP(t, e.udp).join(wt.room.code)
	u := dialUDP(t, e.udp)
	u.write(append([]byte{TypeKeepalive}, wt.room.token[:]...))
	if b := u.read(waitLimit); b == nil || errReason(t, b) != ErrNoSession {
		t.Fatalf("a WebTransport session's token worked over UDP: % x", b)
	}
	u.write(u.dataFrameWithToken(wt.room.token, victim.vaddr(), []byte("forged")))
	if b := u.read(waitLimit); b == nil || errReason(t, b) != ErrNoSession {
		t.Fatalf("DATA with a WebTransport token was accepted: % x", b)
	}
	expectSilence(t, victim)
}

func (c *udpC) dataFrameWithToken(tok [TokenLen]byte, dst []byte, payload []byte) []byte {
	b := []byte{TypeData}
	b = append(b, tok[:]...)
	b = append(b, dst...)
	b = append(b, 0, 1, 0, 1)
	return append(b, payload...)
}

func TestWebTransportHandshakeIsIdempotentOverQUIC(t *testing.T) {
	e := newEnvWT(t, nil)
	c := dialWT(t, e.wt, e.certHash)
	c.nonce = 99
	c.write(handshakeFrame(99, "", 0))
	first := c.read(waitLimit)
	c.write(handshakeFrame(99, "", 0))
	second := c.read(waitLimit)
	if first == nil || !bytes.Equal(first, second) {
		t.Fatalf("a repeated handshake returned %x then %x", first, second)
	}
	if st := e.srv.Stats(); st.Sessions != 1 || st.Rooms != 1 {
		t.Fatalf("stats %+v", st)
	}
}

func TestWebTransportMemberLeavesWhenItsSessionCloses(t *testing.T) {
	e := newEnvWT(t, nil)
	host := dialWT(t, e.wt, e.certHash).create()
	guest := dialWT(t, e.wt, e.certHash).join(host.room.code)
	_ = guest.sess.CloseWithError(0, "bye")
	waitFor(t, "the member to leave", func() bool { return e.srv.Stats().Sessions == 1 })
	host.stop()
	_ = host.sess.CloseWithError(0, "")
	waitFor(t, "everything to be given back", func() bool {
		st := e.srv.Stats()
		e.srv.hub.mu.RLock()
		defer e.srv.hub.mu.RUnlock()
		return st.Sessions == 0 && st.Rooms == 0 && st.WebTransports == 0 && len(e.srv.hub.perIP) == 0 && e.srv.hub.connsHeld == 0
	})
}

func TestWebTransportLeaveFrameFreesTheAddress(t *testing.T) {
	e := newEnvWT(t, nil)
	host := dialWT(t, e.wt, e.certHash).create()
	g := dialWT(t, e.wt, e.certHash).join(host.room.code)
	g.write([]byte{TypeLeave})
	waitFor(t, "the member to leave", func() bool { return e.srv.Stats().Sessions == 1 })
	if again := dialWT(t, e.wt, e.certHash).join(host.room.code); again.octet() != 2 {
		t.Fatalf("octet %d", again.octet())
	}
}

func TestWebTransportOriginAndPath(t *testing.T) {
	e := newEnvWT(t, func(c *Config) { c.AllowedOrigins = []string{"game.example"} })
	_, _, err := dialWTRaw(t, e.wt, e.certHash, http.Header{"Origin": {"https://evil.example"}})
	if err == nil {
		t.Fatal("a session from a disallowed origin was accepted")
	}
	_, sess, err := dialWTRaw(t, e.wt, e.certHash, http.Header{"Origin": {"https://game.example"}})
	if err != nil {
		t.Fatalf("an allowed origin was refused: %v", err)
	}
	_ = sess.CloseWithError(0, "")
	// A wrong path is a 404, not a session.
	d := &webtransport.Dialer{TLSClientConfig: pinnedTLS(e.certHash), QUICConfig: quicClientConfig()}
	ctx, cancel := context.WithTimeout(context.Background(), waitLimit)
	defer cancel()
	rsp, _, err := d.Dial(ctx, "https://"+e.wt.String()+"/elsewhere", nil)
	if err == nil || rsp == nil || rsp.StatusCode != http.StatusNotFound {
		t.Fatalf("wrong path: err=%v rsp=%v", err, rsp)
	}
}

func TestWebTransportWrongCertificateHashIsRefusedByTheClient(t *testing.T) {
	// The hash NewDevCert reports is the hash of the certificate the server
	// really presents (the client accepts nothing else), and a different hash
	// does not work.
	e := newEnvWT(t, nil)
	other := sha256.Sum256([]byte("not the certificate"))
	if _, _, err := dialWTRaw(t, e.wt, hex.EncodeToString(other[:]), nil); err == nil {
		t.Fatal("a client with the wrong hash connected")
	}
	if _, sess, err := dialWTRaw(t, e.wt, e.certHash, nil); err != nil {
		t.Fatalf("the reported hash did not match the served certificate: %v", err)
	} else {
		_ = sess.CloseWithError(0, "")
	}
}

// ----------------------------------------------------- admission limits

// quicOnly opens a QUIC connection to the relay's WebTransport port and does
// nothing else: no HTTP/3 request, no session. It is the unauthenticated
// client the per-address cap has to bound.
func quicOnly(t *testing.T, e *env) (*quic.Conn, error) {
	t.Helper()
	tc := pinnedTLS(e.certHash)
	tc.NextProtos = []string{http3.NextProtoH3}
	ctx, cancel := context.WithTimeout(context.Background(), waitLimit)
	defer cancel()
	conn, err := quic.DialAddr(ctx, e.wt.String(), tc, quicClientConfig())
	if err != nil {
		return nil, err
	}
	t.Cleanup(func() { _ = conn.CloseWithError(0, "") })
	return conn, nil
}

func TestWebTransportConnectionsWithoutASessionAreCappedPerAddress(t *testing.T) {
	e := newEnvWT(t, func(c *Config) { c.MaxPendingWTPerIP = 3; c.MaxWTPerIP = 5 })
	var conns []*quic.Conn
	for i := 0; i < 3; i++ {
		c, err := quicOnly(t, e)
		if err != nil {
			t.Fatalf("connection %d: %v", i, err)
		}
		conns = append(conns, c)
	}
	waitFor(t, "three admitted connections", func() bool { return e.srv.Stats().WebTransports == 3 })
	// A fourth QUIC connection, with no HTTP/3 request at all, is refused.
	if c, err := quicOnly(t, e); err == nil {
		// quic-go reports the refusal either at the handshake or at the first use.
		select {
		case <-c.Context().Done():
		case <-time.After(waitLimit):
			t.Fatal("a fourth pending connection was kept")
		}
	}
	waitFor(t, "the refusal to be counted", func() bool { return e.srv.Stats().WTRefused >= 1 })
	if n := e.srv.Stats().WebTransports; n != 3 {
		t.Fatalf("%d connections held", n)
	}
	// Closing one gives its slot back.
	_ = conns[0].CloseWithError(0, "")
	waitFor(t, "the slot to be released", func() bool { return e.srv.Stats().WebTransports == 2 })
	c, err := quicOnly(t, e)
	if err != nil {
		t.Fatalf("a freed slot was not reusable: %v", err)
	}
	select {
	case <-c.Context().Done():
		t.Fatal("the connection that should have fit was closed")
	case <-time.After(100 * time.Millisecond):
	}
}

func TestWebTransportSessionsArePerAddressCappedAndEnteringARoomFreesPendingSlots(t *testing.T) {
	e := newEnvWT(t, func(c *Config) { c.MaxPendingWTPerIP = 1; c.MaxWTPerIP = 2 })
	first := dialWT(t, e.wt, e.certHash) // pending: has a session but no room
	if _, _, err := dialWTRaw(t, e.wt, e.certHash, nil); err == nil {
		t.Fatal("a second pending session was admitted")
	}
	first.create() // in a room: no longer pending
	second := dialWT(t, e.wt, e.certHash)
	if _, _, err := dialWTRaw(t, e.wt, e.certHash, nil); err == nil {
		t.Fatal("the cap of 2 connections was exceeded")
	}
	second.stop()
	_ = second.sess.CloseWithError(0, "")
	waitFor(t, "a slot to be given back", func() bool { return e.srv.Stats().WebTransports == 1 })
	third := dialWT(t, e.wt, e.certHash)
	_ = third
}

func TestWebTransportHandshakeTimeoutFreesAPendingConnection(t *testing.T) {
	e := newEnvWT(t, func(c *Config) { c.MaxPendingWTPerIP = 1; c.HandshakeTimeout = 2 * time.Second })
	// The fake clock is the relay's: advance it past the timeout and sweep.
	c, err := quicOnly(t, e)
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, "the connection to be admitted", func() bool { return e.srv.Stats().WebTransports == 1 })
	e.clock.Advance(3 * time.Second)
	e.srv.Sweep()
	select {
	case <-c.Context().Done():
	case <-time.After(waitLimit):
		t.Fatal("the idle pending connection was not closed")
	}
	waitFor(t, "the slot to be released", func() bool { return e.srv.Stats().WebTransports == 0 })
}

// An address that was never validated gets a small Retry and nothing else: no
// handshake, no certificate, no state. quic-go sees the Retry through a
// recording socket and the first reply is examined.
type recordingPacketConn struct {
	net.PacketConn
	mu       sync.Mutex
	sent     [][]byte // what the client wrote
	received [][]byte // what the server answered
}

func (r *recordingPacketConn) WriteTo(b []byte, a net.Addr) (int, error) {
	r.mu.Lock()
	r.sent = append(r.sent, append([]byte(nil), b...))
	r.mu.Unlock()
	return r.PacketConn.WriteTo(b, a)
}

func (r *recordingPacketConn) ReadFrom(b []byte) (int, net.Addr, error) {
	n, a, err := r.PacketConn.ReadFrom(b)
	if err == nil {
		r.mu.Lock()
		r.received = append(r.received, append([]byte(nil), b[:n]...))
		r.mu.Unlock()
	}
	return n, a, err
}

func TestWebTransportFirstAnswerToAnUnvalidatedAddressIsASmallRetry(t *testing.T) {
	e := newEnvWT(t, nil)
	raw, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	rec := &recordingPacketConn{PacketConn: raw}
	tr := &quic.Transport{Conn: rec}
	t.Cleanup(func() { _ = tr.Close(); _ = raw.Close() })
	tc := pinnedTLS(e.certHash)
	tc.NextProtos = []string{http3.NextProtoH3}
	ctx, cancel := context.WithTimeout(context.Background(), waitLimit)
	defer cancel()
	conn, err := tr.Dial(ctx, e.wt, tc, quicClientConfig())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseWithError(0, "")
	rec.mu.Lock()
	defer rec.mu.Unlock()
	if len(rec.sent) == 0 || len(rec.received) == 0 {
		t.Fatal("nothing recorded")
	}
	first, reply := rec.sent[0], rec.received[0]
	if len(first) < 1200 {
		t.Fatalf("the client's first packet is %d bytes", len(first))
	}
	// A long-header packet of type Retry: first byte 1 1 (long, fixed) 11 (Retry).
	if reply[0]&0xf0 != 0xf0 {
		t.Fatalf("the first answer is not a Retry (first byte %#x, %d bytes)", reply[0], len(reply))
	}
	t.Logf("the first answer is a Retry of %d bytes against a request of %d bytes", len(reply), len(first))
	if len(reply) >= len(first)/4 {
		t.Fatalf("the Retry is %d bytes against a %d byte request", len(reply), len(first))
	}
}

// -------------------------------------------------------- dead connections

// blackholeConn is a client socket that can be cut off: nothing more is sent
// or received, as when a laptop is closed or a cable is pulled.
type blackholeConn struct {
	net.PacketConn
	cut atomic.Bool
}

func (b *blackholeConn) WriteTo(p []byte, a net.Addr) (int, error) {
	if b.cut.Load() {
		return len(p), nil
	}
	return b.PacketConn.WriteTo(p, a)
}

func (b *blackholeConn) ReadFrom(p []byte) (int, net.Addr, error) {
	for {
		n, a, err := b.PacketConn.ReadFrom(p)
		if err != nil || !b.cut.Load() {
			return n, a, err
		}
	}
}

func TestWebTransportMemberOfADeadConnectionIsDroppedByQUICIdleTimeout(t *testing.T) {
	// SessionTimeout is the QUIC idle timeout: a client that vanishes without
	// a word is gone within about that long (the sweeper does not time out
	// WebTransport members by frames, see Sweep).
	e := newEnvWT(t, func(c *Config) {
		c.SessionTimeout = 3 * time.Second
		c.KeepaliveInterval = time.Second
		c.PingInterval = time.Second
	})
	raw, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	bh := &blackholeConn{PacketConn: raw}
	tr := &quic.Transport{Conn: bh}
	t.Cleanup(func() { _ = tr.Close(); _ = raw.Close() })
	d := &webtransport.Dialer{TLSClientConfig: pinnedTLS(e.certHash), QUICConfig: quicClientConfig(),
		DialAddr: func(ctx context.Context, addr string, tc *tls.Config, qc *quic.Config) (*quic.Conn, error) {
			ua, err := net.ResolveUDPAddr("udp", addr)
			if err != nil {
				return nil, err
			}
			return tr.DialEarly(ctx, ua, tc, qc)
		}}
	ctx, cancel := context.WithTimeout(context.Background(), waitLimit)
	defer cancel()
	_, sess, err := d.Dial(ctx, "https://"+e.wt.String()+wtPath, nil)
	if err != nil {
		t.Fatal(err)
	}
	c := newWTC(t, sess).create()
	_ = c
	// Idle but alive: still a member after longer than the session timeout
	// (the relay pings, the client's QUIC stack answers).
	time.Sleep(4 * time.Second)
	if e.srv.Stats().Sessions != 1 {
		t.Fatal("an idle member of a live connection was dropped")
	}
	bh.cut.Store(true)
	waitFor2(t, 20*time.Second, "the dead connection's member to be dropped", func() bool {
		return e.srv.Stats().Sessions == 0 && e.srv.Stats().WebTransports == 0
	})
}

func waitFor2(t *testing.T, limit time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(limit)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for: %s", what)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// ------------------------------------------------- shutdown and stress

func TestServerCloseWithWebTransportSessionsDoesNotHang(t *testing.T) {
	clk := newFakeClock()
	srv, err := New(Config{Now: clk.Now})
	if err != nil {
		t.Fatal(err)
	}
	cert, hash, _ := NewDevCert(time.Now())
	addr, err := srv.ListenWT("127.0.0.1:0", &tls.Config{Certificates: []tls.Certificate{cert}})
	if err != nil {
		t.Fatal(err)
	}
	_ = dialWT(t, addr, hash).create()
	_ = dialWT(t, addr, hash)
	done := make(chan struct{})
	go func() { srv.Close(); close(done) }()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("Close did not return")
	}
}

func TestManyRoomsOfMixedTransportsConcurrently(t *testing.T) {
	e := newEnvWT(t, func(c *Config) { c.WSSendQueue = 4096; c.WTSendQueue = 4096; c.WTSendQueueBytes = 4 << 20 })
	const rooms = 6
	var wg sync.WaitGroup
	for r := 0; r < rooms; r++ {
		wg.Add(1)
		go func(r int) {
			defer wg.Done()
			var host, g1, g2 member
			if r%2 == 0 {
				host = dialWT(t, e.wt, e.certHash).create()
			} else {
				host = dialUDP(t, e.udp).create()
			}
			code := ""
			switch h := host.(type) {
			case *wtC:
				code = h.room.code
			case *udpC:
				code = h.room.code
			}
			g1 = dialWT(t, e.wt, e.certHash).join(code)
			g2 = dialWS(t, e.wsURL).join(code)
			ms := []member{host, g1, g2}
			// In rounds: everyone sends one datagram to each of the others, then
			// everyone reads the two that are theirs. (Datagrams are unreliable,
			// and a burst of hundreds from a dozen clients under the race detector
			// can overflow a receive queue; the rounds keep it to a handful.)
			const per = 40
			for i := 0; i < per; i++ {
				for _, from := range ms {
					for _, to := range ms {
						if from != to {
							send(from, to, 1, 1, fmt.Sprintf("%d/%d>%d/%d", r, from.octet(), to.octet(), i))
						}
					}
				}
				for _, to := range ms {
					for k := 0; k < 2; k++ {
						b := to.read(waitLimit)
						if b == nil {
							t.Errorf("room %d member %d: timed out in round %d", r, to.octet(), i)
							return
						}
						f := parseFwd(t, b)
						var rr, a, bb, j int
						if _, err := fmt.Sscanf(string(f.payload), "%d/%d>%d/%d", &rr, &a, &bb, &j); err != nil || rr != r || a != int(f.srcOctet) || bb != int(to.octet()) || j != i {
							t.Errorf("room %d member %d got %q from %d in round %d", r, to.octet(), f.payload, f.srcOctet, i)
							return
						}
					}
				}
			}
		}(r)
	}
	wg.Wait()
	if st := e.srv.Stats(); st.Panics != 0 || st.Malformed != 0 {
		t.Fatalf("stats %+v", st)
	}
}

var _ = websocket.BinaryMessage
