#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/queue.h>

#include <libpq-fe.h>
#include <reactor.h>

#include "reactor_postgres.h"
#include "reactor_postgres_client.h"
#include "../../../libreactor/src/debug.h"
#include "../../test/test_state.h" // Include the new test_state.h

// Forward declarations
static void reactor_postgres_client_connection_free(reactor_postgres_client_connection *c);
static void reactor_postgres_client_state_set(reactor_postgres_client *client, int state);
static void reactor_postgres_client_grow(reactor_postgres_client *client);
static void reactor_postgres_client_connection_event(reactor_event *event); // Forward declaration added here

static void reactor_postgres_client_error(reactor_postgres_client *client)
{
  if (client->state == REACTOR_POSTGRES_CLIENT_STATE_ERROR) return; // Already in error state

  client->error = true; // Set client error flag
  reactor_postgres_client_state_set(client, REACTOR_POSTGRES_CLIENT_STATE_ERROR); // Set client state to ERROR

  // Determine an appropriate error message to dispatch
  char *error_message = NULL;
  // Attempt to get error from any busy connection that might have triggered this
  reactor_postgres_client_connection *c;
  TAILQ_FOREACH(c, &client->connections_busy, entries)
    {
      if (c->postgres.connection && PQerrorMessage(c->postgres.connection)[0] != '\0')
        {
          error_message = strdup(PQerrorMessage(c->postgres.connection));
          break;
        }
    }
  // If no specific error from connection, use a generic message or client's last_error
  if (!error_message && client->last_error)
    {
      error_message = strdup(client->last_error);
    }
  if (!error_message)
    {
      error_message = strdup("Unknown client error");
    }

  // Store the error message in client->last_error for external access
  if (client->last_error) free(client->last_error);
  client->last_error = error_message; // client->last_error now owns the string

  REACTOR_DEBUG("[client_error] Client %p encountered an error: %s\n", (void *)client, client->last_error);
  reactor_dispatch(&client->user, REACTOR_POSTGRES_CLIENT_EVENT_ERROR, (uintptr_t)client->last_error);
}

static void reactor_postgres_client_grow(reactor_postgres_client *client)
{
  reactor_postgres_client_connection *c;

  if (client->closing) /* Do not grow if client is closing */
    {
      REACTOR_DEBUG("[grow] Client %p is closing, not growing connection pool.\n", (void *)client);
      return;
    }

  if (client->connections >= client->connections_max)
    {
      REACTOR_DEBUG("[grow] Client %p connection pool at max (%d), not growing.\n", (void *)client, client->connections_max);
      return;
    }

  // Temporary: For testing purposes, simulate connection failure if force_connection_error is set
  struct test_state *test_s = (struct test_state *)client->user.state;
  if (test_s && test_s->force_connection_error)
    {
      REACTOR_DEBUG("[grow] Simulating connection failure for test_client %p (skipping connection creation).\n", (void *)client);
      // Don't create a connection at all - just return and let the event loop retry later
      return;
    }

  c = malloc(sizeof *c);
  if (!c)
    {
      REACTOR_DEBUG("[grow] Malloc failed for new connection. Client %p - will retry later.\n", (void *)client);
      return; // Don't call error - just fail this attempt
    }
  REACTOR_DEBUG("[debug] allocated connection %p\n", (void *)c);
  c->client = client;
  c->query = NULL;
  c->destroy = 0; /* Initialize destroy flag */
  c->reconnect = 0; /* Initialize reconnect flag */
  TAILQ_INSERT_TAIL(&client->connections_busy, c, entries);
  client->connections ++;
  client->stats.connections ++;
  client->stats.connections_busy ++;

  REACTOR_DEBUG("[grow] Opening reactor_postgres for connection %p (client %p).\n", (void *)c, (void *)client);
  reactor_postgres_open(&c->postgres, reactor_postgres_client_connection_event, c, (const char**)client->keys, (const char**)client->values);

  /* Check if connection failed immediately */
  if (c->postgres.socket < 0 || (c->postgres.connection && PQstatus(c->postgres.connection) == CONNECTION_BAD))
    {
      REACTOR_DEBUG("[grow] Initial connection failed for %p. Cleaning up and will retry later.\n", (void *)c);
      /* Connection failed, clean up */
      TAILQ_REMOVE(&client->connections_busy, c, entries);
      client->connections --;
      client->stats.connections_busy --;
      reactor_postgres_close(&c->postgres); // This will call reactor_postgres_release, which calls reactor_delete
      reactor_postgres_client_connection_free(c);
      // Don't call reactor_postgres_client_error - just fail this attempt
      return;
    }
}

void reactor_postgres_client_dequeue(reactor_postgres_client *client)
{
  reactor_postgres_client_connection *connection;
  reactor_postgres_client_query *query;

  // Prevent dequeuing if client is closing
  if (client->closing) {
    return;
  }

  while (!TAILQ_EMPTY(&client->connections_available) && !TAILQ_EMPTY(&client->queries_waiting))
    {
      connection = TAILQ_FIRST(&client->connections_available);
      if (!connection) break; // Safety check

      REACTOR_DEBUG("[dequeue] Dequeuing query for client %p.\n", (void *)client);
      TAILQ_REMOVE(&client->connections_available, connection, entries);
      TAILQ_INSERT_TAIL(&client->connections_busy, connection, entries);
      client->stats.connections_busy ++;

      query = TAILQ_FIRST(&client->queries_waiting);
      if (!query) break; // Safety check

      TAILQ_REMOVE(&client->queries_waiting, query, entries);
      TAILQ_INSERT_TAIL(&client->queries_running, query, entries);

      connection->query = query;
      reactor_postgres_send(&connection->postgres, query->command);
    }
  
  // If there are still waiting queries but no available connections, try to grow the pool
  if (!TAILQ_EMPTY(&client->queries_waiting) && TAILQ_EMPTY(&client->connections_available))
    {
      if (client->connections < client->connections_max)
        {
          REACTOR_DEBUG("[dequeue] Queries waiting but no connections available. Attempting to grow pool.\n");
          reactor_postgres_client_grow(client);
        }
    }
}

static void reactor_postgres_client_connection_available(reactor_postgres_client *client,
                                                         reactor_postgres_client_connection *connection)
{
  client->stats.connections_busy --;
  TAILQ_REMOVE(&client->connections_busy, connection, entries);
  TAILQ_INSERT_TAIL(&client->connections_available, connection, entries);
}

static void reactor_postgres_client_state_set(reactor_postgres_client *client, int state)
{
  if (client->state == state)
    return;

  REACTOR_DEBUG("[client_state_set] Client %p state change from %d to %d.\n", (void *)client, client->state, state);
  client->state = state;
  if (state == REACTOR_POSTGRES_CLIENT_STATE_OPEN)
    reactor_dispatch(&client->user, REACTOR_POSTGRES_CLIENT_EVENT_READY, (uintptr_t)NULL);
  else if (state == REACTOR_POSTGRES_CLIENT_STATE_ERROR)
    reactor_dispatch(&client->user, REACTOR_POSTGRES_CLIENT_EVENT_ERROR, (uintptr_t)client->last_error);
  else if (state == REACTOR_POSTGRES_CLIENT_STATE_CLOSED)
    reactor_dispatch(&client->user, REACTOR_POSTGRES_CLIENT_EVENT_CLOSE, (uintptr_t)NULL);
}

static void reactor_postgres_client_connection_event(reactor_event *event)
{
  reactor_postgres_client_connection *c = event->state;
  int type = event->type;
  void *data = (void *) event->data;

  // Safety check for NULL connection
  if (!c || !c->client) {
    REACTOR_DEBUG("[connection_event] ERROR: NULL connection or client in event handler\n");
    return;
  }

  REACTOR_DEBUG("[connection_event] Connection %p received event type: %d.\n", (void *)c, type);
  switch (type)
    {
    case REACTOR_POSTGRES_EVENT_READY:
      REACTOR_DEBUG("[connection_event] Connection %p ready.\n", (void *)c);
      reactor_postgres_client_connection_available(c->client, c);
      if (c->client->state == REACTOR_POSTGRES_CLIENT_STATE_CONNECTING) // Only set to open if currently connecting
        reactor_postgres_client_state_set(c->client, REACTOR_POSTGRES_CLIENT_STATE_OPEN); // Set client state to OPEN
      if (!c->client->closing) // Only dequeue if client is not closing
        reactor_postgres_client_dequeue(c->client);
      break;
    case REACTOR_POSTGRES_EVENT_QUERY_BAD:
      REACTOR_DEBUG("[connection_event] Connection %p bad query.\n", (void *)c);
      if (c->query)
        reactor_dispatch(&c->query->user, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD, (uintptr_t)data);
      break;
    case REACTOR_POSTGRES_EVENT_RESULT:
      REACTOR_DEBUG("[connection_event] Connection %p query result.\n", (void *)c);
      if (c->query)
        {
          reactor_dispatch(&c->query->user, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT, event->data);
        }
      break;
    case REACTOR_POSTGRES_EVENT_CLOSE:
      REACTOR_DEBUG("[connection_event] Connection %p closed.\n", (void *)c);

      if (c->query)
        {
          reactor_dispatch(&c->query->user, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE, (uintptr_t)NULL);
        }

      // Safely remove from lists - check which list it's actually in
      reactor_postgres_client_connection *tmp;
      int was_in_busy = 0;
      TAILQ_FOREACH(tmp, &c->client->connections_busy, entries) {
        if (tmp == c) {
          TAILQ_REMOVE(&c->client->connections_busy, c, entries);
          was_in_busy = 1;
          break;
        }
      }
      if (!was_in_busy) {
        TAILQ_FOREACH(tmp, &c->client->connections_available, entries) {
          if (tmp == c) {
            TAILQ_REMOVE(&c->client->connections_available, c, entries);
            break;
          }
        }
      }

      c->client->connections --;
      
      // Free the connection immediately
      REACTOR_DEBUG("[connection_event] Freeing connection %p after close.\n", (void *)c);
      reactor_postgres_client_connection_free(c);
      
      // Trigger pool growth to replace the closed connection if there are waiting queries
      if (!TAILQ_EMPTY(&c->client->queries_waiting) && !c->client->closing) {
        REACTOR_DEBUG("[connection_event] Growing pool to replace closed connection.\n");
        reactor_postgres_client_grow(c->client);
        // Try to dequeue immediately in case the new connection is ready
        reactor_postgres_client_dequeue(c->client);
      }
      break;
    case REACTOR_POSTGRES_EVENT_ERROR:
      REACTOR_DEBUG("[connection_event] Connection %p error.\n", (void *)c);
      
      // Store error message in client->last_error for debugging
      if (c->postgres.connection && PQerrorMessage(c->postgres.connection)[0] != '\0')
        {
          if (c->client->last_error) free(c->client->last_error);
          c->client->last_error = strdup(PQerrorMessage(c->postgres.connection));
          REACTOR_DEBUG("[connection_event] Stored error: %s\n", c->client->last_error);
        }
      
      // If there's a query assigned to this connection, return it to the waiting queue
      if (c->query)
        {
          REACTOR_DEBUG("[connection_event] Returning query to waiting queue after connection error.\n");
          // Remove from running queue
          TAILQ_REMOVE(&c->client->queries_running, c->query, entries);
          // Add back to waiting queue at the front for retry
          TAILQ_INSERT_HEAD(&c->client->queries_waiting, c->query, entries);
          c->query = NULL;
        }
      
      // Remove connection from lists
      reactor_postgres_client_connection *tmp_err;
      int was_in_busy_err = 0;
      TAILQ_FOREACH(tmp_err, &c->client->connections_busy, entries) {
        if (tmp_err == c) {
          TAILQ_REMOVE(&c->client->connections_busy, c, entries);
          was_in_busy_err = 1;
          break;
        }
      }
      if (!was_in_busy_err) {
        TAILQ_FOREACH(tmp_err, &c->client->connections_available, entries) {
          if (tmp_err == c) {
            TAILQ_REMOVE(&c->client->connections_available, c, entries);
            break;
          }
        }
      }
      
      c->client->connections --;
      
      // Free the connection immediately
      REACTOR_DEBUG("[connection_event] Freeing connection %p after error.\n", (void *)c);
      reactor_postgres_client_connection_free(c);
      
      // Trigger pool growth to replace the failed connection if there are waiting queries
      if (!TAILQ_EMPTY(&c->client->queries_waiting) && !c->client->closing) {
        REACTOR_DEBUG("[connection_event] Growing pool to replace failed connection.\n");
        reactor_postgres_client_grow(c->client);
        // Try to dequeue immediately in case the new connection is ready
        reactor_postgres_client_dequeue(c->client);
      }
      break;
    case REACTOR_POSTGRES_EVENT_QUERY_DONE:
      REACTOR_DEBUG("[connection_event] Connection %p query done.\n", (void *)c);
      reactor_postgres_client_connection_available(c->client, c);
      if (c->query)
        {
          reactor_handler user_handler = c->query->user; // Store handler
          c->client->stats.queries_completed ++;
          TAILQ_REMOVE(&c->client->queries_running, c->query, entries);
          c->query = NULL; // Clear c->query BEFORE dispatch
          reactor_dispatch(&user_handler, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE, (uintptr_t)NULL);
        }
      reactor_postgres_client_dequeue(c->client);
      break;
    default:
      REACTOR_DEBUG("[connection_event] Connection %p received unknown event type: %d.\n", (void *)c, type);
      break;
    }
}

static void reactor_postgres_client_connection_free(reactor_postgres_client_connection *c)
{
  if (!c) return; // Safety check

  REACTOR_DEBUG("[debug] freeing connection %p\n", (void *)c);

  // Always free the connection now that we control the destruction
  free(c);
}

static void reactor_postgres_client_abort(reactor_postgres_client *client)
{
  reactor_postgres_client_connection *c;
  reactor_postgres_client_query *q;
  TAILQ_HEAD(, reactor_postgres_client_query) aborted_queries_head;
  TAILQ_INIT(&aborted_queries_head);

  REACTOR_DEBUG("[client_abort] Aborting client %p.\n", (void *)client);
  while (!TAILQ_EMPTY(&client->queries_running))
    {
      q = TAILQ_FIRST(&client->queries_running);
      TAILQ_REMOVE(&client->queries_running, q, entries);
      TAILQ_INSERT_TAIL(&aborted_queries_head, q, entries);
    }
  while (!TAILQ_EMPTY(&client->queries_waiting))
    {
      q = TAILQ_FIRST(&client->queries_waiting);
      TAILQ_REMOVE(&client->queries_waiting, q, entries);
      TAILQ_INSERT_TAIL(&aborted_queries_head, q, entries);
    }

  while (!TAILQ_EMPTY(&aborted_queries_head))
    {
      q = TAILQ_FIRST(&aborted_queries_head);
      TAILQ_REMOVE(&aborted_queries_head, q, entries);
      reactor_dispatch(&q->user, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT, (uintptr_t)NULL);
      reactor_dispatch(&q->user, REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE, (uintptr_t)NULL);
    }

  TAILQ_FOREACH(c, &client->connections_busy, entries)
    if (c->query)
      c->query = NULL; // This line should now be safe because c->query is cleared earlier
}

void reactor_postgres_client_hold(reactor_postgres_client *client)
{
  client->ref ++;
}

void reactor_postgres_client_release(reactor_postgres_client *client)
{
  reactor_postgres_client_connection *c;
  reactor_postgres_client_query *q;
  size_t i;

  client->ref --;
  if (client->ref > 0)
    return; // Only free when ref count reaches zero

  REACTOR_DEBUG("[client_release] Releasing client %p.\n", (void *)client);
  /* Free connections */
  while (!TAILQ_EMPTY(&client->connections_busy))
    {
      c = TAILQ_FIRST(&client->connections_busy);
      TAILQ_REMOVE(&client->connections_busy, c, entries);
      reactor_postgres_release(&c->postgres); // Release the underlying postgres object
      reactor_postgres_client_connection_free(c); // Free the connection object itself
    }
  while (!TAILQ_EMPTY(&client->connections_available))
    {
      c = TAILQ_FIRST(&client->connections_available);
      TAILQ_REMOVE(&client->connections_available, c, entries);
      reactor_postgres_release(&c->postgres); // Release the underlying postgres object
      reactor_postgres_client_connection_free(c); // Free the connection object itself
    }

  /* Clear query queues - queries are owned by calling code */
  while (!TAILQ_EMPTY(&client->queries_running))
    {
      q = TAILQ_FIRST(&client->queries_running);
      TAILQ_REMOVE(&client->queries_running, q, entries);
      /* Don't free query objects - they are owned by calling code */
    }
  while (!TAILQ_EMPTY(&client->queries_waiting))
    {
      q = TAILQ_FIRST(&client->queries_waiting);
      TAILQ_REMOVE(&client->queries_waiting, q, entries);
      /* Don't free query objects - they are owned by calling code */
    }

  /* Free stored connection parameters */
  for (i = 0; client->keys[i]; ++i)
    {
      free(client->keys[i]);
      free(client->values[i]);
    }
  free(client->keys);
  free(client->values);

  /* Free last_error */
  if (client->last_error)
    free(client->last_error);

  /* Now clean up the client itself */
  reactor_handler_destruct(&client->user);
  free(client);
  REACTOR_DEBUG("[client_release] Client %p fully released.\n", (void *)client);
}

reactor_postgres_client *reactor_postgres_client_open(reactor_callback *callback, void *state,
                                  const char **keys_in, const char **values_in)
{
  reactor_postgres_client *client = malloc(sizeof(reactor_postgres_client));
  size_t i;

  if (!client)
    return NULL;

  REACTOR_DEBUG("[client_open] Initializing client %p.\n", (void *)client);
  *client = (reactor_postgres_client) {0};
  reactor_handler_construct(&client->user, callback, state);
  client->last_error = NULL; // Initialize last_error
  client->error = false; // Initialize the new error flag
  client->connections_min = 0;
  client->connections_max = 0;
  client->closing = 0;

  // Copy keys and values
  for (i = 0; keys_in[i]; ++i);
  client->keys = malloc(sizeof(char *) * (i + 1));
  client->values = malloc(sizeof(char *) * (i + 1));
  if (!client->keys || !client->values)
    {
      free(client->keys); // Free if one allocation succeeded but the other failed
      free(client->values);
      free(client);
      REACTOR_DEBUG("[client_open] Failed to allocate memory for keys/values.\n");
      return NULL;
    }
  for (i = 0; keys_in[i]; ++i)
    {
      client->keys[i] = strdup(keys_in[i]);
      client->values[i] = strdup(values_in[i]);
      if (!client->keys[i] || !client->values[i])
        {
          // Handle allocation failure, free already copied strings
          for (size_t j = 0; j <= i; ++j)
            {
              free(client->keys[j]);
              free(client->values[j]);
            }
          free(client->keys);
          free(client->values);
          free(client);
          REACTOR_DEBUG("[client_open] Failed to strdup key/value pair.\n");
          return NULL;
        }
    }
  client->keys[i] = NULL;
  client->values[i] = NULL;

  client->connections = 0;
  TAILQ_INIT(&client->connections_available);
  TAILQ_INIT(&client->connections_busy);
  TAILQ_INIT(&client->queries_running);
  TAILQ_INIT(&client->queries_waiting);
  reactor_postgres_client_hold(client);
  reactor_postgres_client_limits(client, 0, REACTOR_POSTGRES_CLIENT_CONNECTIONS_MAX);
  reactor_postgres_client_state_set(client, REACTOR_POSTGRES_CLIENT_STATE_CONNECTING); // Set initial state

  REACTOR_DEBUG("[client_open] Client %p initialized. Connections: %d.\n", (void *)client, client->connections);
  return client;
}

void reactor_postgres_client_limits(reactor_postgres_client *client, int min, int max)
{
  int i;

  client->connections_max = max;
  for (i = client->connections; i < min; i ++)
    {
      REACTOR_DEBUG("[client_limits] Growing client %p connection pool. Current: %d/%d.\n", (void *)client, client->connections, client->connections_max);
      reactor_postgres_client_grow(client);
    }
}

void reactor_postgres_client_query_open(reactor_postgres_client_query *query, reactor_callback *callback, void *state,
                                        reactor_postgres_client *client, char *command)
{
  REACTOR_DEBUG("[client_query_open] Opening query %p for client %p, command: %s.\n", (void *)query, (void *)client, command);
  reactor_handler_construct(&query->user, callback, state);
  query->command = command;
  client->stats.queries_queued ++;
  TAILQ_INSERT_TAIL(&client->queries_waiting, query, entries);
  if (!TAILQ_EMPTY(&client->connections_available))
    {
      REACTOR_DEBUG("[client_query_open] Available connection found, dequeuing.\n");
      reactor_postgres_client_dequeue(client);
    }
  else
    {
      REACTOR_DEBUG("[client_query_open] No available connections, growing pool.\n");
      reactor_postgres_client_grow(client);
    }
}

void reactor_postgres_client_close(reactor_postgres_client *client)
{
  REACTOR_DEBUG("[client_close] Closing client %p.\n", (void *)client);
  if (client->state == REACTOR_POSTGRES_CLIENT_STATE_CLOSED) // Check against CLOSED state
    {
      REACTOR_DEBUG("[client_close] Client %p already closed, returning.\n", (void *)client);
      return;
    }

  if (client->closing) /* Already closing, prevent double close */
    {
      REACTOR_DEBUG("[client_close] Client %p already closing, returning.\n", (void *)client);
      return;
    }

  client->closing = true; /* Set closing flag */
  reactor_postgres_client_abort(client);
  reactor_postgres_client_state_set(client, REACTOR_POSTGRES_CLIENT_STATE_CLOSED); // Set state to closed
  REACTOR_DEBUG("[client_close] Client %p close process initiated.\n", (void *)client);
}

void reactor_postgres_client_get_stats(reactor_postgres_client *client, reactor_postgres_client_stats *stats)
{
  *stats = client->stats;
}
