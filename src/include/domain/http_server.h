/**
 * @file http_server.h
 * @brief Domain layer for HTTP server business logic
 *
 * This module contains the core HTTP server logic for routing requests
 * and coordinating response generation.
 */

#ifndef DOMAIN_HTTP_SERVER_H
#define DOMAIN_HTTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include <reactor.h>

#include "../../include/domain/http_response.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Forward declarations for reactor types */
struct server;
struct server_request;

/** HTTP server error codes */
typedef enum {
    HTTP_SERVER_OK = 0,
    HTTP_SERVER_ERROR_INVALID_PARAM = -1,
    HTTP_SERVER_ERROR_MEMORY = -2,
    HTTP_SERVER_ERROR_RESPONSE_BUILD = -3
} http_server_error_t;

/** HTTP request route */
typedef enum {
    ROUTE_PLAINTEXT,
    ROUTE_JSON,
    ROUTE_UNKNOWN
} http_route_t;

/** HTTP server configuration */
typedef struct {
    const char *plaintext_response;     /** Static plaintext response */
    const char *json_message;           /** JSON message field value */
    bool enable_date_headers;           /** Whether to include Date headers */
} http_server_config_t;

/** HTTP server instance */
typedef struct {
    http_server_config_t config;
    char json_buffer[4096];             /** Buffer for JSON responses */
    size_t json_buffer_size;
} http_server_t;

/**
 * @brief Initialize HTTP server module
 * @return HTTP_SERVER_OK on success, error code otherwise
 */
http_server_error_t http_server_init(void);

/**
 * @brief Cleanup HTTP server module
 */
void http_server_cleanup(void);

/**
 * @brief Initialize HTTP server instance
 * @param[out] server Server instance to initialize
 * @param[in] config Server configuration
 * @return HTTP_SERVER_OK on success, error code otherwise
 */
http_server_error_t http_server_create(http_server_t *server,
                                         const http_server_config_t *config);

/**
 * @brief Cleanup HTTP server instance
 * @param server Server instance to cleanup
 */
void http_server_destroy(http_server_t *server);

/**
 * @brief Handle HTTP request and generate response
 * @param server HTTP server instance
 * @param request Server request from reactor
 * @return HTTP_SERVER_OK on success, error code otherwise
 * @note This function writes the response directly to the stream
 */
http_server_error_t http_server_handle_request(http_server_t *server,
                                                 struct server_request *request);

/**
 * @brief Parse route from request target
 * @param target Request target data
 * @return Parsed route
 */
http_route_t http_server_parse_route(data target);

/**
 * @brief Generate response for a route
 * @param server HTTP server instance
 * @param route Route to generate response for
 * @param[out] response_config Response configuration to fill
 * @return HTTP_SERVER_OK on success, error code otherwise
 */
http_server_error_t http_server_generate_response(const http_server_t *server,
                                                    http_route_t route,
                                                    http_response_config_t *response_config);

#ifdef __cplusplus
}
#endif

#endif /* DOMAIN_HTTP_SERVER_H */
