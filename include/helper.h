#ifndef HELPER_H
#define HELPER_H

#include <stdint.h>
#include <time.h>

static inline double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

static inline double per_million(uint64_t count, uint64_t callbacks)
{
    if (callbacks == 0) {
        return 0.0;
    }

    return 1.0e6 * (double)count / (double)callbacks;
}

#endif // HELPER_H