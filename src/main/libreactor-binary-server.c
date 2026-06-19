/**
 * @file libreactor-binary-server.c
 * @brief Main for the raw-binary protocol server (isolated benchmark binary).
 *
 * Forks one worker per CPU (pinned via sched_setaffinity), each running its own
 * io_uring reactor loop driving a binary_server on BINARY_SERVER_DEFAULT_PORT.
 * No HTTP path — this is the comparison baseline for HTTP/json overhead.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sched.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "../../include/infrastructure/binary_server.h"
#include "../../include/platform/log.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void) sig;
  g_stop = 0;  /* reactor handles abort via SIGTERM/SIGINT in reactor_construct */
}

static int pin_cpu(int cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof set, &set);
}

static void worker_run(int worker_id, int cpu_id, int port)
{
  if (pin_cpu(cpu_id) == 0)
    log_info("binary worker %d pinned to CPU %d", worker_id, cpu_id);

  reactor_construct();

  char port_str[16];
  snprintf(port_str, sizeof port_str, "%d", port);
  struct addrinfo *ai = net_resolve(NULL, port_str, AF_INET, SOCK_STREAM, AI_PASSIVE);
  if (!ai)
  {
    log_error("binary worker %d: net_resolve failed", worker_id);
    reactor_destruct();
    return;
  }
  int fd = net_socket(ai);
  freeaddrinfo(ai);
  if (fd == -1)
  {
    log_error("binary worker %d: net_socket failed", worker_id);
    reactor_destruct();
    return;
  }

  binary_server bs;
  binary_server_construct(&bs);
  binary_server_open(&bs, fd);

  log_info("binary worker %d listening on port %d", worker_id, port);

  reactor_loop();

  binary_server_shutdown(&bs);
  reactor_destruct();
  log_info("binary worker %d exiting", worker_id);
}

int main(int argc, char *argv[])
{
  int port = BINARY_SERVER_DEFAULT_PORT;
  int workers = -1;  /* default: nproc */

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
      workers = atoi(argv[++i]);
    else if (strcmp(argv[i], "--disable-log") == 0)
      is_logging_disabled = 1;
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
    {
      printf("Usage: %s [--port N] [--workers N] [--disable-log]\n", argv[0]);
      printf("  --port N        TCP port (default %d)\n", BINARY_SERVER_DEFAULT_PORT);
      printf("  --workers N     worker process count (default: nproc)\n");
      printf("  --disable-log   disable logging\n");
      return EXIT_SUCCESS;
    }
  }

  if (workers <= 0)
    workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (workers <= 0)
    workers = 1;

  /* ignore SIGPIPE (writes to closed sockets), let reactor catch SIGTERM/INT */
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  log_info("starting binary server: %d workers on port %d", workers, port);

  pid_t *pids = calloc((size_t)workers, sizeof *pids);
  if (!pids)
    return EXIT_FAILURE;

  for (int i = 0; i < workers; i++)
  {
    pid_t pid = fork();
    if (pid < 0)
    {
      log_error("fork failed for worker %d: %s", i, strerror(errno));
      break;
    }
    if (pid == 0)
    {
      /* child: SO_REUSEPORT lets all workers bind the same port; the kernel
       * load-balances accepts across them. */
      worker_run(i, i, port);
      _exit(EXIT_SUCCESS);
    }
    pids[i] = pid;
  }

  /* parent: wait for children */
  for (int i = 0; i < workers; i++)
  {
    if (pids[i] > 0)
    {
      int status;
      waitpid(pids[i], &status, 0);
    }
  }
  free(pids);

  log_info("binary server stopped");
  return EXIT_SUCCESS;
}
