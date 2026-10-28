// SPDX-License-Identifier: MIT

package relay

import (
	"net/netip"
	"sync"
	"time"
)

// bucket is a token bucket. It is not safe for concurrent use; callers
// serialise access.
type bucket struct {
	tokens float64
	last   time.Time
	init   bool
}

// allow takes cost tokens, refilled at rate per second up to burst.
func (b *bucket) allow(now time.Time, rate, burst, cost float64) bool {
	if !b.init {
		b.init = true
		b.tokens = burst
		b.last = now
	} else if now.After(b.last) {
		b.tokens += now.Sub(b.last).Seconds() * rate
		if b.tokens > burst {
			b.tokens = burst
		}
		b.last = now
	}
	if b.tokens < cost {
		return false
	}
	b.tokens -= cost
	return true
}

type ipEntry struct {
	handshake bucket
	reply     bucket
	last      time.Time
}

// ipLimiter rate-limits unauthenticated traffic per source IP: handshakes
// (which create state) and replies (which could be reflected at a victim).
type ipLimiter struct {
	mu     sync.Mutex
	cfg    *Config
	m      map[netip.Addr]*ipEntry
	global bucket // handshakes of every source once the table is full
	greply bucket // replies to unauthenticated packets, overall
}

func newIPLimiter(cfg *Config) *ipLimiter {
	return &ipLimiter{cfg: cfg, m: make(map[netip.Addr]*ipEntry)}
}

func (l *ipLimiter) entry(ip netip.Addr, now time.Time) *ipEntry {
	e := l.m[ip]
	if e == nil {
		if len(l.m) >= l.cfg.MaxTrackedIPs {
			return nil
		}
		e = &ipEntry{}
		l.m[ip] = e
	}
	e.last = now
	return e
}

// allowHandshake reports whether ip may start another handshake.
func (l *ipLimiter) allowHandshake(ip netip.Addr, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	e := l.entry(ip, now)
	if e == nil { // table full: every unknown source shares the global bucket
		return l.global.allow(now, l.cfg.HandshakePerSec, l.cfg.HandshakeBurst, 1)
	}
	return e.handshake.allow(now, l.cfg.HandshakePerSec, l.cfg.HandshakeBurst, 1)
}

// allowReply reports whether the relay may answer an unauthenticated packet
// from ip: it must pass both the per-IP bucket and the global one.
func (l *ipLimiter) allowReply(ip netip.Addr, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	if e := l.entry(ip, now); e != nil {
		if !e.reply.allow(now, l.cfg.ReplyPerSec, l.cfg.ReplyBurst, 1) {
			return false
		}
	}
	return l.greply.allow(now, l.cfg.GlobalReplyPerSec, l.cfg.GlobalReplyBurst, 1)
}

// gc drops entries that have been quiet long enough to be full again.
func (l *ipLimiter) gc(now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for ip, e := range l.m {
		if now.Sub(e.last) > time.Minute {
			delete(l.m, ip)
		}
	}
}
