#include <stdio.h>
#include <time.h>
#include <assert.h>

#include <dynamic.h>

static uint64_t ntime(void)
{
  struct timespec tv;
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return (uint64_t) tv.tv_sec * 1000000000ULL + (uint64_t) tv.tv_nsec;
}

int main(int argc, char **argv)
{
  size_t i, n;
  list_t *l;
  uint64_t t1, t2;

  n = argc == 2 ? strtoull(argv[1], NULL, 0) : 1000000;

  l = list_create();

  t1 = ntime();
  for (i = 0; i < n; i++)
    list_append(l, &i, sizeof (i));
  t2 = ntime();

  fprintf(stderr, "%lu inserts, time %fs\n", n, (double) (t2 - t1) / 1000000000);

  list_destroy(l);
}
