/**
 * @file log.c
 * @brief Implementation of logging abstraction
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "../../include/platform/log.h"

bool is_logging_disabled = false; // Global flag for disabling logging

/** ANSI color codes */
#define LOG_COLOR_RESET   "\033[0m"
#define LOG_COLOR_RED     "\033[31m"
#define LOG_COLOR_YELLOW  "\033[33m"
#define LOG_COLOR_GREEN   "\033[32m"
#define LOG_COLOR_CYAN    "\033[36m"

/** Internal log configuration */
static log_config_t current_config = {
    .level = LOG_LEVEL_INFO,
    .output = NULL,
    .timestamps = true,
    .colors = true,
    .pid = true,
    .tid = false
};

/** Thread-local buffer for formatting */
static __thread char format_buffer[4096];
static __thread char time_buffer[32];

/** Mutex for thread safety */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Level names with colors */
static const char *level_names[] = {
    [LOG_LEVEL_ERROR] = LOG_COLOR_RED "ERROR" LOG_COLOR_RESET,
    [LOG_LEVEL_WARN]  = LOG_COLOR_YELLOW "WARN" LOG_COLOR_RESET,
    [LOG_LEVEL_INFO]  = LOG_COLOR_GREEN "INFO" LOG_COLOR_RESET,
    [LOG_LEVEL_DEBUG] = LOG_COLOR_CYAN "DEBUG" LOG_COLOR_RESET
};

static const char *level_names_plain[] = {
    [LOG_LEVEL_ERROR] = "ERROR",
    [LOG_LEVEL_WARN]  = "WARN",
    [LOG_LEVEL_INFO]  = "INFO",
    [LOG_LEVEL_DEBUG] = "DEBUG"
};

/**
 * @brief Format timestamp
 * @return Formatted timestamp string
 */
static const char *format_timestamp(void)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    if (!tm_info) {
        return "00:00:00";
    }

    strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", tm_info);
    return time_buffer;
}

/**
 * @brief Internal logging function
 */
static void log_internal(log_level_t level, const char *format, va_list args)
{
    /* Skip logging if disabled globally */
    if (is_logging_disabled) {
        return;
    }

    if (level > current_config.level) {
        return;
    }

    /* Thread-safe output */
    pthread_mutex_lock(&log_mutex);

    FILE *out = current_config.output ? current_config.output : stderr;

    /* Format the message */
    int len = vsnprintf(format_buffer, sizeof(format_buffer), format, args);
    if (len < 0 || (size_t)len >= sizeof(format_buffer)) {
        /* Message too long or error */
        fprintf(out, "[LOG ERROR: message too long]\n");
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    /* Build the log line */
    char log_line[8192];
    char *ptr = log_line;
    size_t remaining = sizeof(log_line);

    /* Timestamp */
    if (current_config.timestamps) {
        int written = snprintf(ptr, remaining, "[%s] ", format_timestamp());
        if (written < 0 || (size_t)written >= remaining) goto overflow;
        ptr += written;
        remaining -= written;
    }

    /* Level */
    const char *level_str = current_config.colors ?
        level_names[level] : level_names_plain[level];
    int written = snprintf(ptr, remaining, "[%s] ", level_str);
    if (written < 0 || (size_t)written >= remaining) goto overflow;
    ptr += written;
    remaining -= written;

    /* PID */
    if (current_config.pid) {
        written = snprintf(ptr, remaining, "[%d] ", getpid());
        if (written < 0 || (size_t)written >= remaining) goto overflow;
        ptr += written;
        remaining -= written;
    }

    /* TID */
    if (current_config.tid) {
        written = snprintf(ptr, remaining, "[%lu] ", (unsigned long)pthread_self());
        if (written < 0 || (size_t)written >= remaining) goto overflow;
        ptr += written;
        remaining -= written;
    }

    /* Message */
    written = snprintf(ptr, remaining, "%s\n", format_buffer);
    if (written < 0 || (size_t)written >= remaining) goto overflow;

    /* Output */
    fputs(log_line, out);
    fflush(out);

    pthread_mutex_unlock(&log_mutex);
    return;

overflow:
    fprintf(out, "[LOG ERROR: line too long]\n");
    fflush(out);
    pthread_mutex_unlock(&log_mutex);
}

log_error_t log_init(const log_config_t *config)
{
    /* Skip initialization if logging is disabled */
    if (is_logging_disabled) {
        return LOG_OK;
    }

    if (config) {
        current_config = *config;
    }

    /* Ensure we have a valid output stream */
    if (!current_config.output) {
        current_config.output = stderr;
    }

    return LOG_OK;
}

void log_cleanup(void)
{
    /* Close file if we opened it (not implemented yet) */
    /* For now, just flush */
    if (current_config.output && current_config.output != stdout && current_config.output != stderr) {
        fflush(current_config.output);
    }
}

void log_set_level(log_level_t level)
{
    if (level <= LOG_LEVEL_DEBUG) {
        current_config.level = level;
    }
}

log_level_t log_get_level(void)
{
    return current_config.level;
}

void log_set_output(FILE *output)
{
    if (output) {
        current_config.output = output;
    }
}

void log_set_timestamps(bool enable)
{
    current_config.timestamps = enable;
}

void log_set_colors(bool enable)
{
    current_config.colors = enable;
}

void log_set_pid(bool enable)
{
    current_config.pid = enable;
}

void log_set_tid(bool enable)
{
    current_config.tid = enable;
}

void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_internal(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}

void log_warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_internal(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

void log_info(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_internal(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_debug(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_internal(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

log_config_t log_default_config(void)
{
    return (log_config_t){
        .level = LOG_LEVEL_INFO,
        .output = stderr,
        .timestamps = true,
        .colors = isatty(fileno(stderr)),  /* Colors only for terminals */
        .pid = true,
        .tid = false
    };
}
