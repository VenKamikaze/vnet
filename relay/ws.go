// SPDX-License-Identifier: MIT

package relay

import (
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

// wsConn is one WebSocket client. Frames are binary messages with the same
// layout as UDP datagrams minus the token (the connection is the identity).
type wsConn struct {
	hub      *Hub
	c        *websocket.Conn
	ip       netip.Addr
	accepted time.Time
	sess     atomic.Pointer[session]

	out       chan []byte
	done      chan struct{}
	closeOnce sync.Once
}

// enqueue queues a frame for the writer. A client that cannot keep up loses
// frames, as it would on a lossy network: vnet is unreliable.
func (c *wsConn) enqueue(b []byte) {
	select {
	case c.out <- b:
	default:
		c.hub.stats.wsQueueDrops.Add(1)
	}
}

func (c *wsConn) close() {
	c.closeOnce.Do(func() {
		close(c.done)
		_ = c.c.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""), time.Now().Add(time.Second))
		_ = c.c.Close()
	})
}

func (c *wsConn) writeLoop() {
	ping := time.NewTicker(c.hub.cfg.PingInterval)
	defer ping.Stop()
	for {
		select {
		case b := <-c.out:
			_ = c.c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if err := c.c.WriteMessage(websocket.BinaryMessage, b); err != nil {
				c.close()
				return
			}
		case <-ping.C:
			_ = c.c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if err := c.c.WriteMessage(websocket.PingMessage, nil); err != nil {
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
	_ = c.c.SetWriteDeadline(time.Now().Add(time.Second))
	_ = c.c.WriteMessage(websocket.BinaryMessage, encodeError(reason, zero))
	c.close()
}

// handle processes one binary message; false means close the connection.
func (c *wsConn) handle(msg []byte, now time.Time, scratch *[]byte) bool {
	h := c.hub
	s := c.sess.Load()
	switch msg[0] {
	case TypeCreate, TypeJoin:
		return c.handshake(msg, now)
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
		h.mu.RLock()
		h.forward(s, &d, len(msg), now, scratch)
		h.mu.RUnlock()
	default:
		h.stats.malformed.Add(1)
	}
	return true
}

func (c *wsConn) handshake(msg []byte, now time.Time) bool {
	h := c.hub
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

	if !h.ips.allowHandshake(c.ip, now) {
		h.stats.rateLimited.Add(1)
		send(encodeError(ErrRate, nonce))
		return true
	}
	if msg[1] != ProtocolVersion {
		h.stats.handshakeRejected.Add(1)
		send(encodeError(ErrBadVersion, nonce))
		return true
	}
	req := hsReq{kind: msg[0], nonce: nonce, ws: c}
	if msg[0] == TypeJoin {
		code, ok := normaliseCode(msg[createLen : createLen+CodeLen])
		if !ok {
			h.stats.handshakeRejected.Add(1)
			send(encodeError(ErrRoomNotFound, nonce))
			return true
		}
		req.code = code
	}
	if cur := c.sess.Load(); cur != nil && cur.nonce != nonce {
		send(encodeError(ErrAlreadyInRoom, nonce))
		return true
	}
	reply, sess := h.handshake(req, now)
	if sess != nil {
		c.sess.Store(sess)
	}
	send(reply)
	return true
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
			first := strings.TrimSpace(strings.Split(xff, ",")[0])
			if a, err := netip.ParseAddr(first); err == nil {
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
	h.mu.RLock()
	full := len(h.conns) >= h.cfg.MaxSessions
	h.mu.RUnlock()
	if full {
		http.Error(w, "relay is full", http.StatusServiceUnavailable)
		return
	}
	up := websocket.Upgrader{
		ReadBufferSize:  2048,
		WriteBufferSize: 2048,
		CheckOrigin:     s.originAllowed,
	}
	conn, err := up.Upgrade(w, r, nil)
	if err != nil {
		return // Upgrade has replied
	}
	conn.SetReadLimit(maxWSMessage)
	c := &wsConn{
		hub:      h,
		c:        conn,
		ip:       s.clientIP(r),
		accepted: h.now(),
		out:      make(chan []byte, h.cfg.WSSendQueue),
		done:     make(chan struct{}),
	}
	// Browsers answer pings on their own, even when the page's timers are
	// throttled, so a pong is as good as a frame for liveness.
	conn.SetPongHandler(func(string) error {
		c.touch(h.now())
		return nil
	})
	h.mu.Lock()
	h.conns[c] = struct{}{}
	h.mu.Unlock()

	go c.writeLoop()
	c.readLoop()

	h.mu.Lock()
	delete(h.conns, c)
	if sess := c.sess.Load(); sess != nil {
		h.removeLocked(sess, h.now())
	}
	h.mu.Unlock()
	c.close()
}
