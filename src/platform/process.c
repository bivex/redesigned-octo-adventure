/**
 * @file process.c
 * @brief Implementation of process management abstraction
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <string.h>

#include "../../include/platform/process.h"
#include "../../include/platform/system.h"
#include "../../include/platform/log.h"

process_error_t worker_manager_init(worker_manager_t *manager, const worker_config_t *config)
{
    if (!manager || !config) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    if (config->worker_count <= 0 || !config->cpu_ids) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    /* Initialize system module if not already done */
    system_error_t sys_err = system_init();
    if (sys_err != SYSTEM_OK) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    memset(manager, 0, sizeof(*manager));

    /* Copy configuration */
    manager->config.worker_count = config->worker_count;
    manager->config.enable_affinity = config->enable_affinity;

    /* Allocate and copy CPU IDs */
    manager->config.cpu_ids = system_malloc(config->worker_count * sizeof(int));
    if (!manager->config.cpu_ids) {
        return PROCESS_ERROR_INVALID_PARAM;
    }
    memcpy(manager->config.cpu_ids, config->cpu_ids, config->worker_count * sizeof(int));

    /* Allocate worker contexts */
    manager->workers = system_malloc(config->worker_count * sizeof(worker_context_t));
    if (!manager->workers) {
        system_free(manager->config.cpu_ids);
        return PROCESS_ERROR_INVALID_PARAM;
    }
    memset(manager->workers, 0, sizeof(worker_context_t) * config->worker_count);

    /* Initialize as parent process initially */
    manager->type = PROCESS_TYPE_PARENT;
    manager->current_worker_id = -1;

    return PROCESS_OK;
}

void worker_manager_cleanup(worker_manager_t *manager)
{
    if (!manager) {
        return;
    }

    if (manager->config.cpu_ids) {
        system_free(manager->config.cpu_ids);
    }

    if (manager->workers) {
        /* Close any open eventfds */
        for (int i = 0; i < manager->config.worker_count; i++) {
            if (manager->workers[i].eventfd > 0) {
                close(manager->workers[i].eventfd);
            }
        }
        system_free(manager->workers);
    }

    memset(manager, 0, sizeof(*manager));
}

process_error_t worker_manager_fork_workers(worker_manager_t *manager)
{
    if (!manager) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    /* Fork worker processes */
    for (int i = 0; i < manager->config.worker_count; i++) {
        /* Create eventfd for synchronization */
        int efd = eventfd(0, EFD_SEMAPHORE);
        if (efd == -1) {
            return PROCESS_ERROR_EVENTFD;
        }

        pid_t pid = fork();
        if (pid == -1) {
            close(efd);
            return PROCESS_ERROR_FORK;
        }

        /* Parent process */
        if (pid > 0) {
            manager->workers[i].worker_id = i;
            manager->workers[i].cpu_id = manager->config.cpu_ids[i];
            manager->workers[i].eventfd = efd;
            manager->workers[i].pid = pid;

            /* Wait for worker to signal ready */
            eventfd_t value;
            if (eventfd_read(efd, &value) == -1) {
                close(efd);
                return PROCESS_ERROR_EVENTFD;
            }
            close(efd);
            manager->workers[i].eventfd = -1; /* Mark as closed */

            log_info("Worker %d running on CPU %d (PID: %d)",
                     i, manager->config.cpu_ids[i], pid);
            continue;
        }

        /* Child/Worker process */
        else {
            manager->type = PROCESS_TYPE_WORKER;
            manager->current_worker_id = i;

            /* Set CPU affinity if enabled */
            if (manager->config.enable_affinity) {
                system_error_t err = system_set_cpu_affinity(manager->config.cpu_ids[i]);
                if (err != SYSTEM_OK) {
                    close(efd);
                    return PROCESS_ERROR_INVALID_PARAM;
                }
            }

            /* Store eventfd for later signaling */
            manager->workers[i].eventfd = efd;
            manager->workers[i].cpu_id = manager->config.cpu_ids[i];

            /* Free parent-only resources */
            for (int j = 0; j < i; j++) {
                if (manager->workers[j].eventfd > 0) {
                    close(manager->workers[j].eventfd);
                    manager->workers[j].eventfd = -1;
                }
            }

            return PROCESS_OK;
        }
    }

    log_info("libreactor running with %d worker processes",
             manager->config.worker_count);

    return PROCESS_OK;
}

process_error_t worker_manager_signal_ready(worker_manager_t *manager)
{
    if (!manager || manager->type != PROCESS_TYPE_WORKER) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    int worker_id = manager->current_worker_id;
    if (worker_id < 0 || worker_id >= manager->config.worker_count) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    int efd = manager->workers[worker_id].eventfd;
    if (efd <= 0) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    /* Signal parent that we're ready */
    if (eventfd_write(efd, 1) == -1) {
        return PROCESS_ERROR_EVENTFD;
    }

    /* Close eventfd after signaling */
    close(efd);
    manager->workers[worker_id].eventfd = -1;

    return PROCESS_OK;
}

process_error_t worker_manager_wait_workers(worker_manager_t *manager)
{
    if (!manager || manager->type != PROCESS_TYPE_PARENT) {
        return PROCESS_ERROR_INVALID_PARAM;
    }

    /* Wait for any child to exit */
    int status;
    pid_t pid = wait(&status);
    if (pid == -1) {
        return PROCESS_ERROR_WAIT;
    }

    log_error("A worker process (PID: %d) has exited unexpectedly. Shutting down", pid);
    return PROCESS_OK;
}

process_type_t worker_manager_get_type(const worker_manager_t *manager)
{
    return manager ? manager->type : PROCESS_TYPE_PARENT;
}

int worker_manager_get_worker_id(const worker_manager_t *manager)
{
    if (!manager || manager->type != PROCESS_TYPE_WORKER) {
        return -1;
    }
    return manager->current_worker_id;
}

int worker_manager_get_cpu_id(const worker_manager_t *manager)
{
    if (!manager || manager->type != PROCESS_TYPE_WORKER) {
        return -1;
    }

    int worker_id = manager->current_worker_id;
    if (worker_id < 0 || worker_id >= manager->config.worker_count) {
        return -1;
    }

    return manager->workers[worker_id].cpu_id;
}
