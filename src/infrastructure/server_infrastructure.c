/**
 * @file server_infrastructure.c
 * @brief Implementation of server infrastructure coordination
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #include <dynamic.h>
#include "../../third_party/libreactor/src/reactor.h"

#include "../../include/infrastructure/server_infrastructure.h"
#include "../../include/platform/system.h"
#include "../../include/platform/socket.h"

/** Global infrastructure instance for reactor callback */
static server_infrastructure_t *global_infra = NULL;

server_infra_error_t server_infrastructure_init(void)
{
    /* Initialize all modules */
    system_error_t sys_err = system_init();
    if (sys_err != SYSTEM_OK) {
        return SERVER_INFRA_ERROR_INIT;
    }

    socket_error_t sock_err = socket_init();
    if (sock_err != SOCKET_OK) {
        return SERVER_INFRA_ERROR_INIT;
    }

    http_server_error_t http_err = http_server_init();
    if (http_err != HTTP_SERVER_OK) {
        return SERVER_INFRA_ERROR_INIT;
    }

    /* Initialize logging */
    /* Logging is initialized by the main application */

    return SERVER_INFRA_OK;
}

void server_infrastructure_cleanup(void)
{
    log_cleanup();
    http_server_cleanup();
    /* Platform modules don't need explicit cleanup */
}

server_infra_error_t server_infrastructure_create(server_infrastructure_t *infra,
                                                  const server_config_t *config)
{
    if (!infra || !config) {
        return SERVER_INFRA_ERROR_CONFIG;
    }

    memset(infra, 0, sizeof(*infra));

    /* Copy configuration */
    infra->config = *config;

    /* Initialize HTTP server */
    http_server_config_t http_config = {
        .plaintext_response = config->plaintext_response,
        .json_message = config->json_message,
        .enable_date_headers = config->enable_date_headers
    };

    http_server_error_t http_err = http_server_create(&infra->http_server, &http_config);
    if (http_err != HTTP_SERVER_OK) {
        return SERVER_INFRA_ERROR_INIT;
    }

    /* Initialize worker manager */
    worker_manager_init(&infra->worker_manager, &config->worker_config);

    /* Free the original cpu_ids since worker_manager made a copy */
    if (config->worker_config.cpu_ids) {
        system_free((void *)config->worker_config.cpu_ids);
    }

    /* Initialize signal manager */
    signal_error_t sig_err = signal_manager_init(&infra->signal_manager, &config->signal_config);
    if (sig_err != SIGNAL_OK) {
        worker_manager_cleanup(&infra->worker_manager);
        http_server_destroy(&infra->http_server);
        return SERVER_INFRA_ERROR_INIT;
    }

    infra->initialized = true;
    log_info("Server infrastructure initialized");
    return SERVER_INFRA_OK;
}

void server_infrastructure_destroy(server_infrastructure_t *infra)
{
    if (!infra) {
        return;
    }

    if (infra->initialized) {
        signal_manager_cleanup(&infra->signal_manager);
        worker_manager_cleanup(&infra->worker_manager);
        http_server_destroy(&infra->http_server);
        infra->initialized = false;
    }

    memset(infra, 0, sizeof(*infra));
}

server_infra_error_t server_infrastructure_start(server_infrastructure_t *infra)
{
    if (!infra || !infra->initialized) {
        return SERVER_INFRA_ERROR_CONFIG;
    }

    /* Set global reference for reactor callback */
    global_infra = infra;

    /* Fork worker processes */
    process_error_t proc_err = worker_manager_fork_workers(&infra->worker_manager);
    if (proc_err != PROCESS_OK) {
        return SERVER_INFRA_ERROR_STARTUP;
    }

    /* Check if we're a worker or parent */
    process_type_t proc_type = worker_manager_get_type(&infra->worker_manager);

    if (proc_type == PROCESS_TYPE_WORKER) {
        /* Worker process: initialize reactor and start server */
        log_info("Worker process starting on CPU %d, global_infra: %p", worker_manager_get_cpu_id(&infra->worker_manager), (void*)global_infra);

        reactor_construct();

        server s;
        server_state_t state = { .srv = &s, .infra = global_infra };
        server_construct(&s, server_infrastructure_request_handler, &state);

        /* Create and bind socket manually */
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", infra->config.port);
        struct addrinfo *ai = net_resolve(NULL, port_str, AF_INET, SOCK_STREAM, AI_PASSIVE);
        if (!ai) {
            log_error("Failed to resolve address");
            return SERVER_INFRA_ERROR_STARTUP;
        }
        int fd = net_socket(ai);
        freeaddrinfo(ai);
        if (fd == -1) {
            log_error("Failed to create socket");
            return SERVER_INFRA_ERROR_STARTUP;
        }

        server_open(&s, fd, NULL);
        log_info("Server listening on port %d", infra->config.port);

        /* Apply socket optimizations if enabled */
        if (infra->config.enable_socket_optimizations) {
            socket_error_t sock_err = socket_apply_optimizations(s.descriptor.fd, &infra->config.socket_config);
            if (sock_err != SOCKET_OK) {
                log_warn("Failed to apply socket optimizations");
            }
        }

        /* Signal parent that we're ready */
        worker_manager_signal_ready(&infra->worker_manager);

        /* Start event loop - signals will be handled asynchronously */
        log_info("Worker ready, starting event loop");
        reactor_loop();

        /* Check why we exited */
        if (signal_manager_shutdown_requested(&infra->signal_manager)) {
            log_info("Shutdown requested, stopping server");
        } else {
            log_info("Event loop exited for unknown reason");
        }

        server_destruct(&s);
        reactor_destruct();
        log_info("Worker process shutting down");

    } else {
        /* Parent process: wait for workers or shutdown signal */
        log_info("Parent process started, managing %d workers", infra->config.worker_config.worker_count);

        /* Wait for workers to exit or shutdown signal */
        while (!signal_manager_shutdown_requested(&infra->signal_manager)) {
            /* Check if any worker has exited */
            if (worker_manager_wait_workers(&infra->worker_manager) == PROCESS_OK) {
                log_info("Worker process exited, shutting down");
                break;
            }

            /* Small delay to avoid busy waiting */
            usleep(100000); /* 100ms */
        }

        log_info("Parent process shutting down");
        /* Workers have exited or shutdown requested */
    }

    return SERVER_INFRA_OK;
}

server_config_t server_infrastructure_default_config(void)
{
    /* Get CPU information for worker count */
    system_cpu_info_t cpu_info;
    system_error_t sys_err = system_get_cpu_info(&cpu_info);

    int worker_count = 1; /* Default fallback */
    int *cpu_ids = NULL;

    if (sys_err == SYSTEM_OK && cpu_info.count > 0) {
        worker_count = cpu_info.count;
        cpu_ids = cpu_info.cpu_ids;
    }

    server_config_t config = {
        .port = 2342,
        .plaintext_response = "Hello, World!",
        .json_message = "Hello, World!",
        .enable_date_headers = true,
        .enable_socket_optimizations = false,
        .socket_config = {
            .options = 0, /* No optimizations by default */
            .busy_poll_value = 50,
            .keepalive_enabled = false
        },
        .worker_config = {
            .worker_count = worker_count,
            .cpu_ids = cpu_ids,
            .enable_affinity = true
        },
        .signal_config = signal_manager_default_config()
    };

    return config;
}

void server_infrastructure_request_handler(reactor_event *event)
{
    server_state_t *state = event->state;
    if (!state || !state->infra) {
        log_error("Server state or infrastructure pointer is NULL");
        return;
    }

    server_infrastructure_t *infra = state->infra;
    server_request *request = (server_request *)event->data;

    if (event->type == SERVER_REQUEST) {
        //log_info("Processing HTTP request for: %.*s", (int)request->target.size, (char*)request->target.base);
        http_server_error_t http_err = http_server_handle_request(&infra->http_server, request);
        if (http_err != HTTP_SERVER_OK) {
            /* Log error and return error response */
            log_error("HTTP server error: %d", http_err);
            /* Abort not available in void return, maybe close connection? */
            /* server_close(request); */
        }
    } else {
        /* Error or connection close */
        /* server_destruct(state->srv); - Don't destruct server on single connection error */
    }
}
