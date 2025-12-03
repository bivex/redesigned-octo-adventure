#ifndef REACTOR_REACTOR_H_INCLUDED
#define REACTOR_REACTOR_H_INCLUDED

#include <stdlib.h>
#include <stdint.h>
#include <time.h>

enum reactor_event_type
{
  REACTOR_EPOLL_EVENT
};

typedef struct reactor_handler    reactor_handler;
typedef struct reactor_event      reactor_event;
typedef struct timer_node         timer_node;
typedef struct timer_guard        timer_guard;
typedef struct reactor_guard      reactor_guard;
typedef void                      reactor_callback(reactor_event *);

struct reactor_handler
{
  reactor_callback   *callback;
  void               *state;
};

struct reactor_event
{
  int                 type;
  void               *state;
  uintptr_t           data;
};

/* reactor_handler */

void     reactor_handler_construct(reactor_handler *, reactor_callback *, void *);
void     reactor_handler_destruct(reactor_handler *);

/* reactor */

uint64_t reactor_now(void);
void     reactor_construct(void);
void     reactor_destruct(void);
void     reactor_add(reactor_handler *, int, uint32_t);
void     reactor_modify(reactor_handler *, int, uint32_t);
void     reactor_delete(reactor_handler *, int);
void     reactor_dispatch(reactor_handler *, int, uintptr_t);
int      reactor_loop_once(void); /* Returns number of events processed */
int      reactor_loop(void); /* Returns total events processed */
void     reactor_abort(void);

/* Timer management API */
timer_node *reactor_timer_add(int fd, time_t expire, void (*callback)(int), void *user_data);
void        reactor_timer_del(timer_node *node);
void        reactor_timer_update(timer_node *node, time_t expire);

/* Configuration API */
void     reactor_set_timeout(int timeout_ms);
int      reactor_get_timeout(void);
size_t   reactor_get_descriptor_count(void);
size_t   reactor_get_event_count(void);
int      reactor_get_fd(void);

/* RAII resource management */
reactor_guard      *reactor_create_guard(void);
void                reactor_destroy_guard(reactor_guard *guard);
timer_guard        *reactor_create_timer_guard(int fd, time_t expire, void (*callback)(int), void *user_data);
void                reactor_destroy_timer_guard(timer_guard *guard);

/* IX-inspired optimizations */
int      reactor_get_batch_size(void);
int      reactor_is_congested(void);

/* Zero-copy I/O functions */
int      reactor_writev(int fd, struct iovec *iov, int iovcnt);
int      reactor_sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
void    *reactor_mmap_file(const char *filename, size_t *size);
void     reactor_unmap_file(void *addr, size_t size);

/* IX-inspired memory pool functions */
void    *reactor_mem_alloc(size_t size);
void     reactor_mem_free(void *ptr);

/* IX run-to-completion functions */
void     reactor_enable_run_to_completion(void (*callback)(void *state), void *state);
void     reactor_disable_run_to_completion(void);
int      reactor_is_run_to_completion_enabled(void);

/* Optimized run-to-completion event loop */
void     reactor_set_rtc_handlers(void (*accept_handler)(void),
                                 int (*read_handler)(int fd, void *buf, size_t len),
                                 void (*process_handler)(int fd, void *data, size_t len),
                                 int (*write_handler)(int fd, void *data, size_t len));
int      reactor_loop_once_optimized(void);

/* IX kernel-level transaction processing */
typedef struct ix_transaction {
    int accept_count;      /* New connections accepted */
    int read_count;        /* Data reads processed */
    int process_count;     /* Requests processed */
    int write_count;       /* Responses written */
    uint64_t start_time;   /* Transaction start timestamp */
    uint64_t end_time;     /* Transaction end timestamp */
} ix_transaction;

int      reactor_process_ix_transaction(void (*accept_handler)(void),
                                       int (*read_handler)(void *data, size_t len),
                                       void (*process_handler)(void *result),
                                       int (*write_handler)(void *data, size_t len));
ix_transaction reactor_get_last_transaction(void);
int      reactor_process_ix_batch(int batch_size,
                                 void (*accept_handler)(void),
                                 int (*read_handler)(void *data, size_t len),
                                 void (*process_handler)(void *result),
                                 int (*write_handler)(void *data, size_t len));

/* Global IX transaction context */
extern ix_transaction current_transaction;

#endif /* REACTOR_REACTOR_H_INCLUDED */
