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
