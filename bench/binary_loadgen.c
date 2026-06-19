/**
 * @file binary_loadgen.c
 * @brief Load generator for the raw-binary protocol server (pthread TCP client).
 *
 * Usage:
 *   binary-loadgen --proto raw|msgpack [--host 127.0.0.1] [--port 3985]
 *                  [--threads N] [--conns N] [--duration S]
 *
 * Opens `threads * conns` persistent TCP connections and floods the server with
 * length-prefixed binary packets, reading each reply before sending the next on
 * that connection (ping-pong per conn). Reports packets/sec (the binary analogue
 * of HTTP RPS).
 *
 * Blocking sockets + pthreads: the loadgen's job is to generate load, not to be
 * fast itself; threads suffice.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_MSGPACK
#include <msgpack.h>
#endif

#define BINARY_PKT_RAW     0x01
#define BINARY_PKT_MSGPACK 0x02

/* fixed-struct payload (network byte order) */
#pragma pack(push, 1)
typedef struct {
  uint8_t  type;
  uint16_t x;
  uint16_t y;
  uint8_t  hp;
} player_state_raw;
#pragma pack(pop)

static volatile sig_atomic_t g_stop = 0;
static void on_sigalrm(int s) { (void) s; g_stop = 1; }

typedef struct {
  int      thread_id;
  int      conns;
  int      proto;       /* BINARY_PKT_RAW or BINARY_PKT_MSGPACK */
  int      depth;       /* in-flight requests per connection (pipeline-like) */
  uint32_t addr;
  uint16_t port;
  int      duration_s;
  /* out */
  uint64_t round_trips;
  uint64_t errors;
} thread_arg;

/* ---- packet encode/decode helpers (client side) -------------------------- */

static size_t build_raw_request(uint8_t *out, uint16_t x, uint16_t y, uint8_t hp)
{
  player_state_raw ps = { .type = BINARY_PKT_RAW, .x = htobe16(x), .y = htobe16(y), .hp = hp };
  out[0] = 0; out[1] = (uint8_t)sizeof ps;   /* len = 6 */
  memcpy(out + 2, &ps, sizeof ps);
  return 2 + sizeof ps;
}

#ifdef HAVE_MSGPACK
static size_t build_msgpack_request(uint8_t *out, size_t outcap, uint16_t x, uint16_t y, uint8_t hp)
{
  msgpack_sbuffer sbuf;
  msgpack_sbuffer_init(&sbuf);
  msgpack_packer pk;
  msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
  msgpack_pack_map(&pk, 3);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "x", 1); msgpack_pack_uint16(&pk, x);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "y", 1); msgpack_pack_uint16(&pk, y);
  msgpack_pack_str(&pk, 1); msgpack_pack_str_body(&pk, "h", 1); msgpack_pack_uint8(&pk, hp);

  /* payload = tag + msgpack */
  size_t payload_len = 1 + sbuf.size;
  out[0] = (uint8_t)(payload_len >> 8);
  out[1] = (uint8_t)(payload_len & 0xFF);
  out[2] = BINARY_PKT_MSGPACK;
  memcpy(out + 3, sbuf.data, sbuf.size);
  size_t total = 2 + payload_len;
  msgpack_sbuffer_destroy(&sbuf);
  (void)outcap;
  return total;
}
#endif

/* read exactly n bytes (blocking). returns 0 on success, -1 on EOF/error. */
static int read_n(int fd, void *buf, size_t n)
{
  size_t got = 0;
  while (got < n)
  {
    ssize_t r = read(fd, (char *)buf + got, n - got);
    if (r == 0) return -1;
    if (r < 0)
    {
      if (errno == EINTR) { if (g_stop) return -1; continue; }
      return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

/* read one framed reply. returns payload length on success, -1 on error. */
static int read_reply(int fd, uint8_t *payload, size_t payload_cap)
{
  uint8_t lenbuf[2];
  if (read_n(fd, lenbuf, 2) < 0) return -1;
  uint16_t len = ((uint16_t)lenbuf[0] << 8) | lenbuf[1];
  if (len < 1 || len > payload_cap) return -1;
  if (read_n(fd, payload, len) < 0) return -1;
  return len;
}

static int connect_one(uint32_t addr, uint16_t port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = addr };
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
  {
    close(fd);
    return -1;
  }
  return fd;
}

/* ---- worker thread ------------------------------------------------------- */

static void *thread_main(void *arg)
{
  thread_arg *t = arg;
  int *fds = calloc((size_t)t->conns, sizeof *fds);
  if (!fds) return NULL;

  for (int i = 0; i < t->conns; i++)
  {
    fds[i] = connect_one(t->addr, t->port);
    if (fds[i] < 0)
      fprintf(stderr, "[thread %d] conn %d failed: %s\n", t->thread_id, i, strerror(errno));
  }

  uint16_t x = (uint16_t)(t->thread_id * 1000);
  uint16_t y = (uint16_t)(t->thread_id * 1000 + 1);
  uint8_t  hp = 200;
  uint8_t  outbuf[64];
  uint8_t  reply[64];
  int depth = t->depth > 0 ? t->depth : 1;

  while (!g_stop)
  {
    for (int i = 0; i < t->conns; i++)
    {
      if (fds[i] < 0) continue;

      /* send `depth` requests, then read `depth` replies — pseudo-pipeline so
       * the server has multiple packets in flight per connection, mirroring
       * wrk's HTTP pipeline behaviour. */
      int sent = 0;
      for (int d = 0; d < depth; d++)
      {
        size_t outlen;
        if (t->proto == BINARY_PKT_RAW)
          outlen = build_raw_request(outbuf, x, y, hp);
#ifdef HAVE_MSGPACK
        else
          outlen = build_msgpack_request(outbuf, sizeof outbuf, x, y, hp);
#else
        else { t->errors++; goto next_conn; }
#endif
        ssize_t w = write(fds[i], outbuf, outlen);
        if (w < 0 || (size_t)w != outlen) { t->errors++; goto next_conn; }
        sent++;
        x = (uint16_t)((x + 1) & 0xFFFF);
      }

      for (int d = 0; d < sent; d++)
      {
        int rlen = read_reply(fds[i], reply, sizeof reply - 1);
        if (rlen < 0) { t->errors++; goto next_conn; }
        if (rlen < 1 || reply[0] != t->proto) { t->errors++; goto next_conn; }
        t->round_trips++;
      }
      next_conn: ;
    }
  }

  for (int i = 0; i < t->conns; i++)
    if (fds[i] >= 0) close(fds[i]);
  free(fds);
  return NULL;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
  const char *host = "127.0.0.1";
  int port = 3985;
  int threads = 4;
  int conns = 64;        /* per thread */
  int duration_s = 10;
  int depth = 1;         /* in-flight requests per connection (pipeline-like) */
  int proto = BINARY_PKT_RAW;

  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--conns") && i + 1 < argc) conns = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration_s = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--proto") && i + 1 < argc)
    {
      const char *p = argv[++i];
      if (!strcmp(p, "raw")) proto = BINARY_PKT_RAW;
      else if (!strcmp(p, "msgpack")) proto = BINARY_PKT_MSGPACK;
      else { fprintf(stderr, "unknown proto: %s\n", p); return 1; }
    }
    else
    {
      fprintf(stderr, "Usage: %s [--proto raw|msgpack] [--host H] [--port P] "
                      "[--threads N] [--conns N] [--depth N] [--duration S]\n", argv[0]);
      return 1;
    }
  }

  signal(SIGALRM, on_sigalrm);
  uint32_t addr = inet_addr(host);
  if (addr == INADDR_NONE) { fprintf(stderr, "bad host: %s\n", host); return 1; }

  printf("binary-loadgen: proto=%s threads=%d conns/thread=%d depth=%d total_conns=%d "
         "duration=%ds host=%s port=%d\n",
         proto == BINARY_PKT_RAW ? "raw" : "msgpack",
         threads, conns, depth, threads * conns, duration_s, host, port);
  fflush(stdout);

  pthread_t *tids = calloc((size_t)threads, sizeof *tids);
  thread_arg *args = calloc((size_t)threads, sizeof *args);
  if (!tids || !args) { fprintf(stderr, "alloc fail\n"); return 1; }

  /* warmup: 1s to let connections establish */
  struct timespec w = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
  nanosleep(&w, NULL);

  for (int i = 0; i < threads; i++)
  {
    args[i] = (thread_arg){ .thread_id = i, .conns = conns, .proto = proto,
                            .depth = depth, .addr = addr, .port = (uint16_t)port,
                            .duration_s = duration_s };
    if (pthread_create(&tids[i], NULL, thread_main, &args[i]))
    {
      fprintf(stderr, "pthread_create %d failed\n", i);
      return 1;
    }
  }

  /* arm the stop timer */
  alarm(duration_s);

  uint64_t total_rt = 0, total_err = 0;
  for (int i = 0; i < threads; i++)
  {
    pthread_join(tids[i], NULL);
    total_rt  += args[i].round_trips;
    total_err += args[i].errors;
  }

  printf("results: round_trips=%llu  errors=%llu  packets/sec=%.0f\n",
         (unsigned long long)total_rt, (unsigned long long)total_err,
         (double)total_rt / duration_s);

  free(tids);
  free(args);
  return 0;
}
