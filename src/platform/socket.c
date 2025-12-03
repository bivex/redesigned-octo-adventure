/**
 * @file socket.c
 * @brief Implementation of socket operations abstraction
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/filter.h>
#include <errno.h>

#include "../../include/platform/socket.h"

socket_error_t socket_init(void)
{
    /* Currently no initialization needed */
    return SOCKET_OK;
}

socket_error_t socket_apply_optimizations(int socket_fd, const socket_config_t *config)
{
    if (socket_fd < 0 || !config) {
        return SOCKET_ERROR_INVALID_PARAM;
    }

    socket_error_t result = SOCKET_OK;

    /* Apply busy poll if requested */
    if (config->options & SOCKET_OPT_BUSY_POLL) {
        socket_error_t err = socket_set_busy_poll(socket_fd, config->busy_poll_value);
        if (err != SOCKET_OK) {
            result = err;
        }
    }

    /* Apply TCP_NODELAY if requested */
    if (config->options & SOCKET_OPT_NODELAY) {
        socket_error_t err = socket_set_tcp_nodelay(socket_fd, true);
        if (err != SOCKET_OK) {
            result = err;
        }
    }

    /* Apply keepalive setting if requested */
    if (config->options & SOCKET_OPT_KEEPALIVE) {
        socket_error_t err = socket_set_keepalive(socket_fd, config->keepalive_enabled);
        if (err != SOCKET_OK) {
            result = err;
        }
    }

    /* Apply CPU-aware load balancing if requested */
    if (config->options & SOCKET_OPT_REUSEPORT_CBPF) {
        socket_error_t err = socket_enable_reuseport_cbpf(socket_fd);
        if (err != SOCKET_OK) {
            result = err;
        }
    }

    return result;
}

socket_error_t socket_enable_reuseport_cbpf(int socket_fd)
{
    if (socket_fd < 0) {
        return SOCKET_ERROR_INVALID_PARAM;
    }

    /* BPF program that returns the CPU number for load balancing */
    struct sock_filter code[] = {
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF + SKF_AD_CPU},
        {BPF_RET | BPF_A, 0, 0, 0}
    };

    struct sock_fprog prog = {0};  /* Zero-initialize to avoid uninitialized bytes */
    prog.len = sizeof(code) / sizeof(code[0]);
    prog.filter = code;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF,
                   &prog, sizeof(prog)) == -1) {
        return SOCKET_ERROR_BPF;
    }

    return SOCKET_OK;
}

socket_error_t socket_set_busy_poll(int socket_fd, int timeout_us)
{
    if (socket_fd < 0) {
        return SOCKET_ERROR_INVALID_PARAM;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_BUSY_POLL,
                   &timeout_us, sizeof(timeout_us)) == -1) {
        return SOCKET_ERROR_SETSOCKOPT;
    }

    return SOCKET_OK;
}

socket_error_t socket_set_tcp_nodelay(int socket_fd, bool enabled)
{
    if (socket_fd < 0) {
        return SOCKET_ERROR_INVALID_PARAM;
    }

    int flag = enabled ? 1 : 0;
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
                   &flag, sizeof(flag)) == -1) {
        return SOCKET_ERROR_SETSOCKOPT;
    }

    return SOCKET_OK;
}

socket_error_t socket_set_keepalive(int socket_fd, bool enabled)
{
    if (socket_fd < 0) {
        return SOCKET_ERROR_INVALID_PARAM;
    }

    int flag = enabled ? 1 : 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE,
                   &flag, sizeof(flag)) == -1) {
        return SOCKET_ERROR_SETSOCKOPT;
    }

    return SOCKET_OK;
}
