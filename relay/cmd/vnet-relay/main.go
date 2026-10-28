// SPDX-License-Identifier: MIT

// Command vnet-relay is the vnet relay server: one static binary serving
// native UDP clients and browser WebSocket clients. Terminate TLS in front of
// it (Caddy, nginx) for wss://.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/VenKamikaze/vnet/relay"
)

// Every flag can also be set from the environment as VNET_RELAY_<NAME>
// (upper case, '-' -> '_'); the flag wins.
func envName(flagName string) string {
	return "VNET_RELAY_" + strings.ToUpper(strings.ReplaceAll(flagName, "-", "_"))
}

func main() {
	fs := flag.NewFlagSet("vnet-relay", flag.ExitOnError)
	var (
		udpAddr    = fs.String("udp", ":7777", "UDP listen address for native clients (empty disables)")
		httpAddr   = fs.String("http", ":8080", "HTTP listen address; WebSocket clients connect to /vnet (empty disables)")
		maxMembers = fs.Int("max-members", 8, "members per room, including the creator (2..254)")
		maxRooms   = fs.Int("max-rooms", 1000, "rooms at once")
		maxSess    = fs.Int("max-sessions", 20000, "members at once (also caps open WebSockets)")
		perIPSess  = fs.Int("max-sessions-per-ip", 64, "members at once per source address (IPv6: per /64); many players behind one NAT share it")
		perIPRooms = fs.Int("max-rooms-per-ip", 16, "rooms created and still alive per source address")
		perIPWS    = fs.Int("max-ws-per-ip", 64, "open WebSockets per source address")
		perIPPend  = fs.Int("max-pending-ws-per-ip", 8, "WebSockets per source address that have not entered a room yet")
		wsQBytes   = fs.Int("ws-queue-bytes", 65536, "bytes queued per WebSocket client before its datagrams are dropped")
		statsAddr  = fs.String("stats", "", "serve /stats (JSON counters) on this address, e.g. 127.0.0.1:8081; off by default")
		roomIdle   = fs.Duration("room-idle", 30*time.Minute, "close a room that has carried no data for this long")
		sessTO     = fs.Duration("session-timeout", 15*time.Second, "drop a member silent for this long")
		keepalive  = fs.Duration("keepalive", 5*time.Second, "keepalive interval advertised to clients")
		pps        = fs.Float64("client-pps", 600, "per-client sustained packets per second")
		bps        = fs.Float64("client-bps", 600000, "per-client sustained bytes per second")
		hsRate     = fs.Float64("handshake-rate", 20, "CREATE_ROOM/JOIN_ROOM per second per source IP (many players behind one NAT share it)")
		hsBurst    = fs.Float64("handshake-burst", 50, "handshake burst per source IP")
		origins    = fs.String("origins", "*", "comma-separated Origin hosts allowed to open a WebSocket, or *")
		trustXFF   = fs.Bool("trust-forwarded-for", false, "take client IPs from X-Forwarded-For (only behind a trusted proxy)")
		verbose    = fs.Bool("v", false, "log room creation and closure")
	)
	// Environment first, then the command line on top.
	fs.VisitAll(func(f *flag.Flag) {
		if v, ok := os.LookupEnv(envName(f.Name)); ok {
			if err := fs.Set(f.Name, v); err != nil {
				log.Fatalf("%s=%q: %v", envName(f.Name), v, err)
			}
		}
	})
	_ = fs.Parse(os.Args[1:])

	cfg := relay.Config{
		MaxMembers:        *maxMembers,
		MaxRooms:          *maxRooms,
		MaxSessions:       *maxSess,
		MaxSessionsPerIP:  *perIPSess,
		MaxRoomsPerIP:     *perIPRooms,
		MaxWSPerIP:        *perIPWS,
		MaxPendingWSPerIP: *perIPPend,
		WSSendQueueBytes:  *wsQBytes,
		RoomIdleTimeout:   *roomIdle,
		SessionTimeout:    *sessTO,
		KeepaliveInterval: *keepalive,
		ClientPPS:         *pps,
		ClientBPS:         *bps,
		HandshakePerSec:   *hsRate,
		HandshakeBurst:    *hsBurst,
		ReplyPerSec:       *hsRate,
		ReplyBurst:        *hsBurst,
		TrustForwardedFor: *trustXFF,
	}
	if *origins != "" && *origins != "*" {
		cfg.AllowedOrigins = strings.Split(*origins, ",")
	}
	if *verbose {
		cfg.Logf = log.Printf
	}
	srv, err := relay.New(cfg)
	if err != nil {
		log.Fatal(err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	var udpBound, httpBound string
	if *udpAddr != "" {
		a, err := srv.ListenUDP(*udpAddr)
		if err != nil {
			log.Fatalf("udp: %v", err)
		}
		udpBound = a.String()
	}
	var httpSrv *http.Server
	if *httpAddr != "" {
		ln, err := net.Listen("tcp", *httpAddr)
		if err != nil {
			log.Fatalf("http: %v", err)
		}
		httpBound = ln.Addr().String()
		httpSrv = &http.Server{Handler: srv.Handler(), ReadHeaderTimeout: 10 * time.Second}
		go func() {
			if err := httpSrv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Fatalf("http: %v", err)
			}
		}()
	}
	var statsBound string
	var statsSrv *http.Server
	if *statsAddr != "" {
		ln, err := net.Listen("tcp", *statsAddr)
		if err != nil {
			log.Fatalf("stats: %v", err)
		}
		statsBound = ln.Addr().String()
		statsSrv = &http.Server{Handler: srv.StatsHandler(), ReadHeaderTimeout: 10 * time.Second}
		go func() { _ = statsSrv.Serve(ln) }()
	}
	// One machine-readable line for scripts and tests: the bound addresses
	// (useful with port 0).
	fmt.Printf("LISTEN udp=%s http=%s stats=%s\n", udpBound, httpBound, statsBound)
	_ = os.Stdout.Sync()

	srv.Run(ctx) // returns on SIGINT/SIGTERM after closing the server
	if statsSrv != nil {
		_ = statsSrv.Close()
	}
	if httpSrv != nil {
		shutdown, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = httpSrv.Shutdown(shutdown)
	}
}
