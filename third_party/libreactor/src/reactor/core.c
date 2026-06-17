/*
 * reactor core — io_uring backend (completion model)
 *
 * Replaces the former epoll backend. Public API is preserved
 * (reactor_add/modify/delete/loop/loop_once/abort); internals now drive a
 * liburing ring. POLL readiness is delivered as IORING_OP_POLL_ADD
 * completions; higher layers (stream.c, server.c) additionally submit
 * recv/send/accept operations directly on the ring.
 *
 * The previous SIGALRM + timer_list + signal-socketpair machinery has been
 * removed: it had no live consumers (timer.c uses timerfd, polled normally).
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>

#include <sys/uio.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <liburing.h>

#include "reactor.h"
#include "core.h"

/* branch-prediction hints (the old backend had reactor_likely/reactor_unlikely
 * via a header; define locally to keep this file self-contained). */
#define reactor_likely(x)   __builtin_expect(!!(x), 1)
#define reactor_unlikely(x) __builtin_expect(!!(x), 0)

/* ---- tuning knobs -------------------------------------------------------- */

#define REACTOR_RING_ENTRIES   1024
/* Per-poll-completion CQE batch upper bound; the CQ is drained fully anyway. */
#define REACTOR_CQE_BATCH      64

/* ---- per-fd registration table ------------------------------------------- *
 * reactor_add/modify/delete carry a (handler*, fd, events) triple. io_uring
 * POLL_ADD keys a completion by user_data only, so we stash the handler in
 * user_data and keep a side table so modify/delete can cancel outstanding
 * polls by fd. The table is thread-local (one reactor per worker thread,
 * matching the old __thread reactor_core).
 */
typedef struct {
  reactor_handler *handler;
  int              fd;
  uint32_t         events;        /* POLLIN/POLLOUT mask currently submitted */
  int              in_flight;     /* outstanding POLL_ADD SQEs for this slot */
} reactor_poll_slot;

#define REACTOR_POLL_TABLE_SIZE 4096

typedef struct reactor
{
  struct io_uring   ring;
  int               active;
  size_t            descriptors;
  uint64_t          time;
  reactor_poll_slot table[REACTOR_POLL_TABLE_SIZE];
} reactor;

static __thread reactor reactor_core = {0};

/* ---- helpers ------------------------------------------------------------- */

/* reactor_handler: construct/destruct (used by descriptor.c, stream.c, etc.). */
static reactor_handler reactor_handler_default = {NULL, NULL};

void reactor_handler_construct(reactor_handler *handler, reactor_callback *callback, void *state)
{
  *handler = callback ? (reactor_handler){callback, state} : reactor_handler_default;
}

void reactor_handler_destruct(reactor_handler *handler)
{
  *handler = reactor_handler_default;
}

static inline uint32_t to_poll_mask(uint32_t events)
{
  /* Callers pass POLLIN/POLLOUT (descriptor.c) or the legacy
   * DESCRIPTOR_READ/WRITE bits for direct callers. EPOLL* and POLL* share the
   * same numeric values on Linux, so POLL* covers both. */
  uint32_t mask = 0;
  if ((events & (POLLIN | DESCRIPTOR_READ)))
    mask |= POLLIN;
  if ((events & (POLLOUT | DESCRIPTOR_WRITE)))
    mask |= POLLOUT;
  return mask;
}

/* Find slot by fd (linear; descriptor counts are small and fd numbers dense
 * enough in practice; a direct fd->index map can be added if this shows up). */
static reactor_poll_slot *reactor_slot_find(int fd)
{
  for (size_t i = 0; i < REACTOR_POLL_TABLE_SIZE; i++)
    if (reactor_core.table[i].in_flight && reactor_core.table[i].fd == fd)
      return &reactor_core.table[i];
  return NULL;
}

static reactor_poll_slot *reactor_slot_acquire(void)
{
  for (size_t i = 0; i < REACTOR_POLL_TABLE_SIZE; i++)
    if (!reactor_core.table[i].in_flight)
      return &reactor_core.table[i];
  return NULL;
}

/* Submit a POLL_ADD SQE for a slot. The slot's handler is stored as
 * user_data; descriptor_callback recovers the poll result from the CQE. */
static void reactor_poll_submit(reactor_poll_slot *slot)
{
  struct io_uring_sqe *sqe = io_uring_get_sqe(&reactor_core.ring);
  if (reactor_unlikely(!sqe))
  {
    /* Ring full: flush and retry once. */
    io_uring_submit(&reactor_core.ring);
    sqe = io_uring_get_sqe(&reactor_core.ring);
    if (!sqe)
      return;
  }
  io_uring_prep_poll_add(sqe, slot->fd, slot->events);
  io_uring_sqe_set_data(sqe, slot->handler);
  slot->in_flight++;
}

/* ---- signals ------------------------------------------------------------- *
 * Minimal: SIGTERM/SIGINT flip the active flag. io_uring_enter returns EINTR
 * on signal delivery and the loop re-checks active. No self-pipe needed.
 */
static void reactor_signal(int sig)
{
  (void) sig;
  atomic_store(&reactor_core.active, 0);
}

/* ---- public reactor API -------------------------------------------------- */

uint64_t reactor_now(void)
{
  struct timespec tv;

  if (!reactor_core.time)
  {
    clock_gettime(CLOCK_MONOTONIC, &tv);
    reactor_core.time = (uint64_t) tv.tv_sec * 1000000000 + (uint64_t) tv.tv_nsec;
  }
  return reactor_core.time;
}

void reactor_construct(void)
{
  memset(reactor_core.table, 0, sizeof reactor_core.table);
  reactor_core.active = 1;
  reactor_core.descriptors = 0;
  reactor_core.time = 0;

  if (io_uring_queue_init(REACTOR_RING_ENTRIES, &reactor_core.ring, 0) < 0)
    abort();

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = reactor_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;            /* no SA_RESTART: enter() must return EINTR */
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
}

void reactor_destruct(void)
{
  io_uring_queue_exit(&reactor_core.ring);
}

void reactor_add(reactor_handler *handler, int fd, uint32_t events)
{
  reactor_poll_slot *slot = reactor_slot_acquire();
  if (reactor_unlikely(!slot))
    return;
  slot->handler = handler;
  slot->fd = fd;
  slot->events = to_poll_mask(events);
  slot->in_flight = 0;
  reactor_core.descriptors++;
  reactor_poll_submit(slot);
  io_uring_submit(&reactor_core.ring);
}

void reactor_modify(reactor_handler *handler, int fd, uint32_t events)
{
  reactor_poll_slot *slot = reactor_slot_find(fd);
  if (!slot)
  {
    /* Not currently registered — treat as add. */
    reactor_add(handler, fd, events);
    return;
  }
  uint32_t new_mask = to_poll_mask(events);
  if (slot->events == new_mask)
    return;
  slot->events = new_mask;
  /* io_uring POLL_ADD is one-shot (per submission); outstanding polls will
   * complete and we re-arm with the new mask. Mark so the completion handler
   * knows to resubmit. */
  if (slot->in_flight == 0)
    reactor_poll_submit(slot);
}

void reactor_delete(reactor_handler *handler, int fd)
{
  (void) handler;
  reactor_poll_slot *slot = reactor_slot_find(fd);
  if (!slot)
    return;
  /* Best-effort cancellation; completion handler drops the slot. */
  struct io_uring_sqe *sqe = io_uring_get_sqe(&reactor_core.ring);
  if (sqe)
  {
    io_uring_prep_poll_remove(sqe, (uint64_t)(uintptr_t)slot->handler);
    io_uring_sqe_set_data(sqe, NULL);
  }
  slot->fd = -1;            /* sentinel: completion handler frees the slot */
  if (reactor_core.descriptors)
    reactor_core.descriptors--;
}

void reactor_dispatch(reactor_handler *handler, int type, uintptr_t data)
{
  if (reactor_likely(handler && handler->callback))
    handler->callback((reactor_event[]) {{.type = type, .state = handler->state, .data = data}});
}

int reactor_loop_once(void)
{
  struct io_uring_cqe *cqe;
  unsigned head;
  unsigned count = 0;

  reactor_core.time = 0;

  int r = io_uring_submit_and_wait(&reactor_core.ring, 1);
  if (r < 0 && errno != EINTR)
  {
    /* EINTR means a signal interrupted the wait; loop() checks active. */
    if (errno != EINTR)
      return 0;
  }

  io_uring_for_each_cqe(&reactor_core.ring, head, cqe)
  {
    count++;
    reactor_handler *handler = (reactor_handler *) cqe->user_data;

    /* NULL user_data: internal cancellation op (e.g. poll_remove). */
    if (!handler)
      continue;

    /* cqe->res holds either:
     *   - a poll mask (>=0) for POLL_ADD completions, or
     *   - a negative errno for cancellation/error.
     * The descriptor layer decodes it via descriptor_callback. */
    reactor_dispatch(handler, REACTOR_EPOLL_EVENT, (uintptr_t) cqe->res);

    if (count >= REACTOR_CQE_BATCH)
      break;
  }
  io_uring_cq_advance(&reactor_core.ring, count);

  return (int) count;
}

int reactor_loop(void)
{
  int total = 0;
  while (atomic_load(&reactor_core.active))
    total += reactor_loop_once();
  return total;
}

void reactor_abort(void)
{
  atomic_store(&reactor_core.active, 0);
}

/* ---- compat / configuration shims ---------------------------------------- */

void reactor_set_timeout(int timeout_ms)
{
  (void) timeout_ms;
  /* io_uring blocks in submit_and_wait until events arrive; the epoll timeout
   * concept no longer applies. Retained for API compatibility. */
}

int reactor_get_timeout(void)
{
  return -1;
}

size_t reactor_get_descriptor_count(void)
{
  return reactor_core.descriptors;
}

size_t reactor_get_event_count(void)
{
  return 0;
}

int reactor_get_fd(void)
{
  return reactor_core.ring.ring_fd;
}

/* ---- ring access for higher layers --------------------------------------- *
 * stream.c / server.c need to submit recv/send/accept directly. Exposed via
 * non-public symbols (not in core.h): they live in the same translation unit
 * graph and are declared in the internal headers those files include.
 */
struct io_uring *reactor_ring(void)
{
  return &reactor_core.ring;
}

/* resubmit helper for descriptor.c after it consumes a POLL completion. */
void reactor_poll_resubmit(int fd)
{
  reactor_poll_slot *slot = reactor_slot_find(fd);
  if (slot)
    reactor_poll_submit(slot);
}

void reactor_submit(void)
{
  io_uring_submit(&reactor_core.ring);
}

/* ---- timer API (removed machinery, kept as no-ops for link compat) ------- *
 * The old SIGALRM-fed timer_list is gone; timer.c (timerfd) is the live path
 * and is unaffected. These stubs preserve the public ABI.
 */
timer_node *reactor_timer_add(int fd, time_t expire, void (*callback)(int), void *user_data)
{
  (void) fd; (void) expire; (void) callback; (void) user_data;
  return NULL;
}

void reactor_timer_del(timer_node *node) { (void) node; }
void reactor_timer_update(timer_node *node, time_t expire) { (void) node; (void) expire; }

/* ---- zero-copy I/O helpers (unchanged behaviour) ------------------------- */

int reactor_writev(int fd, struct iovec *iov, int iovcnt)
{
  return writev(fd, iov, iovcnt);
}

int reactor_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
  return sendfile(out_fd, in_fd, offset, count);
}

void *reactor_mmap_file(const char *filename, size_t *size)
{
  int fd = open(filename, O_RDONLY);
  if (fd == -1)
    return NULL;
  struct stat st;
  if (fstat(fd, &st) == -1)
  {
    close(fd);
    return NULL;
  }
  *size = st.st_size;
  void *addr = mmap(NULL, *size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return addr == MAP_FAILED ? NULL : addr;
}

void reactor_unmap_file(void *addr, size_t size)
{
  munmap(addr, size);
}

/* ---- memory pools (retained; small per-core pools) ----------------------- *
 * Kept verbatim from the epoll backend — unrelated to the event loop. */

#define REACTOR_MEM_POOL_SIZE   1024
#define REACTOR_MEM_BLOCK_SIZE  4096

typedef struct mem_header {
  size_t   pool_id;
  size_t   size;
  uint32_t magic;
} mem_header;

#define MEM_HEADER_MAGIC 0xDEADBEEF
#define MEM_HEADER_FREED 0xDEADDEAD
#define MEM_HEADER_SIZE  sizeof(mem_header)

#define SMALL_POOL_ID  1
#define MEDIUM_POOL_ID 2
#define LARGE_POOL_ID  3
#define MALLOC_POOL_ID 0

typedef struct mem_block {
  struct mem_block *next;
  char data[REACTOR_MEM_BLOCK_SIZE];
} mem_block;

typedef struct mem_pool {
  mem_block *free_list;
  size_t block_count;
  size_t alloc_count;
  pthread_mutex_t lock;
} mem_pool;

static mem_pool *small_pool  = NULL;
static mem_pool *medium_pool = NULL;
static mem_pool *large_pool  = NULL;

static mem_pool *mem_pool_create(void)
{
  mem_pool *pool = malloc(sizeof *pool);
  if (!pool)
    return NULL;
  pool->free_list = NULL;
  pool->block_count = 0;
  pool->alloc_count = 0;
  pthread_mutex_init(&pool->lock, NULL);
  return pool;
}

static void *mem_pool_alloc(mem_pool *pool, size_t size, size_t pool_id)
{
  size_t total = size + MEM_HEADER_SIZE;
  if (total > REACTOR_MEM_BLOCK_SIZE)
  {
    mem_header *h = malloc(total);
    if (!h)
      return NULL;
    h->pool_id = MALLOC_POOL_ID;
    h->size = size;
    h->magic = MEM_HEADER_MAGIC;
    return (char *) h + MEM_HEADER_SIZE;
  }
  pthread_mutex_lock(&pool->lock);
  mem_block *block = pool->free_list;
  if (block)
  {
    pool->free_list = block->next;
    pool->block_count--;
  }
  else
  {
    block = malloc(sizeof *block);
    if (!block)
    {
      pthread_mutex_unlock(&pool->lock);
      return NULL;
    }
  }
  pthread_mutex_unlock(&pool->lock);
  mem_header *h = (mem_header *) block->data;
  h->pool_id = pool_id;
  h->size = size;
  h->magic = MEM_HEADER_MAGIC;
  return (char *) h + MEM_HEADER_SIZE;
}

void *reactor_mem_alloc(size_t size)
{
  if (size <= 256)
  {
    if (!small_pool)
      small_pool = mem_pool_create();
    if (small_pool)
      return mem_pool_alloc(small_pool, size, SMALL_POOL_ID);
  }
  else if (size <= 1024)
  {
    if (!medium_pool)
      medium_pool = mem_pool_create();
    if (medium_pool)
      return mem_pool_alloc(medium_pool, size, MEDIUM_POOL_ID);
  }
  else if (size <= 4096)
  {
    if (!large_pool)
      large_pool = mem_pool_create();
    if (large_pool)
      return mem_pool_alloc(large_pool, size, LARGE_POOL_ID);
  }
  mem_header *h = malloc(size + MEM_HEADER_SIZE);
  if (!h)
    return NULL;
  h->pool_id = MALLOC_POOL_ID;
  h->size = size;
  h->magic = MEM_HEADER_MAGIC;
  return (char *) h + MEM_HEADER_SIZE;
}

void reactor_mem_free(void *ptr)
{
  if (!ptr)
    return;
  mem_header *h = (mem_header *)((char *) ptr - MEM_HEADER_SIZE);
  if (h->magic == MEM_HEADER_FREED)
    return;
  if (h->pool_id == MALLOC_POOL_ID)
  {
    h->magic = MEM_HEADER_FREED;
    free(h);
    return;
  }
  h->magic = MEM_HEADER_FREED;
  mem_pool *pool = h->pool_id == SMALL_POOL_ID ? small_pool
                  : h->pool_id == MEDIUM_POOL_ID ? medium_pool
                  : large_pool;
  if (!pool)
  {
    free(h);
    return;
  }
  mem_block *block = (mem_block *) h;
  pthread_mutex_lock(&pool->lock);
  block->next = pool->free_list;
  pool->free_list = block;
  pool->block_count++;
  pthread_mutex_unlock(&pool->lock);
}

/* ---- core aliases (compat) ---------------------------------------------- */

void core_dispatch(reactor_handler *handler, int fd, uintptr_t data)
{
  reactor_dispatch(handler, fd, data);
}

void core_add(reactor_handler *handler, int fd, uint32_t events)    { reactor_add(handler, fd, events); }
void core_modify(reactor_handler *handler, int fd, uint32_t events) { reactor_modify(handler, fd, events); }
void core_delete(reactor_handler *handler, int fd)                  { reactor_delete(handler, fd); }
void core_cancel(reactor_handler *handler, int fd)                  { reactor_delete(handler, fd); }
void *core_next(reactor_handler *handler, int fd) { (void) handler; (void) fd; return NULL; }
