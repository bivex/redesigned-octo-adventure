/**
 * @file system.h
 * @brief Platform abstraction for system-level operations
 *
 * This module provides a portable interface to OS-specific functionality
 * like CPU affinity, process management, and system information.
 */

#ifndef PLATFORM_SYSTEM_H
#define PLATFORM_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Error codes for system operations */
typedef enum {
    SYSTEM_OK = 0,
    SYSTEM_ERROR_CPU_COUNT = -1,
    SYSTEM_ERROR_AFFINITY = -2,
    SYSTEM_ERROR_MEMORY = -3,
    SYSTEM_ERROR_INVALID_PARAM = -4
} system_error_t;

/** CPU information structure */
typedef struct {
    int count;           /** Total number of online CPUs */
    int *cpu_ids;        /** Array of CPU IDs (caller must free) */
} system_cpu_info_t;

/**
 * @brief Initialize system module
 * @return SYSTEM_OK on success, error code otherwise
 */
system_error_t system_init(void);

/**
 * @brief Cleanup system module resources
 */
void system_cleanup(void);

/**
 * @brief Get information about available CPUs
 * @param[out] info Pointer to cpu_info structure to fill
 * @return SYSTEM_OK on success, error code otherwise
 * @note Caller must free info->cpu_ids with system_free()
 */
system_error_t system_get_cpu_info(system_cpu_info_t *info);

/**
 * @brief Set CPU affinity for current process
 * @param cpu_id The CPU ID to pin to
 * @return SYSTEM_OK on success, error code otherwise
 */
system_error_t system_set_cpu_affinity(int cpu_id);

/**
 * @brief Get current CPU affinity mask as a string
 * @param[out] buffer Buffer to store the affinity string
 * @param size Size of the buffer
 * @return SYSTEM_OK on success, error code otherwise
 */
system_error_t system_get_cpu_affinity_string(char *buffer, size_t size);

/**
 * @brief Allocate memory with error checking
 * @param size Size in bytes to allocate
 * @return Pointer to allocated memory, NULL on failure
 */
void *system_malloc(size_t size);

/**
 * @brief Free memory allocated with system_malloc
 * @param ptr Pointer to free
 */
void system_free(void *ptr);

/**
 * @brief Reallocate memory with error checking
 * @param ptr Original pointer
 * @param size New size
 * @return Pointer to reallocated memory, NULL on failure
 */
void *system_realloc(void *ptr, size_t size);

/**
 * @brief Duplicate a string with error checking
 * @param str String to duplicate
 * @return Duplicated string, NULL on failure
 */
char *system_strdup(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_SYSTEM_H */
