/**
 * @file binary_server.c
 * @brief Raw-binary protocol server on libreactor stream/descriptor API.
 *
 * Bypasses the HTTP-coupled reactor `server_*`. Per connection: read
 * length-prefixed packets, decode, mutate state, encode, flush. Two codecs
 * (raw fixed-struct, MessagePack) multiplexed by a 1-byte type tag.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <endian.h>
#include <errno.h>

#include <sys/socket.h>

#ifdef HAVE_MSGPACK
#include <msgpack.h>
#endif

#include "../../include/infrastructure/binary_server.h"

/* ---- player state + codec ------------------------------------------------ */

#pragma pack(push, 1)
typedef struct {
  uint8_t  type;   /* BINARY_PKT_RAW */
  uint16_t x;
  uint16_t y;
  uint8_t  hp;
} player_state_raw;   /* 6 bytes, network byte order */
#pragma pack(pop)

static inline void player_mutate(uint16_t *x, uint16_t *y, uint8_t *hp)
{
  *x  = (uint16_t)((*x + 1) & 0xFFFF);
  *y  = (uint16_t)((*y + 1) & 0xFFFF);
  *hp = (uint8_t)(*hp == 0 ? 0 : *hp - 1);
}

/* ---- per-connection state ------------------------------------------------ *
 * The stream input buffer coalesces partial reads; we decode as many full
 * packets as are available, consuming each, mirroring server.c:96.
 */
struct binary_conn
{
  stream    stream;
  int       active;
};

typedef struct binary_conn binary_conn;

/* forward decls */
static void binary_conn_callback(reactor_event *event);
static void binary_conn_process(binary_conn *conn);

/* ---- accept / lifecycle -------------------------------------------------- */

static void binary_conn_open(binary_server *bs, int fd)
{
  binary_conn *conn = list_push_back(&bs->connections, NULL, sizeof *conn);
  if (!conn)
  {
    close(fd);
    return;
  }
  conn->active = 1;
  stream_construct(&conn->stream, binary_conn_callback, conn);
  stream_open(&conn->stream, fd, STREAM_READ, NULL);
}

static void binary_conn_close(binary_conn *conn)
{
  if (!conn->active)
    return;
  conn->active = 0;
  stream_close(&conn->stream);
  list_erase(conn, NULL);
}

/* listener readiness: accept loop, cap 32/cb (mirrors server.c:171-186) */
static void binary_listener_callback(reactor_event *event)
{
  binary_server *bs = event->state;
  int n = 0;
  while (n < 32)
  {
    int fd = accept4(descriptor_fd(&bs->descriptor), NULL, NULL, SOCK_NONBLOCK);
    if (fd == -1)
      break;
    binary_conn_open(bs, fd);
    n++;
  }
}

/* ---- packet processing --------------------------------------------------- *
 * Drain all complete packets from the input buffer. Each packet:
 *   [u16 len BE] [payload len bytes]
 * payload[0] = type tag, rest = codec-specific.
 */
static void binary_conn_process(binary_conn *conn)
{
  for (;;)
  {
    data d = stream_read(&conn->stream);
    size_t avail = data_size(d);
    if (avail < 2)
      return;  /* need length prefix */

    const uint8_t *p = data_base(d);
    uint16_t len = ((uint16_t)p[0] << 8) | p[1];
    if (len < 1 || len > BINARY_MAX_PAYLOAD)
    {
      /* malformed length — drop the connection, don't crash. */
      binary_conn_close(conn);
      return;
    }
    if (avail < (size_t)2 + len)
      return;  /* full payload not yet received */

    const uint8_t *payload = p + 2;
    uint8_t type = payload[0];

    if (type == BINARY_PKT_RAW)
    {
      if (len != sizeof(player_state_raw))
      {
        binary_conn_close(conn);
        return;
      }
      player_state_raw ps;
      memcpy(&ps, payload, sizeof ps);
      /* network -> host */
      ps.x = be16toh(ps.x);
      ps.y = be16toh(ps.y);
      player_mutate(&ps.x, &ps.y, &ps.hp);
      ps.x = htobe16(ps.x);
      ps.y = htobe16(ps.y);
      /* reply: same framing */
      uint8_t out[2 + sizeof ps];
      out[0] = (uint8_t)(sizeof ps >> 8);
      out[1] = (uint8_t)(sizeof ps & 0xFF);
      memcpy(out + 2, &ps, sizeof ps);
      stream_write(&conn->stream, data_construct((const char *)out, sizeof out));
    }
#ifdef HAVE_MSGPACK
    else if (type == BINARY_PKT_MSGPACK)
    {
      msgpack_unpacked msg;
      msgpack_unpacked_init(&msg);
      msgpack_unpack_return ur =
        msgpack_unpack_next(&msg, (const char *)(payload + 1), len - 1, NULL);
      if (ur != MSGPACK_UNPACK_SUCCESS ||
          msg.data.type != MSGPACK_OBJECT_MAP ||
          msg.data.via.map.size != 3)
      {
        msgpack_unpacked_destroy(&msg);
        binary_conn_close(conn);
        return;
      }
      uint16_t x = 0, y = 0;
      uint8_t  hp = 0;
      for (uint32_t i = 0; i < msg.data.via.map.size; i++)
      {
        msgpack_object_kv *kv = &msg.data.via.map.ptr[i];
        const char *key = kv->key.via.str.ptr;
        if (key[0] == 'x' && kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
          x = (uint16_t)kv->val.via.u64;
        else if (key[0] == 'y' && kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
          y = (uint16_t)kv->val.via.u64;
        else if (key[0] == 'h' && kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
          hp = (uint8_t)kv->val.via.u64;
      }
      msgpack_unpacked_destroy(&msg);
      player_mutate(&x, &y, &hp);

      /* re-encode: map of 3 {x,y,hp} with uint8 keys */
      msgpack_sbuffer sbuf;
      msgpack_sbuffer_init(&sbuf);
      msgpack_packer pk;
      msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
      msgpack_pack_map(&pk, 3);
      msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "x", 1);
      msgpack_pack_uint16(&pk, x);
      msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "y", 1);
      msgpack_pack_uint16(&pk, y);
      msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "h", 1);
      msgpack_pack_uint8(&pk, hp);

      uint8_t out[2 + BINARY_MAX_PAYLOAD];
      out[0] = (uint8_t)((sbuf.size + 1) >> 8);   /* payload = tag + msgpack */
      out[1] = (uint8_t)((sbuf.size + 1) & 0xFF);
      out[2] = BINARY_PKT_MSGPACK;
      memcpy(out + 3, sbuf.data, sbuf.size);
      stream_write(&conn->stream,
                   data_construct((const char *)out, 2 + 1 + sbuf.size));
      msgpack_sbuffer_destroy(&sbuf);
    }
#endif
    else
    {
      /* unknown type tag */
      binary_conn_close(conn);
      return;
    }

    stream_flush(&conn->stream);
    /* consumed: 2-byte length prefix + payload */
    stream_consume(&conn->stream, 2 + len);
  }
}

static void binary_conn_callback(reactor_event *event)
{
  binary_conn *conn = event->state;
  switch (event->type)
  {
  case STREAM_READ:
    binary_conn_process(conn);
    break;
  default:
  case STREAM_CLOSE:
    binary_conn_close(conn);
    break;
  }
}

/* ---- public API ---------------------------------------------------------- */

void binary_server_construct(binary_server *bs)
{
  memset(bs, 0, sizeof *bs);
  descriptor_construct(&bs->descriptor, binary_listener_callback, bs);
  list_construct(&bs->connections);
}

void binary_server_destruct(binary_server *bs)
{
  binary_server_shutdown(bs);
}

void binary_server_open(binary_server *bs, int fd)
{
  descriptor_open(&bs->descriptor, fd, DESCRIPTOR_READ);
  bs->is_open = 1;
}

void binary_server_shutdown(binary_server *bs)
{
  if (!bs->is_open)
    return;
  bs->is_open = 0;

  /* close all connections */
  binary_conn *conn = list_front(&bs->connections);
  while (conn != list_end(&bs->connections))
  {
    binary_conn *next = list_next(conn);
    conn->active = 0;
    stream_close(&conn->stream);
    list_erase(conn, NULL);
    conn = next;
  }
  descriptor_destruct(&bs->descriptor);
}
