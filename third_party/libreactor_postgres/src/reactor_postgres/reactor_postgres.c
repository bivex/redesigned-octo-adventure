#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include <libpq-fe.h>
#include <reactor.h>
#include <assert.h>
#include <sys/epoll.h> /* Added for struct epoll_event */

#include "reactor_postgres.h"
#include "../../../libreactor/src/debug.h"

void reactor_postgres_hold(reactor_postgres *pg)
{
  pg->ref ++;
}

void reactor_postgres_release(reactor_postgres *pg)
{
  pg->ref --;
  if (!pg->ref)
    {
      // Only dispatch close event if we haven't been destroyed already
      // This prevents double CLOSE events during connection cleanup
      if (pg->socket >= 0)
        {
          reactor_dispatch(&pg->user, REACTOR_POSTGRES_EVENT_CLOSE, (uintptr_t)NULL);
        }

      if (pg->socket >= 0)
        reactor_delete(&pg->handler, pg->socket);
      pg->socket = -1;
      if (pg->connection)
        {
          PQfinish(pg->connection);
          pg->connection = NULL;
        }
      reactor_handler_destruct(&pg->user);
      reactor_handler_destruct(&pg->handler);
    }
}

static void reactor_postgres_error(reactor_postgres *pg)
{
  // Prevent multiple error dispatches
  if (pg->state == REACTOR_POSTGRES_STATE_ERROR) {
    return;
  }

  char *error = pg->connection ? PQerrorMessage(pg->connection) : NULL;

  reactor_modify(&pg->handler, pg->socket, 0);
  pg->state = REACTOR_POSTGRES_STATE_ERROR;
  reactor_dispatch(&pg->user, REACTOR_POSTGRES_EVENT_ERROR, (uintptr_t)error);
}

static void reactor_postgres_state(reactor_postgres *p)
{
  int state_saved;
  PostgresPollingStatusType poll_status;
  int flush_status; /* Moved declaration here */

  if (!(p->state & (REACTOR_POSTGRES_STATE_CONNECTING |
                    REACTOR_POSTGRES_STATE_BUSY |
                    REACTOR_POSTGRES_STATE_AVAILABLE)))
    return;

  reactor_modify(&p->handler, p->socket, 0); /* Clear all events initially */

  poll_status = PQconnectPoll(p->connection);

  switch (poll_status)
    {
    case PGRES_POLLING_FAILED:
      reactor_postgres_error(p);
      return;
    case PGRES_POLLING_WRITING:
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ | DESCRIPTOR_WRITE); /* Keep both for busy-waiting */
      /* Aggressively flush data until all is sent or an error occurs */
      do
        {
          flush_status = PQflush(p->connection);
          if (flush_status == -1)
            {
              reactor_postgres_error(p);
              return;
            }
        }
      while (flush_status == 1);
      
      /* If all data sent, schedule state re-evaluation for next event loop iteration to avoid recursion */
      if (flush_status == 0) {
        /* State will be re-evaluated on next epoll event */
      }
      break;
    case PGRES_POLLING_READING:
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ);
      break;
    case PGRES_POLLING_OK:
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ | DESCRIPTOR_WRITE);
      if (p->state == REACTOR_POSTGRES_STATE_CONNECTING)
        p->state = REACTOR_POSTGRES_STATE_BUSY;
      break;
    case PGRES_POLLING_ACTIVE:
      break;
    }

  if (p->state == REACTOR_POSTGRES_STATE_CONNECTING)
    return;

  if (PQisBusy(p->connection))
    p->state = REACTOR_POSTGRES_STATE_BUSY;
  else
    {
      state_saved = p->state;
      p->state = REACTOR_POSTGRES_STATE_AVAILABLE;
      if (state_saved == REACTOR_POSTGRES_STATE_BUSY)
        reactor_dispatch(&p->user, REACTOR_POSTGRES_EVENT_READY, (uintptr_t)NULL);
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ);
    }
}

static void reactor_postgres_read(reactor_postgres *p)
{
  int status, broken = 0;
  PGresult *result;

  status = PQconsumeInput(p->connection);
  if (!status) {
    // Only consider it an error if the connection status is bad
    if (PQstatus(p->connection) == CONNECTION_BAD) {
      REACTOR_DEBUG("[postgres_read] PQconsumeInput failed and connection is bad for %p. Error: %s\n", (void *)p, PQerrorMessage(p->connection));
      reactor_postgres_error(p);
    } else {
      REACTOR_DEBUG("[postgres_read] PQconsumeInput returned 0, but connection is still OK for %p. Waiting for more input.\n", (void *)p);
    }
      return;
    }

  reactor_postgres_hold(p);
  while (1)
    {
      result = PQgetResult(p->connection);
      if (!result)
        break;
      switch (PQresultStatus(result))
        {
        case PGRES_SINGLE_TUPLE:
        case PGRES_TUPLES_OK:
        case PGRES_COMMAND_OK:
          reactor_dispatch(&p->user, REACTOR_POSTGRES_EVENT_RESULT, (uintptr_t)result);
          break;
        case PGRES_FATAL_ERROR:
          broken = 1;
          break;
        default:
          reactor_dispatch(&p->user, REACTOR_POSTGRES_EVENT_QUERY_BAD, (uintptr_t)PQerrorMessage(p->connection));
          break;
        }
      PQclear(result);
    }

  if (!PQisBusy(p->connection))
    p->state = REACTOR_POSTGRES_STATE_AVAILABLE;
  reactor_dispatch(&p->user, REACTOR_POSTGRES_EVENT_QUERY_DONE, (uintptr_t)NULL);

  if (broken)
    reactor_postgres_error(p);
  reactor_postgres_release(p);
}

static void reactor_postgres_event(reactor_event *event)
{
  reactor_postgres *p = event->state;
  struct epoll_event *e = (struct epoll_event *) event->data;
  uint32_t revents = e->events;
  int flush_status;

  assert(event->type == REACTOR_EPOLL_EVENT);
  switch (p->state)
    {
    case REACTOR_POSTGRES_STATE_CONNECTING:
      reactor_postgres_state(p);
      break;
    case REACTOR_POSTGRES_STATE_BUSY:
    case REACTOR_POSTGRES_STATE_AVAILABLE:
      if (revents & POLLIN)
        reactor_postgres_read(p);
      if (revents & POLLOUT)
        {
          flush_status = PQflush(p->connection);
          if (flush_status == -1)
            {
              reactor_postgres_error(p);
              return;
            }
          if (flush_status == 0) /* All data sent, stop listening for write events */
            reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ);
          reactor_postgres_state(p); /* Re-evaluate state after flush */
        }
      break;
    }
}

void reactor_postgres_open(reactor_postgres *p, reactor_callback *callback, void *state,
                           const char *keys[], const char *values[])
{
  REACTOR_DEBUG("[postgres_open] Initializing postgres connection %p.\n", (void *)p);
  p->ref = 0;
  p->state = REACTOR_POSTGRES_STATE_CONNECTING;
  reactor_handler_construct(&p->user, callback, state);
  reactor_handler_construct(&p->handler, reactor_postgres_event, p);
  p->connection = PQconnectStartParams(keys, values, 0);
  if (!p->connection)
    {
      REACTOR_DEBUG("[postgres_open] PQconnectStartParams failed for %p. Out of memory?\n", (void *)p);
      p->socket = -1; // Indicate no valid socket
      reactor_postgres_error(p); // Dispatch error
      return;
    }
  if (PQstatus(p->connection) == CONNECTION_BAD)
    {
      REACTOR_DEBUG("[postgres_open] Initial connection status BAD for %p: %s\n", (void *)p, PQerrorMessage(p->connection));
      p->socket = -1; // Indicate no valid socket
      reactor_postgres_error(p); // Dispatch error
      PQfinish(p->connection);
      p->connection = NULL;
      return;
    }

  PQsetnonblocking(p->connection, 1);
  p->socket = PQsocket(p->connection);
  if (p->socket < 0)
    {
      REACTOR_DEBUG("[postgres_open] Failed to get socket for %p. Error: %s\n", (void *)p, PQerrorMessage(p->connection));
      reactor_postgres_error(p); // Dispatch error
      PQfinish(p->connection);
      p->connection = NULL;
      return;
    }
  REACTOR_DEBUG("[postgres_open] Adding socket %d to reactor for %p.\n", p->socket, (void *)p);
  reactor_add(&p->handler, p->socket, DESCRIPTOR_READ | DESCRIPTOR_WRITE); /* Register with initial read/write events */
  reactor_postgres_hold(p);
  REACTOR_DEBUG("[postgres_open] Initial state evaluation for %p.\n", (void *)p);
  reactor_postgres_state(p);
  REACTOR_DEBUG("[postgres_open] postgres connection %p open attempt complete.\n", (void *)p);
}

void reactor_postgres_close(reactor_postgres *pg)
{
  REACTOR_DEBUG("[postgres_close] Closing postgres connection %p.\n", (void *)pg);
  if (pg->state & REACTOR_POSTGRES_STATE_CLOSED)
    {
      REACTOR_DEBUG("[postgres_close] Connection %p already closed, returning.\n", (void *)pg);
      return;
    }

  pg->state = REACTOR_POSTGRES_STATE_CLOSED;
  reactor_postgres_release(pg);
  REACTOR_DEBUG("[postgres_close] Connection %p close initiated.\n", (void *)pg);
}

void reactor_postgres_send(reactor_postgres *p, char *query)
{
  int status;

  // Safety checks
  if (!p || !query) {
    REACTOR_DEBUG("[postgres_send] ERROR: NULL postgres handle or query\n");
    return;
  }

  REACTOR_DEBUG("[postgres_send] Sending query \"%s\" on connection %p.\n", query, (void *)p);
  if (p->state != REACTOR_POSTGRES_STATE_AVAILABLE)
    {
      REACTOR_DEBUG("[postgres_send] Connection %p not available, state: %d. Error.\n", (void *)p, p->state);
      reactor_postgres_error(p);
      return;
    }

  status = PQsendQuery(p->connection, query);
  if (!status)
    {
      REACTOR_DEBUG("[postgres_send] PQsendQuery failed for %p. Error: %s\n", (void *)p, PQerrorMessage(p->connection));
      reactor_postgres_error(p);
      return;
    }
  
  /* Flush any pending output and adjust event mask accordingly */
  status = PQflush(p->connection);
  if (status == -1)
    {
      REACTOR_DEBUG("[postgres_send] PQflush failed for %p. Error: %s\n", (void *)p, PQerrorMessage(p->connection));
      reactor_postgres_error(p);
      return;
    }
  
  if (status == 1) /* Output buffer not empty, still need to write */
    {
      REACTOR_DEBUG("[postgres_send] Output buffer not empty, modifying for write events.\n");
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ | DESCRIPTOR_WRITE);
    }
  else /* Output buffer empty, only listen for read */
    {
      REACTOR_DEBUG("[postgres_send] Output buffer empty, modifying for read events only.\n");
      reactor_modify(&p->handler, p->socket, DESCRIPTOR_READ);
    }
  
  p->state = REACTOR_POSTGRES_STATE_BUSY;
  REACTOR_DEBUG("[postgres_send] Query sent for %p. New state: %d.\n", (void *)p, p->state);
}
