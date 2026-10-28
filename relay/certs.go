// SPDX-License-Identifier: MIT

package relay

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"fmt"
	"math/big"
	"net"
	"os"
	"sync"
	"time"
)

// DevCertValidity is how long a development certificate is valid. Browsers
// that accept a certificate by its hash (WebTransport serverCertificateHashes)
// require a validity period of less than two weeks and an ECDSA P-256 key, so
// this is 13 days: a day of margin below the limit.
const DevCertValidity = 13 * 24 * time.Hour

// devCertSkew is how far before "now" a development certificate starts, for
// clients whose clocks run a little behind. It counts towards the validity
// period.
const devCertSkew = 5 * time.Minute

// NewDevCert makes a short-lived self-signed ECDSA P-256 certificate for
// local development and returns it with the SHA-256 hash of its DER encoding,
// hex encoded, which a page passes as serverCertificateHashes. The names it
// carries are localhost, 127.0.0.1 and ::1, plus any in extraNames.
func NewDevCert(now time.Time, extraNames ...string) (tls.Certificate, string, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 62))
	if err != nil {
		return tls.Certificate{}, "", err
	}
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "vnet relay development certificate"},
		NotBefore:             now.Add(-devCertSkew),
		NotAfter:              now.Add(-devCertSkew + DevCertValidity),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              []string{"localhost"},
		IPAddresses:           []net.IP{net.IPv4(127, 0, 0, 1), net.IPv6loopback},
	}
	for _, n := range extraNames {
		if ip := net.ParseIP(n); ip != nil {
			tmpl.IPAddresses = append(tmpl.IPAddresses, ip)
		} else {
			tmpl.DNSNames = append(tmpl.DNSNames, n)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, "", err
	}
	sum := sha256.Sum256(der)
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, hex.EncodeToString(sum[:]), nil
}

// CertReloader serves a certificate and key from files and picks up a
// renewed pair (certbot, Caddy, ACME clients replace the files) without a
// restart: the files' modification times are checked at most once per
// minute, on a handshake. A pair that fails to load is ignored and the
// previous one keeps being served.
type CertReloader struct {
	certFile, keyFile string
	logf              func(string, ...any)
	now               func() time.Time

	mu       sync.Mutex
	cert     *tls.Certificate
	certMod  time.Time
	keyMod   time.Time
	lastScan time.Time
}

const certRecheck = time.Minute

// NewCertReloader loads the pair once; it fails if that does not work.
func NewCertReloader(certFile, keyFile string, logf func(string, ...any)) (*CertReloader, error) {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	r := &CertReloader{certFile: certFile, keyFile: keyFile, logf: logf, now: time.Now}
	if err := r.load(); err != nil {
		return nil, err
	}
	return r, nil
}

func (r *CertReloader) load() error {
	cm, km, err := r.mods()
	if err != nil {
		return err
	}
	c, err := tls.LoadX509KeyPair(r.certFile, r.keyFile)
	if err != nil {
		return fmt.Errorf("loading %s and %s: %w", r.certFile, r.keyFile, err)
	}
	r.cert, r.certMod, r.keyMod = &c, cm, km
	return nil
}

func (r *CertReloader) mods() (cert, key time.Time, err error) {
	ci, err := os.Stat(r.certFile)
	if err != nil {
		return cert, key, err
	}
	ki, err := os.Stat(r.keyFile)
	if err != nil {
		return cert, key, err
	}
	return ci.ModTime(), ki.ModTime(), nil
}

// GetCertificate is a tls.Config.GetCertificate.
func (r *CertReloader) GetCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if now := r.now(); now.Sub(r.lastScan) >= certRecheck {
		r.lastScan = now
		if cm, km, err := r.mods(); err == nil && (!cm.Equal(r.certMod) || !km.Equal(r.keyMod)) {
			if err := r.load(); err != nil {
				r.logf("certificate not reloaded, keeping the old one: %v", err)
				r.certMod, r.keyMod = cm, km // do not retry the same broken files every minute
			} else {
				r.logf("certificate reloaded from %s", r.certFile)
			}
		}
	}
	return r.cert, nil
}
