/*
 * time.monotonic – Web (Emscripten, performance.now()).
 *
 * Browser vergröbern performance.now() absichtlich (Schutz vor Timing-
 * Angriffen; je nach Browser und Cross-Origin-Isolation 5 µs bis 1 ms).
 * NHAL meldet deshalb nicht die nominelle, sondern die beim Start gemessene
 * Schrittweite. Ob die Uhr in einem Hintergrund-Tab oder im Ruhezustand
 * weiterläuft, legt der Browser fest: NHAL_TIME_SUSPEND_UNSPECIFIED.
 */
#include "nhal/features/time/monotonic.h"
#include "nhal/nhal_backend.h"

#include <emscripten.h>

static uint64_t g_resolution_ns = 1;

static uint64_t now_ns(void) {
    return (uint64_t)(emscripten_get_now() * 1000000.0); /* ms → ns */
}

static uint64_t resolution_ns(void) { return g_resolution_ns; }

static nhal_status init(const nhal_allocator* allocator) {
    (void)allocator;
    /* Kleinsten Schritt messen: einige Wechsel abwarten, höchstens ~50 ms. */
    uint64_t best = 0;
    uint64_t last = now_ns();
    const uint64_t deadline = last + 50000000ull;
    for (int changes = 0; changes < 8;) {
        const uint64_t t = now_ns();
        if (t != last) {
            const uint64_t step = t - last;
            if (best == 0 || step < best) best = step;
            last = t;
            ++changes;
        }
        if (t > deadline) break;
    }
    if (best == 0) {
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "performance.now() steht still");
    }
    g_resolution_ns = best;
    return nhal_status_ok();
}

static const nhal_time_monotonic_v1 table = {
    sizeof(nhal_time_monotonic_v1),
    NHAL_TIME_SUSPEND_UNSPECIFIED,
    now_ns,
    resolution_ns,
};

const nhal_feature_impl nhal_impl_time_monotonic = { 1u, init, NULL, &table };
