/**
 * @file binary_server.h
 * @brief Raw-binary protocol server on top of libreactor's stream/descriptor API.
 *
 * Bypasses the HTTP-coupled reactor `server_*` layer. Listens on a TCP port,
 * accepts connections, and exchanges length-prefixed binary packets. Two codecs
 * are multiplexed by a 1-byte type tag:
 *   0x01 = raw fixed-struct player_state {u8 type, u16 x, u16 y, u8 hp} (6 bytes)
 *   0x02 = MessagePack map {x:u16, y:u16, hp:u8}
 *
 * Per packet the server mutates state (x+1, y+1, hp-1 floor 0) and echoes it
 * back in the same codec. Same mutation for both → codec overhead is the only
 * measured delta.
 *
 * Wire framing: [u16 len BE][payload of len bytes]. len covers payload only.
 */
#ifndef INFRASTRUCTURE_BINARY_SERVER_H
#define INFRASTRUCTURE_BINARY_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#include <reactor.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Binary protocol port (isolated from HTTP 3984). */
#define BINARY_SERVER_DEFAULT_PORT 3985

/** Packet type tags (first payload byte). */
#define BINARY_PKT_RAW     0x01
#define BINARY_PKT_MSGPACK 0x02

/** Maximum payload we will accept (guards against a bogus length prefix). */
#define BINARY_MAX_PAYLOAD 256

typedef struct binary_conn      binary_conn;
typedef struct binary_server    binary_server;

struct binary_server
{
  descriptor descriptor;   /* listening socket, registered with the reactor */
  list       connections;  /* active binary_conn list */
  bool       is_open;
};

/** Construct/destruct a binary server (does not bind). */
void binary_server_construct(binary_server *bs);
void binary_server_destruct(binary_server *bs);

/** Bind the listening fd and register it with the current reactor loop. */
void binary_server_open(binary_server *bs, int fd);

/** Stop accepting and tear down all connections. */
void binary_server_shutdown(binary_server *bs);

#ifdef __cplusplus
}
#endif

#endif /* INFRASTRUCTURE_BINARY_SERVER_H */
