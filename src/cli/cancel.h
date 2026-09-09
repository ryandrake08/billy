// A caller-owned cancellation check for blocking CLI operations.
#pragma once

#include <stdbool.h>

typedef bool (*cancel_check_t)(const volatile void *ctx);

static inline bool cancel_requested(cancel_check_t check, const volatile void *ctx)
{
    return check && check(ctx);
}
