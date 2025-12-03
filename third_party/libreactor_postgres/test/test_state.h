#ifndef TEST_STATE_H
#define TEST_STATE_H

#include <stdbool.h>

typedef struct test_state
{
  reactor_postgres_client *client; // Added back the client member
  bool                   connected;
  bool                   error;
  int                    queries_completed;
  int                    expected_queries;
  char                  *last_error;
  bool                   force_connection_error; // New flag to force connection errors in tests
} test_state;

#endif // TEST_STATE_H
