// SPDX-License-Identifier: MIT

package relay

import (
	"net/netip"
	"sync/atomic"
	"time"
)

// connKind names the stream transports (UDP has no connection object).
type connKind int

const (
	kindWS connKind = iota // WebSocket: every frame is a binary message
	kindWT                 // WebTransport: control frames on one stream, DATA in datagrams
)

// clientConn is a client that is connected to the relay over a connection
// (WebSocket or WebTransport) rather than over the shared UDP socket. The hub
// only needs these operations, so a transport is one more implementation.
//
// enqueue, close and reject may be called from any goroutine, including with
// the hub's lock held (the hub delivers while it routes), so they never
// block: a connection that cannot keep up loses frames, as on a lossy
// network, and one that is hopelessly stuck is closed.
type clientConn interface {
	kind() connKind
	clientIP() netip.Addr // the address as seen (before ipKey)
	acceptedAt() time.Time
	getSession() *session
	swapSession(*session) *session
	// takePending reports, once, whether the connection still holds its
	// "not yet in a room" slot of the per-address cap.
	takePending() bool

	enqueue(b []byte)   // queue a frame for the client (it takes ownership of b)
	reject(reason byte) // send ERROR(reason), then close
	close()

	// transportLive reports whether the transport itself proves the client is
	// alive (WebTransport: its QUIC connection is up), so the session timeout
	// need not rely on frames.
	transportLive() bool
}

// connCounters is the part of a connection that every kind has.
type connState struct {
	pendingHeld atomic.Bool
}

func (s *connState) takePending() bool { return s.pendingHeld.CompareAndSwap(true, false) }
