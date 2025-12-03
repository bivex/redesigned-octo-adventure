#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <unistd.h>
#include <err.h>

#include <reactor.h>
#include <reactor_postgres.h>
#include <reactor_postgres_client.h>

typedef struct test_app {
    reactor_postgres_client *client;
    bool running;
    bool connected;
} test_app;

static void client_event(reactor_event *event) {
    test_app *app = event->state;

    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_EVENT_READY:
            printf("✓ Connected\n");
            app->connected = true;
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_ERROR:
            printf("✗ Connection error\n");
            app->running = false;
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_CLOSE:
            printf("Connection closed\n");
            app->running = false;
            break;
    }
}

static void query_event(reactor_event *event) {
    // Simple query event handler
    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT:
            printf("Query result received\n");
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD:
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT:
            printf("Query error\n");
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE:
            printf("Query closed\n");
            break;
    }
}

int main() {
    test_app app = {0};
    app.running = true;
    app.connected = false;

    reactor_construct();

    // Connect to PostgreSQL
    printf("Connecting...\n");
    app.client = reactor_postgres_client_open(client_event, &app,
        (const char *[]){"host", "user", "password", "dbname", "port", NULL},
        (const char *[]){"127.0.0.1", "pguser", "qwertt111", "postgres", "5432", NULL});

    if (!app.client) {
        err(1, "Failed to create client");
    }

    // Wait for connection
    int attempts = 0;
    while (!app.connected && attempts++ < 50) {
        reactor_loop_once();
        usleep(100000);
    }

    if (!app.connected) {
        printf("Failed to connect\n");
        return 1;
    }

    // Execute a simple query
    printf("Executing query...\n");
    reactor_postgres_client_query q;
    reactor_postgres_client_query_open(&q, query_event, NULL, app.client, "SELECT 1");

    // Wait for query completion
    attempts = 0;
    while (app.running && attempts++ < 50) {
        reactor_loop_once();
        usleep(100000);
    }

    printf("Closing client...\n");
    reactor_postgres_client_close(app.client);
    reactor_postgres_client_release(app.client);

    printf("Cleaning up reactor...\n");
    reactor_destruct();

    printf("Done!\n");
    return 0;
}
