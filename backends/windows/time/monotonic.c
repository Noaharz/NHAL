/*
 * time.monotonic – Windows (QueryPerformanceCounter).
 *
 * Ob der Zähler im Ruhezustand weiterläuft, ist noch nicht verifiziert,
 * deshalb meldet die Tabelle NHAL_TIME_SUSPEND_UNSPECIFIED.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nhal/features/time/monotonic.h"
#include "nhal/nhal_backend.h"

static uint64_t g_frequency = 0; /* Ticks pro Sekunde, ab init fest */

static uint64_t now_ns(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ticks = (uint64_t)c.QuadPart;
    /* In zwei Teilen rechnen, damit ticks * 1e9 nicht überläuft. */
    return (ticks / g_frequency) * 1000000000ull + (ticks % g_frequency) * 1000000000ull / g_frequency;
}

static uint64_t resolution_ns(void) {
    const uint64_t r = 1000000000ull / g_frequency;
    return r ? r : 1;
}

static nhal_status init(const nhal_allocator* allocator) {
    (void)allocator;
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) {
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "QueryPerformanceFrequency liefert keinen Wert");
    }
    g_frequency = (uint64_t)f.QuadPart;
    return nhal_status_ok();
}

static const nhal_time_monotonic_v1 table = {
    sizeof(nhal_time_monotonic_v1),
    NHAL_TIME_SUSPEND_UNSPECIFIED,
    now_ns,
    resolution_ns,
};

const nhal_feature_impl nhal_impl_time_monotonic = { 1u, init, NULL, &table };
