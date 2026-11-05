// SPDX-License-Identifier: MIT

package relay

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net/netip"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/quic-go/webtransport-go"
)

// A fake WebTransport session: the hub's WebTransport code runs against it
// with no QUIC underneath, so a stuck datagram queue, a control stream that
// blocks in Write, and concurrent writes can be provoked deterministically.

type fakeWTStream struct {
	t    *testing.T
	in   chan []byte     // client to relay (the relay's Read); close = the client ended the stream
	dead <-chan struct{} // the session is gone: Read fails, as it does when the QUIC connection closes
	once sync.Once

	rmu  sync.Mutex
	left []byte

	out    chan []byte // relay to client: one entry per Write call
	gate   atomic.Pointer[chan struct{}]
	inW    atomic.Int32
	wdl    atomic.Int64 // write deadline, unix nanos
	cancel atomic.Int32 // CancelRead/CancelWrite calls
}

func newFakeWTStream(t *testing.T) *fakeWTStream {
	return &fakeWTStream{t: t, in: make(chan []byte, 4096), out: make(chan []byte, 4096)}
}

func (f *fakeWTStream) Read(p []byte) (int, error) {
	f.rmu.Lock()
	defer f.rmu.Unlock()
	if len(f.left) == 0 {
		select {
		case b, ok := <-f.in:
			if !ok {
				return 0, io.EOF
			}
			f.left = b
		case <-f.dead:
			return 0, errors.New("stream reset: session gone")
		}
	}
	n := copy(p, f.left)
	f.left = f.left[n:]
	return n, nil
}

// Write fails the test if two writes ever overlap: the relay must serialise
// its writes to the control stream.
func (f *fakeWTStream) Write(p []byte) (int, error) {
	if f.inW.Add(1) > 1 {
		f.t.Error("concurrent Write on the control stream")
	}
	defer f.inW.Add(-1)
	if g := f.gate.Load(); g != nil {
		dl := time.Unix(0, f.wdl.Load())
		select {
		case <-*g:
		case <-time.After(time.Until(dl)):
			return 0, os.ErrDeadlineExceeded
		}
	}
	f.out <- append([]byte(nil), p...)
	return len(p), nil
}

func (f *fakeWTStream) endInput() { f.once.Do(func() { close(f.in) }) }

func (f *fakeWTStream) SetReadDeadline(time.Time) error { return nil }
func (f *fakeWTStream) SetWriteDeadline(t time.Time) error {
	f.wdl.Store(t.UnixNano())
	return nil
}
func (f *fakeWTStream) CancelRead(webtransport.StreamErrorCode)  { f.cancel.Add(1) }
func (f *fakeWTStream) CancelWrite(webtransport.StreamErrorCode) { f.cancel.Add(1) }

type fakeUni struct{ cancelled *atomic.Int32 }

func (u fakeUni) CancelRead(webtransport.StreamErrorCode) { u.cancelled.Add(1) }

type fakeWT struct {
	t      *testing.T
	ctx    context.Context
	cancel context.CancelFunc

	bidi  chan wtStream
	uni   chan wtUniStream
	dgIn  chan []byte
	dgOut chan []byte

	sendGate atomic.Pointer[chan struct{}] // while set, SendDatagram blocks
	inSend   atomic.Int32
	uniSeen  atomic.Int32

	closeCode   atomic.Int64 // code+1 of the first CloseWithError, 0 if none
	maxDatagram atomic.Int64 // largest datagram SendDatagram accepts; 0 = 1200
}

func newFakeWT(t *testing.T) *fakeWT {
	ctx, cancel := context.WithCancel(context.Background())
	return &fakeWT{t: t, ctx: ctx, cancel: cancel,
		bidi: make(chan wtStream, 64), uni: make(chan wtUniStream, 64),
		dgIn: make(chan []byte, 8192), dgOut: make(chan []byte, 8192)}
}

func (f *fakeWT) AcceptStream(ctx context.Context) (wtStream, error) {
	select {
	case s := <-f.bidi:
		return s, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-f.ctx.Done():
		return nil, errors.New("session closed")
	}
}

func (f *fakeWT) AcceptUniStream(ctx context.Context) (wtUniStream, error) {
	select {
	case s := <-f.uni:
		return s, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-f.ctx.Done():
		return nil, errors.New("session closed")
	}
}

func (f *fakeWT) ReceiveDatagram(ctx context.Context) ([]byte, error) {
	select {
	case b := <-f.dgIn:
		return b, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

// SendDatagram fails the test if two calls overlap (the relay sends from one
// goroutine), and blocks while the gate is set, like quic-go does when its
// 32-datagram queue is full.
func (f *fakeWT) SendDatagram(b []byte) error {
	if f.inSend.Add(1) > 1 {
		f.t.Error("concurrent SendDatagram")
	}
	defer f.inSend.Add(-1)
	if g := f.sendGate.Load(); g != nil {
		select {
		case <-*g:
		case <-f.ctx.Done():
			return errors.New("session closed")
		}
	}
	limit := int(f.maxDatagram.Load())
	if limit == 0 {
		limit = 1200
	}
	if len(b) > limit {
		return errWTDatagramTooLarge
	}
	select {
	case f.dgOut <- append([]byte(nil), b...):
	default:
	}
	return nil
}

func (f *fakeWT) CloseWithError(code uint32, msg string) error {
	f.closeCode.CompareAndSwap(0, int64(code)+1)
	f.cancel()
	return nil
}

func (f *fakeWT) Context() context.Context { return f.ctx }

// fwt is a client of the hub over a fake WebTransport session.
type fwt struct {
	t      *testing.T
	th     *testHub
	c      *wtConn
	f      *fakeWT
	ctl    *fakeWTStream
	served chan struct{} // closed when serveWTSession has returned
	room   roomReply
	nonce  uint64
}

// wtClient admits a connection from ip and serves a fake session on it. The
// control stream is opened (the relay does not see it until the client writes).
func (th *testHub) wtClient(ip string) *fwt {
	th.t.Helper()
	c, ok := th.h.wtAdmit(netip.MustParseAddr(ip))
	if !ok {
		th.t.Fatalf("connection from %s was refused", ip)
	}
	w := &fwt{t: th.t, th: th, c: c, f: newFakeWT(th.t), ctl: newFakeWTStream(th.t), served: make(chan struct{})}
	w.ctl.dead = w.f.ctx.Done()
	w.f.bidi <- w.ctl
	go func() {
		defer close(w.served)
		th.h.serveWTSession(c, w.f)
	}()
	th.t.Cleanup(func() {
		w.f.cancel()
		w.ctl.endInput()
		th.h.wtGone(c)
	})
	return w
}

// ctlFrame is a length-prefixed control frame.
func ctlFrame(frame []byte) []byte {
	b := make([]byte, 2+len(frame))
	binary.BigEndian.PutUint16(b, uint16(len(frame)))
	copy(b[2:], frame)
	return b
}

func (w *fwt) ctlSend(frame []byte) { w.ctl.in <- ctlFrame(frame) }
func (w *fwt) ctlRaw(b []byte)      { w.ctl.in <- append([]byte(nil), b...) }
func (w *fwt) dg(b []byte)          { w.f.dgIn <- append([]byte(nil), b...) }

// ctlRecv returns the next control frame (without its length), nil on timeout.
func (w *fwt) ctlRecv() []byte {
	select {
	case b := <-w.ctl.out:
		if len(b) < 2 || int(binary.BigEndian.Uint16(b)) != len(b)-2 {
			w.t.Fatalf("the relay wrote a badly framed control message: % x", b)
		}
		return b[2:]
	case <-time.After(waitLimit):
		return nil
	}
}

func (w *fwt) dgRecv() []byte {
	select {
	case b := <-w.f.dgOut:
		return b
	case <-time.After(waitLimit):
		return nil
	}
}

func (w *fwt) handshake(code string) *fwt {
	w.t.Helper()
	w.nonce = nonceCounter.Add(1) + 1<<42
	w.ctlSend(handshakeFrame(w.nonce, code, 0))
	b := w.ctlRecv()
	if b == nil {
		w.t.Fatal("no reply to the handshake")
	}
	w.room = parseRoom(w.t, b)
	return w
}
func (w *fwt) create() *fwt          { return w.handshake("") }
func (w *fwt) join(code string) *fwt { return w.handshake(code) }

func (w *fwt) octet() byte { return w.room.octet }

func (w *fwt) dataFrame(dstOctet byte, dport, sport uint16, payload []byte) []byte {
	b := []byte{TypeData, 10, 0, 0, dstOctet, byte(dport >> 8), byte(dport), byte(sport >> 8), byte(sport)}
	return append(b, payload...)
}

// syncData returns once the datagrams sent before have been handled by the
// relay (it sends one to itself and waits for it), together with the datagrams
// that arrived before that marker.
func (w *fwt) syncData() [][]byte {
	w.t.Helper()
	marker := []byte("sync-marker-" + time.Now().Format("150405.000000000"))
	w.dg(w.dataFrame(w.room.octet, 1, 1, marker))
	var out [][]byte
	for {
		b := w.dgRecv()
		if b == nil {
			w.t.Fatal("the sync marker never came back")
		}
		if len(b) >= wsDataHdr && string(b[wsDataHdr:]) == string(marker) {
			return out
		}
		out = append(out, b)
	}
}

// syncCtl returns once the control frames sent before have been handled (a
// KEEPALIVE and its answer), with the control frames received before the answer.
func (w *fwt) syncCtl() [][]byte {
	w.t.Helper()
	w.ctlSend([]byte{TypeKeepalive})
	var out [][]byte
	for {
		b := w.ctlRecv()
		if b == nil {
			w.t.Fatal("no KEEPALIVE answer")
		}
		if len(b) == 1 && b[0] == TypeKeepalive {
			return out
		}
		out = append(out, b)
	}
}

// waitEnded waits for the relay to be done with the session.
func (w *fwt) waitEnded() {
	w.t.Helper()
	select {
	case <-w.served:
	case <-time.After(waitLimit):
		w.t.Fatal("the relay did not end the session")
	}
}

func (w *fwt) ended() bool {
	select {
	case <-w.served:
		return true
	default:
		return false
	}
}
