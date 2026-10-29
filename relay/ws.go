// SPDX-License-Identifier: MIT

package relay

import (
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"runtime/debug"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

// wsConn is one WebSocket client. Frames are binary messages with the same
// layout as UDP datagrams minus the token (the connection is the identity).
//
// gorilla/websocket allows one writer at a time and panics on concurrent
// writes, so every write goes through write(), which holds wmu.
type wsConn struct {
	connState
	hub      *Hub
	c        *websocket.Conn
	ip       netip.Addr // client address as seen (before ipKey)
	accepted time.Time
	sess     atomic.Pointer[session]

	out       chan []byte
	qbytes    atomic.Int64 // bytes in out
	done      chan struct{}
	closeOnce sync.Once
	wmu       sync.Mutex
}

func (c *wsConn) kind() connKind                  { return kindWS }
func (c *wsConn) clientIP() netip.Addr            { return c.ip }
func (c *wsConn) acceptedAt() time.Time           { return c.accepted }
func (c *wsConn) getSession() *session            { return c.sess.Load() }
func (c *wsConn) swapSession(s *session) *session { return c.sess.Swap(s) }
func (c *wsConn) transportLive() bool             { return false } // pongs touch the session instead

// write sends one message, serialised with every other write on the
// connection and bounded by a deadline.
func (c *wsConn) write(typ int, b []byte, d time.Duration) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	_ = c.c.SetWriteDeadline(time.Now().Add(d))
	return c.c.WriteMessage(typ, b)
}

// enqueue queues a frame for the writer. A client that cannot keep up loses
// frames, as it would on a lossy network: vnet is unreliable. The queue is
// bounded in frames and in bytes.
func (c *wsConn) enqueue(b []byte) {
	if c.qbytes.Add(int64(len(b))) > int64(c.hub.cfg.WSSendQueueBytes) {
		c.qbytes.Add(-int64(len(b)))
		c.hub.stats.wsQueueDrops.Add(1)
		return
	}
	select {
	case c.out <- b:
	default:
		c.qbytes.Add(-int64(len(b)))
		c.hub.stats.wsQueueDrops.Add(1)
	}
}

// close ends the connection without blocking the caller: the close frame is
// sent (and the socket closed) from a goroutine with a short deadline, so a
// client that has stopped reading cannot stall whoever is closing (the
// sweeper closes many connections in a row).
func (c *wsConn) close() {
	c.closeOnce.Do(func() {
		close(c.done)
		go func() {
			_ = c.c.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""), time.Now().Add(200*time.Millisecond))
			_ = c.c.Close()
		}()
	})
}

func (c *wsConn) writeLoop() {
	defer c.hub.recoverPanic("websocket writer")
	ping := time.NewTicker(c.hub.cfg.PingInterval)
	defer ping.Stop()
	for {
		select {
		case b := <-c.out:
			c.qbytes.Add(-int64(len(b)))
			if err := c.write(websocket.BinaryMessage, b, c.hub.cfg.WSWriteTimeout); err != nil {
				c.close()
				return
			}
		case <-ping.C:
			if err := c.write(websocket.PingMessage, nil, c.hub.cfg.WSWriteTimeout); err != nil {
				c.close()
				return
			}
		case <-c.done:
			return
		}
	}
}

// touch records liveness for the session, if there is one.
func (c *wsConn) touch(now time.Time) {
	if s := c.sess.Load(); s != nil {
		s.touch(now)
	}
}

func (c *wsConn) readLoop() {
	defer c.hub.recoverPanic("websocket reader")
	h := c.hub
	scratch := make([]byte, 0, maxUDPFrame)
	for {
		typ, msg, err := c.c.ReadMessage()
		if err != nil {
			return
		}
		now := h.now()
		h.stats.rxFrames.Add(1)
		if typ != websocket.BinaryMessage || len(msg) == 0 {
			h.stats.malformed.Add(1)
			if c.sess.Load() == nil {
				c.reject(ErrMalformed)
				return
			}
			continue
		}
		c.touch(now)
		if !c.handle(msg, now, &scratch) {
			return
		}
	}
}

// reject sends an ERROR and closes the connection.
func (c *wsConn) reject(reason byte) {
	var zero [NonceLen]byte
	_ = c.write(websocket.BinaryMessage, encodeError(reason, zero), c.hub.cfg.WSWriteTimeout)
	c.close()
}

// handle processes one binary message; false means close the connection.
func (c *wsConn) handle(msg []byte, now time.Time, scratch *[]byte) bool {
	h := c.hub
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
	switch msg[0] {
	case TypeKeepalive:
		h.keepalive(s, len(msg), now)
	case TypeData:
		d, ok := parseData(msg, false)
		if !ok {
			h.stats.malformed.Add(1)
			return true
		}
		h.forwardLocked(s, &d, len(msg), now, scratch)
	default:
		h.stats.malformed.Add(1)
	}
	return true
}

// connHandshake runs CREATE_ROOM / JOIN_ROOM for a connection of either
// stream transport. The replies go through the connection's own queue. False
// means close the connection.
func (h *Hub) connHandshake(c clientConn, msg []byte, now time.Time) bool {
	need := createLen
	if msg[0] == TypeJoin {
		need = joinLen
	}
	if len(msg) < need {
		h.stats.malformed.Add(1)
		c.reject(ErrMalformed)
		return false
	}
	var nonce [NonceLen]byte
	copy(nonce[:], msg[2:2+NonceLen])
	send := func(b []byte) { c.enqueue(b) }

	if !h.ips.allowHandshake(c.clientIP(), now) {
		h.stats.rateLimited.Add(1)
		send(encodeError(ErrRate, nonce))
		return true
	}
	if msg[1] != ProtocolVersion {
		h.stats.handshakeRejected.Add(1)
		send(encodeError(ErrBadVersion, nonce))
		return true
	}
	req := hsReq{kind: msg[0], nonce: nonce, conn: c, ip: ipKey(c.clientIP())}
	if msg[0] == TypeJoin {
		code, ok := normaliseCode(msg[createLen : createLen+CodeLen])
		if !ok {
			h.stats.handshakeRejected.Add(1)
			send(encodeError(ErrRoomNotFound, nonce))
			return true
		}
		req.code = code
	}
	if cur := c.getSession(); cur != nil && cur.nonce != nonce {
		send(encodeError(ErrAlreadyInRoom, nonce))
		return true
	}
	reply, sess := h.handshake(req, now)
	if sess != nil && c.swapSession(sess) == nil {
		h.connAuthenticated(c) // no longer "pending" for the per-address cap
	}
	send(reply)
	return true
}

// acquireConn admits a new connection of the given kind from ip (a limiter
// key) or refuses it. The check and the reservation are one step under the
// lock, so concurrent upgrades cannot together exceed the limits.
func (h *Hub) acquireConn(kind connKind, ip netip.Addr) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.connsHeld >= h.cfg.MaxSessions {
		return false
	}
	if kind == kindWT && h.wtHeld >= h.cfg.MaxWT {
		return false
	}
	for _, k := range keysFor(ip) {
		if c := h.perIP[k]; c != nil {
			caps := h.capsFor(k)
			if kind == kindWS && (c.ws >= caps.ws || c.pendingWS >= caps.pendingWS) {
				return false
			}
			if kind == kindWT && (c.wt >= caps.wt || c.pendingWT >= caps.pendingWT) {
				return false
			}
		}
	}
	h.connsHeld++
	if kind == kindWT {
		h.wtHeld++
	}
	h.charge(ip, func(c *ipCount) {
		if kind == kindWS {
			c.ws++
			c.pendingWS++
		} else {
			c.wt++
			c.pendingWT++
		}
	})
	return true
}

// releaseConn gives back what acquireConn reserved (pending: the connection
// never entered a room, so it still holds its pending slot).
func (h *Hub) releaseConn(kind connKind, ip netip.Addr, pending bool) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.connsHeld--
	if kind == kindWT {
		h.wtHeld--
	}
	h.charge(ip, func(c *ipCount) {
		if kind == kindWS {
			c.ws--
			if pending {
				c.pendingWS--
			}
		} else {
			c.wt--
			if pending {
				c.pendingWT--
			}
		}
	})
}

// connAuthenticated is called once when a connection enters a room: it no
// longer counts as "pending" for the per-address cap.
func (h *Hub) connAuthenticated(c clientConn) {
	if !c.takePending() {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	kind := c.kind()
	h.charge(ipKey(c.clientIP()), func(cnt *ipCount) {
		if kind == kindWS {
			cnt.pendingWS--
		} else {
			cnt.pendingWT--
		}
	})
}

// recoverPanic keeps a bug in one connection from taking the process down.
func (h *Hub) recoverPanic(where string) {
	if r := recover(); r != nil {
		h.stats.panics.Add(1)
		h.cfg.Logf("recovered panic in %s: %v\n%s", where, r, debug.Stack())
	}
}

func (s *Server) originAllowed(r *http.Request) bool {
	if len(s.cfg.AllowedOrigins) == 0 {
		return true
	}
	origin := r.Header.Get("Origin")
	if origin == "" { // not a browser
		return true
	}
	u, err := url.Parse(origin)
	if err != nil {
		return false
	}
	for _, a := range s.cfg.AllowedOrigins {
		if a == "*" || strings.EqualFold(a, u.Host) {
			return true
		}
	}
	return false
}

func (s *Server) clientIP(r *http.Request) netip.Addr {
	if s.cfg.TrustForwardedFor {
		if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
			// The rightmost entry is the one our own proxy appended; the
			// ones before it were written by the client.
			parts := strings.Split(xff, ",")
			last := strings.TrimSpace(parts[len(parts)-1])
			if a, err := netip.ParseAddr(last); err == nil {
				return a.Unmap()
			}
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil {
		if a, err := netip.ParseAddr(host); err == nil {
			return a.Unmap()
		}
	}
	return netip.Addr{}
}

func (s *Server) serveWS(w http.ResponseWriter, r *http.Request) {
	h := s.hub
	ip := s.clientIP(r)
	key := ipKey(ip)
	if !h.acquireConn(kindWS, key) {
		h.stats.wsRefused.Add(1)
		http.Error(w, "too many connections", http.StatusServiceUnavailable)
		return
	}
	up := websocket.Upgrader{
		ReadBufferSize:  2048,
		WriteBufferSize: 2048,
		CheckOrigin:     s.originAllowed,
	}
	conn, err := up.Upgrade(w, r, nil)
	c := &wsConn{
		hub:      h,
		ip:       ip,
		accepted: h.now(),
		out:      make(chan []byte, h.cfg.WSSendQueue),
		done:     make(chan struct{}),
	}
	c.pendingHeld.Store(true)
	if err == nil {
		func() {
			h.mu.Lock()
			defer h.mu.Unlock()
			c.c = conn
			h.conns[c] = struct{}{}
		}()
	}
	// Whatever happens next, the connection's holdings are given back, and
	// the locks are released even if something panics (and is recovered).
	defer func() {
		if err == nil {
			func() {
				h.mu.Lock()
				defer h.mu.Unlock()
				delete(h.conns, c)
				if sess := c.sess.Load(); sess != nil {
					h.removeLocked(sess, h.now())
				}
			}()
			c.close()
		}
		h.releaseConn(kindWS, key, c.takePending())
	}()
	if err != nil {
		return // Upgrade has replied
	}
	conn.SetReadLimit(maxWSMessage)
	// Browsers answer pings on their own, even when the page's timers are
	// throttled, so a pong is as good as a frame for liveness.
	conn.SetPongHandler(func(string) error {
		c.touch(h.now())
		return nil
	})

	go c.writeLoop()
	c.readLoop()
}
