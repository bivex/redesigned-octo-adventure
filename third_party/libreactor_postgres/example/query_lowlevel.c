#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/queue.h>

#include <libpq-fe.h>

#include <reactor.h>

#include "reactor_postgres.h"

typedef struct app_state
{
  reactor_postgres *p;
  int               insert_done;
} app_state;

void postgres_event(reactor_event *ev)
{
  app_state *state = ev->state;
  reactor_postgres *p = state->p;
  PGresult *result;

  (void) fprintf(stderr, "[postgres_event] type: %d, state: %d\n", ev->type, p->state);

  switch (ev->type)
    {
    case REACTOR_POSTGRES_EVENT_READY:
      if (!state->insert_done)
        {
          (void) fprintf(stderr, "[postgres_event] READY: sending INSERT query\n");
          reactor_postgres_send(p, "INSERT INTO test_data (message) VALUES ('hello asap')");
          state->insert_done = 1;
        }
      break;
    case REACTOR_POSTGRES_EVENT_CLOSE:
      (void) fprintf(stderr, "[postgres_event] CLOSE: connection closed\n");
      break;
    case REACTOR_POSTGRES_EVENT_QUERY_BAD:
      (void) fprintf(stderr, "[postgres_event] QUERY_BAD: %s\n", (char *) ev->data);
      reactor_postgres_close(p);
      break;
    case REACTOR_POSTGRES_EVENT_RESULT:
      result = (PGresult *) ev->data;
      if (state->insert_done == 1) /* This was the result of the INSERT */
        {
          (void) fprintf(stderr, "[postgres_event] INSERT RESULT: status: %s\n",
                         PQresStatus(PQresultStatus(result)));
          if (PQresultStatus(result) == PGRES_COMMAND_OK)
            {
              (void) fprintf(stderr, "[postgres_event] INSERT successful. Sending SELECT query.\n");
              reactor_postgres_send(p, "SELECT id, message FROM test_data WHERE message = 'hello asap'");
              state->insert_done = 2; /* Mark as select pending */
            }
          else
            {
              (void) fprintf(stderr, "[postgres_event] INSERT failed!\n");
              reactor_postgres_close(p);
            }
        }
      else if (state->insert_done == 2) /* This is the result of the SELECT */
        {
          (void) fprintf(stderr, "[postgres_event] SELECT RESULT: query result received\n");
          (void) fprintf(stderr, "[postgres_event] SELECT RESULT: status: %s, rows: %d\n",
                         PQresStatus(PQresultStatus(result)), PQntuples(result));
          if (PQntuples(result) > 0 && PQnfields(result) > 0)
            {
              for (int i = 0; i < PQntuples(result); i++)
                {
                  (void) fprintf(stderr, "[postgres_event] SELECT RESULT: row %d: id=%s, message='%s'\n",
                                 i, PQgetvalue(result, i, 0), PQgetvalue(result, i, 1));
                }
            }
          else
            {
              (void) fprintf(stderr, "[postgres_event] SELECT RESULT: No data found!\n");
            }
        }
      break;
    case REACTOR_POSTGRES_EVENT_QUERY_DONE:
      (void) fprintf(stderr, "[postgres_event] QUERY_DONE: closing connection\n");
      reactor_postgres_close(p);
      break;
    case REACTOR_POSTGRES_EVENT_ERROR:
      (void) fprintf(stderr, "[postgres_event] ERROR: '%s'\n", (char *) ev->data);
      reactor_postgres_close(p);
      break;
    }
}

int main()
{
  reactor_postgres p;
  app_state state = {.p = &p, .insert_done = 0};

  (void) fprintf(stderr, "[main] Starting reactor...\n");
  reactor_construct();
  (void) fprintf(stderr, "[main] Opening PostgreSQL connection...\n");
  reactor_postgres_open(&p, postgres_event, &state,
                        (const char *[]){"host", "user", "password", "dbname", "port", NULL},
                        (const char *[]){"127.0.0.1", "pguser", "qwertt111", "postgres", "5432", NULL});
  (void) fprintf(stderr, "[main] Entering reactor loop...\n");
  reactor_loop();
  (void) fprintf(stderr, "[main] Reactor loop exited, destructing...\n");
  reactor_destruct();
  return 0;
}
