/**
 * @file compat/reactor.h
 * @brief Compatibility header that redirects to io_uring adapter
 */

#ifndef COMPAT_REACTOR_H_INCLUDED
#define COMPAT_REACTOR_H_INCLUDED

// Redirect to our io_uring implementation
#include "../../platform/io_uring_adapter/io_uring_adapter.h"

#endif /* COMPAT_REACTOR_H_INCLUDED */
