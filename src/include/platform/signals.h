/**
 * @file signals.h
 * @brief Platform abstraction for signal handling
 *
 * This module provides portable signal handling for graceful shutdown,
 * configuration reloading, and other signal-based operations.
 */

#ifndef PLATFORM_SIGNALS_H
#define PLATFORM_SIGNALS_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Signal handler callback type */
typedef void (*signal_callback_t)(int signum);

/** Signal configuration */
typedef struct {
    bool handle_sigterm;         /** Handle SIGTERM for graceful shutdown */
    bool handle_sigint;          /** Handle SIGINT (Ctrl+C) */
    bool handle_sighup;          /** Handle SIGHUP for configuration reload */
    bool handle_sigusr1;         /** Handle SIGUSR1 for custom operations */
    bool handle_sigusr2;         /** Handle SIGUSR2 for custom operations */
    bool handle_sigpipe;         /** Handle SIGPIPE (ignore by default) */
} signal_config_t;

/** Signal manager state */
typedef struct {
    signal_config_t config;
    signal_callback_t sigterm_handler;
    signal_callback_t sigint_handler;
    signal_callback_t sighup_handler;
    signal_callback_t sigusr1_handler;
    signal_callback_t sigusr2_handler;
    volatile sig_atomic_t shutdown_requested;
    volatile sig_atomic_t reload_requested;
} signal_manager_t;

/** Signal error codes */
typedef enum {
    SIGNAL_OK = 0,
    SIGNAL_ERROR_INVALID_PARAM = -1,
    SIGNAL_ERROR_SETUP = -2,
    SIGNAL_ERROR_MEMORY = -3
} signal_error_t;

/**
 * @brief Initialize signal manager
 * @param manager Signal manager to initialize
 * @param config Signal configuration
 * @return SIGNAL_OK on success, error code otherwise
 */
signal_error_t signal_manager_init(signal_manager_t *manager, const signal_config_t *config);

/**
 * @brief Cleanup signal manager
 * @param manager Signal manager to cleanup
 */
void signal_manager_cleanup(signal_manager_t *manager);

/**
 * @brief Set signal handler callback
 * @param manager Signal manager
 * @param signum Signal number
 * @param callback Handler callback, NULL to remove
 * @return SIGNAL_OK on success, error code otherwise
 */
signal_error_t signal_manager_set_handler(signal_manager_t *manager, int signum, signal_callback_t callback);

/**
 * @brief Check if shutdown was requested
 * @param manager Signal manager
 * @return true if shutdown requested
 */
bool signal_manager_shutdown_requested(const signal_manager_t *manager);

/**
 * @brief Check if reload was requested
 * @param manager Signal manager
 * @return true if reload requested
 */
bool signal_manager_reload_requested(const signal_manager_t *manager);

/**
 * @brief Reset shutdown request flag
 * @param manager Signal manager
 */
void signal_manager_reset_shutdown(signal_manager_t *manager);

/**
 * @brief Reset reload request flag
 * @param manager Signal manager
 */
void signal_manager_reset_reload(signal_manager_t *manager);

/**
 * @brief Wait for signals (blocking)
 * @param manager Signal manager
 * @note This function blocks until a signal is received
 */
void signal_manager_wait(signal_manager_t *manager);

/**
 * @brief Get default signal configuration
 * @return Default configuration
 */
signal_config_t signal_manager_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SIGNALS_H */
