/**
 * @file libreactor.c
 * @brief Main program for basic libreactor server using server infrastructure
 */

#include <stdio.h>
#include <stdlib.h>

#include "../../include/infrastructure/server_infrastructure.h"
#include "../../include/platform/log.h"

int main(void)
{
    server_infra_error_t err;

    log_info("Starting simple test server on port 2342");

    /* Initialize server infrastructure */
    err = server_infrastructure_init();
    if (err != SERVER_INFRA_OK) {
        log_error("Failed to initialize server infrastructure: %d", err);
        return EXIT_FAILURE;
    }

    /* Get default configuration and customize for simple server */
    server_config_t config = server_infrastructure_default_config();
    config.port = 2342;
    config.plaintext_response = "Hello, World!";
    config.json_message = "Hello, World!";

    /* Create server infrastructure */
    server_infrastructure_t infra;
    err = server_infrastructure_create(&infra, &config);
    if (err != SERVER_INFRA_OK) {
        log_error("Failed to create server infrastructure: %d", err);
        server_infrastructure_cleanup();
        return EXIT_FAILURE;
    }

    log_info("Server ready, starting event loop");

    /* Start server */
    err = server_infrastructure_start(&infra);
    if (err != SERVER_INFRA_OK) {
        log_error("Failed to start server: %d", err);
    } else {
        log_info("Server shutdown complete");
    }

    /* Cleanup */
    server_infrastructure_destroy(&infra);
    server_infrastructure_cleanup();

    return (err == SERVER_INFRA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
