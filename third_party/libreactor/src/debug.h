#ifndef REACTOR_DEBUG_H
#define REACTOR_DEBUG_H

#include <stdio.h>

#ifdef DEBUG_MODE
#define REACTOR_DEBUG(fmt, ...) fprintf(stderr, "[REACTOR_DEBUG] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define REACTOR_DEBUG(fmt, ...) do {} while (0)
#endif

#endif // REACTOR_DEBUG_H
