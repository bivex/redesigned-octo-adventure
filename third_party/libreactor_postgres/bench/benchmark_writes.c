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

typedef struct benchmark_stats {
    int queries_sent;
    int queries_completed;
    int errors;
    struct timeval start_time;
    struct timeval end_time;
    double total_latency_ms;
    double min_latency_ms;
    double max_latency_ms;
    int concurrent_queries;
} benchmark_stats;

typedef struct query_context {
    struct timeval send_time;
    int query_id;
    benchmark_stats *stats;
    int target_queries; // Add target_queries here for progress reporting
    reactor_postgres_client_query *query; // Reference to query structure for cleanup
} query_context;

typedef struct benchmark_app {
    reactor_postgres_client *client;
    benchmark_stats stats;
    int target_queries;
    int concurrent_limit;
    bool running;
} benchmark_app;

static double timeval_diff_ms(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_usec - start->tv_usec) / 1000.0;
}

static void print_progress(query_context *ctx, int interval_ms) {
    static int last_completed = 0;
    static struct timeval last_time = {0};
    benchmark_stats *stats = ctx->stats;

    struct timeval now;
    gettimeofday(&now, NULL);

    if (last_time.tv_sec == 0) {
        last_time = now;
        return;
    }

    double elapsed = timeval_diff_ms(&last_time, &now);
    if (elapsed < interval_ms) return;

    int completed = stats->queries_completed - last_completed;
    double qps = completed / (elapsed / 1000.0);

    printf("Progress: %d/%d queries (%.1f qps) | Errors: %d\n",
           stats->queries_completed, ctx->target_queries, qps, stats->errors);

    last_completed = stats->queries_completed;
    last_time = now;
}

static void benchmark_query_event(reactor_event *event) {
    query_context *ctx = event->state;

    // Skip if no context (setup queries)
    if (!ctx) return;

    benchmark_stats *stats = ctx->stats;

    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT:
            {
    struct timeval recv_time;
    gettimeofday(&recv_time, NULL);
    double latency = timeval_diff_ms(&ctx->send_time, &recv_time);
    stats->total_latency_ms += latency;

    if (latency < stats->min_latency_ms || stats->min_latency_ms == 0) {
        stats->min_latency_ms = latency;
    }
    if (latency > stats->max_latency_ms) {
        stats->max_latency_ms = latency;
    }

            stats->queries_completed++;
            }
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD:
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT:
            stats->errors++;
            stats->queries_completed++;
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE:
            // Query finished - cleanup memory
            free(ctx->query->command); // Free the command string
            free(ctx->query);          // Free the query structure
            free(ctx);                 // Free the context
            return; // Don't continue after cleanup
    }

    // Don't free context here - wait for CLOSE event
}

static void benchmark_client_event(reactor_event *event) {
    benchmark_app *app = event->state;

    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_EVENT_READY:
            printf("✓ Connected to PostgreSQL\n");
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_ERROR:
            fprintf(stderr, "✗ Connection error\n");
            app->running = false;
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_CLOSE:
            printf("Connection closed\n");
            app->running = false;
            break;
    }
}

static void send_insert_query(benchmark_app *app, int query_id) {
    // Allocate both query structure and context in heap
    reactor_postgres_client_query *q = malloc(sizeof(reactor_postgres_client_query));
    query_context *ctx = malloc(sizeof(query_context));

    if (!q || !ctx) {
        fprintf(stderr, "Failed to allocate memory for query\n");
        free(q);
        free(ctx);
        return;
    }

    ctx->query_id = query_id;
    ctx->stats = &app->stats;
    ctx->target_queries = app->target_queries;
    ctx->query = q; // Store reference to query for cleanup
    gettimeofday(&ctx->send_time, NULL);

    // Generate INSERT query for ads transitions simulation
    char *query_str = malloc(512);
    if (!query_str) {
        fprintf(stderr, "Failed to allocate memory for query string\n");
        free(q);
        free(ctx);
        return;
    }

    snprintf(query_str, 512,
             "INSERT INTO benchmark_test (id, user_id, ad_id, action, timestamp) "
             "VALUES (%d, %d, %d, '%s', NOW())",
             query_id,
             rand() % 10000,  // random user_id
             rand() % 100,    // random ad_id
             (rand() % 2) ? "click" : "view"); // random action

    reactor_postgres_client_query_open(q, benchmark_query_event, ctx, app->client, query_str);

    app->stats.queries_sent++;
}

static void run_benchmark(benchmark_app *app) {
    printf("Starting write benchmark...\n");
    printf("Target queries: %d\n", app->target_queries);
    printf("Concurrent limit: %d\n", app->concurrent_limit);
    printf("─\n");

    gettimeofday(&app->stats.start_time, NULL);

    int next_query_id = 1;
    query_context dummy_ctx = {.stats = &app->stats, .target_queries = app->target_queries};

    while (app->stats.queries_completed < app->target_queries && app->running) {
        // Send queries up to concurrent limit
        while (app->stats.queries_sent - app->stats.queries_completed < app->concurrent_limit &&
               app->stats.queries_sent < app->target_queries) {
            send_insert_query(app, next_query_id++);
        }

        // Progress reporting every 50ms (faster updates)
        print_progress(&dummy_ctx, 50);

        // Process events
        reactor_loop_once();
    }

    gettimeofday(&app->stats.end_time, NULL);
}

static void print_results(benchmark_stats *stats) {
    double total_time = timeval_diff_ms(&stats->start_time, &stats->end_time);
    double qps = stats->queries_completed / (total_time / 1000.0);
    double avg_latency = stats->total_latency_ms / stats->queries_completed;

    printf("\n📊 Benchmark Results:\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("Total queries:     %d\n", stats->queries_completed);
    printf("Errors:            %d (%.2f%%)\n", stats->errors,
           stats->queries_completed > 0 ? (stats->errors * 100.0 / stats->queries_completed) : 0);
    printf("Total time:        %.2f seconds\n", total_time / 1000.0);
    printf("Queries/second:    %.1f qps\n", qps);
    printf("Average latency:   %.2f ms\n", avg_latency);
    printf("Min latency:       %.2f ms\n", stats->min_latency_ms);
    printf("Max latency:       %.2f ms\n", stats->max_latency_ms);
    printf("95th percentile:   %.2f ms (estimated)\n", avg_latency * 1.5); // rough estimate
}

static void setup_database(reactor_postgres_client *client) {
    printf("Setting up benchmark database...\n");

    char *setup_queries[] = {
        "DROP TABLE IF EXISTS benchmark_test",
        "CREATE TABLE benchmark_test ("
        "  id SERIAL PRIMARY KEY,"
        "  user_id INTEGER,"
        "  ad_id INTEGER,"
        "  action TEXT,"
        "  timestamp TIMESTAMP DEFAULT NOW()"
        ")",
        NULL
    };

    for (int i = 0; setup_queries[i]; i++) {
        printf("Executing: %s\n", setup_queries[i]);

        // Simple synchronous execution for setup
        reactor_postgres_client_query q;
        // Note: This is simplified - in real code you'd want proper event handling
        reactor_postgres_client_query_open(&q, NULL, NULL, client, setup_queries[i]);

        // Wait for completion (simplified) - faster without delays
        int attempts = 0;
        while (attempts++ < 100) { // 10 seconds timeout
            reactor_loop_once();
            // Removed usleep(100000) for faster testing
        }
    }

    printf("✓ Database setup complete\n");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <num_queries> <concurrent>\n", argv[0]);
        fprintf(stderr, "Example: %s 10000 50\n", argv[0]);
        exit(1);
    }

    int num_queries = atoi(argv[1]);
    int concurrent = atoi(argv[2]);

    if (num_queries <= 0 || concurrent <= 0) {
        fprintf(stderr, "Both arguments must be positive integers\n");
        exit(1);
    }

    benchmark_app app = {
        .target_queries = num_queries,
        .concurrent_limit = concurrent,
        .running = true,
        .stats = {0}
    };

    srand(time(NULL));

    reactor_construct();

    // Connect to PostgreSQL
    app.client = reactor_postgres_client_open(benchmark_client_event, &app,
        (const char *[]){"host", "user", "password", "dbname", "port", NULL},
        (const char *[]){"127.0.0.1", "pguser", "qwertt111", "postgres", "5432", NULL});

    if (!app.client) {
        err(1, "Failed to create PostgreSQL client");
    }

    // Wait for connection
    printf("Connecting to PostgreSQL...\n");
    int connect_attempts = 0;
    while (connect_attempts++ < 50) { // 5 seconds timeout
        reactor_loop_once();
        // Removed usleep(100000) for faster testing

        reactor_postgres_client_stats stats;
        reactor_postgres_client_get_stats(app.client, &stats);
        if (stats.connections > 0) break;
    }

    setup_database(app.client);

    run_benchmark(&app);

    print_results(&app.stats);

    reactor_postgres_client_close(app.client);
    reactor_postgres_client_release(app.client);
    reactor_destruct();

    return 0;
}
