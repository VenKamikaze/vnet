// SPDX-License-Identifier: MIT

package relay

import (
	"crypto/rand"
	"crypto/subtle"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"
)

// session is one member of one room. It is the unit that owns a vaddr, a
// token and the per-client rate limits.
type session struct {
	token   [TokenLen]byte
	nonce   [NonceLen]byte
	kind    byte   // TypeCreate or TypeJoin: the request that made it
	reqCode string // for TypeJoin, the normalised code asked for
	room    *room
	octet   byte // last byte of the vaddr 10.0.0.octet
	ws      *wsConn

	created   time.Time
	ip        netip.Addr                     // limiter key of the source that created it (IPv6 as /64)
	udpAddr   atomic.Pointer[netip.AddrPort] // UDP sessions: where to send. Moves only after path validation
	validated atomic.Bool                    // UDP: a frame bearing the token has arrived from udpAddr, so the
	//                                         member really receives there. Until then nothing is sent to it.
	lastSeen atomic.Int64 // unix nanos of the last frame or pong

	// Address validation (UDP): a frame with a valid token from a source other
	// than udpAddr does not move the session; it earns a PATH_CHALLENGE to
	// that source, and the session moves only when the source echoes it.
	chMu       sync.Mutex
	chAddr     netip.AddrPort
	chCookie   [cookieLen]byte
	chExpires  time.Time
	chLastSent time.Time

	// Token buckets, touched only by the goroutine that reads this
	// session's transport.
	pps, bps bucket
}

func (s *session) touch(now time.Time) { s.lastSeen.Store(now.UnixNano()) }
func (s *session) seen() time.Time     { return time.Unix(0, s.lastSeen.Load()) }

func (s *session) setAddr(a netip.AddrPort) {
	a = netip.AddrPortFrom(a.Addr().Unmap(), a.Port())
	if cur := s.udpAddr.Load(); cur != nil && *cur == a {
		return
	}
	s.udpAddr.Store(&a)
}

type room struct {
	creatorIP netip.Addr // limiter key of the creator, for the per-IP room count
	code      string
	members   [256]*session // indexed by the vaddr's last byte; 0 unused
	n         int
	max       int          // highest vaddr octet handed out
	lastData  atomic.Int64 // unix nanos of the last DATA sent into the room
}

// freeOctet returns the lowest free joiner address, or 0 if the room is full.
// Address 1 is the creator's for the whole life of the room and is never
// handed to anyone else, even after the creator has left.
func (r *room) freeOctet() byte {
	for o := 2; o <= r.max; o++ {
		if r.members[o] == nil {
			return byte(o)
		}
	}
	return 0
}

// ipCount is what one source address (IPv6: one /64) currently holds.
type ipCount struct {
	sessions  int
	rooms     int
	ws        int // open WebSockets
	pendingWS int // of those, not yet in a room
}

type nonceEntry struct {
	sess  *session  // nil for a tombstone
	until time.Time // tombstones expire
}

// Hub holds all relay state. It is transport-agnostic: the UDP loop and the
// WebSocket handler feed it frames.
type Hub struct {
	cfg Config

	mu      sync.RWMutex
	rooms   map[string]*room
	byToken map[[TokenLen]byte]*session
	byNonce map[[NonceLen]byte]*nonceEntry
	conns   map[*wsConn]struct{}
	perIP   map[netip.Addr]*ipCount

	ips   *ipLimiter
	stats stats

	sendUDP func(netip.AddrPort, []byte) // set by the Server once it listens
}

func newHub(cfg Config) *Hub {
	h := &Hub{
		cfg:     cfg,
		rooms:   make(map[string]*room),
		byToken: make(map[[TokenLen]byte]*session),
		byNonce: make(map[[NonceLen]byte]*nonceEntry),
		conns:   make(map[*wsConn]struct{}),
		perIP:   make(map[netip.Addr]*ipCount),
	}
	h.ips = newIPLimiter(&h.cfg)
	return h
}

func (h *Hub) now() time.Time { return h.cfg.Now() }

// hsReq is a parsed CREATE_ROOM or JOIN_ROOM.
type hsReq struct {
	kind  byte
	nonce [NonceLen]byte
	code  string // normalised; only for TypeJoin with a valid code
	udp   *netip.AddrPort
	ws    *wsConn
	ip    netip.Addr // limiter key of the requester
}

// counts returns (creating if needed) the holdings of ip. Caller holds h.mu.
func (h *Hub) counts(ip netip.Addr) *ipCount {
	c := h.perIP[ip]
	if c == nil {
		c = &ipCount{}
		h.perIP[ip] = c
	}
	return c
}

// release drops the entry when it is back to zero. Caller holds h.mu.
func (h *Hub) trim(ip netip.Addr, c *ipCount) {
	if c.sessions == 0 && c.rooms == 0 && c.ws == 0 && c.pendingWS == 0 {
		delete(h.perIP, ip)
	}
}

func (h *Hub) newCodeLocked() string {
	for {
		var b [CodeLen]byte
		if _, err := rand.Read(b[:]); err != nil {
			panic("relay: no randomness: " + err.Error())
		}
		for i := range b {
			b[i] = codeAlphabet[b[i]&31]
		}
		if _, taken := h.rooms[string(b[:])]; !taken {
			return string(b[:])
		}
	}
}

func (h *Hub) roomReply(s *session) []byte {
	return encodeRoom(s.nonce, s.octet, s.token, s.room.code, s.room.max, int(h.cfg.KeepaliveInterval/time.Second))
}

// handshake runs CREATE_ROOM / JOIN_ROOM. It returns the reply frame (ROOM or
// ERROR) and, on success, the session. It is idempotent: a repeat of a
// request with the same nonce returns the same session.
func (h *Hub) handshake(r hsReq, now time.Time) (reply []byte, sess *session) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if e := h.byNonce[r.nonce]; e != nil {
		if e.sess == nil {
			h.stats.handshakeRejected.Add(1)
			return encodeError(ErrClosed, r.nonce), nil
		}
		s := e.sess
		if s.kind != r.kind || s.reqCode != r.code || s.ws != r.ws {
			h.stats.handshakeRejected.Add(1)
			return encodeError(ErrNonceReused, r.nonce), nil
		}
		// A repeat from another address gets the same ROOM, but does not move
		// the session: only a validated path (PATH_CHALLENGE) does that.
		s.touch(now)
		h.stats.handshakeRetries.Add(1)
		return h.roomReply(s), s
	}

	if len(h.byToken) >= h.cfg.MaxSessions {
		h.stats.handshakeRejected.Add(1)
		return encodeError(ErrLimit, r.nonce), nil
	}

	ipc := h.counts(r.ip)
	defer h.trim(r.ip, ipc) // an entry created for a refused request must not stay
	if ipc.sessions >= h.cfg.MaxSessionsPerIP {
		h.stats.handshakeRejected.Add(1)
		return encodeError(ErrLimit, r.nonce), nil
	}

	var rm *room
	var octet byte
	if r.kind == TypeCreate {
		if len(h.rooms) >= h.cfg.MaxRooms || ipc.rooms >= h.cfg.MaxRoomsPerIP {
			h.stats.handshakeRejected.Add(1)
			return encodeError(ErrLimit, r.nonce), nil
		}
		rm = &room{code: h.newCodeLocked(), max: h.cfg.MaxMembers, creatorIP: r.ip}
		octet = 1
	} else {
		rm = h.rooms[r.code]
		if rm == nil {
			h.stats.handshakeRejected.Add(1)
			return encodeError(ErrRoomNotFound, r.nonce), nil
		}
		if octet = rm.freeOctet(); octet == 0 {
			h.stats.handshakeRejected.Add(1)
			return encodeError(ErrRoomFull, r.nonce), nil
		}
	}

	s := &session{nonce: r.nonce, kind: r.kind, reqCode: r.code, ws: r.ws, octet: octet, ip: r.ip}
	if _, err := rand.Read(s.token[:]); err != nil {
		panic("relay: no randomness: " + err.Error())
	}
	if r.udp != nil {
		s.setAddr(*r.udp)
	} else {
		s.validated.Store(true) // a WebSocket is its own proof of path
	}
	s.touch(now)
	s.created = now
	ipc.sessions++
	if r.kind == TypeCreate {
		ipc.rooms++
		h.rooms[rm.code] = rm
		rm.lastData.Store(now.UnixNano())
		h.stats.roomsCreated.Add(1)
		h.cfg.Logf("room %s created", rm.code)
	}
	s.room = rm
	rm.members[octet] = s
	rm.n++
	h.byToken[s.token] = s
	h.byNonce[s.nonce] = &nonceEntry{sess: s}
	h.stats.sessionsJoined.Add(1)
	return h.roomReply(s), s
}

// removeLocked takes s out of its room and the lookup tables. It reports
// whether s was still registered. The caller holds h.mu for writing.
func (h *Hub) removeLocked(s *session, now time.Time) bool {
	if h.byToken[s.token] != s {
		return false
	}
	delete(h.byToken, s.token)
	h.byNonce[s.nonce] = &nonceEntry{until: now.Add(h.cfg.NonceTTL)}
	if c := h.perIP[s.ip]; c != nil {
		c.sessions--
		h.trim(s.ip, c)
	}
	rm := s.room
	if rm.members[s.octet] == s {
		rm.members[s.octet] = nil
		rm.n--
		if rm.n == 0 {
			if c := h.perIP[rm.creatorIP]; c != nil {
				c.rooms--
				h.trim(rm.creatorIP, c)
			}
			delete(h.rooms, rm.code)
			h.stats.roomsClosed.Add(1)
			h.cfg.Logf("room %s closed (empty)", rm.code)
		}
	}
	return true
}

// leave removes a member at its own request (LEAVE, or its WebSocket closing).
func (h *Hub) leave(s *session) {
	h.mu.Lock()
	h.removeLocked(s, h.now())
	h.mu.Unlock()
}

// replyUnauth sends a reply to a source that has not proved it holds a
// session. It enforces the anti-reflection rules: the reply is never larger
// than the packet it answers, and replies are rate-limited per source IP and
// globally.
func (h *Hub) replyUnauth(src netip.AddrPort, reqLen int, reply []byte, now time.Time) {
	if len(reply) > reqLen {
		h.stats.repliesSuppressed.Add(1)
		return
	}
	if !h.ips.allowReply(src.Addr().Unmap(), now) {
		h.stats.repliesSuppressed.Add(1)
		return
	}
	h.send(src, reply)
}

func (h *Hub) send(dst netip.AddrPort, b []byte) {
	if h.sendUDP != nil {
		h.sendUDP(dst, b)
		h.stats.txFrames.Add(1)
	}
}

// deliver hands an outbound frame to a member over its transport. buf is
// only valid for the call, so the WebSocket path copies it.
func (h *Hub) deliver(s *session, b []byte) {
	if s.ws != nil {
		s.ws.enqueue(append([]byte(nil), b...))
		return
	}
	if !s.validated.Load() {
		h.stats.unvalidatedDrops.Add(1) // nothing proves anyone receives at this address yet
		return
	}
	if a := s.udpAddr.Load(); a != nil {
		h.send(*a, b)
	}
}

// HandleUDP processes one datagram from src. It is called from a single
// goroutine (the UDP read loop), so processing is ordered. scratch is a
// reusable buffer.
func (h *Hub) HandleUDP(p []byte, src netip.AddrPort, scratch *[]byte) {
	now := h.now()
	h.stats.rxFrames.Add(1)
	if len(p) == 0 {
		h.stats.malformed.Add(1)
		return
	}
	if len(p) > maxUDPFrame {
		h.stats.oversize.Add(1)
		return
	}
	switch p[0] {
	case TypeCreate, TypeJoin:
		h.udpHandshake(p, src, now)
	case TypeKeepalive, TypeLeave, TypeData, TypePathResponse:
		h.udpAuthenticated(p, src, now, scratch)
	default:
		h.stats.malformed.Add(1)
	}
}

func (h *Hub) udpHandshake(p []byte, src netip.AddrPort, now time.Time) {
	// Padding rule: a datagram too short to be answered safely is dropped
	// without a reply and without creating any state.
	if len(p) < MinUDPHandshake {
		h.stats.malformed.Add(1)
		return
	}
	ip := src.Addr().Unmap()
	var nonce [NonceLen]byte
	copy(nonce[:], p[2:2+NonceLen])
	if !h.ips.allowHandshake(ip, now) {
		h.stats.rateLimited.Add(1)
		h.replyUnauth(src, len(p), encodeError(ErrRate, nonce), now)
		return
	}
	if p[1] != ProtocolVersion {
		h.stats.handshakeRejected.Add(1)
		h.replyUnauth(src, len(p), encodeError(ErrBadVersion, nonce), now)
		return
	}
	req := hsReq{kind: p[0], nonce: nonce, ip: ipKey(ip)}
	if p[0] == TypeJoin {
		code, ok := normaliseCode(p[createLen : createLen+CodeLen])
		if !ok {
			h.stats.handshakeRejected.Add(1)
			h.replyUnauth(src, len(p), encodeError(ErrRoomNotFound, nonce), now)
			return
		}
		req.code = code
	}
	addr := netip.AddrPortFrom(ip, src.Port())
	req.udp = &addr
	reply, _ := h.handshake(req, now)
	h.replyUnauth(src, len(p), reply, now)
}

func (h *Hub) udpAuthenticated(p []byte, src netip.AddrPort, now time.Time, scratch *[]byte) {
	minLen := 1 + TokenLen
	switch p[0] {
	case TypeData:
		minLen = udpDataHdr
	case TypePathResponse:
		minLen = responseLen
	}
	if len(p) < minLen {
		h.stats.malformed.Add(1)
		return
	}
	var tok [TokenLen]byte
	copy(tok[:], p[1:1+TokenLen])
	src = netip.AddrPortFrom(src.Addr().Unmap(), src.Port())

	h.mu.RLock()
	s := h.byToken[tok]
	h.mu.RUnlock()
	if s == nil || s.ws != nil {
		h.stats.unknownToken.Add(1)
		var zero [NonceLen]byte
		h.replyUnauth(src, len(p), encodeError(ErrNoSession, zero), now)
		return
	}
	s.touch(now)

	if p[0] == TypePathResponse {
		h.pathResponse(s, p, src, now)
		return
	}
	// The token is the identity, but where to SEND is another matter: the
	// source address of a datagram is forgeable, and a session that followed
	// it would let anyone holding a token aim the relay at a victim. So the
	// session stays where it was, and a frame from elsewhere is answered with
	// a challenge that only a host receiving at that address can echo.
	if cur := s.udpAddr.Load(); cur != nil && *cur == src {
		s.validated.Store(true)
	} else {
		h.challenge(s, src, len(p), now)
	}
	switch p[0] {
	case TypeLeave:
		h.leave(s)
	case TypeKeepalive:
		h.keepalive(s, len(p), now)
	case TypeData:
		d, ok := parseData(p, true)
		if !ok {
			h.stats.malformed.Add(1)
			return
		}
		h.mu.RLock()
		h.forward(s, &d, len(p), now, scratch)
		h.mu.RUnlock()
	}
}

// challenge sends a PATH_CHALLENGE to src, a source the session is not
// registered at. The challenge is smaller than any frame that can trigger it,
// is sent at most once per PathChallengeInterval per session, and counts
// against the per-source reply limit, so spoofed sources cannot turn this into
// a stream of packets at a victim.
func (h *Hub) challenge(s *session, src netip.AddrPort, reqLen int, now time.Time) {
	s.chMu.Lock()
	if now.Sub(s.chLastSent) < h.cfg.PathChallengeInterval && !s.chLastSent.IsZero() {
		s.chMu.Unlock()
		return
	}
	if s.chAddr != src || now.After(s.chExpires) {
		s.chAddr = src
		if _, err := rand.Read(s.chCookie[:]); err != nil {
			s.chMu.Unlock()
			panic("relay: no randomness: " + err.Error())
		}
	}
	s.chExpires = now.Add(h.cfg.PathChallengeTimeout)
	s.chLastSent = now
	frame := make([]byte, challengeLen)
	frame[0] = TypePathChallenge
	copy(frame[1:], s.chCookie[:])
	s.chMu.Unlock()
	h.stats.pathChallenges.Add(1)
	h.replyUnauth(src, reqLen, frame, now)
}

// pathResponse completes a validation: the source echoed the cookie that was
// sent to it, so it does receive there, and the session moves.
func (h *Hub) pathResponse(s *session, p []byte, src netip.AddrPort, now time.Time) {
	s.chMu.Lock()
	ok := s.chAddr == src && !s.chExpires.IsZero() && !now.After(s.chExpires) &&
		subtle.ConstantTimeCompare(p[1+TokenLen:1+TokenLen+cookieLen], s.chCookie[:]) == 1
	if ok {
		s.chExpires = time.Time{} // single use
	}
	s.chMu.Unlock()
	if !ok {
		h.stats.badPathResponses.Add(1)
		return
	}
	s.setAddr(src)
	s.validated.Store(true)
	h.stats.pathMigrations.Add(1)
}

// keepalive answers a KEEPALIVE from an authenticated member with a
// KEEPALIVE, so that the client can tell the relay is alive.
func (h *Hub) keepalive(s *session, frameLen int, now time.Time) {
	if !s.pps.allow(now, h.cfg.ClientPPS, h.cfg.ClientPPSBurst, 1) ||
		!s.bps.allow(now, h.cfg.ClientBPS, h.cfg.ClientBPSBurst, float64(frameLen)) {
		h.stats.rateLimited.Add(1)
		return
	}
	h.deliver(s, []byte{TypeKeepalive})
}

// forward routes one DATA frame from s to the member of s's own room whose
// vaddr it names. Nothing else is ever a destination. The caller holds h.mu
// (read).
func (h *Hub) forward(s *session, d *dataFrame, frameLen int, now time.Time, scratch *[]byte) {
	if !s.pps.allow(now, h.cfg.ClientPPS, h.cfg.ClientPPSBurst, 1) ||
		!s.bps.allow(now, h.cfg.ClientBPS, h.cfg.ClientBPSBurst, float64(frameLen)) {
		h.stats.rateLimited.Add(1)
		return
	}
	rm := s.room
	if rm.members[s.octet] != s {
		return
	}
	rm.lastData.Store(now.UnixNano())
	var dst *session
	if o := vaddrOctet(d.addr[:]); o != 0 {
		dst = rm.members[o]
	}
	if dst == nil {
		h.stats.unroutable.Add(1)
		return
	}
	*scratch = encodeForward(*scratch, s.octet, d)
	h.deliver(dst, *scratch)
	h.stats.dataForwarded.Add(1)
}

// Sweep expires members that have gone quiet, rooms that have been idle,
// finished-handshake nonces and unauthenticated WebSockets. The server calls
// it every second; tests call it with a fake clock.
func (h *Hub) Sweep() {
	now := h.now()
	type note struct {
		addr  netip.AddrPort
		frame []byte
	}
	var notes []note
	var closeConns []*wsConn

	drop := func(s *session, reason byte) {
		if !h.removeLocked(s, now) {
			return
		}
		if s.ws != nil {
			closeConns = append(closeConns, s.ws)
		} else if a := s.udpAddr.Load(); a != nil && s.validated.Load() {
			notes = append(notes, note{*a, encodeError(reason, s.nonce)})
		}
	}

	h.mu.Lock()
	for _, s := range h.byToken {
		if now.Sub(s.seen()) > h.cfg.SessionTimeout {
			h.stats.sessionsTimedOut.Add(1)
			drop(s, ErrTimeout)
		} else if s.ws == nil && !s.validated.Load() && now.Sub(s.created) > h.cfg.UnvalidatedTimeout {
			// Never proved it receives at the address its handshake came from
			// (a forged source, or a client that vanished at once).
			h.stats.sessionsTimedOut.Add(1)
			drop(s, ErrTimeout)
		}
	}
	for _, rm := range h.rooms {
		if now.Sub(time.Unix(0, rm.lastData.Load())) > h.cfg.RoomIdleTimeout {
			h.stats.roomsIdleClosed.Add(1)
			h.cfg.Logf("room %s closed (idle)", rm.code)
			for _, m := range rm.members {
				if m != nil {
					drop(m, ErrClosed)
				}
			}
		}
	}
	for n, e := range h.byNonce {
		if e.sess == nil && now.After(e.until) {
			delete(h.byNonce, n)
		}
	}
	for c := range h.conns {
		if c.sess.Load() == nil && now.Sub(c.accepted) > h.cfg.HandshakeTimeout {
			closeConns = append(closeConns, c)
		}
	}
	h.mu.Unlock()

	for _, n := range notes {
		h.send(n.addr, n.frame)
	}
	for _, c := range closeConns {
		c.close()
	}
	h.ips.gc(now)
}
