// SPDX-License-Identifier: MIT

// Package relay implements the vnet relay: it puts clients (browser WebSocket
// and native UDP) into rooms and forwards datagrams between the members of a
// room. It never forwards to anything that is not a member of the sender's
// room. The wire format is documented in doco/relay-protocol.md.
package relay

import "encoding/binary"

// Protocol constants. Keep in step with src/relay_proto.h and
// doco/relay-protocol.md.
const (
	// ProtocolVersion is carried in CREATE_ROOM and JOIN_ROOM and echoed in
	// ROOM and ERROR.
	ProtocolVersion = 1

	// MaxPayload is VNET_MAX_DATAGRAM.
	MaxPayload = 1100

	TokenLen = 16
	NonceLen = 8
	CodeLen  = 6

	// MinUDPHandshake is the smallest UDP datagram carrying CREATE_ROOM or
	// JOIN_ROOM. Every reply to an unauthenticated datagram is smaller, so
	// the relay can never amplify.
	MinUDPHandshake = 64

	// Frame types, client to relay.
	TypeCreate       = 0x01
	TypeJoin         = 0x02
	TypeLeave        = 0x03
	TypeKeepalive    = 0x04 // both directions
	TypePathResponse = 0x06 // client to relay: answers a PATH_CHALLENGE
	TypeData         = 0x10 // both directions
	// Frame types, relay to client.
	TypePathChallenge = 0x05
	TypeRetry         = 0x07 // UDP: "prove you receive at this address, then ask again"
	TypeRoom          = 0x81
	TypeError         = 0x82
)

// Error reasons carried in ERROR frames.
const (
	ErrBadVersion    = 1  // unsupported protocol version; ver field = ours
	ErrRoomNotFound  = 2  // JOIN_ROOM: no such room
	ErrRoomFull      = 3  // JOIN_ROOM: no free address in the room
	ErrLimit         = 4  // server limit reached (rooms or sessions)
	ErrRate          = 5  // too many handshakes from this source
	ErrMalformed     = 6  // frame could not be parsed
	ErrNoSession     = 7  // unknown session token (expired, or relay restarted)
	ErrClosed        = 8  // session or room ended (also a late duplicate handshake)
	ErrNonceReused   = 9  // nonce already used for a different request
	ErrTimeout       = 10 // evicted for inactivity
	ErrAlreadyInRoom = 11 // WebSocket connection already has a session
)

// Frame sizes.
const (
	createLen    = 1 + 1 + NonceLen    // type ver nonce
	joinLen      = createLen + CodeLen // ... code
	roomLen      = 1 + 1 + NonceLen + 4 + TokenLen + CodeLen + 1 + 1
	errorLen     = 1 + 1 + NonceLen + 1
	cookieLen    = 8                            // PATH_CHALLENGE cookie
	CookieLen    = 16                           // handshake (RETRY) cookie
	retryLen     = 1 + 1 + NonceLen + CookieLen // RETRY: type ver nonce cookie
	challengeLen = 1 + cookieLen                // PATH_CHALLENGE: type cookie
	responseLen  = 1 + TokenLen + cookieLen     // PATH_RESPONSE: type token cookie (UDP only)
	udpDataHdr   = 1 + TokenLen + 4 + 2 + 2     // type token addr dport sport
	wsDataHdr    = 1 + 4 + 2 + 2                // type addr dport sport
	maxUDPFrame  = udpDataHdr + MaxPayload      // largest valid datagram
	maxWSMessage = maxUDPFrame                  // largest valid WebSocket message
)

// Alphabet of room codes: A-Z without I and O, and 2-9. 32 symbols, so that a
// code is 30 bits and there are no 0/O or 1/I look-alikes.
const codeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

// normaliseCode upper-cases b into a CodeLen string, reporting whether every
// symbol is in the alphabet.
func normaliseCode(b []byte) (string, bool) {
	if len(b) != CodeLen {
		return "", false
	}
	var out [CodeLen]byte
	for i, c := range b {
		if c >= 'a' && c <= 'z' {
			c -= 'a' - 'A'
		}
		ok := false
		for j := 0; j < len(codeAlphabet); j++ {
			if codeAlphabet[j] == c {
				ok = true
				break
			}
		}
		if !ok {
			return "", false
		}
		out[i] = c
	}
	return string(out[:]), true
}

func putVAddr(b []byte, octet byte) {
	b[0], b[1], b[2], b[3] = 10, 0, 0, octet
}

// vaddrOctet returns the host octet of a room address, or 0 if a is not in
// 10.0.0.0/24 (and so not a possible member).
func vaddrOctet(a []byte) byte {
	if a[0] == 10 && a[1] == 0 && a[2] == 0 {
		return a[3]
	}
	return 0
}

func encodeError(reason byte, nonce [NonceLen]byte) []byte {
	b := make([]byte, errorLen)
	b[0] = TypeError
	b[1] = ProtocolVersion
	copy(b[2:], nonce[:])
	b[2+NonceLen] = reason
	return b
}

func encodeRetry(nonce [NonceLen]byte, cookie [CookieLen]byte) []byte {
	b := make([]byte, retryLen)
	b[0] = TypeRetry
	b[1] = ProtocolVersion
	copy(b[2:], nonce[:])
	copy(b[2+NonceLen:], cookie[:])
	return b
}

func encodeRoom(nonce [NonceLen]byte, octet byte, token [TokenLen]byte, code string, maxMembers int, keepaliveS int) []byte {
	b := make([]byte, roomLen)
	b[0] = TypeRoom
	b[1] = ProtocolVersion
	copy(b[2:], nonce[:])
	putVAddr(b[2+NonceLen:], octet)
	copy(b[2+NonceLen+4:], token[:])
	copy(b[2+NonceLen+4+TokenLen:], code)
	b[2+NonceLen+4+TokenLen+CodeLen] = byte(maxMembers)
	b[2+NonceLen+4+TokenLen+CodeLen+1] = byte(keepaliveS)
	return b
}

// dataHeader describes a parsed DATA frame (either direction).
type dataFrame struct {
	token   [TokenLen]byte // zero over WebSocket
	addr    [4]byte        // destination when sent by a client
	dstPort uint16
	srcPort uint16
	payload []byte
}

// parseData parses a client DATA frame (frame[0] is already known to be
// TypeData). udp selects the layout with a token. ok is false if the frame is
// too short or the payload exceeds MaxPayload.
func parseData(frame []byte, udp bool) (d dataFrame, ok bool) {
	hdr := wsDataHdr
	if udp {
		hdr = udpDataHdr
	}
	if len(frame) < hdr || len(frame)-hdr > MaxPayload {
		return d, false
	}
	p := frame[1:]
	if udp {
		copy(d.token[:], p[:TokenLen])
		p = p[TokenLen:]
	}
	copy(d.addr[:], p[:4])
	d.dstPort = binary.BigEndian.Uint16(p[4:6])
	d.srcPort = binary.BigEndian.Uint16(p[6:8])
	d.payload = p[8:]
	return d, true
}

// encodeForward builds the relay-to-client DATA frame (no token) for a packet
// from member `src`, appending to buf[:0].
func encodeForward(buf []byte, src byte, d *dataFrame) []byte {
	buf = append(buf[:0], TypeData, 10, 0, 0, src,
		byte(d.dstPort>>8), byte(d.dstPort), byte(d.srcPort>>8), byte(d.srcPort))
	return append(buf, d.payload...)
}
