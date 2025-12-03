/**
 * @file log.h
 * @brief Platform abstraction for logging
 *
 * This module provides a portable logging interface with different
 * log levels, output formatting, and thread safety.
 */

#ifndef PLATFORM_LOG_H
#define PLATFORM_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Global flag to disable all logging */
extern bool is_logging_disabled;

/** Log levels */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

/** Log error codes */
typedef enum {
    LOG_OK = 0,
    LOG_ERROR_INVALID_PARAM = -1,
    LOG_ERROR_MEMORY = -2,
    LOG_ERROR_IO = -3
} log_error_t;

/** Log configuration */
typedef struct {
    log_level_t level;           /** Minimum log level to output */
    FILE *output;                /** Output stream (stdout, stderr, or file) */
    bool timestamps;             /** Include timestamps */
    bool colors;                 /** Use ANSI color codes */
    bool pid;                    /** Include process ID */
    bool tid;                    /** Include thread ID */
} log_config_t;

/**
 * @brief Initialize logging module
 * @param config Logging configuration, NULL for defaults
 * @return LOG_OK on success, error code otherwise
 */
log_error_t log_init(const log_config_t *config);

/**
 * @brief Cleanup logging module
 */
void log_cleanup(void);

/**
 * @brief Set log level
 * @param level New log level
 */
void log_set_level(log_level_t level);

/**
 * @brief Get current log level
 * @return Current log level
 */
log_level_t log_get_level(void);

/**
 * @brief Set output stream
 * @param output Output stream (must be valid FILE*)
 */
void log_set_output(FILE *output);

/**
 * @brief Enable/disable timestamps
 * @param enable True to enable timestamps
 */
void log_set_timestamps(bool enable);

/**
 * @brief Enable/disable colors
 * @param enable True to enable ANSI colors
 */
void log_set_colors(bool enable);

/**
 * @brief Enable/disable PID in logs
 * @param enable True to include PID
 */
void log_set_pid(bool enable);

/**
 * @brief Enable/disable TID in logs
 * @param enable True to include TID
 */
void log_set_tid(bool enable);

/**
 * @brief Log error message
 * @param format Format string (printf style)
 * @param ... Format arguments
 */
void log_error(const char *format, ...);

/**
 * @brief Log warning message
 * @param format Format string (printf style)
 * @param ... Format arguments
 */
void log_warn(const char *format, ...);

/**
 * @brief Log info message
 * @param format Format string (printf style)
 * @param ... Format arguments
 */
void log_info(const char *format, ...);

/**
 * @brief Log debug message
 * @param format Format string (printf style)
 * @param ... Format arguments
 */
void log_debug(const char *format, ...);

/**
 * @brief Get default log configuration
 * @return Default configuration
 */
log_config_t log_default_config(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_LOG_H */
