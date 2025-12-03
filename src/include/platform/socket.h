/**
 * @file socket.h
 * @brief Platform abstraction for socket operations
 *
 * This module provides a portable interface for socket configuration
 * and optimization operations.
 */

#ifndef PLATFORM_SOCKET_H
#define PLATFORM_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "../../include/platform/system.h" /* for system_error_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Socket error codes */
typedef enum {
    SOCKET_OK = 0,
    SOCKET_ERROR_SETSOCKOPT = -1,
    SOCKET_ERROR_INVALID_PARAM = -2,
    SOCKET_ERROR_BPF = -3
} socket_error_t;

/** Socket optimization flags */
typedef enum {
    SOCKET_OPT_BUSY_POLL = 1 << 0,      /** Enable busy polling */
    SOCKET_OPT_NODELAY = 1 << 1,        /** Disable Nagle's algorithm */
    SOCKET_OPT_KEEPALIVE = 1 << 2,      /** Enable keepalive (disable) */
    SOCKET_OPT_REUSEPORT_CBPF = 1 << 3  /** Enable CPU-aware load balancing */
} socket_option_t;

/** Socket optimization configuration */
typedef struct {
    uint32_t options;         /** Bitmask of socket_option_t flags */
    int busy_poll_value;      /** Busy poll timeout value (microseconds) */
    bool keepalive_enabled;   /** Whether keepalive is enabled */
} socket_config_t;

/**
 * @brief Initialize socket module
 * @return SOCKET_OK on success, error code otherwise
 */
socket_error_t socket_init(void);

/**
 * @brief Apply socket optimizations to a server socket
 * @param socket_fd The socket file descriptor
 * @param config Socket configuration
 * @return SOCKET_OK on success, error code otherwise
 */
socket_error_t socket_apply_optimizations(int socket_fd, const socket_config_t *config);

/**
 * @brief Enable CPU-aware connection distribution using BPF
 * @param socket_fd The socket file descriptor
 * @return SOCKET_OK on success, error code otherwise
 * @note This requires SO_REUSEPORT to be enabled on the socket
 */
socket_error_t socket_enable_reuseport_cbpf(int socket_fd);

/**
 * @brief Set busy poll timeout
 * @param socket_fd The socket file descriptor
 * @param timeout_us Timeout in microseconds
 * @return SOCKET_OK on success, error code otherwise
 */
socket_error_t socket_set_busy_poll(int socket_fd, int timeout_us);

/**
 * @brief Set TCP_NODELAY option
 * @param socket_fd The socket file descriptor
 * @param enabled Whether to enable TCP_NODELAY
 * @return SOCKET_OK on success, error code otherwise
 */
socket_error_t socket_set_tcp_nodelay(int socket_fd, bool enabled);

/**
 * @brief Set SO_KEEPALIVE option
 * @param socket_fd The socket file descriptor
 * @param enabled Whether to enable keepalive
 * @return SOCKET_OK on success, error code otherwise
 */
socket_error_t socket_set_keepalive(int socket_fd, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SOCKET_H */
