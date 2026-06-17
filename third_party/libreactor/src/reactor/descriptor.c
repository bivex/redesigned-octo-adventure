/*
 * descriptor — io_uring backend
 *
 * Wraps an fd registered with the reactor ring via IORING_OP_POLL_ADD. Unlike
 * epoll, io_uring polls are one-shot: each completion must be re-armed
 * (reactor_poll_resubmit) for the next ready event to be delivered. That
 * re-arm happens here after dispatch so behaviour matches the old level/edge
 * epoll contract from the consumer's perspective.
 */
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <assert.h>

#include "reactor.h"
#include "descriptor.h"

/* Internal core hooks (defined in core.c, not part of public core.h). */
struct io_uring *reactor_ring(void);
void reactor_poll_resubmit(int fd);
void reactor_submit(void);

static uint32_t descriptor_events(descriptor *descriptor)
{
  /* EPOLLIN/POLLOUT values are identical on Linux; POLL_ADD accepts them. */
  return
    (descriptor->mask & DESCRIPTOR_READ ? POLLIN : 0) |
    (descriptor->mask & DESCRIPTOR_WRITE ? POLLOUT : 0);
}

static void descriptor_callback(reactor_event *event)
{
  descriptor *descriptor = event->state;
  /* event->data is the poll mask delivered by the io_uring POLL CQE (cqe->res). */
  uint32_t mask = (uint32_t) event->data;

  assert(event->type == REACTOR_EPOLL_EVENT);

  if (mask & POLLIN)
    reactor_dispatch(&descriptor->handler, DESCRIPTOR_READ, 0);
  if (mask & POLLOUT)
    reactor_dispatch(&descriptor->handler, DESCRIPTOR_WRITE, 0);
  if (mask & ~(POLLIN | POLLOUT))
    reactor_dispatch(&descriptor->handler, DESCRIPTOR_CLOSE, 0);

  /* Re-arm: io_uring POLL_ADD is one-shot per submission. */
  if (descriptor->fd >= 0)
    reactor_poll_resubmit(descriptor->fd);
}

void descriptor_construct(descriptor *descriptor, reactor_callback *callback, void *state)
{
  reactor_handler_construct(&descriptor->handler, callback, state);
  reactor_handler_construct(&descriptor->epoll_handler, descriptor_callback, descriptor);
  descriptor->fd = -1;
}

void descriptor_destruct(descriptor *descriptor)
{
  descriptor_close(descriptor);
  reactor_handler_destruct(&descriptor->handler);
  reactor_handler_destruct(&descriptor->epoll_handler);
}

void descriptor_open(descriptor *descriptor, int fd, enum descriptor_mask mask)
{
  descriptor->fd = fd;
  descriptor->mask = mask;
  reactor_add(&descriptor->epoll_handler, descriptor->fd, descriptor_events(descriptor));
}

void descriptor_mask(descriptor *descriptor, enum descriptor_mask mask)
{
  if (descriptor->mask == mask)
    return;
  descriptor->mask = mask;
  reactor_modify(&descriptor->epoll_handler, descriptor->fd, descriptor_events(descriptor));
}

void descriptor_close(descriptor *descriptor)
{
  if (!descriptor_is_open(descriptor))
    return;

  reactor_delete(&descriptor->epoll_handler, descriptor->fd);
  close(descriptor->fd);
  descriptor->fd = -1;
  descriptor->mask = 0;
}

int descriptor_fd(descriptor *descriptor)
{
  return descriptor->fd;
}

int descriptor_is_open(descriptor *descriptor)
{
  return descriptor->fd >= 0;
}
