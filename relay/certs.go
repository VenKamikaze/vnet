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

	notAfter time.Time // end of validity of the certificate being served
	lastNote time.Time // when an expiry warning was last logged
}

const (
	certRecheck = time.Minute
	// A certificate with less than this left is reported at every handshake
	// after certNoteEvery since the last report; one that has expired too.
	// (Let's Encrypt certificates are renewed with 30 days to go, so a warning
	// at 7 days means the renewal is failing; a development certificate lasts
	// 13 days, so it warns from the sixth.)
	certWarnBefore = 7 * 24 * time.Hour
	certNoteEvery  = time.Hour
)

// NewStaticCert serves one certificate that is never reloaded (the development
// certificate), with the same expiry warnings as a CertReloader.
func NewStaticCert(cert tls.Certificate, logf func(string, ...any)) (*CertReloader, error) {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	r := &CertReloader{logf: logf, now: time.Now, cert: &cert}
	if err := r.parseExpiry(); err != nil {
		return nil, err
	}
	return r, nil
}

func (r *CertReloader) parseExpiry() error {
	if len(r.cert.Certificate) == 0 {
		return fmt.Errorf("empty certificate")
	}
	x, err := x509.ParseCertificate(r.cert.Certificate[0])
	if err != nil {
		return err
	}
	r.notAfter = x.NotAfter
	return nil
}

// noteExpiry logs a warning when the certificate is close to the end of its
// validity and an error when it has passed it, at most once per certNoteEvery.
// Browsers refuse an expired certificate, and a relay that nobody watches
// would otherwise just stop being reachable over WebTransport.
func (r *CertReloader) noteExpiry(now time.Time) {
	left := r.notAfter.Sub(now)
	if left >= certWarnBefore || (!r.lastNote.IsZero() && now.Sub(r.lastNote) < certNoteEvery) {
		return
	}
	r.lastNote = now
	if left <= 0 {
		r.logf("ERROR: the WebTransport certificate expired at %s; browsers refuse it, so WebTransport clients fall back to WebSocket or fail. Renew it (or restart, with -dev-cert)", r.notAfter.Format(time.RFC3339))
		return
	}
	r.logf("WARNING: the WebTransport certificate expires in %s (at %s)", left.Round(time.Minute), r.notAfter.Format(time.RFC3339))
}

// DevCertWarning returns a warning to log if a development certificate is
// being served on an address that other machines can reach (empty if it is a
// loopback address): such a relay cannot be used by pages that pin the
// certificate hash from elsewhere either, and is almost certainly a mistake.
func DevCertWarning(addr net.Addr) string {
	ua, ok := addr.(*net.UDPAddr)
	if ok && ua.IP != nil && ua.IP.IsLoopback() {
		return ""
	}
	return fmt.Sprintf("WARNING: -dev-cert on %s, which is not a loopback address: a development certificate is for local trials only (it lasts 13 days, changes at every start and is accepted only by pages that pin its hash). Use -wt-cert and -wt-key in production", addr)
}

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
	prev := r.cert
	r.cert, r.certMod, r.keyMod = &c, cm, km
	if err := r.parseExpiry(); err != nil {
		r.cert = prev
		return fmt.Errorf("parsing %s: %w", r.certFile, err)
	}
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
	now := r.now()
	defer r.noteExpiry(now)
	if r.certFile != "" && now.Sub(r.lastScan) >= certRecheck {
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
