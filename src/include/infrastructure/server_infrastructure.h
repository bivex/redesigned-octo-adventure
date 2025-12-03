/**
 * @file server_infrastructure.h
 * @brief Infrastructure layer for server coordination
 *
 * This module coordinates between domain logic, platform abstractions,
 * and reactor framework to provide complete server functionality.
 */

#ifndef INFRASTRUCTURE_SERVER_INFRASTRUCTURE_H
#define INFRASTRUCTURE_SERVER_INFRASTRUCTURE_H

#include <stdbool.h>
#include <stdint.h>

// #include <dynamic.h>
#include <reactor.h>

#include "../../include/domain/http_server.h"
#include "../../include/platform/process.h"
#include "../../include/platform/socket.h"
#include "../../include/platform/log.h"
#include "../../include/platform/signals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Server infrastructure error codes */
typedef enum {
    SERVER_INFRA_OK = 0,
    SERVER_INFRA_ERROR_INIT = -1,
    SERVER_INFRA_ERROR_CONFIG = -2,
    SERVER_INFRA_ERROR_RESOURCE = -3,
    SERVER_INFRA_ERROR_STARTUP = -4
} server_infra_error_t;

/** Server configuration */
typedef struct {
    uint16_t port;                          /** Server port */
    const char *plaintext_response;         /** Plaintext response content */
    const char *json_message;               /** JSON message content */
    bool enable_date_headers;               /** Whether to include Date headers */
    bool enable_socket_optimizations;       /** Enable socket optimizations */
    socket_config_t socket_config;          /** Socket optimization config */
    worker_config_t worker_config;          /** Worker process config */
    log_config_t log_config;                /** Logging configuration */
    signal_config_t signal_config;          /** Signal handling configuration */
} server_config_t;

/** Forward declaration */
struct server_infrastructure;

/** Server state for reactor callbacks */
typedef struct {
    server *srv;
    struct server_infrastructure *infra;
} server_state_t;

/** Server infrastructure instance */
typedef struct server_infrastructure {
    server_config_t config;
    http_server_t http_server;
    worker_manager_t worker_manager;
    signal_manager_t signal_manager;
    bool initialized;
} server_infrastructure_t;

/**
 * @brief Initialize server infrastructure module
 * @return SERVER_INFRA_OK on success, error code otherwise
 */
server_infra_error_t server_infrastructure_init(void);

/**
 * @brief Cleanup server infrastructure module
 */
void server_infrastructure_cleanup(void);

/**
 * @brief Create server infrastructure instance
 * @param[out] infra Infrastructure instance to initialize
 * @param[in] config Server configuration
 * @return SERVER_INFRA_OK on success, error code otherwise
 */
server_infra_error_t server_infrastructure_create(server_infrastructure_t *infra,
                                                  const server_config_t *config);

/**
 * @brief Destroy server infrastructure instance
 * @param infra Infrastructure instance to destroy
 */
void server_infrastructure_destroy(server_infrastructure_t *infra);

/**
 * @brief Start server (fork workers and begin listening)
 * @param infra Infrastructure instance
 * @return SERVER_INFRA_OK on success, error code otherwise
 * @note For worker processes, returns after forking
 * @note For parent process, waits for workers to exit
 */
server_infra_error_t server_infrastructure_start(server_infrastructure_t *infra);

/**
 * @brief Get default server configuration
 * @return Default configuration
 */
server_config_t server_infrastructure_default_config(void);

/**
 * @brief Reactor callback for handling HTTP requests
 * @param event Reactor event
 * @return Reactor status
 * @note This is the main request handler that coordinates domain logic
 */
void server_infrastructure_request_handler(reactor_event *event);

#ifdef __cplusplus
}
#endif

#endif /* INFRASTRUCTURE_SERVER_INFRASTRUCTURE_H */
