/**
 * @file process.h
 * @brief Platform abstraction for process management
 *
 * This module provides a portable interface for process creation,
 * synchronization, and management operations.
 */

#ifndef PLATFORM_PROCESS_H
#define PLATFORM_PROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "../../include/platform/system.h" /* for system_error_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Process management error codes */
typedef enum {
    PROCESS_OK = 0,
    PROCESS_ERROR_FORK = -1,
    PROCESS_ERROR_EVENTFD = -2,
    PROCESS_ERROR_WAIT = -3,
    PROCESS_ERROR_INVALID_PARAM = -4,
    PROCESS_ERROR_SIGNAL = -5
} process_error_t;

/** Process type enumeration */
typedef enum {
    PROCESS_TYPE_PARENT,
    PROCESS_TYPE_WORKER
} process_type_t;

/** Worker process context */
typedef struct {
    int worker_id;           /** Worker ID (0-based) */
    int cpu_id;              /** CPU this worker is pinned to */
    int eventfd;             /** EventFD for synchronization */
    pid_t pid;               /** Process ID */
} worker_context_t;

/** Worker manager configuration */
typedef struct {
    int worker_count;        /** Number of worker processes to create */
    int *cpu_ids;            /** Array of CPU IDs to pin workers to */
    bool enable_affinity;    /** Whether to set CPU affinity */
} worker_config_t;

/** Worker manager state */
typedef struct {
    worker_config_t config;     /** Configuration */
    worker_context_t *workers;  /** Array of worker contexts */
    process_type_t type;        /** Whether this is parent or worker */
    int current_worker_id;      /** For worker processes: which worker this is */
} worker_manager_t;

/**
 * @brief Initialize worker manager
 * @param[out] manager Manager to initialize
 * @param[in] config Configuration for worker processes
 * @return PROCESS_OK on success, error code otherwise
 */
process_error_t worker_manager_init(worker_manager_t *manager, const worker_config_t *config);

/**
 * @brief Cleanup worker manager resources
 * @param manager Manager to cleanup
 */
void worker_manager_cleanup(worker_manager_t *manager);

/**
 * @brief Fork worker processes
 * @param manager Initialized worker manager
 * @return For parent: PROCESS_OK, for worker: PROCESS_OK with different context
 * @note This function only returns in the worker processes; parent waits for synchronization
 */
process_error_t worker_manager_fork_workers(worker_manager_t *manager);

/**
 * @brief Signal that worker is ready (called by worker processes)
 * @param manager Worker manager
 * @return PROCESS_OK on success, error code otherwise
 */
process_error_t worker_manager_signal_ready(worker_manager_t *manager);

/**
 * @brief Wait for all workers to exit (called by parent)
 * @param manager Worker manager
 * @return PROCESS_OK on success, error code otherwise
 */
process_error_t worker_manager_wait_workers(worker_manager_t *manager);

/**
 * @brief Get current process type
 * @param manager Worker manager
 * @return Process type
 */
process_type_t worker_manager_get_type(const worker_manager_t *manager);

/**
 * @brief Get worker ID for current process
 * @param manager Worker manager
 * @return Worker ID, or -1 if not a worker
 */
int worker_manager_get_worker_id(const worker_manager_t *manager);

/**
 * @brief Get CPU ID for current worker
 * @param manager Worker manager
 * @return CPU ID, or -1 if not applicable
 */
int worker_manager_get_cpu_id(const worker_manager_t *manager);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PROCESS_H */
