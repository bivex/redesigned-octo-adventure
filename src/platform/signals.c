/**
 * @file signals.c
 * @brief Implementation of signal handling abstraction
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "../../include/platform/signals.h"
#include "../../include/platform/log.h"

/** Global signal manager reference for handlers */
static signal_manager_t *global_signal_manager = NULL;

/**
 * @brief Generic signal handler
 */
static void signal_handler(int signum)
{
    if (!global_signal_manager) {
        return;
    }

    switch (signum) {
        case SIGTERM:
            global_signal_manager->shutdown_requested = 1;
            if (global_signal_manager->sigterm_handler) {
                global_signal_manager->sigterm_handler(signum);
            } else {
                log_info("SIGTERM received, initiating graceful shutdown");
            }
            break;

        case SIGINT:
            global_signal_manager->shutdown_requested = 1;
            if (global_signal_manager->sigint_handler) {
                global_signal_manager->sigint_handler(signum);
            } else {
                log_info("SIGINT received, initiating graceful shutdown");
            }
            break;

        case SIGHUP:
            global_signal_manager->reload_requested = 1;
            if (global_signal_manager->sighup_handler) {
                global_signal_manager->sighup_handler(signum);
            } else {
                log_info("SIGHUP received, configuration reload requested");
            }
            break;

        case SIGUSR1:
            if (global_signal_manager->sigusr1_handler) {
                global_signal_manager->sigusr1_handler(signum);
            }
            break;

        case SIGUSR2:
            if (global_signal_manager->sigusr2_handler) {
                global_signal_manager->sigusr2_handler(signum);
            }
            break;

        case SIGPIPE:
            /* Ignore SIGPIPE by default */
            break;

        default:
            log_warn("Unhandled signal: %d", signum);
            break;
    }
}

/**
 * @brief Setup signal handler
 */
static signal_error_t setup_signal_handler(int signum, bool enable)
{
    struct sigaction sa;

    if (enable) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);

        /* Block signals while handling */
        sigaddset(&sa.sa_mask, SIGTERM);
        sigaddset(&sa.sa_mask, SIGINT);
        sigaddset(&sa.sa_mask, SIGHUP);
        sigaddset(&sa.sa_mask, SIGUSR1);
        sigaddset(&sa.sa_mask, SIGUSR2);

        sa.sa_flags = SA_RESTART;

        if (sigaction(signum, &sa, NULL) == -1) {
            log_error("Failed to setup signal handler for %d: %s", signum, strerror(errno));
            return SIGNAL_ERROR_SETUP;
        }
    } else {
        /* Restore default handler */
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        if (sigaction(signum, &sa, NULL) == -1) {
            log_error("Failed to restore default handler for %d: %s", signum, strerror(errno));
            return SIGNAL_ERROR_SETUP;
        }
    }

    return SIGNAL_OK;
}

signal_error_t signal_manager_init(signal_manager_t *manager, const signal_config_t *config)
{
    if (!manager) {
        return SIGNAL_ERROR_INVALID_PARAM;
    }

    memset(manager, 0, sizeof(*manager));

    if (config) {
        manager->config = *config;
    } else {
        manager->config = signal_manager_default_config();
    }

    /* Set global reference */
    global_signal_manager = manager;

    /* Setup signal handlers */
    signal_error_t err;

    if (manager->config.handle_sigterm) {
        err = setup_signal_handler(SIGTERM, true);
        if (err != SIGNAL_OK) return err;
    }

    if (manager->config.handle_sigint) {
        err = setup_signal_handler(SIGINT, true);
        if (err != SIGNAL_OK) return err;
    }

    if (manager->config.handle_sighup) {
        err = setup_signal_handler(SIGHUP, true);
        if (err != SIGNAL_OK) return err;
    }

    if (manager->config.handle_sigusr1) {
        err = setup_signal_handler(SIGUSR1, true);
        if (err != SIGNAL_OK) return err;
    }

    if (manager->config.handle_sigusr2) {
        err = setup_signal_handler(SIGUSR2, true);
        if (err != SIGNAL_OK) return err;
    }

    if (manager->config.handle_sigpipe) {
        err = setup_signal_handler(SIGPIPE, true);
        if (err != SIGNAL_OK) return err;
    }

    log_debug("Signal manager initialized");
    return SIGNAL_OK;
}

void signal_manager_cleanup(signal_manager_t *manager)
{
    if (!manager) {
        return;
    }

    /* Remove global reference */
    if (global_signal_manager == manager) {
        global_signal_manager = NULL;
    }

    /* Restore default handlers */
    if (manager->config.handle_sigterm) {
        setup_signal_handler(SIGTERM, false);
    }
    if (manager->config.handle_sigint) {
        setup_signal_handler(SIGINT, false);
    }
    if (manager->config.handle_sighup) {
        setup_signal_handler(SIGHUP, false);
    }
    if (manager->config.handle_sigusr1) {
        setup_signal_handler(SIGUSR1, false);
    }
    if (manager->config.handle_sigusr2) {
        setup_signal_handler(SIGUSR2, false);
    }
    if (manager->config.handle_sigpipe) {
        setup_signal_handler(SIGPIPE, false);
    }

    memset(manager, 0, sizeof(*manager));
    log_debug("Signal manager cleaned up");
}

signal_error_t signal_manager_set_handler(signal_manager_t *manager, int signum, signal_callback_t callback)
{
    if (!manager) {
        return SIGNAL_ERROR_INVALID_PARAM;
    }

    switch (signum) {
        case SIGTERM:
            manager->sigterm_handler = callback;
            break;
        case SIGINT:
            manager->sigint_handler = callback;
            break;
        case SIGHUP:
            manager->sighup_handler = callback;
            break;
        case SIGUSR1:
            manager->sigusr1_handler = callback;
            break;
        case SIGUSR2:
            manager->sigusr2_handler = callback;
            break;
        default:
            return SIGNAL_ERROR_INVALID_PARAM;
    }

    return SIGNAL_OK;
}

bool signal_manager_shutdown_requested(const signal_manager_t *manager)
{
    return manager ? manager->shutdown_requested : false;
}

bool signal_manager_reload_requested(const signal_manager_t *manager)
{
    return manager ? manager->reload_requested : false;
}

void signal_manager_reset_shutdown(signal_manager_t *manager)
{
    if (manager) {
        manager->shutdown_requested = 0;
    }
}

void signal_manager_reset_reload(signal_manager_t *manager)
{
    if (manager) {
        manager->reload_requested = 0;
    }
}

void signal_manager_wait(signal_manager_t *manager)
{
    if (!manager) {
        return;
    }

    /* Simple busy wait - in production might want to use sigsuspend */
    while (!manager->shutdown_requested && !manager->reload_requested) {
        usleep(100000); /* 100ms */
    }
}

signal_config_t signal_manager_default_config(void)
{
    return (signal_config_t){
        .handle_sigterm = true,
        .handle_sigint = true,
        .handle_sighup = false,
        .handle_sigusr1 = false,
        .handle_sigusr2 = false,
        .handle_sigpipe = true  /* Ignore SIGPIPE */
    };
}
