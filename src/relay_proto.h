// SPDX-License-Identifier: MIT
// Wire constants of the vnet relay protocol. Keep in step with
// relay/protocol.go and doco/relay-protocol.md.
#ifndef VNET_RELAY_PROTO_H
#define VNET_RELAY_PROTO_H

#include "vnet.h"

#define RELAY_PROTO_VERSION 1

#define RELAY_TOKEN_LEN 16
#define RELAY_NONCE_LEN 8
#define RELAY_CODE_LEN 6

// Every UDP datagram that carries CREATE_ROOM or JOIN_ROOM is at least this
// long (zero padded): replies to unauthenticated packets are always smaller.
#define RELAY_MIN_UDP_HANDSHAKE 64

#define RELAY_T_CREATE 0x01
#define RELAY_T_JOIN 0x02
#define RELAY_T_LEAVE 0x03
#define RELAY_T_KEEPALIVE 0x04
#define RELAY_T_PATH_CHALLENGE 0x05  /* relay to client: prove you receive at this address */
#define RELAY_T_PATH_RESPONSE 0x06   /* client to relay (UDP): type token cookie[8] */
#define RELAY_T_DATA 0x10
#define RELAY_T_ROOM 0x81
#define RELAY_T_ERROR 0x82

#define RELAY_E_BAD_VERSION 1
#define RELAY_E_ROOM_NOT_FOUND 2
#define RELAY_E_ROOM_FULL 3
#define RELAY_E_LIMIT 4
#define RELAY_E_RATE 5
#define RELAY_E_MALFORMED 6
#define RELAY_E_NO_SESSION 7
#define RELAY_E_CLOSED 8
#define RELAY_E_NONCE_REUSED 9
#define RELAY_E_TIMEOUT 10
#define RELAY_E_ALREADY_IN_ROOM 11

#define RELAY_COOKIE_LEN 8
#define RELAY_CHALLENGE_LEN (1 + RELAY_COOKIE_LEN)
#define RELAY_RESPONSE_LEN (1 + RELAY_TOKEN_LEN + RELAY_COOKIE_LEN)

// ROOM: type ver nonce[8] vaddr[4] token[16] code[6] max_members keepalive_s
#define RELAY_ROOM_LEN (1 + 1 + RELAY_NONCE_LEN + 4 + RELAY_TOKEN_LEN + RELAY_CODE_LEN + 1 + 1)
// ERROR: type ver nonce[8] reason
#define RELAY_ERROR_LEN (1 + 1 + RELAY_NONCE_LEN + 1)
// DATA header: type [token] addr[4] dst_port[2] src_port[2]
#define RELAY_DATA_HDR_WS (1 + 4 + 2 + 2)
#define RELAY_DATA_HDR_UDP (RELAY_DATA_HDR_WS + RELAY_TOKEN_LEN)
// The largest frame in either direction.
#define RELAY_MAX_FRAME (RELAY_DATA_HDR_UDP + VNET_MAX_DATAGRAM)

#endif
