#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <unistd.h>
#include <err.h>
#include <fcntl.h>

#include <reactor.h>
#include <reactor_postgres.h>
#include <reactor_postgres_client.h>
#include "../../../libreactor/src/debug.h"
#define LOCAL_REACTOR_MAX_EVENTS 8192 /* Local definition for benchmark */
#include <sys/uio.h> /* For scatter-gather I/O */

#define MAX_QUERIES 5000000

typedef struct benchmark_stats {
    int queries_sent;
    int queries_completed;
    int errors;
    struct timeval start_time;
    struct timeval end_time;
    double total_latency_ms;
    double min_latency_ms;
    double max_latency_ms;
    double *latencies; // Array to store individual latencies - now a pointer
    int latency_count;           // Number of latencies recorded
} benchmark_stats;

// IX run-to-completion callback state
typedef struct rtc_state {
    struct benchmark_app *app;
    int next_query_id;
} rtc_state;

// Optimized RTC handlers for PostgreSQL client architecture
static void optimized_accept_handler(void)
{
    // In our benchmark, we maintain a persistent connection
    // This tracks that we're "accepting" the connection state
    current_transaction.accept_count++;
}

static int optimized_read_handler(int fd, void *buf, size_t len)
{
    // Reading is handled by PostgreSQL client library in event handlers
    // We don't do direct socket reads
    (void)fd; (void)buf; (void)len;
    return 0;
}

static void optimized_process_handler(int fd, void *data, size_t len)
{
    // Processing happens in benchmark_query_event
    // This is just for transaction tracking
    (void)fd; (void)data; (void)len;
}

static int optimized_write_handler(int fd, void *data, size_t len)
{
    // Writing happens via send_query calls
    // This is just for transaction tracking
    (void)fd; (void)data; (void)len;
    return 0;
}

// IX kernel-level transaction handlers
static void ix_accept_handler(void)
{
    // In real IX: accept new connections from NIC
    // In our case: we already have persistent connection, just account for it
    // current_transaction.accept_count++; // Would increment in real accept
}

static int ix_read_handler(void *data, size_t len)
{
    // In real IX: read data directly from NIC rings
    // In our case: this is handled by PostgreSQL events, so we return 0
    // to indicate no direct reads available
    (void)data;
    (void)len;
    return 0; // No direct reads in our architecture
}

static void ix_process_handler(void *result)
{
    // In real IX: process the data (TCP/IP stack + application logic)
    // In our case: this is handled in benchmark_query_event
    (void)result;
    current_transaction.process_count++;
}

static int ix_write_handler(void *data, size_t len)
{
    // In real IX: write response directly to NIC
    // In our case: this is handled by send_query calls elsewhere
    // Just account for the write operation
    (void)data;
    (void)len;

    // We don't do direct writes in this handler - queries are sent via callbacks
    return 0; // No direct write in this simulation
}

typedef struct benchmark_app benchmark_app; // Forward declaration

typedef struct query_info {
    reactor_postgres_client_query query;
    char *query_str; // Store query string
    struct timeval send_time;
    int completed;
    benchmark_app *app; // Reference to app for accessing stats
} query_info;

typedef enum { 
    QUERY_TYPE_INSERT,
    QUERY_TYPE_SELECT,
    QUERY_TYPE_UPDATE,
    QUERY_TYPE_MIXED
} query_type;

typedef struct benchmark_app {
    reactor_postgres_client *client;
    benchmark_stats stats;
    int target_queries;
    int concurrent_limit;
    query_type current_query_type; // Added to store the chosen query type
    bool running;
    query_info *queries; // Array of queries - now a pointer
} benchmark_app;

// Silent mode control
static int original_stderr = -1;
static FILE *dev_null = NULL;

static void suppress_output(void) {
    // Redirect stderr to /dev/null to suppress reactor_postgres logs
    original_stderr = dup(STDERR_FILENO);
    if (original_stderr >= 0) {
        dev_null = freopen("/dev/null", "w", stderr);
    }
}

static void restore_output(void) {
    if (original_stderr >= 0) {
        dup2(original_stderr, STDERR_FILENO);
        close(original_stderr);
        original_stderr = -1;
    }
    if (dev_null) {
        fclose(dev_null);
        dev_null = NULL;
    }
}

static double timeval_diff_ms(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 +
           (end->tv_usec - start->tv_usec) / 1000.0;
}

static void print_progress(benchmark_stats *stats, int target) {
    // Silent progress - no console output during benchmark
    (void)stats; // Suppress unused parameter warning
    (void)target;
}

static void benchmark_query_event(reactor_event *event) {
    query_info *qinfo = event->state;
    benchmark_stats *real_stats = &qinfo->app->stats;

    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_RESULT:
            {
                struct timeval recv_time;
                gettimeofday(&recv_time, NULL);
                double latency = timeval_diff_ms(&qinfo->send_time, &recv_time);
                real_stats->total_latency_ms += latency;

                if (real_stats->latency_count < MAX_QUERIES) { // Store latency if space available
                    real_stats->latencies[real_stats->latency_count++] = latency;
                }

                if (latency < real_stats->min_latency_ms || real_stats->min_latency_ms == 0) {
                    real_stats->min_latency_ms = latency;
                }
                if (latency > real_stats->max_latency_ms) {
                    real_stats->max_latency_ms = latency;
                }

                real_stats->queries_completed++;
                qinfo->completed = 1;

                // IX kernel-level: account for read/process operations
                current_transaction.read_count++;    // Read response from PostgreSQL
                current_transaction.process_count++;  // Process the result
            }
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_BAD:
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_ABORT:
            real_stats->errors++;
            real_stats->queries_completed++;
            qinfo->completed = 1;
            break;
        case REACTOR_POSTGRES_CLIENT_QUERY_EVENT_CLOSE:
            // Query finished
            break;
    }
}

static void benchmark_client_event(reactor_event *event) {
    benchmark_app *app = event->state;

    switch (event->type) {
        case REACTOR_POSTGRES_CLIENT_EVENT_READY:
            // Silent - no console output
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_ERROR:
            // Silent - no console output, but stop benchmark
            REACTOR_DEBUG("Client error received: %s\n", (char *)event->data);
            app->running = false;
            break;
        case REACTOR_POSTGRES_CLIENT_EVENT_CLOSE:
            // Silent - no console output, but stop benchmark
            app->running = false;
            break;
    }
}

static void send_insert_query(benchmark_app *app, int query_id) {
    query_info *qinfo = &app->queries[query_id - 1];
    qinfo->completed = 0;
    qinfo->app = app; // Set reference to app
    gettimeofday(&qinfo->send_time, NULL);

    // Generate INSERT query and store it
    qinfo->query_str = malloc(512);
    if (!qinfo->query_str) {
        // Silent - no console output on allocation failure
        return;
    }

    snprintf(qinfo->query_str, 512,
             "INSERT INTO benchmark_test (id, user_id, ad_id, action, timestamp) "
             "VALUES (%d, %d, %d, '%s', NOW())",
             query_id,
             rand() % 10000,
             rand() % 100,
             (rand() % 2) ? "click" : "view");

    reactor_postgres_client_query_open(&qinfo->query, benchmark_query_event, qinfo, app->client, qinfo->query_str);

    app->stats.queries_sent++;
}

static void send_select_query(benchmark_app *app, int query_id) {
    query_info *qinfo = &app->queries[query_id - 1];
    qinfo->completed = 0;
    qinfo->app = app;
    gettimeofday(&qinfo->send_time, NULL);

    qinfo->query_str = malloc(512);
    if (!qinfo->query_str) {
        REACTOR_DEBUG("Failed to allocate memory for SELECT query string.\n");
        return;
    }
    // Use IDs that actually exist (1 to target_queries) instead of random 1-5M
    int max_id = (app->target_queries > 20000) ? app->target_queries : 20000;
    snprintf(qinfo->query_str, 512, "SELECT * FROM benchmark_test WHERE id = %d", (rand() % max_id) + 1);

    reactor_postgres_client_query_open(&qinfo->query, benchmark_query_event, qinfo, app->client, qinfo->query_str);
    app->stats.queries_sent++;
}

static void send_update_query(benchmark_app *app, int query_id) {
    query_info *qinfo = &app->queries[query_id - 1];
    qinfo->completed = 0;
    qinfo->app = app;
    gettimeofday(&qinfo->send_time, NULL);

    qinfo->query_str = malloc(512);
    if (!qinfo->query_str) {
        REACTOR_DEBUG("Failed to allocate memory for UPDATE query string.\n");
        return;
    }
    snprintf(qinfo->query_str, 512,
             "UPDATE benchmark_test SET action = '%s' WHERE id = %d",
             (rand() % 2) ? "updated_click" : "updated_view",
             rand() % MAX_QUERIES + 1);

    reactor_postgres_client_query_open(&qinfo->query, benchmark_query_event, qinfo, app->client, qinfo->query_str);
    app->stats.queries_sent++;
}

static void send_query(benchmark_app *app, int query_id) {
    switch (app->current_query_type) {
        case QUERY_TYPE_INSERT:
            send_insert_query(app, query_id);
            break;
        case QUERY_TYPE_SELECT:
            send_select_query(app, query_id);
            break;
        case QUERY_TYPE_UPDATE:
            send_update_query(app, query_id);
            break;
        case QUERY_TYPE_MIXED:
            switch (rand() % 3) { // 0=INSERT, 1=SELECT, 2=UPDATE
                case 0: send_insert_query(app, query_id); break;
                case 1: send_select_query(app, query_id); break;
                case 2: send_update_query(app, query_id); break;
            }
            break;
    }

    // IX kernel-level: account for write operation (sending query to PostgreSQL)
    current_transaction.write_count++;
}

// IX run-to-completion callback function
static void rtc_work_callback(void *state)
{
    rtc_state *rtc = (rtc_state *)state;
    struct benchmark_app *app = rtc->app;

    // Send queries if we have capacity (within concurrent limit)
    while (app->stats.queries_sent - app->stats.queries_completed < app->concurrent_limit &&
           app->stats.queries_sent < app->target_queries &&
           app->running) {
        send_query(app, rtc->next_query_id++);
    }
}

static void run_benchmark_run_to_completion(benchmark_app *app) {
    // IX kernel-level run-to-completion benchmark
    REACTOR_DEBUG("Starting IX kernel-level run-to-completion benchmark with %d queries and %d concurrent connections.\n",
                  app->target_queries, app->concurrent_limit);

    gettimeofday(&app->stats.start_time, NULL);

    // Setup IX transaction handlers
    rtc_state rtc = {.app = app, .next_query_id = 1};
    reactor_enable_run_to_completion(NULL, &rtc); // Disable callback, use direct IX processing

    // Send initial batch of queries to start the pipeline
    int initial_batch = app->concurrent_limit < 10 ? app->concurrent_limit : 10;
    for (int i = 0; i < initial_batch && app->stats.queries_sent < app->target_queries; i++) {
        send_query(app, rtc.next_query_id++);
    }

    // Main IX kernel-level transaction loop
    while (app->stats.queries_completed < app->target_queries && app->running) {
        // IX-style: Process complete transactions (accept + read + process + write)
        int batch_size = reactor_get_batch_size();
        if (batch_size <= 0) batch_size = 1;

        // Process batch of IX transactions - this simulates kernel-level processing
        int processed = reactor_process_ix_batch(batch_size,
                                                ix_accept_handler,
                                                ix_read_handler,
                                                ix_process_handler,
                                                ix_write_handler);

        // If no transactions processed, fall back to regular event processing
        if (processed == 0) {
            reactor_loop_once();
        }

        // Silent progress - no console output
        print_progress(&app->stats, app->target_queries);

        // Debug: Print transaction stats occasionally
        static int debug_counter = 0;
        if (++debug_counter % 100 == 0) {
            ix_transaction tx = reactor_get_last_transaction();
            REACTOR_DEBUG("IX Transaction: accept=%d, read=%d, process=%d, write=%d, time=%lu ns\n",
                         tx.accept_count, tx.read_count, tx.process_count, tx.write_count,
                         tx.end_time - tx.start_time);
        }
    }

    reactor_disable_run_to_completion();
    gettimeofday(&app->stats.end_time, NULL);

    // Final transaction stats
    ix_transaction final_tx = reactor_get_last_transaction();
    REACTOR_DEBUG("IX kernel-level run-to-completion benchmark completed\n");
    REACTOR_DEBUG("Final transaction stats: accept=%d, read=%d, process=%d, write=%d\n",
                 final_tx.accept_count, final_tx.read_count, final_tx.process_count, final_tx.write_count);
}

// Main benchmark function - now uses IX run-to-completion by default
static void run_benchmark(benchmark_app *app) {
    // Check run-to-completion mode
    const char *rtc_mode = getenv("RTC_MODE"); // "legacy", "callback", "optimized"

    if (rtc_mode && strcmp(rtc_mode, "legacy") == 0) {
        // Legacy mode for compatibility
        REACTOR_DEBUG("Starting legacy benchmark (no run-to-completion) with %d queries and %d concurrent connections.\n",
                      app->target_queries, app->concurrent_limit);

        gettimeofday(&app->stats.start_time, NULL);

        int next_query_id = 1;
        while (app->stats.queries_completed < app->target_queries && app->running) {
            // Send queries up to concurrent limit
            while (app->stats.queries_sent - app->stats.queries_completed < app->concurrent_limit &&
                   app->stats.queries_sent < app->target_queries) {
                send_query(app, next_query_id++);
            }

            // Silent progress - no console output
            print_progress(&app->stats, app->target_queries);

            // Process events
            reactor_loop_once();
        }

        gettimeofday(&app->stats.end_time, NULL);
    } else if (rtc_mode && strcmp(rtc_mode, "optimized") == 0) {
        // IX optimized run-to-completion with direct accept+read+process+write
        REACTOR_DEBUG("Starting IX optimized run-to-completion benchmark with %d queries and %d concurrent connections.\n",
                      app->target_queries, app->concurrent_limit);

        gettimeofday(&app->stats.start_time, NULL);

        // Set up optimized RTC handlers
        reactor_set_rtc_handlers(optimized_accept_handler,
                                optimized_read_handler,
                                optimized_process_handler,
                                optimized_write_handler);

        // Send initial batch
        int next_query_id = 1;
        int initial_batch = app->concurrent_limit < 10 ? app->concurrent_limit : 10;
        for (int i = 0; i < initial_batch && app->stats.queries_sent < app->target_queries; i++) {
            send_query(app, next_query_id++);
        }

        // Main optimized event loop
        while (app->stats.queries_completed < app->target_queries && app->running) {
            // Use optimized event loop: accept + read + process + write in one pass
            reactor_loop_once_optimized();

            // Silent progress
            print_progress(&app->stats, app->target_queries);
        }

        gettimeofday(&app->stats.end_time, NULL);
        REACTOR_DEBUG("IX optimized run-to-completion benchmark completed\n");
    } else {
        // Default: IX callback-based run-to-completion
        run_benchmark_run_to_completion(app);
    }
}

// Comparison function for qsort
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *) a;
    double db = *(const double *) b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

// Function to calculate percentile
static double calculate_percentile(double *latencies, int count, double percentile) {
    if (count == 0) return 0.0;
    if (percentile <= 0.0) return latencies[0];
    if (percentile >= 100.0) return latencies[count - 1];

    double rank = (percentile / 100.0) * (count - 1);
    int lower_index = (int) rank;
    double fraction = rank - lower_index;

    if (lower_index + 1 < count) {
        return latencies[lower_index] + (latencies[lower_index + 1] - latencies[lower_index]) * fraction;
    } else {
        return latencies[lower_index];
    }
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
    printf("Average latency:   %.2f ms (%.0f µs, %.0f ns)\n", avg_latency, avg_latency * 1000.0, avg_latency * 1000000.0);
    printf("Min latency:       %.2f ms (%.0f µs, %.0f ns)\n", stats->min_latency_ms, stats->min_latency_ms * 1000.0, stats->min_latency_ms * 1000000.0);
    printf("Max latency:       %.2f ms (%.0f µs, %.0f ns)\n", stats->max_latency_ms, stats->max_latency_ms * 1000.0, stats->max_latency_ms * 1000000.0);

    // Calculate and print percentiles
    if (stats->latency_count > 0) {
        qsort(stats->latencies, stats->latency_count, sizeof(double), compare_doubles);

        printf("\nLatency Percentiles (ms, µs, ns):\n");
        printf("  10th:            %.2f ms (%.0f µs, %.0f ns)\n", calculate_percentile(stats->latencies, stats->latency_count, 10.0), calculate_percentile(stats->latencies, stats->latency_count, 10.0) * 1000.0, calculate_percentile(stats->latencies, stats->latency_count, 10.0) * 1000000.0);
        printf("  30th:            %.2f ms (%.0f µs, %.0f ns)\n", calculate_percentile(stats->latencies, stats->latency_count, 30.0), calculate_percentile(stats->latencies, stats->latency_count, 30.0) * 1000.0, calculate_percentile(stats->latencies, stats->latency_count, 30.0) * 1000000.0);
        printf("  50th (Median):   %.2f ms (%.0f µs, %.0f ns)\n", calculate_percentile(stats->latencies, stats->latency_count, 50.0), calculate_percentile(stats->latencies, stats->latency_count, 50.0) * 1000.0, calculate_percentile(stats->latencies, stats->latency_count, 50.0) * 1000000.0);
        printf("  90th:            %.2f ms (%.0f µs, %.0f ns)\n", calculate_percentile(stats->latencies, stats->latency_count, 90.0), calculate_percentile(stats->latencies, stats->latency_count, 90.0) * 1000.0, calculate_percentile(stats->latencies, stats->latency_count, 90.0) * 1000000.0);
        printf("  99th:            %.2f ms (%.0f µs, %.0f ns)\n", calculate_percentile(stats->latencies, stats->latency_count, 99.0), calculate_percentile(stats->latencies, stats->latency_count, 99.0) * 1000.0, calculate_percentile(stats->latencies, stats->latency_count, 99.0) * 1000000.0);
    }
}

static void setup_database(reactor_postgres_client *client) {
    // Silent database setup - no console output

    const char *setup_queries[] = {
        "DROP TABLE IF EXISTS benchmark_test",
        "CREATE TABLE benchmark_test ("
        "  id SERIAL PRIMARY KEY,"
        "  user_id INTEGER,"
        "  ad_id INTEGER,"
        "  action TEXT,"
        "  timestamp TIMESTAMP DEFAULT NOW()"
        ")",
        "CREATE INDEX CONCURRENTLY IF NOT EXISTS idx_benchmark_test_user_id ON benchmark_test(user_id)",
        "CREATE INDEX CONCURRENTLY IF NOT EXISTS idx_benchmark_test_ad_id ON benchmark_test(ad_id)",
        NULL
    };

    for (int i = 0; setup_queries[i]; i++) {
        // Silent execution - no console output

        // Simple synchronous execution
        reactor_postgres_client_query q;
        reactor_postgres_client_query_open(&q, NULL, NULL, client, setup_queries[i]);

        // Wait for completion
        int attempts = 0;
        while (attempts++ < 100) {
            reactor_loop_once();
        }
    }

    // Silent completion - no console output
}

int main(int argc, char **argv) {
    if (argc != 4) {
        REACTOR_DEBUG("Usage: %s <num_queries> <concurrent> <query_type>\n", argv[0]);
        REACTOR_DEBUG("Query types: insert, select, update, mixed\n", argv[0]);
        REACTOR_DEBUG("Example: %s 1000 10 insert\n", argv[0]);
        exit(1);
    }

    int num_queries = atoi(argv[1]);
    int concurrent = atoi(argv[2]);
    char *query_type_str = argv[3];

    query_type selected_query_type;
    if (strcmp(query_type_str, "insert") == 0) {
        selected_query_type = QUERY_TYPE_INSERT;
    } else if (strcmp(query_type_str, "select") == 0) {
        selected_query_type = QUERY_TYPE_SELECT;
    } else if (strcmp(query_type_str, "update") == 0) {
        selected_query_type = QUERY_TYPE_UPDATE;
    } else if (strcmp(query_type_str, "mixed") == 0) {
        selected_query_type = QUERY_TYPE_MIXED;
    } else {
        REACTOR_DEBUG("Invalid query type: %s. Use insert, select, update, or mixed.\n", query_type_str);
        exit(1);
    }

    if (num_queries <= 0 || concurrent <= 0 || num_queries > MAX_QUERIES) {
        REACTOR_DEBUG("Queries must be 1-%d, concurrent > 0\n", MAX_QUERIES);
        exit(1);
    }

    benchmark_app app = {
        .target_queries = num_queries,
        .concurrent_limit = concurrent,
        .current_query_type = selected_query_type,
        .running = true,
        .stats = { .total_latency_ms = 0, .min_latency_ms = 0, .max_latency_ms = 0, .latency_count = 0 } // Initialize scalar stats
    };

    // Dynamically allocate memory for queries and latencies
    app.queries = malloc(sizeof(query_info) * num_queries);
    if (!app.queries) {
        REACTOR_DEBUG("Failed to allocate memory for queries array.\n");
        exit(1);
    }

    app.stats.latencies = malloc(sizeof(double) * num_queries);
    if (!app.stats.latencies) {
        REACTOR_DEBUG("Failed to allocate memory for latencies array.\n");
        free(app.queries); // Free previously allocated memory
        exit(1);
    }

    srand(time(NULL));

    reactor_construct();

    // Suppress all output from the beginning
    suppress_output();

    // Connect to PostgreSQL
    app.client = reactor_postgres_client_open(benchmark_client_event, &app,
        (const char *[]){"host", "user", "password", "dbname", "port", NULL},
        (const char *[]){"127.0.0.1", "pguser", "qwertt111", "postgres", "5432", NULL});

    if (!app.client) {
        restore_output();
        REACTOR_DEBUG("Failed to create PostgreSQL client\n");
        exit(1);
    }

    // Wait for connection - silent
    int connect_attempts = 0;
    while (connect_attempts++ < 50) {
        reactor_loop_once();

        reactor_postgres_client_stats stats;
        reactor_postgres_client_get_stats(app.client, &stats);
        if (stats.connections > 0) break;
    }

    setup_database(app.client);

    run_benchmark(&app);

    // Restore output for results only
    restore_output();

    print_results(&app.stats);

    /* IX-inspired optimizations demo - before closing connections */
    const char *rtc_mode = getenv("RTC_MODE");
    const char *run_style;
    if (!rtc_mode) {
        run_style = "IX Callback Run-to-Completion (default)";
    } else if (strcmp(rtc_mode, "legacy") == 0) {
        run_style = "Legacy (no run-to-completion)";
    } else if (strcmp(rtc_mode, "optimized") == 0) {
        run_style = "IX Optimized Run-to-Completion";
    } else {
        run_style = "IX Callback Run-to-Completion";
    }

    printf("\n🎯 IX Optimizations Status:\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("Run style:               %s\n", run_style);
    printf("Current batch size:      %d\n", reactor_get_batch_size());
    printf("Active descriptors:      %zu\n", reactor_get_descriptor_count());
    printf("Event count:             %zu\n", reactor_get_event_count());

    /* Show IX transaction statistics */
    ix_transaction tx = reactor_get_last_transaction();
    printf("Last IX Transaction:\n");
    printf("  Accept ops:            %d\n", tx.accept_count);
    printf("  Read ops:              %d\n", tx.read_count);
    printf("  Process ops:           %d\n", tx.process_count);
    printf("  Write ops:             %d\n", tx.write_count);
    if (tx.end_time > tx.start_time) {
        printf("  Transaction time:      %lu ns\n", tx.end_time - tx.start_time);
    }
    // printf("Congestion detected:     %s\n", reactor_is_congested() ? "YES" : "NO"); // TODO: Fix linking

    reactor_postgres_client_close(app.client);
    reactor_postgres_client_release(app.client);
    reactor_destruct();

    // Free dynamically allocated memory
    free(app.queries);
    free(app.stats.latencies);

    return 0;
}
