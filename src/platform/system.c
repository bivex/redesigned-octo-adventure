/**
 * @file system.c
 * @brief Implementation of platform abstraction for system operations
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include "../../include/platform/system.h"

/** Internal state */
static bool system_initialized = false;

system_error_t system_init(void)
{
    if (system_initialized) {
        return SYSTEM_OK;
    }

    /* Set up signal handling for common issues */
    signal(SIGPIPE, SIG_IGN);

    system_initialized = true;
    return SYSTEM_OK;
}

void system_cleanup(void)
{
    system_initialized = false;
}

system_error_t system_get_cpu_info(system_cpu_info_t *info)
{
    if (!info) {
        return SYSTEM_ERROR_INVALID_PARAM;
    }

    cpu_set_t online_cpus;
    CPU_ZERO(&online_cpus);

    if (sched_getaffinity(0, sizeof(online_cpus), &online_cpus) == -1) {
        return SYSTEM_ERROR_CPU_COUNT;
    }

    int num_online_cpus = CPU_COUNT(&online_cpus);
    if (num_online_cpus <= 0) {
        return SYSTEM_ERROR_CPU_COUNT;
    }

    int *cpu_ids = system_malloc(num_online_cpus * sizeof(int));
    if (!cpu_ids) {
        return SYSTEM_ERROR_MEMORY;
    }

    int rel_cpu_index = 0;
    for (int abs_cpu_index = 0; abs_cpu_index < CPU_SETSIZE; abs_cpu_index++) {
        if (CPU_ISSET(abs_cpu_index, &online_cpus)) {
            cpu_ids[rel_cpu_index] = abs_cpu_index;
            rel_cpu_index++;

            if (rel_cpu_index == num_online_cpus) {
                break;
            }
        }
    }

    info->count = num_online_cpus;
    info->cpu_ids = cpu_ids;

    return SYSTEM_OK;
}

system_error_t system_set_cpu_affinity(int cpu_id)
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu_id, &cpu_set);

    if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) == -1) {
        return SYSTEM_ERROR_AFFINITY;
    }

    return SYSTEM_OK;
}

system_error_t system_get_cpu_affinity_string(char *buffer, size_t size)
{
    if (!buffer || size == 0) {
        return SYSTEM_ERROR_INVALID_PARAM;
    }

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);

    if (sched_getaffinity(0, sizeof(cpu_set), &cpu_set) == -1) {
        return SYSTEM_ERROR_AFFINITY;
    }

    /* Convert CPU set to string representation */
    size_t written = 0;
    bool first = true;

    for (int i = 0; i < CPU_SETSIZE && written < size - 1; i++) {
        if (CPU_ISSET(i, &cpu_set)) {
            int len;
            if (first) {
                len = snprintf(buffer + written, size - written, "%d", i);
                first = false;
            } else {
                len = snprintf(buffer + written, size - written, ",%d", i);
            }

            if (len < 0 || (size_t)len >= size - written) {
                break; /* Buffer would overflow */
            }
            written += len;
        }
    }

    if (written >= size) {
        return SYSTEM_ERROR_MEMORY; /* Buffer too small */
    }

    return SYSTEM_OK;
}

void *system_malloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    void *ptr = malloc(size);
    if (!ptr) {
        /* In a real implementation, you might want to log this error */
        return NULL;
    }

    return ptr;
}

void system_free(void *ptr)
{
    free(ptr);
}

void *system_realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);
    if (size > 0 && !new_ptr) {
        /* In a real implementation, you might want to log this error */
        return NULL;
    }

    return new_ptr;
}

char *system_strdup(const char *str)
{
    if (!str) {
        return NULL;
    }

    char *dup = strdup(str);
    if (!dup) {
        /* In a real implementation, you might want to log this error */
        return NULL;
    }

    return dup;
}
