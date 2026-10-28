// SPDX-License-Identifier: MIT

package relay

import (
	"context"
	"crypto/tls"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/http"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"

	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/webtransport-go"
)

// WebTransport carries the same frames as the other transports (see
// doco/relay-protocol.md, "WebTransport"):
//
//   - the client opens ONE bidirectional stream and sends every control frame
//     (CREATE_ROOM, JOIN_ROOM, LEAVE, KEEPALIVE) on it, each preceded by a
//     2-byte big-endian length; the relay answers on the same stream
//     (ROOM, ERROR, KEEPALIVE) in the same way;
//   - DATA travels in datagrams, one frame per datagram, in the WebSocket
//     layout (no token: the QUIC connection is the identity).
//
// A QUIC connection holds at most one session, and no stream other than that
// one control stream is ever read.

const (
	wtPath = "/vnet"

	// maxCtlFrame is the longest control frame accepted on the control stream
	// (the longest real one is 16 bytes; the relay's own longest is 38).
	maxCtlFrame = 64

	wtCtlQueue = 32 // control frames queued per session; a client that lets this overflow is closed

	// maxBadStreams is how many surplus streams a session may open before it
	// is closed for abuse.
	maxBadStreams = 16

	// Application error codes of a closing session.
	wtCloseNormal   = 0
	wtCloseProtocol = 1
	wtCloseOverload = 2

	wtStreamRefused webtransport.StreamErrorCode = 1
)

var errWTDatagramTooLarge = errors.New("datagram too large for the connection")

// wtStream, wtUniStream and wtSession are what the relay needs of a
// WebTransport session. *webtransport.Stream satisfies wtStream; the session
// is adapted (realWTSession) so that tests can stand in for it, and so that a
// stuck datagram write can be simulated.
type wtStream interface {
	io.Reader
	io.Writer
	SetReadDeadline(time.Time) error
	SetWriteDeadline(time.Time) error
	CancelRead(webtransport.StreamErrorCode)
	CancelWrite(webtransport.StreamErrorCode)
}

type wtUniStream interface {
	CancelRead(webtransport.StreamErrorCode)
}

type wtSession interface {
	AcceptStream(ctx context.Context) (wtStream, error)
	AcceptUniStream(ctx context.Context) (wtUniStream, error)
	ReceiveDatagram(ctx context.Context) ([]byte, error)
	SendDatagram(b []byte) error // may block while the connection's send queue is full
	CloseWithError(code uint32, msg string) error
	Context() context.Context // done when the session (its QUIC connection) is gone
}

type realWTSession struct{ s *webtransport.Session }

func (r realWTSession) AcceptStream(ctx context.Context) (wtStream, error) {
	st, err := r.s.AcceptStream(ctx)
	if err != nil {
		return nil, err
	}
	return st, nil
}

func (r realWTSession) AcceptUniStream(ctx context.Context) (wtUniStream, error) {
	st, err := r.s.AcceptUniStream(ctx)
	if err != nil {
		return nil, err
	}
	return st, nil
}

func (r realWTSession) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	return r.s.ReceiveDatagram(ctx)
}

func (r realWTSession) SendDatagram(b []byte) error {
	err := r.s.SendDatagram(b)
	var tooLarge *quic.DatagramTooLargeError
	if errors.As(err, &tooLarge) {
		return errWTDatagramTooLarge
	}
	return err
}

func (r realWTSession) CloseWithError(code uint32, msg string) error {
	return r.s.CloseWithError(webtransport.SessionErrorCode(code), msg)
}

func (r realWTSession) Context() context.Context { return r.s.Context() }

// wtConn is one WebTransport client: a QUIC connection from admission to its
// end, and the session on it once there is one.
//
// Writes: datagrams are sent only by datagramWriter (SendDatagram can block
// while the connection's queue is full, so nothing else may wait on it), and
// control-stream writes go through writeCtl, which holds wmu, because both the
// control writer and reject() write there (the lesson of the WebSocket
// writer panic in brief 10).
type wtConn struct {
	connState
	hub      *Hub
	ip       netip.Addr // as seen (before ipKey)
	accepted time.Time
	sess     atomic.Pointer[session]

	mu      sync.Mutex
	ws      wtSession // the upgraded session; nil until then
	abort   func()    // closes the QUIC connection; nil until it has been accepted
	started bool      // a session has been taken on this connection
	stream  wtStream  // the control stream; nil until the client has opened it

	ctl    chan []byte // control frames to the client (bounded)
	out    chan []byte // DATA frames to the client (bounded in frames and bytes)
	qbytes atomic.Int64
	done   chan struct{}

	closeOnce sync.Once
	goneOnce  sync.Once
	badStream atomic.Int32
	wmu       sync.Mutex // serialises writes to the control stream
}

func newWTConn(h *Hub, ip netip.Addr) *wtConn {
	c := &wtConn{
		hub:      h,
		ip:       ip,
		accepted: h.now(),
		ctl:      make(chan []byte, wtCtlQueue),
		out:      make(chan []byte, h.cfg.WTSendQueue),
		done:     make(chan struct{}),
	}
	c.pendingHeld.Store(true)
	return c
}

func (c *wtConn) kind() connKind                  { return kindWT }
func (c *wtConn) clientIP() netip.Addr            { return c.ip }
func (c *wtConn) acceptedAt() time.Time           { return c.accepted }
func (c *wtConn) getSession() *session            { return c.sess.Load() }
func (c *wtConn) swapSession(s *session) *session { return c.sess.Swap(s) }

func (c *wtConn) transportLive() bool {
	select {
	case <-c.done:
		return false
	default:
	}
	c.mu.Lock()
	ws := c.ws
	c.mu.Unlock()
	return ws != nil && ws.Context().Err() == nil
}

func (c *wtConn) touch(now time.Time) {
	if s := c.sess.Load(); s != nil {
		s.touch(now)
	}
}

func (c *wtConn) closed() bool {
	select {
	case <-c.done:
		return true
	default:
		return false
	}
}

// start claims the connection's one session.
func (c *wtConn) start() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.started {
		return false
	}
	c.started = true
	return true
}

// attachQUIC records how to close the QUIC connection.
func (c *wtConn) attachQUIC(abort func()) {
	c.mu.Lock()
	c.abort = abort
	c.mu.Unlock()
	if c.closed() {
		c.terminate()
	}
}

func (c *wtConn) attachSession(ws wtSession) {
	c.mu.Lock()
	c.ws = ws
	c.mu.Unlock()
	if c.closed() {
		c.terminate()
	}
}

func (c *wtConn) setStream(st wtStream) {
	c.mu.Lock()
	c.stream = st
	c.mu.Unlock()
}

func (c *wtConn) getStream() wtStream {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.stream
}

// terminate closes the session and the QUIC connection, without blocking the
// caller: a stuck peer cannot stall whoever is closing (the sweeper closes many
// connections in a row, and the hub calls close with its lock held).
func (c *wtConn) terminate() {
	c.mu.Lock()
	ws, abort := c.ws, c.abort
	c.mu.Unlock()
	if ws == nil && abort == nil {
		return
	}
	go func() {
		defer c.hub.recoverPanic("webtransport close")
		if ws != nil {
			_ = ws.CloseWithError(wtCloseNormal, "")
		}
		if abort != nil {
			abort()
		}
	}()
}

func (c *wtConn) close() {
	c.closeOnce.Do(func() {
		close(c.done)
		c.terminate()
	})
}

// enqueue queues a frame for the client: DATA as a datagram (dropped when the
// client cannot keep up, like a lossy network), everything else on the control
// stream (reliable, so a client that cannot keep up is closed instead).
func (c *wtConn) enqueue(b []byte) {
	if len(b) == 0 {
		return
	}
	h := c.hub
	if b[0] == TypeData {
		n := int64(len(b))
		if c.qbytes.Add(n) > int64(h.cfg.WTSendQueueBytes) {
			c.qbytes.Add(-n)
			h.stats.wtDatagramDrops.Add(1)
			return
		}
		select {
		case c.out <- b:
		default:
			c.qbytes.Add(-n)
			h.stats.wtDatagramDrops.Add(1)
		}
		return
	}
	select {
	case c.ctl <- b:
	default:
		h.stats.wtCtlOverflow.Add(1)
		c.close()
	}
}

// writeCtl writes one length-prefixed control frame, serialised with every
// other write to the control stream and bounded by a deadline.
func (c *wtConn) writeCtl(b []byte) error {
	st := c.getStream()
	if st == nil {
		return errors.New("no control stream")
	}
	buf := make([]byte, 2+len(b))
	binary.BigEndian.PutUint16(buf, uint16(len(b)))
	copy(buf[2:], b)
	c.wmu.Lock()
	defer c.wmu.Unlock()
	_ = st.SetWriteDeadline(time.Now().Add(c.hub.cfg.WTWriteTimeout))
	_, err := st.Write(buf)
	return err
}

// reject sends an ERROR and closes the session.
func (c *wtConn) reject(reason byte) {
	var zero [NonceLen]byte
	_ = c.writeCtl(encodeError(reason, zero))
	c.close()
}

func (c *wtConn) controlWriter() {
	defer c.hub.recoverPanic("webtransport control writer")
	for {
		select {
		case b := <-c.ctl:
			if err := c.writeCtl(b); err != nil {
				c.close()
				return
			}
		case <-c.done:
			return
		}
	}
}

func (c *wtConn) datagramWriter(ws wtSession) {
	defer c.hub.recoverPanic("webtransport datagram writer")
	for {
		select {
		case b := <-c.out:
			c.qbytes.Add(-int64(len(b)))
			if err := ws.SendDatagram(b); err != nil {
				if errors.Is(err, errWTDatagramTooLarge) {
					c.hub.stats.wtDatagramDrops.Add(1)
					continue
				}
				c.close()
				return
			}
		case <-c.done:
			return
		}
	}
}

// datagramReader forwards the client's DATA datagrams.
func (c *wtConn) datagramReader(ws wtSession) {
	h := c.hub
	defer h.recoverPanic("webtransport datagram reader")
	defer c.close()
	ctx := ws.Context()
	scratch := make([]byte, 0, maxUDPFrame)
	for {
		msg, err := ws.ReceiveDatagram(ctx)
		if err != nil {
			return
		}
		h.stats.rxFrames.Add(1)
		now := h.now()
		s := c.sess.Load()
		if s == nil { // DATA before the room: the client has no address yet
			h.stats.wtEarlyDatagrams.Add(1)
			continue
		}
		if len(msg) == 0 || msg[0] != TypeData { // control frames belong on the stream
			h.stats.malformed.Add(1)
			continue
		}
		d, ok := parseData(msg, false)
		if !ok {
			h.stats.malformed.Add(1)
			continue
		}
		s.touch(now)
		h.forwardLocked(s, &d, len(msg), now, &scratch)
	}
}

// refuseStreams resets every stream the client opens after the control stream:
// the relay reads no other stream, and a client that keeps opening them is
// closed.
func (c *wtConn) refuseStreams(ws wtSession) {
	h := c.hub
	ctx := ws.Context()
	var wg sync.WaitGroup
	wg.Add(2)
	bad := func() {
		h.stats.wtBadStreams.Add(1)
		if c.badStream.Add(1) > maxBadStreams {
			c.hub.stats.malformed.Add(1)
			go func() { _ = ws.CloseWithError(wtCloseProtocol, "too many streams"); c.close() }()
		}
	}
	go func() {
		defer wg.Done()
		defer h.recoverPanic("webtransport stream refuser")
		for {
			st, err := ws.AcceptStream(ctx)
			if err != nil {
				return
			}
			st.CancelRead(wtStreamRefused)
			st.CancelWrite(wtStreamRefused)
			bad()
		}
	}()
	go func() {
		defer wg.Done()
		defer h.recoverPanic("webtransport stream refuser")
		for {
			st, err := ws.AcceptUniStream(ctx)
			if err != nil {
				return
			}
			st.CancelRead(wtStreamRefused)
			bad()
		}
	}()
	wg.Wait()
}

// controlReader reads length-prefixed control frames from the control stream
// until the session ends.
func (c *wtConn) controlReader(st wtStream) {
	h := c.hub
	var hdr [2]byte
	buf := make([]byte, maxCtlFrame)
	// Until the client has entered a room it has HandshakeTimeout (in real
	// time: this is a socket deadline, not the hub's clock) to say so. The
	// sweeper enforces the same limit on the hub's clock.
	deadline := time.Now().Add(h.cfg.HandshakeTimeout)
	pending := true
	for {
		if pending {
			if c.sess.Load() == nil {
				_ = st.SetReadDeadline(deadline)
			} else {
				pending = false
				_ = st.SetReadDeadline(time.Time{})
			}
		}
		if _, err := io.ReadFull(st, hdr[:]); err != nil {
			return
		}
		n := int(binary.BigEndian.Uint16(hdr[:]))
		if n == 0 || n > maxCtlFrame { // the stream cannot be resynchronised
			h.stats.malformed.Add(1)
			c.reject(ErrMalformed)
			return
		}
		if _, err := io.ReadFull(st, buf[:n]); err != nil {
			return
		}
		now := h.now()
		h.stats.rxFrames.Add(1)
		c.touch(now)
		if !h.wtControl(c, buf[:n], now) {
			return
		}
	}
}

// wtControl processes one control frame; false means close the session.
func (h *Hub) wtControl(c *wtConn, msg []byte, now time.Time) bool {
	s := c.sess.Load()
	switch msg[0] {
	case TypeCreate, TypeJoin:
		return h.connHandshake(c, msg, now)
	case TypeLeave:
		if s != nil {
			h.leave(s)
		}
		return false
	}
	if s == nil { // everything else needs a session
		h.stats.malformed.Add(1)
		c.reject(ErrMalformed)
		return false
	}
	if msg[0] == TypeKeepalive {
		h.keepalive(s, len(msg), now)
		return true
	}
	h.stats.malformed.Add(1) // DATA belongs in a datagram; anything else is unknown
	return true
}

// serveWTSession runs an upgraded session until it ends. c was admitted with
// wtAdmit. It returns when the session is over; the session's membership is
// then gone.
func (h *Hub) serveWTSession(c *wtConn, ws wtSession) {
	defer h.recoverPanic("webtransport session")
	defer func() {
		c.close()
		func() {
			h.mu.Lock()
			defer h.mu.Unlock()
			if s := c.sess.Load(); s != nil {
				h.removeLocked(s, h.now())
			}
		}()
	}()
	c.attachSession(ws)

	// The client opens the control stream (it is not visible to us until the
	// client has written to it) and says CREATE_ROOM or JOIN_ROOM on it.
	actx, cancel := context.WithTimeout(ws.Context(), h.cfg.HandshakeTimeout)
	st, err := ws.AcceptStream(actx)
	cancel()
	if err != nil || c.closed() {
		return
	}
	c.setStream(st)
	go c.refuseStreams(ws)
	go c.controlWriter()
	go c.datagramWriter(ws)
	go c.datagramReader(ws)
	c.controlReader(st)
}

// wtAdmit admits a new WebTransport (QUIC) connection from ip, or refuses it
// because a limit is reached. Whatever is admitted must be given back with
// wtGone.
func (h *Hub) wtAdmit(ip netip.Addr) (*wtConn, bool) {
	if !h.acquireConn(kindWT, ipKey(ip)) {
		h.stats.wtRefused.Add(1)
		return nil, false
	}
	c := newWTConn(h, ip)
	h.mu.Lock()
	defer h.mu.Unlock()
	h.conns[c] = struct{}{}
	return c, true
}

// wtGone ends a WebTransport connection's life at the relay: the member leaves
// its room and the per-address counters are given back, exactly once.
func (h *Hub) wtGone(c *wtConn) {
	c.goneOnce.Do(func() {
		func() {
			h.mu.Lock()
			defer h.mu.Unlock()
			delete(h.conns, c)
			if s := c.sess.Load(); s != nil {
				h.removeLocked(s, h.now())
			}
		}()
		c.close()
		h.releaseConn(kindWT, ipKey(c.ip), c.takePending())
	})
}

type wtConnKey struct{}

func addrOf(a net.Addr) netip.Addr {
	if ua, ok := a.(*net.UDPAddr); ok {
		if ip, ok := netip.AddrFromSlice(ua.IP); ok {
			return ip.Unmap()
		}
	}
	return netip.Addr{}
}

// ListenWT serves WebTransport (HTTP/3) on a UDP socket. addr "127.0.0.1:0"
// picks a free port; the bound address is returned. tlsConf must carry the
// certificate (Certificates or GetCertificate); QUIC needs TLS 1.3.
//
// Every connection first goes through QUIC's Retry (address validation), so a
// forged source address gets only a small Retry packet and no state, and
// nothing else is ever sent to an address that has not proved it receives.
func (s *Server) ListenWT(addr string, tlsConf *tls.Config) (net.Addr, error) {
	ua, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	pc, err := net.ListenUDP("udp", ua)
	if err != nil {
		return nil, err
	}
	tc := tlsConf.Clone()
	tc.NextProtos = []string{http3.NextProtoH3}
	if tc.MinVersion < tls.VersionTLS13 {
		tc.MinVersion = tls.VersionTLS13
	}
	cfg := s.cfg
	qcfg := &quic.Config{
		EnableDatagrams:                  true,
		EnableStreamResetPartialDelivery: true,
		// HTTP/3 itself opens three unidirectional streams; the session needs
		// the CONNECT request stream and the control stream.
		MaxIncomingStreams:     4,
		MaxIncomingUniStreams:  8,
		HandshakeIdleTimeout:   cfg.HandshakeTimeout,
		MaxIdleTimeout:         cfg.SessionTimeout,
		KeepAlivePeriod:        cfg.PingInterval,
		MaxStreamReceiveWindow: 64 << 10, MaxConnectionReceiveWindow: 128 << 10,
		InitialStreamReceiveWindow: 16 << 10, InitialConnectionReceiveWindow: 32 << 10,
	}
	mux := http.NewServeMux()
	mux.HandleFunc(wtPath, s.serveWTHTTP)
	h3 := &http3.Server{TLSConfig: tc, QUICConfig: qcfg, Handler: mux, MaxHeaderBytes: 4096}
	wt := &webtransport.Server{H3: h3, CheckOrigin: s.originAllowed}
	webtransport.ConfigureHTTP3Server(h3)

	tr := &quic.Transport{
		Conn:                pc,
		VerifySourceAddress: func(net.Addr) bool { return true },
		ConnContext:         s.wtConnContext,
	}
	ln, err := tr.ListenEarly(tc, qcfg)
	if err != nil {
		_ = pc.Close()
		return nil, err
	}
	s.mu.Lock()
	s.wt, s.wtLn, s.wtTr, s.wtPC = wt, ln, tr, pc
	s.mu.Unlock()
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		defer s.hub.recoverPanic("webtransport accept")
		for {
			qc, err := ln.Accept(context.Background())
			if err != nil {
				return
			}
			s.wg.Add(1)
			go func() {
				defer s.wg.Done()
				defer s.hub.recoverPanic("webtransport connection")
				if wc, _ := qc.Context().Value(wtConnKey{}).(*wtConn); wc != nil {
					wc.attachQUIC(func() { _ = qc.CloseWithError(quic.ApplicationErrorCode(http3.ErrCodeNoError), "") })
				}
				_ = wt.ServeQUICConn(qc)
			}()
		}
	}()
	return pc.LocalAddr(), nil
}

// wtConnContext is quic-go's admission hook: it runs for every connection
// whose source address has been validated (by Retry), before any handshake
// state is built, and refuses it when the per-address or relay-wide limit
// is reached. The connection is released when its context ends: when it
// closes or when its handshake fails.
func (s *Server) wtConnContext(ctx context.Context, ci *quic.ClientInfo) (context.Context, error) {
	h := s.hub
	ip := addrOf(ci.RemoteAddr)
	if !ci.AddrVerified || !ip.IsValid() {
		return nil, errors.New("source address not validated")
	}
	c, ok := h.wtAdmit(ip)
	if !ok {
		return nil, errors.New("connection limit reached")
	}
	ctx = context.WithValue(ctx, wtConnKey{}, c)
	context.AfterFunc(ctx, func() { h.wtGone(c) })
	return ctx, nil
}

// serveWTHTTP handles the CONNECT request that opens a session.
func (s *Server) serveWTHTTP(w http.ResponseWriter, r *http.Request) {
	h := s.hub
	defer h.recoverPanic("webtransport request")
	if r.URL.Path != wtPath {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodConnect {
		http.Error(w, "WebTransport only", http.StatusMethodNotAllowed)
		return
	}
	c, _ := r.Context().Value(wtConnKey{}).(*wtConn)
	if c == nil {
		http.Error(w, "unavailable", http.StatusInternalServerError)
		return
	}
	if !s.originAllowed(r) {
		http.Error(w, "origin not allowed", http.StatusForbidden)
		return
	}
	if !c.start() {
		http.Error(w, "one session per connection", http.StatusTooManyRequests)
		return
	}
	s.mu.Lock()
	wt := s.wt
	s.mu.Unlock()
	sess, err := wt.Upgrade(w, r)
	if err != nil {
		http.Error(w, "bad WebTransport request", http.StatusBadRequest)
		c.close()
		return
	}
	h.serveWTSession(c, realWTSession{sess})
}
