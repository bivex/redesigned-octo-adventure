#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>

#include "reactor.h"
#include "core.h"
#include "descriptor.h" /* Added for descriptor_mask enum */

#define REACTOR_MAX_EVENTS 8192  /* Increased from 1024 to reduce epoll_wait calls and do_epoll_ctl overhead (0.189s) */
#define REACTOR_DEFAULT_TIMEOUT_MS 1  /* Default epoll timeout - prevents blocking when work is available */
#define REACTOR_TIMESLOT 5  /* Timer check interval in seconds */
#define REACTOR_MAX_BATCH_SIZE 64  /* Max batch size for adaptive batching (IX paper) */
#define REACTOR_MEM_POOL_SIZE 1024  /* Size of per-core memory pools */
#define REACTOR_MEM_BLOCK_SIZE 4096 /* Size of each memory block */

/* reactor_handler */

static void reactor_handler_default_callback(__attribute__((unused)) reactor_event *event) {}
static reactor_handler reactor_handler_default = {reactor_handler_default_callback, NULL};

void reactor_handler_construct(reactor_handler *handler, reactor_callback *callback, void *state)
{
  *handler = callback ? (reactor_handler) {callback, state} : reactor_handler_default;
}

void reactor_handler_destruct(reactor_handler *handler)
{
  *handler = reactor_handler_default;
}

static uint32_t to_epoll_events(uint32_t events)
{
  uint32_t epoll_events = 0;
  if (events & DESCRIPTOR_READ)
    epoll_events |= EPOLLIN;
  if (events & DESCRIPTOR_WRITE)
    epoll_events |= EPOLLOUT;
  return epoll_events;
}

/* Timer management structures - based on dissertation design */
typedef struct timer_node {
    struct timer_node *prev;
    struct timer_node *next;
    int fd;                    /* Associated file descriptor */
    time_t expire;            /* Expiration time */
    void (*callback)(int);    /* Cleanup callback function */
    void *user_data;          /* User data for callback */
} timer_node;

typedef struct timer_list {
    timer_node *head;
    timer_node *tail;
    pthread_mutex_t lock;
} timer_list;

/* Forward declarations */
void reactor_update_batch_size(void);

/* IX-inspired per-core memory pools for zero-contention allocation */
typedef struct mem_header {
    size_t pool_id;     /* Identifier to track which pool this allocation belongs to */
    size_t size;        /* Original allocation size for validation */
    uint32_t magic;     /* Magic number to detect double frees */
} mem_header;

#define MEM_HEADER_MAGIC 0xDEADBEEF
#define MEM_HEADER_FREED 0xDEADDEAD

typedef struct mem_block {
    struct mem_block *next;
    char data[REACTOR_MEM_BLOCK_SIZE];
} mem_block;

#define SMALL_POOL_ID  1
#define MEDIUM_POOL_ID 2
#define LARGE_POOL_ID  3
#define MALLOC_POOL_ID 0  /* For malloc allocations */

#define MEM_HEADER_SIZE sizeof(mem_header)

typedef struct mem_pool {
    mem_block *free_list;
    size_t block_count;
    size_t alloc_count;
    pthread_mutex_t lock;  /* Per-pool mutex to avoid contention */
} mem_pool;

/* Specialized pools for common allocation sizes */
static mem_pool *small_pool = NULL;    /* 64-256 bytes */
static mem_pool *medium_pool = NULL;   /* 256-1024 bytes */
static mem_pool *large_pool = NULL;    /* 1024-4096 bytes */

/* reactor */
typedef struct reactor
{
  int                 epoll_fd;
  int                 active;
  size_t              descriptors;
  uint64_t            time;
  int                 timeout_ms;  /* Configurable epoll timeout */
  struct epoll_event  events[REACTOR_MAX_EVENTS] __attribute__((aligned(64))); /* Cache line aligned */
  size_t              event_count;
  /* Timer management - dissertation inspired */
  timer_list         *timers;
  /* Signal handling */
  int                 pipefd[2];   /* Socketpair for signal handling */
  /* IX-inspired adaptive batching */
  int                 batch_size;  /* Current batch size (1-REACTOR_MAX_BATCH_SIZE) */
  int                 congestion_detected; /* Flag for congestion detection */
  /* IX-inspired per-core memory pools */
  mem_pool           *mem_pool;
  /* IX run-to-completion support */
  void              (*work_callback)(void *state);  /* Callback for new work */
  void               *work_state;                   /* State for work callback */
  int                 run_to_completion;            /* Enable run-to-completion mode */
} reactor;

static __thread reactor reactor_core = {0};

/* Signal handling with socketpair - dissertation approach */
static int signal_pipefd[2] = {-1, -1};
static timer_list timer_lst = {NULL, NULL, PTHREAD_MUTEX_INITIALIZER};

static void reactor_signal(__attribute__((unused)) int arg)
{
  reactor_abort();
}

/* Timer management functions */
static void timer_list_init(timer_list *lst)
{
    lst->head = NULL;
    lst->tail = NULL;
    pthread_mutex_init(&lst->lock, NULL);
}

static void timer_node_add(timer_list *lst, timer_node *node, time_t expire)
{
    node->expire = time(NULL) + expire;
    node->prev = NULL;
    node->next = NULL;

    pthread_mutex_lock(&lst->lock);

    if (!lst->head) {
        lst->head = lst->tail = node;
    } else {
        node->next = lst->head;
        lst->head->prev = node;
        lst->head = node;
    }

    pthread_mutex_unlock(&lst->lock);
}

static void timer_node_del(timer_list *lst, timer_node *node)
{
    pthread_mutex_lock(&lst->lock);

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        lst->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        lst->tail = node->prev;
    }

    pthread_mutex_unlock(&lst->lock);
}

static void timer_tick(timer_list *lst)
{
    time_t cur = time(NULL);
    timer_node *tmp = NULL;

    pthread_mutex_lock(&lst->lock);

    for (timer_node *node = lst->tail; node != NULL; ) {
        tmp = node->prev;

        if (node->expire > cur) break;

        /* Timer expired - call callback and remove */
        if (node->callback) {
            node->callback(node->fd);
        }

        /* Remove from list */
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            lst->head = node->next;
        }

        if (node->next) {
            node->next->prev = node->prev;
        } else {
            lst->tail = node->prev;
        }

        reactor_mem_free(node);
        node = tmp;
    }

    pthread_mutex_unlock(&lst->lock);
}

/* Signal handling functions */
static void signal_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(signal_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

static void add_signal(int sig)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

uint64_t reactor_now(void)
{
  struct timespec tv;

  if (!reactor_core.time)
  {
    clock_gettime(CLOCK_REALTIME, &tv);
    reactor_core.time = (uint64_t) tv.tv_sec * 1000000000 + (uint64_t) tv.tv_nsec;
  }
  return reactor_core.time;
}

void reactor_construct(void)
{
  reactor_core = (reactor) {
      .active = 1,
      .timeout_ms = REACTOR_DEFAULT_TIMEOUT_MS,
      .timers = &timer_lst,
      .batch_size = 1,  /* Start with no batching for low latency (IX approach) */
      .congestion_detected = 0,
      .mem_pool = NULL,
      .work_callback = NULL,
      .work_state = NULL,
      .run_to_completion = 1  /* IX: Enabled by default for run-to-completion behavior */
  };

  /* Initialize timer list */
  timer_list_init(&timer_lst);

  reactor_core.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (reactor_unlikely(reactor_core.epoll_fd == -1))
    {
      /* In production, you might want to log this error */
      abort(); /* Keep old behavior for compatibility */
    }

  /* Create socketpair for signal handling - dissertation approach */
  if (socketpair(PF_UNIX, SOCK_STREAM, 0, reactor_core.pipefd) == -1) {
    close(reactor_core.epoll_fd);
    abort(); /* Keep old behavior for compatibility */
  }

  /* Set non-blocking for pipe */
  int flags = fcntl(reactor_core.pipefd[0], F_GETFL, 0);
  fcntl(reactor_core.pipefd[0], F_SETFL, flags | O_NONBLOCK);
  flags = fcntl(reactor_core.pipefd[1], F_GETFL, 0);
  fcntl(reactor_core.pipefd[1], F_SETFL, flags | O_NONBLOCK);

  /* Add pipe read end to epoll */
  reactor_add(NULL, reactor_core.pipefd[0], DESCRIPTOR_READ);

  /* Setup signal handlers */
  add_signal(SIGTERM);
  add_signal(SIGINT);
  add_signal(SIGALRM);
  signal(SIGPIPE, SIG_IGN);

  /* Set up periodic timer */
  alarm(REACTOR_TIMESLOT);
}

void reactor_destruct(void)
{
  if (reactor_core.epoll_fd != -1) // Only close if it's a valid file descriptor
    {
      if (close(reactor_core.epoll_fd) == -1)
        {
          /* In production, you might want to log this error */
        }
      reactor_core.epoll_fd = -1; // Mark as closed
    }
}

void reactor_add(reactor_handler *handler, int fd, uint32_t events)
{
  struct epoll_event event = {.events = to_epoll_events(events), .data.ptr = handler};

  reactor_core.descriptors++;
  if (epoll_ctl(reactor_core.epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1)
    {
      reactor_core.descriptors--; // Rollback on error
      /* In production, you might want to log this error */
      abort(); /* Keep old behavior for compatibility */
    }
}

void reactor_modify(reactor_handler *handler, int fd, uint32_t events)
{
  struct epoll_event event = {.events = to_epoll_events(events), .data.ptr = handler};

  if (epoll_ctl(reactor_core.epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1)
    {
      /* In production, you might want to log this error */
      abort(); /* Keep old behavior for compatibility */
    }
}

void reactor_delete(reactor_handler *handler, int fd)
{
  /* Clean up any pending events for this handler */
  for (size_t i = 0; i < reactor_core.event_count; i++)
    if (reactor_core.events[i].data.ptr == handler)
      reactor_core.events[i] = (struct epoll_event) {.data.ptr = &reactor_handler_default};

  if (epoll_ctl(reactor_core.epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
    {
      /* In production, you might want to log this error */
      abort(); /* Keep old behavior for compatibility */
    }

  reactor_core.descriptors--;
}

void reactor_dispatch(reactor_handler *handler, int type, uintptr_t data)
{
  /* Optimized: Use likely hint for common case (handler has callback) */
  if (reactor_likely(handler->callback != reactor_handler_default_callback))
    handler->callback((reactor_event[]) {{.type = type, .state = handler->state, .data = data}});
}

int reactor_loop_once(void)
{
  int n;

  reactor_core.time = 0;

  /* Update adaptive batch size based on current load (IX approach) */
  reactor_update_batch_size();

  /* Use batch size to determine how many events to process per call */
  int max_events = (reactor_core.congestion_detected) ?
                   reactor_core.batch_size : REACTOR_MAX_EVENTS;

  /* Optimized: Use configurable timeout instead of blocking (-1) to reduce context switch overhead */
  /* This prevents threads from sleeping when work is available and improves responsiveness */
  n = epoll_wait(reactor_core.epoll_fd, reactor_core.events, max_events, reactor_core.timeout_ms);
  if (reactor_unlikely(n == -1))
    {
      if (errno == EINTR)
        return 0; /* Interrupted by signal, not an error */
      /* In production, you might want to log this error */
      abort(); /* Keep old behavior for compatibility */
    }

  reactor_core.event_count = (size_t)n;

  /* IX-inspired run-to-completion: process all events in this batch to completion */
  /* This amortizes system call overheads and improves cache locality */
  for (size_t i = 0; reactor_likely(i < reactor_core.event_count); i++)
    {
      /* Handle signal events first (highest priority) */
      if (reactor_core.events[i].data.fd == reactor_core.pipefd[0])
        {
          int sig;
          char signals[1024];
          int ret = recv(reactor_core.pipefd[0], signals, sizeof(signals), 0);
          if (ret > 0)
            {
              for (int j = 0; j < ret; j++)
                {
                  sig = signals[j];
                  switch (sig)
                    {
                      case SIGALRM:
                        timer_tick(reactor_core.timers);
                        alarm(REACTOR_TIMESLOT);
                        /* IX: After timer processing, check if we can schedule new work */
                        if (reactor_core.run_to_completion && reactor_core.work_callback)
                          {
                            reactor_core.work_callback(reactor_core.work_state);
                          }
                        break;
                      case SIGTERM:
                      case SIGINT:
                        reactor_abort();
                        break;
                    }
                }
            }
        }
      else
        {
          /* Regular I/O events - dispatch to handlers */
    reactor_dispatch(reactor_core.events[i].data.ptr, REACTOR_EPOLL_EVENT, (uintptr_t) &reactor_core.events[i]);

          /* IX run-to-completion: After processing an I/O event, check if we can schedule new work */
          /* This implements the "run to completion" pattern from IX paper */
          if (reactor_core.run_to_completion && reactor_core.work_callback)
            {
              reactor_core.work_callback(reactor_core.work_state);
            }
        }
    }

  return n; /* Return number of events processed */
}

int reactor_loop(void)
{
  int total_events = 0;
  while (__atomic_load_n(&reactor_core.active, __ATOMIC_ACQUIRE))
    {
      int processed = reactor_loop_once();
      total_events += processed;
    }
  return total_events;
}

void reactor_abort(void)
{
  __atomic_store_n(&reactor_core.active, 0, __ATOMIC_RELEASE);
}

/* New utility functions for configuration and monitoring */

void reactor_set_timeout(int timeout_ms)
{
  reactor_core.timeout_ms = timeout_ms > 0 ? timeout_ms : REACTOR_DEFAULT_TIMEOUT_MS;
}

int reactor_get_timeout(void)
{
  return reactor_core.timeout_ms;
}

size_t reactor_get_descriptor_count(void)
{
  return reactor_core.descriptors;
}

size_t reactor_get_event_count(void)
{
  return reactor_core.event_count;
}

int reactor_get_fd(void)
{
  return reactor_core.epoll_fd;
}

/* Timer management public API */
timer_node *reactor_timer_add(int fd, time_t expire, void (*callback)(int), void *user_data)
{
  timer_node *node = (timer_node *)reactor_mem_alloc(sizeof(timer_node));
  if (!node) return NULL;

    node->fd = fd;
    node->callback = callback;
    node->user_data = user_data;

    timer_node_add(reactor_core.timers, node, expire);
    return node;
}

void reactor_timer_del(timer_node *node)
{
    timer_node_del(reactor_core.timers, node);
}

void reactor_timer_update(timer_node *node, time_t expire)
{
    timer_node_del(reactor_core.timers, node);
    timer_node_add(reactor_core.timers, node, expire);
}

/* RAII wrappers for automatic resource management - dissertation inspired */

/* RAII wrapper for reactor lifecycle */
typedef struct reactor_guard {
    int initialized;
} reactor_guard;

static reactor_guard *reactor_guard_create(void)
{
    reactor_guard *guard = (reactor_guard *)malloc(sizeof(reactor_guard));
    if (!guard) return NULL;

    reactor_construct();
    guard->initialized = 1;
    return guard;
}

static void reactor_guard_destroy(reactor_guard *guard)
{
    if (guard && guard->initialized) {
        reactor_destruct();
        guard->initialized = 0;
    }
    free(guard);
}

/* RAII wrapper for timer nodes */
typedef struct timer_guard {
    timer_node *node;
} timer_guard;

static timer_guard *timer_guard_create(int fd, time_t expire, void (*callback)(int), void *user_data)
{
    timer_guard *guard = (timer_guard *)malloc(sizeof(timer_guard));
    if (!guard) return NULL;

    guard->node = reactor_timer_add(fd, expire, callback, user_data);
    if (!guard->node) {
        free(guard);
        return NULL;
    }
    return guard;
}

static void timer_guard_destroy(timer_guard *guard)
{
    if (guard && guard->node) {
        reactor_timer_del(guard->node);
    }
    free(guard);
}

/* Public RAII API */
reactor_guard *reactor_create_guard(void) { return reactor_guard_create(); }
void reactor_destroy_guard(reactor_guard *guard) { reactor_guard_destroy(guard); }

timer_guard *reactor_create_timer_guard(int fd, time_t expire, void (*callback)(int), void *user_data) {
    return timer_guard_create(fd, expire, callback, user_data);
}
void reactor_destroy_timer_guard(timer_guard *guard) { timer_guard_destroy(guard); }

/* IX run-to-completion API */
void reactor_enable_run_to_completion(void (*callback)(void *state), void *state)
{
    reactor_core.work_callback = callback;
    reactor_core.work_state = state;
    reactor_core.run_to_completion = 1;
}

void reactor_disable_run_to_completion(void)
{
    reactor_core.work_callback = NULL;
    reactor_core.work_state = NULL;
    reactor_core.run_to_completion = 0;
}

int reactor_is_run_to_completion_enabled(void)
{
    return reactor_core.run_to_completion;
}

/* IX-inspired adaptive batching functions */
void reactor_update_batch_size(void)
{
    /* Simple congestion detection based on event count and descriptors */
    int event_density = reactor_core.event_count * 100 / REACTOR_MAX_EVENTS;
    int descriptor_load = reactor_core.descriptors * 100 / REACTOR_MAX_EVENTS;

    /* Congestion if >70% events or descriptors utilized */
    int congestion_level = (event_density > 70 || descriptor_load > 70) ? 1 : 0;

    if (congestion_level && !reactor_core.congestion_detected) {
        /* Congestion started - increase batch size */
        reactor_core.congestion_detected = 1;
        reactor_core.batch_size = REACTOR_MAX_BATCH_SIZE / 4; /* Start conservative */
    } else if (!congestion_level && reactor_core.congestion_detected) {
        /* Congestion ended - reduce batch size */
        reactor_core.congestion_detected = 0;
        reactor_core.batch_size = 1; /* Back to no batching for latency */
    } else if (congestion_level) {
        /* Existing congestion - adapt batch size */
        if (event_density > 90) {
            reactor_core.batch_size = REACTOR_MAX_BATCH_SIZE; /* Max batching */
        } else if (event_density > 80) {
            reactor_core.batch_size = REACTOR_MAX_BATCH_SIZE / 2;
        }
    }
}

/* IX-kernel-level-inspired integrated transaction processing - struct defined in header */

/* Global transaction context for IX-style processing */
ix_transaction current_transaction = {0};

/* IX integrated transaction processor - kernel-level style approach */
int reactor_process_ix_transaction(void (*accept_handler)(void),
                                   int (*read_handler)(void *data, size_t len),
                                   void (*process_handler)(void *result),
                                   int (*write_handler)(void *data, size_t len))
{
    current_transaction = (ix_transaction){0};
    current_transaction.start_time = reactor_now();

    int total_operations = 0;

    /* Phase 1: Accept - handle new connections/incoming requests */
    if (accept_handler) {
        accept_handler();
        total_operations += current_transaction.accept_count;
    }

    /* Phase 2: Read - process all available input data */
    /* In IX, this would read from NIC rings directly */
    int max_reads = reactor_core.batch_size;
    for (int i = 0; i < max_reads; i++) {
        /* Check for available data to read */
        void *data = NULL;
        size_t len = 0;

        /* Try to read data (this would be NIC ring in real IX) */
        if (read_handler && read_handler(data, len)) {
            current_transaction.read_count++;
            total_operations++;

            /* Phase 3: Process - handle the data immediately */
            if (process_handler) {
                process_handler(data);
                current_transaction.process_count++;
            }

            /* Phase 4: Write - send response immediately */
            if (write_handler && write_handler(data, len)) {
                current_transaction.write_count++;
            }
        } else {
            break; /* No more data to read */
        }
    }

    current_transaction.end_time = reactor_now();

    /* Return total operations processed in this transaction */
    return total_operations;
}

/* Get current transaction statistics */
ix_transaction reactor_get_last_transaction(void)
{
    return current_transaction;
}

/* IX-style batch processing of multiple transactions */
int reactor_process_ix_batch(int batch_size,
                            void (*accept_handler)(void),
                            int (*read_handler)(void *data, size_t len),
                            void (*process_handler)(void *result),
                            int (*write_handler)(void *data, size_t len))
{
    int total_transactions = 0;

    for (int i = 0; i < batch_size; i++) {
        int ops = reactor_process_ix_transaction(accept_handler, read_handler,
                                                process_handler, write_handler);
        if (ops == 0) break; /* No more work to do */
        total_transactions += ops;
    }

    return total_transactions;
}

int reactor_get_batch_size(void)
{
    return reactor_core.batch_size;
}

int reactor_is_congested(void)
{
    return reactor_core.congestion_detected;
}

/* IX-inspired zero-copy I/O functions */

/* Scatter-gather write using writev for zero-copy HTTP responses */
int reactor_writev(int fd, struct iovec *iov, int iovcnt)
{
    return writev(fd, iov, iovcnt);
}

/* Zero-copy file serving with sendfile */
int reactor_sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
    return sendfile(out_fd, in_fd, offset, count);
}

/* Memory-mapped file serving (zero-copy for static content) */
void *reactor_mmap_file(const char *filename, size_t *size)
{
    int fd = open(filename, O_RDONLY);
    if (fd == -1) return NULL;

    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        return NULL;
    }

    *size = st.st_size;
    void *addr = mmap(NULL, *size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* File descriptor no longer needed after mmap */

    if (addr == MAP_FAILED) return NULL;
    return addr;
}

void reactor_unmap_file(void *addr, size_t size)
{
    munmap(addr, size);
}

/* IX-inspired memory pool functions for low-contention allocation */
static mem_pool *mem_pool_create(void)
{
    mem_pool *pool = (mem_pool *)malloc(sizeof(mem_pool));
    if (!pool) return NULL;

    pool->free_list = NULL;
    pool->block_count = 0;
    pthread_mutex_init(&pool->lock, NULL);

    return pool;
}

static void mem_pool_destroy(mem_pool *pool)
{
    if (!pool) return;

    mem_block *block = pool->free_list;
    while (block) {
        mem_block *next = block->next;
        free(block);
        block = next;
    }

    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

static void *mem_pool_alloc(mem_pool *pool, size_t size, size_t pool_id)
{
    size_t total_size = size + MEM_HEADER_SIZE;

    if (total_size > REACTOR_MEM_BLOCK_SIZE) {
        /* Use malloc with header for large allocations */
        mem_header *header = (mem_header *)malloc(total_size);
        if (!header) return NULL;

        header->pool_id = MALLOC_POOL_ID;  /* Mark as malloc allocation */
        header->size = size;
        header->magic = MEM_HEADER_MAGIC;
        return (char *)header + MEM_HEADER_SIZE;
    }

    pthread_mutex_lock(&pool->lock);

    mem_block *block = pool->free_list;
    if (block) {
        pool->free_list = block->next;
        pool->block_count--;
    } else {
        /* Allocate new block */
        block = (mem_block *)malloc(sizeof(mem_block));
        if (!block) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
    }

    pthread_mutex_unlock(&pool->lock);

    /* Add header to track allocation */
    mem_header *header = (mem_header *)block->data;
    header->pool_id = pool_id;
    header->size = size;
    header->magic = MEM_HEADER_MAGIC;  /* Mark as valid allocation */

    return (char *)header + MEM_HEADER_SIZE;  // V773: False positive - block is properly allocated and managed
}

static int mem_pool_free(mem_pool *pool, void *ptr, size_t expected_pool_id)
{
    if (!ptr || !pool) return 0;

    /* Check header to verify this allocation belongs to expected pool */
    mem_header *header = (mem_header *)((char *)ptr - MEM_HEADER_SIZE);

    /* Check magic number to detect corruption or double free */
    if (header->magic != MEM_HEADER_MAGIC) {
        if (header->magic == MEM_HEADER_FREED) {
            /* Double free detected! */
            fprintf(stderr, "DOUBLE FREE DETECTED: %p was already freed\n", ptr);
            abort(); /* Or handle gracefully */
        } else {
            /* Memory corruption */
            fprintf(stderr, "MEMORY CORRUPTION: invalid magic number in header\n");
            return 0;
        }
    }

    if (header->pool_id != expected_pool_id) {
        return 0; /* This pointer doesn't belong to this pool */
    }

    if (header->pool_id == MALLOC_POOL_ID) {
        /* This was allocated with malloc, use free */
        header->magic = MEM_HEADER_FREED;  /* Mark as freed */
        free(header);
        return 1;
    }

    /* Mark as freed before returning to pool */
    header->magic = MEM_HEADER_FREED;

    /* Return block to pool */
    mem_block *block = (mem_block *)header;

    pthread_mutex_lock(&pool->lock);

    block->next = pool->free_list;
    pool->free_list = block;
    pool->block_count++;

    pthread_mutex_unlock(&pool->lock);

    return 1; /* Successfully freed from pool */
}

/* Public memory pool API */
void *reactor_mem_alloc(size_t size)
{
    /* Use specialized pools for common sizes to reduce contention */
    if (size <= 256) {
        if (!small_pool) {
            small_pool = mem_pool_create();
            if (!small_pool) {
                /* Fallback to malloc with header for large or uncommon sizes */
                mem_header *header = (mem_header *)malloc(size + MEM_HEADER_SIZE);
                if (!header) return NULL;

                header->pool_id = MALLOC_POOL_ID;
                header->size = size;
                header->magic = MEM_HEADER_MAGIC;
                return (char *)header + MEM_HEADER_SIZE;
            }
        }
        return mem_pool_alloc(small_pool, size, SMALL_POOL_ID);
    } else if (size <= 1024) {
        if (!medium_pool) {
            medium_pool = mem_pool_create();
            if (!medium_pool) {
                /* Fallback to malloc with header for large or uncommon sizes */
                mem_header *header = (mem_header *)malloc(size + MEM_HEADER_SIZE);
                if (!header) return NULL;

                header->pool_id = MALLOC_POOL_ID;
                header->size = size;
                header->magic = MEM_HEADER_MAGIC;
                return (char *)header + MEM_HEADER_SIZE;
            }
        }
        return mem_pool_alloc(medium_pool, size, MEDIUM_POOL_ID);
    } else if (size <= 4096) {
        if (!large_pool) {
            large_pool = mem_pool_create();
            if (!large_pool) {
                /* Fallback to malloc with header for large or uncommon sizes */
                mem_header *header = (mem_header *)malloc(size + MEM_HEADER_SIZE);
                if (!header) return NULL;

                header->pool_id = MALLOC_POOL_ID;
                header->size = size;
                header->magic = MEM_HEADER_MAGIC;
                return (char *)header + MEM_HEADER_SIZE;
            }
        }
        return mem_pool_alloc(large_pool, size, LARGE_POOL_ID);
    }

    /* Fallback to malloc with header for large or uncommon sizes */
    mem_header *header = (mem_header *)malloc(size + MEM_HEADER_SIZE);
    if (!header) return NULL;

    header->pool_id = MALLOC_POOL_ID;
    header->size = size;
    header->magic = MEM_HEADER_MAGIC;
    return (char *)header + MEM_HEADER_SIZE;
}

void reactor_mem_free(void *ptr)
{
    if (!ptr) return;

    /* Check header to determine which pool this belongs to */
    mem_header *header = (mem_header *)((char *)ptr - MEM_HEADER_SIZE);

    if (header->pool_id == SMALL_POOL_ID && small_pool) {
        mem_pool_free(small_pool, ptr, SMALL_POOL_ID);
    } else if (header->pool_id == MEDIUM_POOL_ID && medium_pool) {
        mem_pool_free(medium_pool, ptr, MEDIUM_POOL_ID);
    } else if (header->pool_id == LARGE_POOL_ID && large_pool) {
        mem_pool_free(large_pool, ptr, LARGE_POOL_ID);
    } else {
        /* Fallback to free for malloc allocations */
        free(header);
    }
}

/* core aliases for compatibility */

void core_dispatch(reactor_handler *handler, int fd, uintptr_t data)
{
  reactor_dispatch(handler, fd, data);
}

void core_add(reactor_handler *handler, int fd, uint32_t events)
{
  reactor_add(handler, fd, events);
}

void core_modify(reactor_handler *handler, int fd, uint32_t events)
{
  reactor_modify(handler, fd, events);
}

void core_delete(reactor_handler *handler, int fd)
{
  reactor_delete(handler, fd);
}

void core_cancel(reactor_handler *handler, int fd)
{
  reactor_delete(handler, fd);
}

void *core_next(reactor_handler *handler, int fd)
{
  (void) handler;
  (void) fd;
  return NULL; /* not implemented */
}

/* Run-to-Completion optimized event loop */
typedef struct rtc_handlers {
    void (*accept_handler)(void);           /* Handle new connections */
    int (*read_handler)(int fd, void *buf, size_t len); /* Read from ready fd */
    void (*process_handler)(int fd, void *data, size_t len); /* Process read data */
    int (*write_handler)(int fd, void *data, size_t len); /* Write response */
} rtc_handlers;

static rtc_handlers rtc_handler_callbacks = {NULL, NULL, NULL, NULL};

/* Set run-to-completion handlers */
void reactor_set_rtc_handlers(void (*accept_handler)(void),
                             int (*read_handler)(int fd, void *buf, size_t len),
                             void (*process_handler)(int fd, void *data, size_t len),
                             int (*write_handler)(int fd, void *data, size_t len))
{
    rtc_handler_callbacks.accept_handler = accept_handler;
    rtc_handler_callbacks.read_handler = read_handler;
    rtc_handler_callbacks.process_handler = process_handler;
    rtc_handler_callbacks.write_handler = write_handler;
}

/* Optimized run-to-completion event loop: accept + read + process + write in one pass */
int reactor_loop_once_optimized(void)
{
    // For our PostgreSQL client architecture, we can't do direct socket I/O
    // because reading/writing happens through the PostgreSQL library in event handlers.
    // Instead, we use the optimized event loop as a wrapper that tracks transactions
    // but delegates actual I/O to the regular event processing.

    reactor_core.time = 0;
    reactor_update_batch_size();

    int max_events = (reactor_core.congestion_detected) ?
                     reactor_core.batch_size : REACTOR_MAX_EVENTS;

    /* Phase 1: ACCEPT - Handle new connections first */
    if (rtc_handler_callbacks.accept_handler) {
        rtc_handler_callbacks.accept_handler();
        current_transaction.accept_count++;
    }

    /* Phase 2-4: Use regular event loop but track transactions */
    // The actual read/process/write happens in the event handlers
    // We just track that operations occurred
    int events_processed = reactor_loop_once();

    // Estimate operations based on events processed
    // In real IX, this would be actual read/write operations
    current_transaction.read_count += events_processed;
    current_transaction.process_count += events_processed;
    current_transaction.write_count += events_processed;

    return events_processed;
}
