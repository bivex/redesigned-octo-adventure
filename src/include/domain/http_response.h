/**
 * @file http_response.h
 * @brief Domain layer for HTTP response generation
 *
 * This module handles HTTP response creation and formatting,
 * providing a clean interface for building HTTP responses.
 */

#ifndef DOMAIN_HTTP_RESPONSE_H
#define DOMAIN_HTTP_RESPONSE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** HTTP response error codes */
typedef enum {
    HTTP_RESPONSE_OK = 0,
    HTTP_RESPONSE_ERROR_INVALID_PARAM = -1,
    HTTP_RESPONSE_ERROR_MEMORY = -2,
    HTTP_RESPONSE_ERROR_BUFFER_OVERFLOW = -3
} http_response_error_t;

/** HTTP status codes */
typedef enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_NOT_FOUND = 404,
    HTTP_STATUS_INTERNAL_ERROR = 500
} http_status_t;

/** Content types */
typedef enum {
    CONTENT_TYPE_TEXT_PLAIN,
    CONTENT_TYPE_APPLICATION_JSON
} content_type_t;

/** HTTP response configuration */
typedef struct {
    http_status_t status_code;
    content_type_t content_type;
    const char *body;           /** Response body (NULL for empty body) */
    size_t body_length;         /** Length of body, 0 if body is NULL */
    bool include_date_header;   /** Whether to include Date header */
} http_response_config_t;

/** HTTP response buffer */
typedef struct {
    char *buffer;       /** Response buffer */
    size_t size;        /** Size of buffer */
    size_t used;        /** Bytes used in buffer */
} http_response_buffer_t;

/**
 * @brief Initialize HTTP response module
 * @return HTTP_RESPONSE_OK on success, error code otherwise
 */
http_response_error_t http_response_init(void);

/**
 * @brief Cleanup HTTP response module
 */
void http_response_cleanup(void);

/**
 * @brief Calculate required buffer size for a response
 * @param config Response configuration
 * @return Required buffer size in bytes, 0 on error
 */
size_t http_response_calculate_size(const http_response_config_t *config);

/**
 * @brief Build HTTP response into buffer
 * @param[out] buffer Response buffer to fill
 * @param[in] config Response configuration
 * @return HTTP_RESPONSE_OK on success, error code otherwise
 * @note Buffer must be large enough (use http_response_calculate_size)
 */
http_response_error_t http_response_build(http_response_buffer_t *buffer,
                                          const http_response_config_t *config);

/**
 * @brief Initialize response buffer
 * @param[out] buffer Buffer to initialize
 * @param buffer_ptr Pointer to buffer memory
 * @param buffer_size Size of buffer memory
 * @return HTTP_RESPONSE_OK on success, error code otherwise
 */
http_response_error_t http_response_buffer_init(http_response_buffer_t *buffer,
                                                char *buffer_ptr,
                                                size_t buffer_size);

/**
 * @brief Reset response buffer for reuse
 * @param buffer Buffer to reset
 */
void http_response_buffer_reset(http_response_buffer_t *buffer);

/**
 * @brief Get content type string
 * @param type Content type enum
 * @return String representation, NULL on invalid type
 */
const char *http_response_content_type_string(content_type_t type);

/**
 * @brief Get HTTP status line string
 * @param status Status code
 * @return Status line string, NULL on invalid status
 */
const char *http_response_status_string(http_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* DOMAIN_HTTP_RESPONSE_H */
