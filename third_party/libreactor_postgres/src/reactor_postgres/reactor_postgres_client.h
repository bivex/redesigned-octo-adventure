#ifndef REACTOR_POSTGRES_CLIENT_H_INCLUDED
#define REACTOR_POSTGRES_CLIENT_H_INCLUDED

#ifndef REACTOR_POSTGRES_CLIENT_CONNECTIONS_MAX
#define REACTOR_POSTGRES_CLIENT_CONNECTIONS_MAX 1024
#endif

#include <netdb.h>
#include <sys/queue.h>
#include <stdbool.h>

#include <libpq-fe.h>
#include <reactor_postgres.h>

enum reactor_postgres_client_event
{
  REACTOR_POSTGRES_CLIENT_EVENT_READY,
  REACTOR_POSTGRES_CLIENT_EVENT_ERROR,
  REACTOR_POSTGRES_CLIENT_EVENT_CLOSE
};

enum reactor_postgres_client_state
{
  REACTOR_POSTGRES_CLIENT_STATE_OPEN        = 0x01,
  REACTOR_POSTGRES_CLIENT_STATE_CLOSED      = 0x02,
  REACTOR_POSTGRES_CLIENT_STATE_CONNECTING  = 0x04,
  REACTOR_POSTGRES_CLIENT_STATE_ERROR       = 0x08
};

enum reactor_postgres_client_query_event
{
  REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD,
  REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT,
  REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT,
  REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE
};

typedef struct reactor_postgres_client reactor_postgres_client;
typedef struct reactor_postgres_client_stats reactor_postgres_client_stats;
typedef struct reactor_postgres_client_query reactor_postgres_client_query; // Forward declaration

typedef struct reactor_postgres_client_connection {
  reactor_postgres postgres;
  struct reactor_postgres_client *client;
  reactor_postgres_client_query *query;
  int destroy; /* Mark connection for destruction */
  int reconnect; /* Flag to indicate if this connection needs to be re-established */
  TAILQ_ENTRY(reactor_postgres_client_connection) entries;
} reactor_postgres_client_connection;

struct reactor_postgres_client_stats
{
  size_t                                            queries_queued;
  size_t                                            queries_completed;
  size_t                                            connections;
  size_t                                            connections_busy;
};

struct reactor_postgres_client
{
  short                 ref;
  int                   connections;
  int                   connections_min; // Minimum connections to maintain
  int                   connections_max; // Maximum connections to allow
  reactor_handler        user;
  size_t                      queries_max;
  bool                        error; // New: indicates if the client is in an error state

  // Stats
  reactor_postgres_client_stats stats;
  bool                  closing;
  int                   state;
  char                **keys;
  char                **values;
  char                 *last_error;
  TAILQ_HEAD(, reactor_postgres_client_connection) connections_available;
  TAILQ_HEAD(, reactor_postgres_client_connection) connections_busy;
  TAILQ_HEAD(, reactor_postgres_client_query)     queries_running;
  TAILQ_HEAD(, reactor_postgres_client_query)     queries_waiting;
};

struct reactor_postgres_client_query
{
  reactor_handler                                      user;
  char                                             *command;
  TAILQ_ENTRY(reactor_postgres_client_query)        entries;
};

void reactor_postgres_client_hold(reactor_postgres_client *);
void reactor_postgres_client_release(reactor_postgres_client *);
reactor_postgres_client *reactor_postgres_client_open(reactor_callback *, void *,
                                  const char **, const char **);
void reactor_postgres_client_limits(reactor_postgres_client *, int, int);
void reactor_postgres_client_query_open(reactor_postgres_client_query *, reactor_callback *, void *,
                                        reactor_postgres_client *, char *);
void reactor_postgres_client_close(reactor_postgres_client *);
void reactor_postgres_client_get_stats(reactor_postgres_client *, reactor_postgres_client_stats *);

#endif /* REACTOR_POSTGRES_CLIENT_H_INCLUDED */
