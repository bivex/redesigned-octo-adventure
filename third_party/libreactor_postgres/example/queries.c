#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <err.h>
#include <stdbool.h>
#include <unistd.h>

#include <libpq-fe.h>

#include <reactor.h>

#include "reactor_postgres.h"

typedef struct app app;
struct app
{
  reactor_postgres_client        *client;
  timer                          timer;
  size_t                         queries;
  bool                           timer_active;
  bool                           client_closed;
  TAILQ_HEAD(, app_query)        all_queries;
};

typedef struct app_query app_query;
struct app_query
{
  app                           *app;
  reactor_postgres_client_query  query;
  bool                          completed; // Флаг завершения запроса
  TAILQ_ENTRY(app_query)        entries;
};

static void app_stats(app *app)
{
  reactor_postgres_client_stats stats;

  reactor_postgres_client_get_stats(app->client, &stats);
  (void) fprintf(stderr, "[stats] connections %lu/%lu, qeuries %lu/%lu\n",
                 stats.connections_busy, stats.connections,
                 stats.queries_queued, stats.queries_completed);
}

static void query_event(reactor_event *event)
{
  app_query *q = event->state;
  PGresult *result;
  int type = event->type;
  void *data = (void *) event->data;


  switch (type)
    {
    case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT:
      result = data;
      (void) fprintf(stderr, "[query] result: status=%s, rows=%d, cols=%d\n",
                     PQresStatus(PQresultStatus(result)),
                     PQntuples(result), PQnfields(result));
      if (PQntuples(result) > 0 && PQnfields(result) > 0)
        {
          for (int i = 0; i < PQntuples(result); i++)
            {
              (void) fprintf(stderr, "[query] row %d: ", i);
              for (int j = 0; j < PQnfields(result); j++)
                {
                  (void) fprintf(stderr, "%s='%s'%s",
                                 PQfname(result, j),
                                 PQgetvalue(result, i, j),
                                 j < PQnfields(result) - 1 ? ", " : "\n");
                }
            }
        }
      else
        {
          (void) fprintf(stderr, "[query] no rows or columns in result\n");
        }
      break;
    case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD:
      (void) fprintf(stderr, "[query] bad: %s\n", (char *) data);
      break;
    case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT:
      (void) fprintf(stderr, "[query] abort\n");
      break;
    case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE:
      if (!q->completed)
        {
          q->completed = true;
          q->app->queries--;
          if (!q->app->queries)
            {
              app_stats(q->app);
              if (q->app->timer_active)
                {
                  timer_clear(&q->app->timer);
                  q->app->timer_active = false;
                }
              reactor_postgres_client_close(q->app->client);
              (void) fprintf(stderr, "[query] calling reactor_abort()\n");
              reactor_abort(); // Exit reactor loop - cleanup happens in main()
            }
        }
      break;
    }
}

static void timer_event(reactor_event *event)
{
  app *app = event->state;

  (void) event->data;
  switch (event->type)
    {
    case TIMER_EXPIRATION:
      // Stats are now only printed when queries complete, not periodically
      (void) app; // Suppress unused variable warning
      break;
    }
}

static void timeout_event(reactor_event *event)
{
  (void) event->state;
  (void) event->data;

  switch (event->type)
    {
    case TIMER_EXPIRATION:
      (void) fprintf(stderr, "[timeout] Forcing exit after 2 seconds\n");
      reactor_abort(); 
      break;
    }
}

static void client_event(reactor_event *event)
{
  app *app = event->state;
  (void) event->data;
  switch (event->type)
    {
    case REACTOR_POSTGRES_CLIENT_EVENT_READY:
      (void) fprintf(stderr, "[client] successfully connected to PostgreSQL\n");
      break;
    case REACTOR_POSTGRES_CLIENT_EVENT_ERROR:
      (void) fprintf(stderr, "[client] connection error - cannot connect to PostgreSQL\n");
      break;
    case REACTOR_POSTGRES_CLIENT_EVENT_CLOSE:
      (void) fprintf(stderr, "[client] connection closed\n");
      app->client_closed = true;
      break;
    }
}

void usage()
{
  extern char *__progname;

  (void) fprintf(stderr, "usage: %s <parallel> <count> <command>\n", __progname);
  (void) fprintf(stderr, "  parallel: number of parallel connections\n");
  (void) fprintf(stderr, "  count: number of queries to execute\n");
  (void) fprintf(stderr, "  command: SQL query to execute\n");
  (void) fprintf(stderr, "\nExamples:\n");
  (void) fprintf(stderr, "  %s 2 5 \"SELECT * FROM test_data\"\n", __progname);
  (void) fprintf(stderr, "  %s 1 1 \"INSERT INTO test_data (message) VALUES ('new message')\"\n", __progname);
  (void) fprintf(stderr, "  %s 3 10 \"SELECT id, message FROM test_data WHERE id > 0\"\n", __progname);
  exit(1);
}

int main(int argc, char **argv)
{
  app app = {0};
  app_query *q;
  size_t i, p, n;
  char *command;

  app.client_closed = false;
  timer timeout_timer;

  if (argc != 4)
    usage();
  p = strtoul(argv[1], NULL, 0);
  n = strtoul(argv[2], NULL, 0);
  command = argv[3];

  reactor_construct();
  timer_construct(&app.timer, timer_event, &app);
  timer_construct(&timeout_timer, timeout_event, NULL); // Таймер для таймауту
  TAILQ_INIT(&app.all_queries);

  app.client = reactor_postgres_client_open(client_event, &app,
                               (const char *[]){"host", "user", "password", "dbname", "port", NULL},
                               (const char *[]){"127.0.0.1", "pguser", "qwertt111", "postgres", "5432", NULL});
  if (!app.client)
    err(1, "reactor_postgres_client_open");

  reactor_postgres_client_limits(app.client, 0, p);

  for (i = 0; i < n; i++)
    {
      app.queries++;
      q = malloc(sizeof *q);
      if (!q)
        {
          (void) fprintf(stderr, "[error] malloc failed\n");
          exit(1);
        }
      q->app = &app;
      q->completed = false; // Инициализируем флаг
      TAILQ_INSERT_TAIL(&app.all_queries, q, entries);
      reactor_postgres_client_query_open(&q->query, query_event, q, app.client, command);
    }

  // Timer disabled - stats only printed when queries complete
  // timer_set(&app.timer, 100000000, 100000000);
  app.timer_active = false;

  // timer_set(&timeout_timer, 2000000000ULL, 0); // Временно отключаем таймаут

  reactor_loop();

  // Clean up timer if it was active
  if (app.timer_active)
    timer_clear(&app.timer);
  timer_destruct(&app.timer);
  timer_destruct(&timeout_timer);
  reactor_postgres_client_release(app.client);

  while (!TAILQ_EMPTY(&app.all_queries))
    {
      q = TAILQ_FIRST(&app.all_queries);
      TAILQ_REMOVE(&app.all_queries, q, entries);
      free(q);
    }

  reactor_destruct();
}
