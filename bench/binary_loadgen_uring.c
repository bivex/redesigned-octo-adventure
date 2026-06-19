/**
 * @file binary_loadgen_uring.c
 * @brief io_uring binary load generator — the binary analogue of wrk.
 *
 * Same wire protocol as libreactor-binary-server / binary-loadgen, but driven
 * by a single io_uring ring: many in-flight RECV/SEND SQEs per connection,
 * capped by --depth (the binary equivalent of wrk's HTTP pipeline depth).
 * This measures the real server ceiling, not RTT-bound ping-pong.
 *
 * Usage:
 *   binary-loadgen-uring --proto raw|msgpack [--host 127.0.0.1] [--port 3985]
 *                        [--conns N] [--depth N] [--duration S]
 *
 * Per connection: maintain `depth` outstanding round-trips. On a SEND CQE,
 * submit a matching RECV. On a RECV CQE, parse framed packets out of the
 * accumulated buffer, count each, and for each completed packet submit a fresh
 * SEND — keeping `depth` in flight.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/io_uring.h>

#include <liburing.h>

#ifdef HAVE_MSGPACK
#include <msgpack.h>
#endif

#define BINARY_PKT_RAW     0x01
#define BINARY_PKT_MSGPACK 0x02

#define RECV_BUF_SIZE 4096
#define MAX_CONNS     4096
#define RING_ENTRIES  8192

#pragma pack(push, 1)
typedef struct { uint8_t type; uint16_t x; uint16_t y; uint8_t hp; } player_state_raw;
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;
static void on_sigalrm(int s) { (void) s; g_stop = 1; }

/* SQE op tag (encoded in user_data low bits, like the server side). */
enum { OP_SEND = 0, OP_RECV = 1 };

typedef struct {
  int    fd;
  int    in_flight;          /* outstanding round-trips (sent, not yet replied) */
  int    target_depth;       /* desired in_flight */
  int    proto;
  /* recv accumulation */
  uint8_t rbuf[RECV_BUF_SIZE];
  size_t  rlen;
  /* current request state */
  uint16_t x, y;
  uint8_t  hp;
  uint64_t completed;
} conn_t;

static conn_t g_conns[MAX_CONNS];
static int g_nconns;

/* pre-encoded request templates (per-proto): 2-byte len + payload */
static uint8_t g_req_raw[2 + sizeof(player_state_raw)];
static size_t  g_req_raw_len;
#ifdef HAVE_MSGPACK
static uint8_t g_req_msgpack[64];
static size_t  g_req_msgpack_len;
#endif

static atomic_uint_least64_t g_total_completed;
static atomic_uint_least64_t g_total_errors;

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- request encoder (one-time) ----------------------------------------- */

static void build_templates(void)
{
  /* raw */
  player_state_raw ps = { .type = BINARY_PKT_RAW, .x = htobe16(1), .y = htobe16(2), .hp = 200 };
  g_req_raw[0] = 0; g_req_raw[1] = (uint8_t)sizeof ps;
  memcpy(g_req_raw + 2, &ps, sizeof ps);
  g_req_raw_len = 2 + sizeof ps;
#ifdef HAVE_MSGPACK
  msgpack_sbuffer sbuf; msgpack_sbuffer_init(&sbuf);
  msgpack_packer pk; msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
  msgpack_pack_map(&pk, 3);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "x", 1); msgpack_pack_uint16(&pk, 1);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "y", 1); msgpack_pack_uint16(&pk, 2);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "h", 1); msgpack_pack_uint8(&pk, 200);
  size_t plen = 1 + sbuf.size;
  g_req_msgpack[0] = (uint8_t)(plen >> 8); g_req_msgpack[1] = (uint8_t)(plen & 0xFF);
  g_req_msgpack[2] = BINARY_PKT_MSGPACK;
  memcpy(g_req_msgpack + 3, sbuf.data, sbuf.size);
  g_req_msgpack_len = 2 + plen;
  msgpack_sbuffer_destroy(&sbuf);
#endif
}

static const uint8_t *request_for(int proto, size_t *len)
{
  if (proto == BINARY_PKT_RAW) { *len = g_req_raw_len; return g_req_raw; }
#ifdef HAVE_MSGPACK
  return (*len = g_req_msgpack_len), g_req_msgpack;
#else
  *len = 0; return NULL;
#endif
}

/* ---- io_uring helpers ---------------------------------------------------- */

/* Pool of send buffers: each in-flight SEND SQE needs a stable buffer until it
 * completes. With pipeline depth D and N conns there can be up to D*N sends in
 * flight, so the pool is sized to RING_ENTRIES (the SQE capacity). Recycled on
 * SEND completion by SQE slot index (encoded in user_data). */
#define SEND_POOL_SIZE RING_ENTRIES
static uint8_t g_send_pool[SEND_POOL_SIZE][32];

/* tag: bits 0..1 = op, bits 2..15 = cid, bits 16..31 = send-pool slot (only
 * meaningful for OP_SEND). */
static inline void *tag_send_ud(int cid, int slot)
{
  return (void *)((uintptr_t)(slot << 16) | (uintptr_t)(cid << 2) | OP_SEND);
}
static inline void *tag_recv_ud(int cid)
{
  return (void *)((uintptr_t)(cid << 2) | OP_RECV);
}
static inline int ud_cid(uintptr_t ud) { return (int)((ud >> 2) & 0x3FFF); }
static inline int ud_op(uintptr_t ud)  { return (int)(ud & 3); }
static inline int ud_slot(uintptr_t ud){ return (int)(ud >> 16); }

/* simple free-list (stack) of send-pool slots: alloc pops, SEND completion
 * pushes. Guarantees a slot is never reused while its SEND is in flight. */
static int g_free_stack[SEND_POOL_SIZE];
static int g_free_top;

static void pool_init(void)
{
  for (int i = 0; i < SEND_POOL_SIZE; i++) g_free_stack[i] = i;
  g_free_top = SEND_POOL_SIZE;
}
static int alloc_send_slot(void)
{
  /* must be called single-threaded (the main loop is). */
  if (g_free_top <= 0) return -1;  /* pool exhausted — caller backs off */
  return g_free_stack[--g_free_top];
}
static void free_send_slot(int slot)
{
  if (slot >= 0 && g_free_top < SEND_POOL_SIZE) g_free_stack[g_free_top++] = slot;
}

static void submit_send(struct io_uring *ring, int cid)
{
  conn_t *c = &g_conns[cid];
  size_t reqlen;
  const uint8_t *req = request_for(c->proto, &reqlen);
  if (!req) return;
  int slot = alloc_send_slot();
  if (slot < 0) return;  /* pool full this tick; we'll retry on the next CQE */
  memcpy(g_send_pool[slot], req, reqlen);
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) { io_uring_submit(ring); sqe = io_uring_get_sqe(ring); if (!sqe) { free_send_slot(slot); return; } }
  io_uring_prep_send(sqe, c->fd, g_send_pool[slot], reqlen, 0);
  io_uring_sqe_set_data(sqe, tag_send_ud(cid, slot));
}

static void submit_recv(struct io_uring *ring, int cid)
{
  conn_t *c = &g_conns[cid];
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (!sqe) { io_uring_submit(ring); sqe = io_uring_get_sqe(ring); if (!sqe) return; }
  /* recv into the tail of the accumulation buffer */
  size_t off = c->rlen;
  if (off >= RECV_BUF_SIZE) off = 0; /* shouldn't happen if we drain promptly */
  io_uring_prep_recv(sqe, c->fd, c->rbuf + off, RECV_BUF_SIZE - off, 0);
  io_uring_sqe_set_data(sqe, tag_recv_ud(cid));
}

/* drain complete framed packets from c->rbuf; return count completed */
static int drain_packets(conn_t *c)
{
  int done = 0;
  for (;;)
  {
    if (c->rlen < 2) break;
    uint16_t len = ((uint16_t)c->rbuf[0] << 8) | c->rbuf[1];
    if (len < 1 || len > 64) { atomic_fetch_add(&g_total_errors, 1); return done; }
    if (c->rlen < (size_t)2 + len) break;
    /* packet complete; validate type tag */
    if (c->rbuf[2] != c->proto) atomic_fetch_add(&g_total_errors, 1);
    else { c->completed++; done++; }
    /* shift buffer */
    size_t total = 2 + len;
    memmove(c->rbuf, c->rbuf + total, c->rlen - total);
    c->rlen -= total;
  }
  return done;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
  const char *host = "127.0.0.1";
  int port = 3985, conns = 256, depth = 16, duration_s = 10, proto = BINARY_PKT_RAW;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--host") && i+1 < argc) host = argv[++i];
    else if (!strcmp(argv[i], "--port") && i+1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--conns") && i+1 < argc) conns = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--depth") && i+1 < argc) depth = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--duration") && i+1 < argc) duration_s = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--proto") && i+1 < argc)
    {
      const char *p = argv[++i];
      if (!strcmp(p, "raw")) proto = BINARY_PKT_RAW;
      else if (!strcmp(p, "msgpack")) proto = BINARY_PKT_MSGPACK;
      else { fprintf(stderr, "bad proto\n"); return 1; }
    }
    else { fprintf(stderr, "Usage: %s [--proto raw|msgpack] [--host H] [--port P] "
                           "[--conns N] [--depth N] [--duration S]\n", argv[0]); return 1; }
  }
  if (conns > MAX_CONNS) { fprintf(stderr, "max conns %d\n", MAX_CONNS); return 1; }
  if ((long)conns * depth > SEND_POOL_SIZE)
    fprintf(stderr, "warning: conns*depth (%ld) > send pool (%d); throughput may be pool-limited\n",
            (long)conns * depth, SEND_POOL_SIZE);

  signal(SIGALRM, on_sigalrm);
  uint32_t addr = inet_addr(host);
  build_templates();
  pool_init();

  printf("binary-loadgen-uring: proto=%s conns=%d depth=%d duration=%ds %s:%d\n",
         proto == BINARY_PKT_RAW ? "raw" : "msgpack", conns, depth, duration_s, host, port);
  fflush(stdout);

  /* connect all conns (blocking), then flip to nonblock for the ring */
  for (int i = 0; i < conns; i++)
  {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = addr };
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); return 1; }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    g_conns[i] = (conn_t){ .fd = fd, .in_flight = 0, .target_depth = depth, .proto = proto,
                           .x = (uint16_t)(i & 0xFFFF), .y = 1, .hp = 200 };
  }
  g_nconns = conns;

  struct io_uring ring;
  if (io_uring_queue_init(RING_ENTRIES, &ring, IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER) < 0)
  {
    if (io_uring_queue_init(RING_ENTRIES, &ring, 0) < 0) { perror("queue_init"); return 1; }
  }

  /* seed: for each conn, submit `depth` sends then a recv per send */
  for (int i = 0; i < conns; i++)
  {
    for (int d = 0; d < depth; d++) { submit_send(&ring, i); g_conns[i].in_flight++; }
    submit_recv(&ring, i);
  }
  io_uring_submit(&ring);

  alarm(duration_s);
  uint64_t t0 = now_ns();

  struct io_uring_cqe *cqe;
  unsigned head;
  while (!g_stop)
  {
    int r = io_uring_submit_and_wait(&ring, 1);
    if (r < 0 && errno != EINTR) break;

    unsigned count = 0;
    io_uring_for_each_cqe(&ring, head, cqe)
    {
      count++;
      uintptr_t ud = (uintptr_t)cqe->user_data;
      int cid = ud_cid(ud), op = ud_op(ud);
      conn_t *c = &g_conns[cid];
      int res = cqe->res;

      if (op == OP_SEND)
      {
        free_send_slot(ud_slot(ud));
        if (res < 0) atomic_fetch_add(&g_total_errors, 1);
      }
      else /* OP_RECV */
      {
        if (res <= 0) { atomic_fetch_add(&g_total_errors, 1); submit_recv(&ring, cid); continue; }
        c->rlen += (size_t)res;
        int done = drain_packets(c);
        if (done > 0)
        {
          atomic_fetch_add(&g_total_completed, (uint64_t)done);
          c->in_flight -= done;
          /* refill: send `done` new requests to keep depth in flight, then recv */
          for (int k = 0; k < done; k++) { submit_send(&ring, cid); c->in_flight++; }
        }
        submit_recv(&ring, cid);
      }
      if (count >= 512) break;
    }
    io_uring_cq_advance(&ring, count);
  }

  uint64_t dt_ns = now_ns() - t0;
  double secs = (double)dt_ns / 1e9;
  uint64_t done = atomic_load(&g_total_completed);
  uint64_t err  = atomic_load(&g_total_errors);

  printf("results: completed=%llu  errors=%llu  duration=%.2fs  packets/sec=%.0f\n",
         (unsigned long long)done, (unsigned long long)err, secs, secs > 0 ? done / secs : 0.0);

  io_uring_queue_exit(&ring);
  for (int i = 0; i < conns; i++) close(g_conns[i].fd);
  return 0;
}
