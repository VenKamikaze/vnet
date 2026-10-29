// SPDX-License-Identifier: MIT

// Command vnet-loadtest drives a running vnet relay with many rooms of
// synthetic clients and reports delivery, latency and the relay process's CPU
// and memory. It is a test tool, not part of the relay.
//
//	vnet-loadtest -udp 127.0.0.1:7777 -http 127.0.0.1:8080 -pid 1234 \
//	    -rooms 50 -clients 4 -pps 30 -duration 30s
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/quic-go/quic-go"
	"github.com/quic-go/webtransport-go"
)

const (
	typeCreate    = 0x01
	typeJoin      = 0x02
	typeKeepalive = 0x04
	typeData      = 0x10
	typeRoom      = 0x81
	version       = 1
	payloadLen    = 100
)

type client struct {
	udp     *net.UDPConn
	ws      *websocket.Conn
	wt      *webtransport.Session // WebTransport: control frames on ctl, DATA as datagrams
	ctl     *webtransport.Stream
	wtRx    chan []byte
	wtStop  context.CancelFunc
	token   [16]byte
	octet   byte
	code    string
	sent    atomic.Uint64
	recv    atomic.Uint64
	latMu   sync.Mutex
	lat     []time.Duration
	members int
}

// tokenless: WebSocket and WebTransport frames carry no session token.
func (c *client) tokenless() bool { return c.ws != nil || c.wt != nil }

func (c *client) write(b []byte) error {
	if c.wt != nil {
		if b[0] == typeData {
			return c.wt.SendDatagram(b)
		}
		_, err := c.ctl.Write(append([]byte{byte(len(b) >> 8), byte(len(b))}, b...))
		return err
	}
	if c.ws != nil {
		return c.ws.WriteMessage(websocket.BinaryMessage, b)
	}
	_, err := c.udp.Write(b)
	return err
}

func (c *client) read(b []byte) (int, error) {
	if c.wt != nil {
		select {
		case m, ok := <-c.wtRx:
			if !ok {
				return 0, io.EOF
			}
			return copy(b, m), nil
		case <-time.After(2 * time.Second):
			return 0, errors.New("timeout")
		}
	}
	if c.ws != nil {
		_, m, err := c.ws.ReadMessage()
		return copy(b, m), err
	}
	return c.udp.Read(b)
}

func (c *client) close() {
	switch {
	case c.wt != nil:
		c.wtStop()
		_ = c.wt.CloseWithError(0, "")
	case c.ws != nil:
		c.ws.Close()
	default:
		c.udp.Close()
	}
}

// dialWT opens a WebTransport session whose certificate must have the given
// SHA-256 (a relay -dev-cert), as a browser's serverCertificateHashes does.
func dialWT(url, hexHash string) (*client, error) {
	want, err := hex.DecodeString(hexHash)
	if err != nil || len(want) != 32 {
		return nil, errors.New("-wt-hash must be 64 hex digits")
	}
	d := &webtransport.Dialer{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13,
			VerifyPeerCertificate: func(raw [][]byte, _ [][]*x509.Certificate) error {
				if len(raw) == 0 {
					return errors.New("no certificate")
				}
				if sum := sha256.Sum256(raw[0]); !bytes.Equal(sum[:], want) {
					return errors.New("certificate hash mismatch")
				}
				return nil
			}},
		QUICConfig: &quic.Config{EnableDatagrams: true, EnableStreamResetPartialDelivery: true},
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_, sess, err := d.Dial(ctx, url, nil)
	if err != nil {
		return nil, err
	}
	rctx, stop := context.WithCancel(context.Background())
	st, err := sess.OpenStreamSync(ctx)
	if err != nil {
		stop()
		return nil, err
	}
	c := &client{wt: sess, ctl: st, wtRx: make(chan []byte, 1024), wtStop: stop}
	go func() { // control frames, length prefixed
		var hdr [2]byte
		for {
			if _, err := io.ReadFull(st, hdr[:]); err != nil {
				return
			}
			b := make([]byte, int(hdr[0])<<8|int(hdr[1]))
			if _, err := io.ReadFull(st, b); err != nil {
				return
			}
			select {
			case c.wtRx <- b:
			case <-rctx.Done():
				return
			}
		}
	}()
	go func() { // datagrams
		for {
			b, err := sess.ReceiveDatagram(rctx)
			if err != nil {
				return
			}
			select {
			case c.wtRx <- b:
			default: // a receiver that cannot keep up loses datagrams
			}
		}
	}()
	return c, nil
}

func handshake(c *client, code string) error {
	var nonce [8]byte
	_, _ = rand.Read(nonce[:])
	build := func(cookie []byte) []byte {
		f := []byte{typeCreate, version}
		f = append(f, nonce[:]...)
		if code != "" {
			f[0] = typeJoin
			f = append(f, code...)
		}
		if !c.tokenless() { // UDP: the retry cookie follows the fields, and the datagram is padded
			f = append(f, make([]byte, 16)...)
			if cookie != nil {
				copy(f[len(f)-16:], cookie)
			}
			if len(f) < 64 {
				f = append(f, make([]byte, 64-len(f))...)
			}
		}
		return f
	}
	buf := make([]byte, 2048)
	var cookie []byte
	for try := 0; try < 10; try++ {
		if err := c.write(build(cookie)); err != nil {
			return err
		}
		if c.udp != nil {
			_ = c.udp.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		}
		n, err := c.read(buf)
		if err != nil {
			continue
		}
		if n == 26 && buf[0] == 0x07 { // RETRY: repeat with the cookie
			cookie = append([]byte(nil), buf[10:26]...)
			continue
		}
		if n == 11 && buf[0] == 0x82 && buf[10] == 5 { // rate limited: back off, same request
			time.Sleep(200 * time.Millisecond)
			continue
		}
		if n == 38 && buf[0] == typeRoom {
			c.octet = buf[13]
			copy(c.token[:], buf[14:30])
			c.code = string(buf[30:36])
			return nil
		}
		return fmt.Errorf("unexpected reply % x", buf[:n])
	}
	return fmt.Errorf("no reply")
}

func (c *client) frame(dstOctet byte, payload []byte) []byte {
	b := []byte{typeData}
	if !c.tokenless() {
		b = append(b, c.token[:]...)
	}
	b = append(b, 10, 0, 0, dstOctet, 0x13, 0x88, 0x13, 0x88)
	return append(b, payload...)
}

func cpuSeconds(pid int) (float64, error) {
	b, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/stat")
	if err != nil {
		return 0, err
	}
	s := string(b)
	f := strings.Fields(s[strings.LastIndex(s, ")")+2:])
	ut, _ := strconv.ParseFloat(f[11], 64)
	st, _ := strconv.ParseFloat(f[12], 64)
	return (ut + st) / 100, nil // USER_HZ = 100
}

func statusKB(pid int, key string) int {
	b, err := os.ReadFile("/proc/" + strconv.Itoa(pid) + "/status")
	if err != nil {
		return -1
	}
	for _, l := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(l, key+":") {
			f := strings.Fields(l)
			v, _ := strconv.Atoi(f[1])
			return v
		}
	}
	return -1
}

func main() {
	udpAddr := flag.String("udp", "127.0.0.1:7777", "relay UDP address")
	wsURL := flag.String("ws", "", "relay WebSocket URL (used for the clients selected by -ws-every)")
	wsEvery := flag.Int("ws-every", 0, "every Nth client uses WebSocket (0: none, 1: all)")
	wtURL := flag.String("wt", "", "relay WebTransport URL (used for the clients selected by -wt-every)")
	wtHash := flag.String("wt-hash", "", "SHA-256 of the relay's -dev-cert certificate, hex")
	wtEvery := flag.Int("wt-every", 0, "every Nth client uses WebTransport (0: none, 1: all); checked before -ws-every")
	pid := flag.Int("pid", 0, "relay process id, to report its CPU and memory")
	rooms := flag.Int("rooms", 50, "rooms")
	members := flag.Int("clients", 4, "clients per room")
	pps := flag.Int("pps", 30, "packets per second per client")
	fanout := flag.Int("fanout", 1, "destinations per tick: 1 = round robin over the others, N = N others at once")
	dur := flag.Duration("duration", 30*time.Second, "how long to send")
	flag.Parse()

	var all []*client
	roomsOf := make([][]*client, *rooms)
	idx := 0
	for r := 0; r < *rooms; r++ {
		code := ""
		for m := 0; m < *members; m++ {
			c := &client{members: *members}
			if *wtEvery > 0 && idx%*wtEvery == 0 {
				w, err := dialWT(*wtURL, *wtHash)
				if err != nil {
					fmt.Fprintln(os.Stderr, "wt dial:", err)
					os.Exit(1)
				}
				w.members = *members
				c = w
			} else if *wsEvery > 0 && idx%*wsEvery == 0 {
				conn, _, err := websocket.DefaultDialer.Dial(*wsURL, nil)
				if err != nil {
					fmt.Fprintln(os.Stderr, "ws dial:", err)
					os.Exit(1)
				}
				c.ws = conn
			} else {
				ua, _ := net.ResolveUDPAddr("udp", *udpAddr)
				conn, err := net.DialUDP("udp", nil, ua)
				if err != nil {
					fmt.Fprintln(os.Stderr, "udp dial:", err)
					os.Exit(1)
				}
				c.udp = conn
			}
			idx++
			if err := handshake(c, code); err != nil {
				fmt.Fprintf(os.Stderr, "room %d client %d: %v\n", r, m, err)
				os.Exit(1)
			}
			if m == 0 {
				code = c.code
			}
			roomsOf[r] = append(roomsOf[r], c)
			all = append(all, c)
		}
	}
	fmt.Printf("%d rooms x %d clients = %d clients, %d pps each, fanout %d, %v\n",
		*rooms, *members, len(all), *pps, *fanout, *dur)

	// Receivers.
	var wg sync.WaitGroup
	for _, c := range all {
		wg.Add(1)
		go func(c *client) {
			defer wg.Done()
			buf := make([]byte, 2048)
			for {
				if c.udp != nil {
					_ = c.udp.SetReadDeadline(time.Now().Add(2 * time.Second))
				}
				n, err := c.read(buf)
				if err != nil {
					return
				}
				if n == 9+payloadLen && buf[0] == typeData {
					ts := int64(binary.BigEndian.Uint64(buf[9 : 9+8]))
					d := time.Duration(time.Now().UnixNano() - ts)
					c.recv.Add(1)
					c.latMu.Lock()
					c.lat = append(c.lat, d)
					c.latMu.Unlock()
				}
			}
		}(c)
	}

	var cpu0 float64
	if *pid > 0 {
		cpu0, _ = cpuSeconds(*pid)
	}
	start := time.Now()
	stop := start.Add(*dur)
	var sw sync.WaitGroup
	for r, rc := range roomsOf {
		for i, c := range rc {
			sw.Add(1)
			go func(r, i int, c *client, rc []*client) {
				defer sw.Done()
				period := time.Second / time.Duration(*pps)
				// Spread clients over the tick so the load is not one burst.
				time.Sleep(time.Duration(int64(period) * int64(r*len(rc)+i) / int64(len(roomsOf)*len(rc))))
				t := time.NewTicker(period)
				defer t.Stop()
				payload := make([]byte, payloadLen)
				rr := 0
				for now := range t.C {
					if now.After(stop) {
						return
					}
					binary.BigEndian.PutUint64(payload, uint64(time.Now().UnixNano()))
					for k := 0; k < *fanout; k++ {
						// Another member of the same room, round robin.
						dst := rc[(i+1+(rr%(len(rc)-1)))%len(rc)]
						rr++
						if err := c.write(c.frame(dst.octet, payload)); err == nil {
							c.sent.Add(1)
						}
					}
					if int(c.sent.Load())%(*pps*3) == 0 { // an occasional keepalive, like a real client
						if c.tokenless() {
							_ = c.write([]byte{typeKeepalive})
						} else {
							_ = c.write(append([]byte{typeKeepalive}, c.token[:]...))
						}
					}
				}
			}(r, i, c, rc)
		}
	}
	sw.Wait()
	elapsed := time.Since(start)
	time.Sleep(500 * time.Millisecond) // let the last packets arrive
	var cpu1 float64
	if *pid > 0 {
		cpu1, _ = cpuSeconds(*pid)
	}
	rss, hwm := statusKB(*pid, "VmRSS"), statusKB(*pid, "VmHWM")
	for _, c := range all {
		c.close()
	}
	wg.Wait()

	var sent, recv uint64
	var lats []time.Duration
	for _, c := range all {
		sent += c.sent.Load()
		recv += c.recv.Load()
		lats = append(lats, c.lat...)
	}
	sort.Slice(lats, func(i, j int) bool { return lats[i] < lats[j] })
	pct := func(p float64) time.Duration {
		if len(lats) == 0 {
			return 0
		}
		return lats[int(float64(len(lats)-1)*p)]
	}
	fmt.Printf("sent %d, received %d (%.3f%% lost), %.0f packets/s into the relay\n",
		sent, recv, 100*float64(sent-recv)/float64(sent), float64(sent)/elapsed.Seconds())
	fmt.Printf("latency through the relay (client to client, loopback): p50 %v  p99 %v  max %v\n",
		pct(.5).Round(time.Microsecond), pct(.99).Round(time.Microsecond), pct(1).Round(time.Microsecond))
	if *pid > 0 {
		fmt.Printf("relay CPU: %.2f s over %.1f s = %.1f%% of one core\n", cpu1-cpu0, elapsed.Seconds(), 100*(cpu1-cpu0)/elapsed.Seconds())
		fmt.Printf("relay memory: RSS %.1f MB, peak (VmHWM) %.1f MB\n", float64(rss)/1024, float64(hwm)/1024)
	}
}
