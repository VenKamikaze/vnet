// SPDX-License-Identifier: MIT

package relay

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/pem"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// What a browser demands of a certificate it accepts by hash
// (serverCertificateHashes): X.509v3, ECDSA P-256 (no RSA), valid now, and a
// validity period of less than two weeks.
func TestDevCertIsWhatBrowsersAcceptByHash(t *testing.T) {
	now := time.Now()
	cert, hash, err := NewDevCert(now, "relay.test", "192.0.2.7")
	if err != nil {
		t.Fatal(err)
	}
	x, err := x509.ParseCertificate(cert.Certificate[0])
	if err != nil {
		t.Fatal(err)
	}
	if x.Version != 3 {
		t.Errorf("version %d", x.Version)
	}
	pub, ok := x.PublicKey.(*ecdsa.PublicKey)
	if !ok || pub.Curve != elliptic.P256() {
		t.Errorf("public key is %T, want ECDSA P-256", x.PublicKey)
	}
	if v := x.NotAfter.Sub(x.NotBefore); v >= 14*24*time.Hour || v < 24*time.Hour {
		t.Errorf("validity period %v, want less than 14 days", v)
	}
	if x.NotBefore.After(now) || x.NotAfter.Before(now.Add(12*24*time.Hour)) {
		t.Errorf("not valid now and for the next 12 days: %v .. %v", x.NotBefore, x.NotAfter)
	}
	sum := sha256.Sum256(cert.Certificate[0])
	if hash != hex.EncodeToString(sum[:]) || len(hash) != 64 {
		t.Errorf("hash %q is not the SHA-256 of the DER certificate", hash)
	}
	pool := x509.NewCertPool()
	pool.AddCert(x)
	for _, name := range []string{"localhost", "relay.test"} {
		if _, err := x.Verify(x509.VerifyOptions{Roots: pool, DNSName: name, CurrentTime: now}); err != nil {
			t.Errorf("does not verify for %s: %v", name, err)
		}
	}
	if !x.IPAddresses[0].Equal(net.IPv4(127, 0, 0, 1)) || len(x.IPAddresses) != 3 || !strings.Contains(strings.Join(x.DNSNames, ","), "relay.test") {
		t.Errorf("names: %v %v", x.DNSNames, x.IPAddresses)
	}
	_, hash2, _ := NewDevCert(now)
	if hash2 == hash {
		t.Error("two development certificates are identical")
	}
}

func writePair(t *testing.T, dir, name string, cert tls.Certificate) (certFile, keyFile string) {
	t.Helper()
	certFile, keyFile = filepath.Join(dir, name+".crt"), filepath.Join(dir, name+".key")
	der, err := x509.MarshalECPrivateKey(cert.PrivateKey.(*ecdsa.PrivateKey))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert.Certificate[0]}), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	return
}

func TestCertReloaderPicksUpRenewedCertificatesAndKeepsGoodOnesOnErrors(t *testing.T) {
	dir := t.TempDir()
	a, hashA, _ := NewDevCert(time.Now())
	b, hashB, _ := NewDevCert(time.Now())
	crt, key := writePair(t, dir, "live", a)
	var mu sync.Mutex
	var logs []string
	r, err := NewCertReloader(crt, key, func(f string, args ...any) {
		mu.Lock()
		logs = append(logs, f)
		mu.Unlock()
	})
	if err != nil {
		t.Fatal(err)
	}
	clk := newFakeClock()
	r.now = clk.Now
	got := func() string {
		c, err := r.GetCertificate(nil)
		if err != nil {
			t.Fatal(err)
		}
		s := sha256.Sum256(c.Certificate[0])
		return hex.EncodeToString(s[:])
	}
	if got() != hashA {
		t.Fatal("initial certificate")
	}
	// Renewal: the files are replaced (new modification time). It is noticed
	// at the next check, at most a minute later, not on every handshake.
	bc, bk := writePair(t, dir, "new", b)
	if err := os.Rename(bc, crt); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(bk, key); err != nil {
		t.Fatal(err)
	}
	future := time.Now().Add(time.Hour)
	_ = os.Chtimes(crt, future, future)
	_ = os.Chtimes(key, future, future)
	if got() != hashA {
		t.Fatal("the files were re-read on every handshake")
	}
	clk.Advance(certRecheck + time.Second)
	if got() != hashB {
		t.Fatal("a renewed certificate was not picked up")
	}
	// A broken replacement is ignored; the good certificate keeps being served.
	if err := os.WriteFile(crt, []byte("garbage"), 0o600); err != nil {
		t.Fatal(err)
	}
	future = future.Add(time.Hour)
	_ = os.Chtimes(crt, future, future)
	clk.Advance(certRecheck + time.Second)
	if got() != hashB {
		t.Fatal("a broken certificate file replaced a good one")
	}
	mu.Lock()
	n := len(logs)
	mu.Unlock()
	if n == 0 {
		t.Error("the failed reload was not logged")
	}
	// A mismatched pair (new certificate, old key) is ignored too.
	cc, _ := writePair(t, dir, "mismatch", a)
	other, _ := os.ReadFile(cc)
	if err := os.WriteFile(crt, other, 0o600); err != nil { // cert A with key B
		t.Fatal(err)
	}
	future = future.Add(time.Hour)
	_ = os.Chtimes(crt, future, future)
	clk.Advance(certRecheck + time.Second)
	if got() != hashB {
		t.Fatal("a certificate that does not match the key was served")
	}
	// Starting with unusable files fails at once.
	if _, err := NewCertReloader(filepath.Join(dir, "none.crt"), key, nil); err == nil {
		t.Fatal("missing file accepted")
	}
}

// A certificate that is about to expire, or has, is reported in the log, so
// that a relay nobody is watching does not just stop being reachable over
// WebTransport. Reports are rate limited.
func TestCertificateExpiryIsReported(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	cert, _, _ := NewDevCert(now) // valid 13 days from 5 minutes before now
	var mu sync.Mutex
	var logs []string
	r, err := NewStaticCert(cert, func(f string, a ...any) {
		mu.Lock()
		logs = append(logs, fmt.Sprintf(f, a...))
		mu.Unlock()
	})
	if err != nil {
		t.Fatal(err)
	}
	at := now
	r.now = func() time.Time { return at }
	handshake := func() {
		if c, err := r.GetCertificate(nil); err != nil || c == nil {
			t.Fatalf("no certificate served: %v", err)
		}
	}
	got := func() []string { mu.Lock(); defer mu.Unlock(); return append([]string(nil), logs...) }
	handshake()
	at = now.Add(5 * 24 * time.Hour) // 8 days left: quiet
	handshake()
	if len(got()) != 0 {
		t.Fatalf("a certificate with 8 days left was reported: %v", got())
	}
	at = now.Add(7 * 24 * time.Hour) // 6 days left: a warning
	handshake()
	if l := got(); len(l) != 1 || !strings.Contains(l[0], "WARNING") || !strings.Contains(l[0], "expires in") {
		t.Fatalf("no warning with 6 days left: %v", l)
	}
	handshake() // rate limited
	at = at.Add(30 * time.Minute)
	handshake()
	if len(got()) != 1 {
		t.Fatalf("the warning is not rate limited: %v", got())
	}
	at = at.Add(time.Hour) // again, an hour later
	handshake()
	if len(got()) != 2 {
		t.Fatalf("no second warning an hour later: %v", got())
	}
	at = now.Add(14 * 24 * time.Hour) // expired: an error, and the certificate is still served
	handshake()
	if l := got(); len(l) != 3 || !strings.Contains(l[2], "ERROR") || !strings.Contains(l[2], "expired") {
		t.Fatalf("no error for an expired certificate: %v", l)
	}
}

func TestExpiryIsTrackedAcrossReloads(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	soon, _, _ := NewDevCert(now.Add(-12 * 24 * time.Hour)) // expires in about a day
	fresh, _, _ := NewDevCert(now)
	crt, key := writePair(t, dir, "live", soon)
	var logs []string
	r, err := NewCertReloader(crt, key, func(f string, a ...any) { logs = append(logs, fmt.Sprintf(f, a...)) })
	if err != nil {
		t.Fatal(err)
	}
	clk := newFakeClock()
	clk.t = now
	r.now = clk.Now
	_, _ = r.GetCertificate(nil)
	if len(logs) != 1 || !strings.Contains(logs[0], "WARNING") {
		t.Fatalf("an old certificate was not reported at the first handshake: %v", logs)
	}
	// Renewed: the new expiry is the one that counts.
	nc, nk := writePair(t, dir, "new", fresh)
	_ = os.Rename(nc, crt)
	_ = os.Rename(nk, key)
	future := now.Add(time.Hour)
	_ = os.Chtimes(crt, future, future)
	_ = os.Chtimes(key, future, future)
	clk.Advance(2 * time.Hour) // past the report interval and the recheck interval
	logs = nil
	_, _ = r.GetCertificate(nil)
	for _, l := range logs {
		if strings.Contains(l, "expire") {
			t.Fatalf("the renewed certificate was still reported as expiring: %v", logs)
		}
	}
}

func TestDevCertOnAReachableAddressIsWarnedAbout(t *testing.T) {
	for _, tc := range []struct {
		addr string
		warn bool
	}{{"127.0.0.1:4433", false}, {"[::1]:4433", false}, {"0.0.0.0:4433", true}, {"[::]:4433", true}, {"192.0.2.7:4433", true}} {
		ua, err := net.ResolveUDPAddr("udp", tc.addr)
		if err != nil {
			t.Fatal(err)
		}
		w := DevCertWarning(ua)
		if (w != "") != tc.warn || (tc.warn && !strings.Contains(w, "local trials only")) {
			t.Errorf("%s: warning %q, want one: %v", tc.addr, w, tc.warn)
		}
	}
}
