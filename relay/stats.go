// SPDX-License-Identifier: MIT

package relay

import "sync/atomic"

type stats struct {
	rxFrames          atomic.Uint64 // UDP datagrams and WebSocket messages received
	txFrames          atomic.Uint64 // UDP datagrams sent
	malformed         atomic.Uint64 // frames that could not be parsed or were too short
	oversize          atomic.Uint64 // frames longer than any valid frame
	rateLimited       atomic.Uint64 // frames dropped by a per-client or per-source limit
	unroutable        atomic.Uint64 // DATA whose destination is not a member of the sender's room
	dataForwarded     atomic.Uint64
	unknownToken      atomic.Uint64 // authenticated-type frames with an unknown token
	handshakeRejected atomic.Uint64
	handshakeRetries  atomic.Uint64 // CREATE/JOIN repeats answered idempotently
	repliesSuppressed atomic.Uint64 // unauthenticated replies not sent (rate limit, size rule)
	roomsCreated      atomic.Uint64
	roomsClosed       atomic.Uint64 // emptied
	roomsIdleClosed   atomic.Uint64
	sessionsJoined    atomic.Uint64
	sessionsTimedOut  atomic.Uint64
	wsQueueDrops      atomic.Uint64 // frames dropped because a WebSocket client was too slow
	retriesSent       atomic.Uint64 // RETRY frames (stateless: no state is kept for them)
	badCookies        atomic.Uint64 // handshakes whose cookie was missing, expired or for another source
	wsRefused         atomic.Uint64 // WebSocket upgrades refused (global or per-address cap)
	wtRefused         atomic.Uint64 // WebTransport (QUIC) connections refused (global or per-address cap)
	wtDatagramDrops   atomic.Uint64 // datagrams dropped because a WebTransport client was too slow (or the datagram did not fit)
	wtCtlOverflow     atomic.Uint64 // WebTransport sessions closed because their control stream could not keep up
	wtEarlyDatagrams  atomic.Uint64 // datagrams that arrived before the session had entered a room
	wtBadStreams      atomic.Uint64 // streams other than the one control stream, reset on arrival
	pathChallenges    atomic.Uint64
	pathMigrations    atomic.Uint64 // sessions moved after a validated path change
	badPathResponses  atomic.Uint64
	panics            atomic.Uint64 // recovered panics (a bug; the relay keeps running)
}

// Stats is a snapshot of the relay's counters and gauges.
type Stats struct {
	Rooms             int    `json:"rooms"`
	Sessions          int    `json:"sessions"`
	WebSockets        int    `json:"websockets"`
	WebTransports     int    `json:"webtransports"`
	RxFrames          uint64 `json:"rx_frames"`
	TxFrames          uint64 `json:"tx_frames"`
	Malformed         uint64 `json:"malformed"`
	Oversize          uint64 `json:"oversize"`
	RateLimited       uint64 `json:"rate_limited"`
	Unroutable        uint64 `json:"unroutable"`
	DataForwarded     uint64 `json:"data_forwarded"`
	UnknownToken      uint64 `json:"unknown_token"`
	HandshakeRejected uint64 `json:"handshake_rejected"`
	HandshakeRetries  uint64 `json:"handshake_retries"`
	RepliesSuppressed uint64 `json:"replies_suppressed"`
	RoomsCreated      uint64 `json:"rooms_created"`
	RoomsClosed       uint64 `json:"rooms_closed"`
	RoomsIdleClosed   uint64 `json:"rooms_idle_closed"`
	SessionsJoined    uint64 `json:"sessions_joined"`
	SessionsTimedOut  uint64 `json:"sessions_timed_out"`
	WSQueueDrops      uint64 `json:"ws_queue_drops"`
	RetriesSent       uint64 `json:"retries_sent"`
	BadCookies        uint64 `json:"bad_cookies"`
	WSRefused         uint64 `json:"ws_refused"`
	WTRefused         uint64 `json:"wt_refused"`
	WTDatagramDrops   uint64 `json:"wt_datagram_drops"`
	WTCtlOverflow     uint64 `json:"wt_ctl_overflow"`
	WTEarlyDatagrams  uint64 `json:"wt_early_datagrams"`
	WTBadStreams      uint64 `json:"wt_bad_streams"`
	PathChallenges    uint64 `json:"path_challenges"`
	PathMigrations    uint64 `json:"path_migrations"`
	BadPathResponses  uint64 `json:"bad_path_responses"`
	Panics            uint64 `json:"panics"`
}

func (h *Hub) snapshot() Stats {
	h.mu.RLock()
	rooms, sessions := len(h.rooms), len(h.byToken)
	var wss, wts int
	for c := range h.conns {
		if c.kind() == kindWT {
			wts++
		} else {
			wss++
		}
	}
	h.mu.RUnlock()
	s := &h.stats
	return Stats{
		Rooms: rooms, Sessions: sessions, WebSockets: wss, WebTransports: wts,
		RxFrames: s.rxFrames.Load(), TxFrames: s.txFrames.Load(),
		Malformed: s.malformed.Load(), Oversize: s.oversize.Load(),
		RateLimited: s.rateLimited.Load(), Unroutable: s.unroutable.Load(),
		DataForwarded: s.dataForwarded.Load(), UnknownToken: s.unknownToken.Load(),
		HandshakeRejected: s.handshakeRejected.Load(), HandshakeRetries: s.handshakeRetries.Load(),
		RepliesSuppressed: s.repliesSuppressed.Load(),
		RoomsCreated:      s.roomsCreated.Load(), RoomsClosed: s.roomsClosed.Load(),
		RoomsIdleClosed: s.roomsIdleClosed.Load(), SessionsJoined: s.sessionsJoined.Load(),
		SessionsTimedOut: s.sessionsTimedOut.Load(), WSQueueDrops: s.wsQueueDrops.Load(),
		RetriesSent: s.retriesSent.Load(), BadCookies: s.badCookies.Load(), WSRefused: s.wsRefused.Load(), WTRefused: s.wtRefused.Load(),
		WTDatagramDrops: s.wtDatagramDrops.Load(), WTCtlOverflow: s.wtCtlOverflow.Load(),
		WTEarlyDatagrams: s.wtEarlyDatagrams.Load(), WTBadStreams: s.wtBadStreams.Load(), PathChallenges: s.pathChallenges.Load(),
		PathMigrations: s.pathMigrations.Load(), BadPathResponses: s.badPathResponses.Load(),
		Panics: s.panics.Load(),
	}
}
