// SPDX-License-Identifier: MIT

package relay

import (
	"fmt"
	"time"
)

// Config holds every tunable of the relay. The zero value of each field means
// "use the default" (see withDefaults); nothing is unlimited.
type Config struct {
	// Rooms.
	MaxMembers  int // members per room incl. the creator (2..254); default 8
	MaxRooms    int // rooms at once; default 1000
	MaxSessions int // members (UDP, WebSocket, WebTransport) at once, also caps open connections; default 20000
	// Per source address (IPv6 sources are grouped by /64):
	MaxSessionsPerIP  int           // members at once; default 128 (everyone behind one NAT shares it)
	MaxRoomsPerIP     int           // rooms created and still alive; default 16
	MaxWSPerIP        int           // open WebSockets; default 128
	MaxPendingWSPerIP int           // WebSockets that have not entered a room yet; default 32
	MaxWT             int           // WebTransport (QUIC) connections at once, whether or not they have a session (each costs roughly 250 KB; see relay/README.md); default 4000
	MaxWTPerIP        int           // WebTransport (QUIC) connections per source address, whether or not they have a session; default 128
	MaxPendingWTPerIP int           // of those, the ones that have not entered a room yet; default 32
	PrefixFactor      int           // an IPv6 /48 may hold this many times the per-address caps above; default 4
	RoomIdleTimeout   time.Duration // a room with no DATA for this long is closed; default 30 min

	// Liveness.
	KeepaliveInterval     time.Duration // advertised to clients in ROOM; default 5 s
	SessionTimeout        time.Duration // no frame (or WebSocket pong) for this long drops a member; default 15 s
	HandshakeTimeout      time.Duration // a WebSocket must send CREATE_ROOM/JOIN_ROOM within this; default 10 s
	NonceTTL              time.Duration // how long a finished handshake nonce is remembered; default 60 s
	RetryCookieBucket     time.Duration // a handshake cookie is valid for one to two of these; default 16 s
	PathChallengeInterval time.Duration // at most one address-validation challenge per session in this time; default 1 s
	PathChallengeTimeout  time.Duration // a challenge can be answered for this long; default 10 s
	PingInterval          time.Duration // WebSocket ping interval; default 5 s

	// Per-client limits (token buckets; excess frames are dropped).
	ClientPPS      float64 // sustained packets per second; default 600
	ClientPPSBurst float64 // default 2 * ClientPPS
	ClientBPS      float64 // sustained bytes per second (whole frames); default 600000
	ClientBPSBurst float64 // default 2 * ClientBPS

	// Limits on unauthenticated sources (per source IP, plus one global
	// bucket for replies).
	HandshakePerSec   float64 // CREATE_ROOM/JOIN_ROOM per source IP; default 20
	HandshakeBurst    float64 // default 50
	ReplyPerSec       float64 // replies to unauthenticated packets per source IP; default 40
	ReplyBurst        float64 // default 100
	GlobalReplyPerSec float64 // replies to unauthenticated packets overall; default 5000
	GlobalReplyBurst  float64 // default 5000
	// RETRY and PATH_CHALLENGE are what an honest client needs in order to make
	// progress, and they are smaller than the packet that draws them, so they
	// have an allowance of their own: a flood of forged packets that uses up
	// the budget above cannot stop real clients from starting a handshake.
	GlobalVitalReplyPerSec float64 // default 50000
	GlobalVitalReplyBurst  float64 // default 50000
	MaxTrackedIPs          int     // per-IP limiter entries; default 100000

	// WebSocket.
	WSSendQueue       int           // frames queued per WebSocket client before drops; default 128
	WSSendQueueBytes  int           // bytes queued per WebSocket client before drops; default 65536
	WSWriteTimeout    time.Duration // a WebSocket write that takes longer closes the connection; default 5 s
	AllowedOrigins    []string      // Origin hosts allowed to open a WebSocket or WebTransport session; empty or "*" = any
	TrustForwardedFor bool          // take the client IP from X-Forwarded-For (only behind a trusted proxy)

	// WebTransport. The connection is also bounded by SessionTimeout (QUIC's
	// idle timeout) and PingInterval (QUIC keep-alive).
	WTSendQueue      int           // datagrams queued per WebTransport client before drops; default 128
	WTSendQueueBytes int           // bytes queued per WebTransport client before drops; default 65536
	WTWriteTimeout   time.Duration // a control-stream write that takes longer closes the session; default 5 s

	// Test and logging hooks.
	Now  func() time.Time
	Logf func(format string, args ...any)
}

func (c Config) withDefaults() (Config, error) {
	def := func(v *int, d int) {
		if *v == 0 {
			*v = d
		}
	}
	defD := func(v *time.Duration, d time.Duration) {
		if *v == 0 {
			*v = d
		}
	}
	defF := func(v *float64, d float64) {
		if *v == 0 {
			*v = d
		}
	}
	def(&c.MaxMembers, 8)
	def(&c.MaxRooms, 1000)
	def(&c.MaxSessions, 20000)
	def(&c.MaxSessionsPerIP, 128)
	def(&c.MaxRoomsPerIP, 16)
	def(&c.MaxWSPerIP, 128)
	def(&c.MaxPendingWSPerIP, 32)
	def(&c.PrefixFactor, 4)
	def(&c.MaxWT, 4000)
	def(&c.MaxWTPerIP, 128)
	def(&c.MaxPendingWTPerIP, 32)
	def(&c.WTSendQueue, 128)
	def(&c.WTSendQueueBytes, 65536)
	defD(&c.WTWriteTimeout, 5*time.Second)
	def(&c.WSSendQueueBytes, 65536)
	defD(&c.WSWriteTimeout, 5*time.Second)
	defD(&c.RetryCookieBucket, 16*time.Second)
	defD(&c.PathChallengeInterval, time.Second)
	defD(&c.PathChallengeTimeout, 10*time.Second)
	defD(&c.RoomIdleTimeout, 30*time.Minute)
	defD(&c.KeepaliveInterval, 5*time.Second)
	defD(&c.SessionTimeout, 15*time.Second)
	defD(&c.HandshakeTimeout, 10*time.Second)
	defD(&c.NonceTTL, 60*time.Second)
	defD(&c.PingInterval, 5*time.Second)
	defF(&c.ClientPPS, 600)
	defF(&c.ClientPPSBurst, 2*c.ClientPPS)
	defF(&c.ClientBPS, 600000)
	defF(&c.ClientBPSBurst, 2*c.ClientBPS)
	defF(&c.HandshakePerSec, 20)
	defF(&c.HandshakeBurst, 50)
	defF(&c.ReplyPerSec, 40)
	defF(&c.ReplyBurst, 100)
	defF(&c.GlobalReplyPerSec, 5000)
	defF(&c.GlobalReplyBurst, 5000)
	defF(&c.GlobalVitalReplyPerSec, 50000)
	defF(&c.GlobalVitalReplyBurst, 50000)
	def(&c.MaxTrackedIPs, 100000)
	def(&c.WSSendQueue, 128)
	if c.Now == nil {
		c.Now = time.Now
	}
	if c.Logf == nil {
		c.Logf = func(string, ...any) {}
	}
	if c.MaxMembers < 2 || c.MaxMembers > 254 {
		return c, fmt.Errorf("MaxMembers must be 2..254, got %d", c.MaxMembers)
	}
	if c.MaxRooms < 0 || c.MaxSessions < 0 || c.WSSendQueue < 0 || c.WTSendQueue < 0 || c.MaxWT < 0 || c.MaxWTPerIP < 0 || c.MaxPendingWTPerIP < 0 {
		return c, fmt.Errorf("negative limit")
	}
	if c.KeepaliveInterval < time.Second {
		return c, fmt.Errorf("KeepaliveInterval must be at least 1 s (it is advertised in whole seconds)")
	}
	if 2*c.RetryCookieBucket >= c.NonceTTL {
		return c, fmt.Errorf("2 * RetryCookieBucket (%v) must be shorter than NonceTTL (%v): a handshake cookie must have expired before the nonce of a finished session is forgotten, or a late replay could resurrect a room", 2*c.RetryCookieBucket, c.NonceTTL)
	}
	if c.KeepaliveInterval*3 > c.SessionTimeout {
		return c, fmt.Errorf("KeepaliveInterval (%v) must be at most a third of SessionTimeout (%v), or healthy clients would time out", c.KeepaliveInterval, c.SessionTimeout)
	}
	return c, nil
}
