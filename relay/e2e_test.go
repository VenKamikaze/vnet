// SPDX-License-Identifier: MIT

package relay

import (
	"bytes"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// Real sockets in these tests: UDP on 127.0.0.1 and WebSockets through
// httptest. Every wait has a deadline; a deadline is a bound, never an
// expected duration. Time-based relay behaviour uses a fake clock.

const waitLimit = 5 * time.Second

type env struct {
	t     *testing.T
	srv   *Server
	clock *fakeClock
	udp   *net.UDPAddr
	http  *httptest.Server
	wsURL string
}

func newEnv(t *testing.T, mod func(*Config)) *env {
	t.Helper()
	clk := newFakeClock()
	cfg := Config{Now: clk.Now, PingInterval: time.Hour}
	if mod != nil {
		mod(&cfg)
	}
	srv, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	addr, err := srv.ListenUDP("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ts := httptest.NewServer(srv.Handler())
	e := &env{t: t, srv: srv, clock: clk, udp: addr.(*net.UDPAddr), http: ts,
		wsURL: "ws" + strings.TrimPrefix(ts.URL, "http") + "/vnet"}
	t.Cleanup(func() { ts.Close(); srv.Close() })
	return e
}

// member is a client of either transport.
type member interface {
	write(b []byte)
	read(d time.Duration) []byte // nil on timeout
	octet() byte
	vaddr() []byte
}

// ---- UDP client

type udpC struct {
	t     *testing.T
	conn  *net.UDPConn
	srv   *net.UDPAddr
	room  roomReply
	nonce uint64
}

func dialUDP(t *testing.T, srv *net.UDPAddr) *udpC {
	t.Helper()
	conn, err := net.DialUDP("udp", nil, srv)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &udpC{t: t, conn: conn, srv: srv}
}

func (c *udpC) write(b []byte) {
	if _, err := c.conn.Write(b); err != nil {
		c.t.Fatal(err)
	}
}

func (c *udpC) read(d time.Duration) []byte {
	_ = c.conn.SetReadDeadline(time.Now().Add(d))
	buf := make([]byte, 2048)
	n, err := c.conn.Read(buf)
	if err != nil {
		return nil
	}
	return buf[:n]
}

func (c *udpC) octet() byte   { return c.room.octet }
func (c *udpC) vaddr() []byte { return []byte{10, 0, 0, c.room.octet} }

var nonceCounter atomic.Uint64

func (c *udpC) handshake(code string) *udpC {
	c.t.Helper()
	c.nonce = nonceCounter.Add(1) + 1<<40
	c.write(handshakeFrame(c.nonce, code, MinUDPHandshake))
	b := c.read(waitLimit)
	if b == nil {
		c.t.Fatal("no reply to handshake")
	}
	c.room = parseRoom(c.t, b)
	return c
}

func (c *udpC) create() *udpC          { return c.handshake("") }
func (c *udpC) join(code string) *udpC { return c.handshake(code) }

func (c *udpC) dataFrame(dst []byte, dport, sport uint16, payload []byte) []byte {
	b := []byte{TypeData}
	b = append(b, c.room.token[:]...)
	b = append(b, dst...)
	b = append(b, byte(dport>>8), byte(dport), byte(sport>>8), byte(sport))
	return append(b, payload...)
}

// ---- WebSocket client

type wsC struct {
	t     *testing.T
	conn  *websocket.Conn
	room  roomReply
	nonce uint64
}

func dialWS(t *testing.T, url string) *wsC {
	t.Helper()
	conn, resp, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	_ = resp
	t.Cleanup(func() { conn.Close() })
	return &wsC{t: t, conn: conn}
}

func (c *wsC) write(b []byte) {
	if err := c.conn.WriteMessage(websocket.BinaryMessage, b); err != nil {
		c.t.Fatal(err)
	}
}

func (c *wsC) read(d time.Duration) []byte {
	_ = c.conn.SetReadDeadline(time.Now().Add(d))
	typ, b, err := c.conn.ReadMessage()
	if err != nil || typ != websocket.BinaryMessage {
		return nil
	}
	return b
}

func (c *wsC) octet() byte   { return c.room.octet }
func (c *wsC) vaddr() []byte { return []byte{10, 0, 0, c.room.octet} }

// wsHandshake frames have no padding.
func (c *wsC) handshake(code string) *wsC {
	c.t.Helper()
	c.nonce = nonceCounter.Add(1) + 1<<41
	c.write(handshakeFrame(c.nonce, code, 0))
	b := c.read(waitLimit)
	if b == nil {
		c.t.Fatal("no reply to handshake")
	}
	c.room = parseRoom(c.t, b)
	return c
}
func (c *wsC) create() *wsC          { return c.handshake("") }
func (c *wsC) join(code string) *wsC { return c.handshake(code) }

func (c *wsC) dataFrame(dst []byte, dport, sport uint16, payload []byte) []byte {
	b := []byte{TypeData}
	b = append(b, dst...)
	b = append(b, byte(dport>>8), byte(dport), byte(sport>>8), byte(sport))
	return append(b, payload...)
}

// ---- helpers over both

type framer interface {
	dataFrame(dst []byte, dport, sport uint16, payload []byte) []byte
}

func send(from member, to member, dport, sport uint16, payload string) {
	from.write(from.(framer).dataFrame(to.vaddr(), dport, sport, []byte(payload)))
}

func expectData(t *testing.T, m member, wantSrc member, wantDport, wantSport uint16, wantPayload string) {
	t.Helper()
	b := m.read(waitLimit)
	if b == nil {
		t.Fatalf("timed out waiting for %q", wantPayload)
	}
	f := parseFwd(t, b)
	if f.srcOctet != wantSrc.octet() || f.dport != wantDport || f.sport != wantSport || string(f.payload) != wantPayload {
		t.Fatalf("got %+v, want src=%d dport=%d sport=%d %q", f, wantSrc.octet(), wantDport, wantSport, wantPayload)
	}
}

// pending sends a keepalive as m and returns every frame received before the
// reply: what the relay had already delivered to m. The relay handles the
// frames of one transport in arrival order, so this covers everything sent
// earlier over the same transport (UDP is one stream, each WebSocket another);
// to cover another sender, call pending on that sender first.
func pending(t *testing.T, m member) [][]byte {
	t.Helper()
	switch c := m.(type) {
	case *udpC:
		c.write(append([]byte{TypeKeepalive}, c.room.token[:]...))
	case *wsC:
		c.write([]byte{TypeKeepalive})
	}
	var out [][]byte
	for {
		b := m.read(waitLimit)
		if b == nil {
			t.Fatal("no keepalive reply")
		}
		if len(b) == 1 && b[0] == TypeKeepalive {
			return out
		}
		out = append(out, b)
	}
}

func expectSilence(t *testing.T, m member) {
	t.Helper()
	if got := pending(t, m); len(got) != 0 {
		t.Fatalf("member 10.0.0.%d received %d unexpected frame(s), first: % x", m.octet(), len(got), got[0])
	}
}

// ------------------------------------------------------------------ tests

func TestUDPEndToEnd(t *testing.T) {
	e := newEnv(t, nil)
	host := dialUDP(t, e.udp).create()
	guest := dialUDP(t, e.udp).join(host.room.code)
	if host.octet() != 1 || guest.octet() != 2 {
		t.Fatalf("vaddrs %d %d", host.octet(), guest.octet())
	}
	for i := 0; i < 50; i++ {
		send(host, guest, 9000, 9001, fmt.Sprintf("h%d", i))
		send(guest, host, 9001, 9000, fmt.Sprintf("g%d", i))
	}
	for i := 0; i < 50; i++ {
		expectData(t, guest, host, 9000, 9001, fmt.Sprintf("h%d", i))
		expectData(t, host, guest, 9001, 9000, fmt.Sprintf("g%d", i))
	}
}

func TestWebSocketEndToEnd(t *testing.T) {
	e := newEnv(t, nil)
	host := dialWS(t, e.wsURL).create()
	guest := dialWS(t, e.wsURL).join(strings.ToLower(host.room.code))
	if host.octet() != 1 || guest.octet() != 2 || guest.room.code != host.room.code {
		t.Fatalf("room %+v / %+v", host.room, guest.room)
	}
	for i := 0; i < 50; i++ {
		send(host, guest, 1, 2, fmt.Sprintf("h%d", i))
		send(guest, host, 2, 1, fmt.Sprintf("g%d", i))
	}
	for i := 0; i < 50; i++ {
		expectData(t, guest, host, 1, 2, fmt.Sprintf("h%d", i))
		expectData(t, host, guest, 2, 1, fmt.Sprintf("g%d", i))
	}
}

func TestBrowserAndNativeShareARoom(t *testing.T) {
	e := newEnv(t, nil)
	native := dialUDP(t, e.udp).create()                 // the host is a native client
	browser := dialWS(t, e.wsURL).join(native.room.code) // the joiner is a "browser"
	third := dialUDP(t, e.udp).join(native.room.code)
	for i := 0; i < 20; i++ {
		send(browser, native, 5000, 5001, fmt.Sprintf("b2n-%d", i))
		send(native, browser, 5001, 5000, fmt.Sprintf("n2b-%d", i))
		send(third, browser, 7, 8, fmt.Sprintf("t2b-%d", i))
	}
	for i := 0; i < 20; i++ {
		expectData(t, native, browser, 5000, 5001, fmt.Sprintf("b2n-%d", i))
		expectData(t, browser, native, 5001, 5000, fmt.Sprintf("n2b-%d", i))
		expectData(t, browser, third, 7, 8, fmt.Sprintf("t2b-%d", i))
	}
	expectSilence(t, third)
}

func TestSocketsRouteOnlyWithinTheRoom(t *testing.T) {
	e := newEnv(t, nil)
	a1 := dialUDP(t, e.udp).create()
	a2 := dialWS(t, e.wsURL).join(a1.room.code)
	b1 := dialWS(t, e.wsURL).create()
	b2 := dialUDP(t, e.udp).join(b1.room.code)
	// Both rooms use the same vaddrs 10.0.0.1 and 10.0.0.2.
	if a2.octet() != 2 || b2.octet() != 2 {
		t.Fatal("setup")
	}
	send(a1, a2, 1, 1, "a1->a2")
	send(b1, b2, 1, 1, "b1->b2")
	// a1 also probes every address 10.0.0.1..19.
	for o := byte(1); o < 20; o++ {
		a1.write(a1.dataFrame([]byte{10, 0, 0, o}, 3, 3, []byte("probe")))
	}
	expectData(t, a2, a1, 1, 1, "a1->a2")
	expectData(t, b2, b1, 1, 1, "b1->b2")
	// Of the probes only those for 10.0.0.1 (a1 itself) and 10.0.0.2 (a2)
	// are deliverable, and only inside room A. a1 syncs first: its own
	// transport handles frames in order, so after its keepalive reply every
	// probe has been routed and queued for its destination.
	if got := pending(t, a1); len(got) != 1 {
		t.Fatalf("a1 (self probe) saw %d frames", len(got))
	}
	got := pending(t, a2)
	if len(got) != 1 || string(parseFwd(t, got[0]).payload) != "probe" {
		t.Fatalf("a2 saw %d frames", len(got))
	}
	expectSilence(t, b1)
	if got := pending(t, b2); len(got) != 0 {
		t.Fatalf("room B saw room A's probes: %d frames", len(got))
	}
}

func TestWebSocketTokenIsNotValidOverUDP(t *testing.T) {
	e := newEnv(t, nil)
	ws := dialWS(t, e.wsURL).create()
	u := dialUDP(t, e.udp)
	u.room.token = ws.room.token
	u.write(append([]byte{TypeKeepalive}, ws.room.token[:]...))
	b := u.read(waitLimit)
	if b == nil || errReason(t, b) != ErrNoSession {
		t.Fatalf("a WebSocket session's token worked over UDP: % x", b)
	}
}

func TestWebSocketHandshakeIsIdempotent(t *testing.T) {
	e := newEnv(t, nil)
	c := dialWS(t, e.wsURL)
	c.nonce = 123
	c.write(handshakeFrame(123, "", 0))
	first := c.read(waitLimit)
	c.write(handshakeFrame(123, "", 0))
	second := c.read(waitLimit)
	if first == nil || !bytes.Equal(first, second) {
		t.Fatalf("retry on the same connection differs:\n% x\n% x", first, second)
	}
	if e.srv.Stats().Rooms != 1 {
		t.Fatal("two rooms")
	}
	// A second, different request on a connection that is in a room is refused.
	c.write(handshakeFrame(124, "", 0))
	b := c.read(waitLimit)
	if b == nil || errReason(t, b) != ErrAlreadyInRoom {
		t.Fatalf("got % x", b)
	}
	if e.srv.Stats().Rooms != 1 {
		t.Fatal("second room created")
	}
	// Another connection cannot take over this session by repeating the nonce.
	d := dialWS(t, e.wsURL)
	d.write(handshakeFrame(123, "", 0))
	if b := d.read(waitLimit); b == nil || errReason(t, b) != ErrNonceReused {
		t.Fatalf("got % x", b)
	}
}

func TestWebSocketMalformedFirstFrameCloses(t *testing.T) {
	e := newEnv(t, nil)
	for name, frame := range map[string][]byte{
		"data before a room": append([]byte{TypeData}, make([]byte, 20)...),
		"keepalive first":    {TypeKeepalive},
		"unknown type":       {0x55, 1, 2, 3},
		"short create":       {TypeCreate, ProtocolVersion, 1, 2},
		"short join":         {TypeJoin, ProtocolVersion, 1, 2, 3, 4, 5, 6, 7, 8, 'A'},
	} {
		c := dialWS(t, e.wsURL)
		c.write(frame)
		b := c.read(waitLimit)
		if b == nil || errReason(t, b) != ErrMalformed {
			t.Errorf("%s: got % x", name, b)
			continue
		}
		if c.read(waitLimit) != nil {
			t.Errorf("%s: connection stayed open", name)
		}
	}
	if st := e.srv.Stats(); st.Sessions != 0 || st.Rooms != 0 {
		t.Fatalf("%+v", st)
	}
	// A text frame is not a vnet frame either.
	c := dialWS(t, e.wsURL)
	_ = c.conn.WriteMessage(websocket.TextMessage, []byte("hello"))
	if b := c.read(waitLimit); b == nil || errReason(t, b) != ErrMalformed {
		t.Fatalf("text frame: % x", b)
	}
}

func TestWebSocketMalformedFramesInARoomAreIgnored(t *testing.T) {
	e := newEnv(t, nil)
	host := dialWS(t, e.wsURL).create()
	guest := dialUDP(t, e.udp).join(host.room.code)
	for _, f := range [][]byte{
		{0x77}, {TypeData}, {TypeData, 10, 0, 0, 2, 0, 1, 0},
		append([]byte{TypeData, 10, 0, 0, 2, 0, 1, 0, 1}, make([]byte, MaxPayload+1)...),
		{TypeRoom, 1, 2, 3}, {TypeError},
	} {
		host.write(f)
	}
	_ = host.conn.WriteMessage(websocket.TextMessage, []byte("text"))
	send(host, guest, 1, 2, "still-works")
	expectData(t, guest, host, 1, 2, "still-works")
	if e.srv.Stats().Malformed < 6 {
		t.Fatalf("malformed = %d", e.srv.Stats().Malformed)
	}
}

func TestWebSocketOversizeMessageClosesTheConnection(t *testing.T) {
	e := newEnv(t, nil)
	host := dialWS(t, e.wsURL).create()
	host.write(make([]byte, maxWSMessage+100))
	if b := host.read(waitLimit); b != nil {
		t.Fatalf("unexpected frame % x", b[:min(len(b), 8)])
	}
	waitFor(t, "member removed", func() bool { return e.srv.Stats().Sessions == 0 })
}

func TestWebSocketHandshakeTimeout(t *testing.T) {
	e := newEnv(t, nil)
	c := dialWS(t, e.wsURL)
	waitFor(t, "connection registered", func() bool { return e.srv.Stats().WebSockets == 1 })
	e.srv.Sweep()
	if e.srv.Stats().WebSockets != 1 {
		t.Fatal("closed too early")
	}
	e.clock.Advance(e.srv.cfg.HandshakeTimeout + time.Second)
	e.srv.Sweep()
	// The server closed the connection: the client sees a close, not a timeout.
	_ = c.conn.SetReadDeadline(time.Now().Add(waitLimit))
	if _, _, err := c.conn.ReadMessage(); err == nil || isTimeout(err) {
		t.Fatalf("connection should have been closed by the relay, err=%v", err)
	}
	waitFor(t, "connection removed", func() bool { return e.srv.Stats().WebSockets == 0 })
}

func TestWebSocketDisconnectFreesTheMember(t *testing.T) {
	e := newEnv(t, nil)
	host := dialUDP(t, e.udp).create()
	g := dialWS(t, e.wsURL).join(host.room.code)
	if g.octet() != 2 {
		t.Fatal("setup")
	}
	g.conn.Close()
	waitFor(t, "member removed", func() bool { return e.srv.Stats().Sessions == 1 })
	g2 := dialWS(t, e.wsURL).join(host.room.code)
	if g2.octet() != 2 {
		t.Fatalf("address not freed: %d", g2.octet())
	}
	// An explicit LEAVE does the same and closes the connection.
	g2.write([]byte{TypeLeave})
	if b := g2.read(waitLimit); b != nil {
		t.Fatalf("unexpected frame after LEAVE: % x", b)
	}
	waitFor(t, "LEAVE processed", func() bool { return e.srv.Stats().Sessions == 1 })
}

func TestWebSocketSessionTimeoutAndPongs(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.PingInterval = 10 * time.Millisecond })
	c := dialWS(t, e.wsURL).create()
	sess := e.srv.hub.byToken[c.room.token]

	// The client answers the relay's pings only while `answer` is set. A
	// reader goroutine runs the library's control-frame handling.
	var answer atomic.Bool
	answer.Store(true)
	c.conn.SetPingHandler(func(data string) error {
		if answer.Load() {
			return c.conn.WriteControl(websocket.PongMessage, []byte(data), time.Now().Add(time.Second))
		}
		return nil
	})
	closed := make(chan error, 1)
	go func() {
		for {
			if _, _, err := c.conn.ReadMessage(); err != nil {
				closed <- err
				return
			}
		}
	}()

	// Each pong refreshes the session (on the fake clock).
	e.clock.Advance(10 * time.Second)
	waitFor(t, "a pong refreshed the session", func() bool { return sess.seen().Equal(e.clock.Now()) })
	e.clock.Advance(10 * time.Second) // 20 s after the handshake; pongs keep coming
	waitFor(t, "a pong after the second advance", func() bool { return sess.seen().Equal(e.clock.Now()) })
	e.srv.Sweep()
	if e.srv.Stats().Sessions != 1 {
		t.Fatal("member answering pings was expired")
	}

	// Stop answering: once the timeout has passed the relay drops the member
	// and closes the connection.
	answer.Store(false)
	// A pong that was already in flight can refresh the member once more, so
	// keep moving the clock until the relay has seen nothing for the whole
	// timeout; this converges after at most a couple of rounds.
	for i := 0; i < 10 && e.srv.Stats().Sessions != 0; i++ {
		e.clock.Advance(e.srv.cfg.SessionTimeout + time.Second)
		e.srv.Sweep()
	}
	if e.srv.Stats().Sessions != 0 {
		t.Fatal("silent WebSocket member not expired")
	}
	select {
	case err := <-closed:
		if isTimeout(err) {
			t.Fatalf("unexpected timeout: %v", err)
		}
	case <-time.After(waitLimit):
		t.Fatal("relay did not close the expired connection")
	}
}

func TestWebSocketSlowClientLosesFramesNotTheRelay(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.WSSendQueue = 4 })
	c := &wsConn{hub: e.srv.hub, out: make(chan []byte, 4), done: make(chan struct{})}
	for i := 0; i < 10; i++ {
		c.enqueue([]byte{byte(i)})
	}
	if len(c.out) != 4 || e.srv.Stats().WSQueueDrops != 6 {
		t.Fatalf("queued %d, dropped %d", len(c.out), e.srv.Stats().WSQueueDrops)
	}
}

func TestWebSocketOriginCheck(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.AllowedOrigins = []string{"game.example.org"} })
	try := func(origin string) bool {
		h := http.Header{}
		if origin != "" {
			h.Set("Origin", origin)
		}
		conn, _, err := websocket.DefaultDialer.Dial(e.wsURL, h)
		if err == nil {
			conn.Close()
		}
		return err == nil
	}
	if !try("https://game.example.org") {
		t.Error("allowed origin refused")
	}
	if try("https://evil.example.net") {
		t.Error("foreign origin accepted")
	}
	if !try("") {
		t.Error("non-browser client (no Origin) refused")
	}
}

func TestWebSocketConnectionCap(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.MaxSessions = 2 })
	dialWS(t, e.wsURL)
	dialWS(t, e.wsURL)
	waitFor(t, "two connections registered", func() bool { return e.srv.Stats().WebSockets == 2 })
	_, resp, err := websocket.DefaultDialer.Dial(e.wsURL, nil)
	if err == nil || resp == nil || resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("third connection: err=%v resp=%v", err, resp)
	}
}

func TestTimeoutAndRoomIdleOverRealSockets(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.RoomIdleTimeout = time.Minute })
	host := dialUDP(t, e.udp).create()
	_ = dialWS(t, e.wsURL).join(host.room.code)
	for i := 0; i < 2; i++ {
		e.clock.Advance(40 * time.Second)
		host.write(append([]byte{TypeKeepalive}, host.room.token[:]...))
		if host.read(waitLimit) == nil { // the reply proves the relay handled it
			t.Fatal("no keepalive reply")
		}
	}
	e.srv.Sweep() // guest silent for 80 s: gone; room idle for 80 s: closed
	if st := e.srv.Stats(); st.Rooms != 0 || st.Sessions != 0 {
		t.Fatalf("%+v", st)
	}
}

func TestStatsAndHealth(t *testing.T) {
	e := newEnv(t, nil)
	dialUDP(t, e.udp).create()
	resp, err := http.Get(e.http.URL + "/stats")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var buf bytes.Buffer
	_, _ = buf.ReadFrom(resp.Body)
	if !strings.Contains(buf.String(), `"rooms":1`) {
		t.Fatalf("stats: %s", buf.String())
	}
	resp2, err := http.Get(e.http.URL + "/healthz")
	if err != nil || resp2.StatusCode != 200 {
		t.Fatal("healthz")
	}
	resp2.Body.Close()
}

// Many rooms at once, both transports, under the race detector.
func TestManyRoomsConcurrently(t *testing.T) {
	e := newEnv(t, func(c *Config) {
		c.WSSendQueue = 4096 // the test sends in bursts
		c.ClientPPS, c.ClientPPSBurst = 1e6, 1e6
		c.ClientBPS, c.ClientBPSBurst = 1e9, 1e9
		c.HandshakePerSec, c.HandshakeBurst = 1e6, 1e6
		c.ReplyPerSec, c.ReplyBurst = 1e6, 1e6
		c.GlobalReplyPerSec, c.GlobalReplyBurst = 1e6, 1e6
	})
	const rooms, perRoom, msgs = 12, 4, 40
	var wg sync.WaitGroup
	errs := make(chan string, rooms*perRoom)
	for r := 0; r < rooms; r++ {
		wg.Add(1)
		go func(r int) {
			defer wg.Done()
			var ms []member
			host := dialUDP(t, e.udp).create()
			ms = append(ms, host)
			for i := 1; i < perRoom; i++ {
				if i%2 == 0 {
					ms = append(ms, dialWS(t, e.wsURL).join(host.room.code))
				} else {
					ms = append(ms, dialUDP(t, e.udp).join(host.room.code))
				}
			}
			// Everybody sends msgs datagrams to everybody (self included).
			for i, from := range ms {
				for _, to := range ms {
					for k := 0; k < msgs; k++ {
						send(from, to, 1, 2, fmt.Sprintf("r%d-m%d-k%d", r, i, k))
					}
				}
			}
			// Every member must receive exactly perRoom*msgs frames, all
			// from its own room, in order per sender.
			for _, m := range ms {
				next := make(map[byte]int)
				for n := 0; n < perRoom*msgs; n++ {
					b := m.read(waitLimit)
					if b == nil {
						errs <- fmt.Sprintf("room %d member %d: only %d of %d frames", r, m.octet(), n, perRoom*msgs)
						return
					}
					f := parseFwd(t, b)
					var rr, mm, kk int
					if _, err := fmt.Sscanf(string(f.payload), "r%d-m%d-k%d", &rr, &mm, &kk); err != nil || rr != r || byte(mm+1) != f.srcOctet {
						errs <- fmt.Sprintf("room %d member %d got foreign or mislabeled frame %q from %d", r, m.octet(), f.payload, f.srcOctet)
						return
					}
					if kk != next[f.srcOctet] {
						errs <- fmt.Sprintf("room %d member %d: out of order from %d: %d != %d", r, m.octet(), f.srcOctet, kk, next[f.srcOctet])
						return
					}
					next[f.srcOctet]++
				}
			}
		}(r)
	}
	wg.Wait()
	close(errs)
	for s := range errs {
		t.Error(s)
	}
	if st := e.srv.Stats(); st.Rooms != rooms || st.Unroutable != 0 {
		t.Fatalf("%+v", st)
	}
}

// ---- small helpers

func isTimeout(err error) bool {
	ne, ok := err.(net.Error)
	return ok && ne.Timeout()
}

// waitFor polls cond until it holds or the deadline passes.
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(waitLimit)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for: %s", what)
		}
		time.Sleep(time.Millisecond)
	}
}
