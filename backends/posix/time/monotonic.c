/*
 * time.monotonic – POSIX (macOS, Linux).
 *
 * macOS: CLOCK_UPTIME_RAW. CLOCK_MONOTONIC zählt dort den Ruhezustand mit,
 *        CLOCK_UPTIME_RAW nicht; so verhalten sich macOS und Linux gleich.
 * Linux: CLOCK_MONOTONIC (zählt den Ruhezustand nicht mit).
 */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nhal/features/time/monotonic.h"
#include "nhal/nhal_backend.h"

#include <time.h>

#if defined(__APPLE__)
#define NHAL_CLOCK CLOCK_UPTIME_RAW
#else
#define NHAL_CLOCK CLOCK_MONOTONIC
#endif

static uint64_t to_ns(const struct timespec* ts) {
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(NHAL_CLOCK, &ts);
    return to_ns(&ts);
}

static uint64_t resolution_ns(void) {
    struct timespec ts;
    if (clock_getres(NHAL_CLOCK, &ts) != 0) return 1;
    const uint64_t r = to_ns(&ts);
    return r ? r : 1;
}

static nhal_status init(const nhal_allocator* allocator) {
    (void)allocator;
    struct timespec ts;
    if (clock_gettime(NHAL_CLOCK, &ts) != 0) {
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "clock_gettime liefert keine monotone Uhr");
    }
    return nhal_status_ok();
}

static const nhal_time_monotonic_v1 table = {
    sizeof(nhal_time_monotonic_v1),
    NHAL_TIME_SUSPEND_PAUSES,
    now_ns,
    resolution_ns,
};

const nhal_feature_impl nhal_impl_time_monotonic = { 1u, init, NULL, &table };
