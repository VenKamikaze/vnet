module github.com/VenKamikaze/vnet/relay

go 1.24

require (
	github.com/gorilla/websocket v1.5.3
	github.com/quic-go/quic-go v0.59.0
	github.com/quic-go/webtransport-go v0.10.0
)

require (
	github.com/dunglas/httpsfv v1.1.0 // indirect
	github.com/quic-go/qpack v0.6.0 // indirect
	golang.org/x/crypto v0.41.0 // indirect
	golang.org/x/net v0.43.0 // indirect
	golang.org/x/sys v0.35.0 // indirect
	golang.org/x/text v0.28.0 // indirect
)

// quic-go v0.59.0 with two constants changed, both marked VNET PATCH: the
// receive queues for datagrams, per session (http3, 32 upstream) and per
// connection (128 upstream), both raised to 1024. See relay/README.md.
replace github.com/quic-go/quic-go => ./third_party/quic-go
