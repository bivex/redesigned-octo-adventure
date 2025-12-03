/**
 * @file libreactor-server.c
 * @brief Main program for enhanced libreactor-server with optimizations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../../include/infrastructure/server_infrastructure.h"
#include "../../include/platform/log.h"

int main(int argc, char *argv[])
{
    server_infra_error_t err;
    bool disable_logging = false;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disable-log") == 0) {
            disable_logging = true;
            is_logging_disabled = true;  /* Set global flag immediately */
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --disable-log    Disable logging output\n");
            printf("  --help, -h       Show this help message\n");
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use --help for usage information\n");
            return EXIT_FAILURE;
        }
    }

    /* Initialize server infrastructure */
    err = server_infrastructure_init();
    if (err != SERVER_INFRA_OK) {
        log_error("Failed to initialize server infrastructure: %d", err);
        return EXIT_FAILURE;
    }

    /* Get default configuration */
    server_config_t config = server_infrastructure_default_config();

    /* Configure for enhanced server */
    config.port = 3984; /* Different port for enhanced server */
    config.enable_socket_optimizations = true;
    config.socket_config.options = SOCKET_OPT_BUSY_POLL |
                                   SOCKET_OPT_NODELAY |
                                   SOCKET_OPT_KEEPALIVE |
                                   SOCKET_OPT_REUSEPORT_CBPF;
    config.socket_config.busy_poll_value = 50;
    config.socket_config.keepalive_enabled = false;

    /* Configure enhanced logging */
    if (disable_logging) {
        config.log_config.level = 99; /* Disable all logging */
        config.log_config.pid = false;
        config.log_config.timestamps = false;
        config.log_config.colors = false;
    } else {
    config.log_config.level = LOG_LEVEL_INFO;
    config.log_config.pid = true;
    config.log_config.timestamps = true;
    config.log_config.colors = true;
    }

    /* Create server infrastructure */
    server_infrastructure_t infra;
    err = server_infrastructure_create(&infra, &config);
    if (err != SERVER_INFRA_OK) {
        fprintf(stderr, "Failed to create server infrastructure: %d\n", err);
        server_infrastructure_cleanup();
        return EXIT_FAILURE;
    }

    log_info("Starting enhanced libreactor-server with socket optimizations");
    log_info("Server will listen on port %d with %d worker processes",
             config.port, config.worker_config.worker_count);

    /* Start server (this will fork workers and start listening) */
    err = server_infrastructure_start(&infra);
    if (err != SERVER_INFRA_OK) {
        log_error("Failed to start server: %d", err);
    } else {
        log_info("Enhanced server shutdown complete");
    }

    /* Cleanup */
    server_infrastructure_destroy(&infra);
    server_infrastructure_cleanup();

    return (err == SERVER_INFRA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
