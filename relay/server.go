// SPDX-License-Identifier: MIT

package relay

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"net/netip"
	"sync"
	"time"
)

// Server runs a Hub over UDP and WebSocket.
type Server struct {
	cfg Config
	hub *Hub

	mu   sync.Mutex
	udp  *net.UDPConn
	wg   sync.WaitGroup
	stop chan struct{}
	once sync.Once
}

// New validates cfg and returns a server that is not yet listening.
func New(cfg Config) (*Server, error) {
	cfg, err := cfg.withDefaults()
	if err != nil {
		return nil, err
	}
	s := &Server{cfg: cfg, hub: newHub(cfg), stop: make(chan struct{})}
	return s, nil
}

// Stats returns a snapshot of the counters.
func (s *Server) Stats() Stats { return s.hub.snapshot() }

// Sweep expires stale members, rooms and nonces once. Run does this every
// second; tests with a fake clock call it directly.
func (s *Server) Sweep() { s.hub.Sweep() }

// ListenUDP binds a UDP socket and starts reading it. addr "127.0.0.1:0"
// picks a free port; the bound address is returned.
func (s *Server) ListenUDP(addr string) (net.Addr, error) {
	ua, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", ua)
	if err != nil {
		return nil, err
	}
	_ = conn.SetReadBuffer(4 << 20)
	_ = conn.SetWriteBuffer(4 << 20)
	s.mu.Lock()
	s.udp = conn
	s.mu.Unlock()
	s.hub.sendUDP = func(dst netip.AddrPort, b []byte) { _, _ = conn.WriteToUDPAddrPort(b, dst) }
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		// One reader: datagrams are processed in arrival order, which keeps
		// replies ordered per source and keeps the hot path simple.
		buf := make([]byte, 4096)
		scratch := make([]byte, 0, maxUDPFrame)
		for {
			n, src, err := conn.ReadFromUDPAddrPort(buf)
			if err != nil {
				if errors.Is(err, net.ErrClosed) {
					return
				}
				continue
			}
			s.hub.HandleUDP(buf[:n], src, &scratch)
		}
	}()
	return conn.LocalAddr(), nil
}

// Handler serves /vnet (WebSocket), /healthz and /stats.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/vnet", s.serveWS)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok\n"))
	})
	mux.HandleFunc("/stats", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(s.Stats())
	})
	return mux
}

// Run sweeps once a second until ctx is cancelled, then closes the server.
func (s *Server) Run(ctx context.Context) {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			s.Close()
			return
		case <-s.stop:
			return
		case <-t.C:
			s.hub.Sweep()
		}
	}
}

// Close stops the UDP reader and closes every WebSocket.
func (s *Server) Close() {
	s.once.Do(func() {
		close(s.stop)
		s.mu.Lock()
		if s.udp != nil {
			_ = s.udp.Close()
		}
		s.mu.Unlock()
		s.hub.mu.RLock()
		var conns []*wsConn
		for c := range s.hub.conns {
			conns = append(conns, c)
		}
		s.hub.mu.RUnlock()
		for _, c := range conns {
			c.close()
		}
		s.wg.Wait()
	})
}
