/*
 * time.monotonic, Version 1: monotone Uhr.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - now_ns() fällt nie. Der Startpunkt ist beliebig, sinnvoll sind nur
 *     Differenzen zwischen zwei Werten.
 *   - Thread-sicher, ohne Allokation, im Hot Path nutzbar.
 *   - resolution_ns() ist die kleinste Schrittweite, die die Uhr liefern
 *     kann (>= 1).
 *
 * Plattformabhängig (in der Tabelle beschrieben, nicht versteckt):
 *   - suspend: ob die Uhr im Ruhezustand des Geräts weiterläuft.
 */
#ifndef NHAL_FEATURES_TIME_MONOTONIC_H
#define NHAL_FEATURES_TIME_MONOTONIC_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nhal_time_suspend {
    NHAL_TIME_SUSPEND_UNSPECIFIED = 0, /* auf dieser Plattform noch nicht geprüft */
    NHAL_TIME_SUSPEND_PAUSES = 1,      /* Ruhezustand zählt nicht mit */
    NHAL_TIME_SUSPEND_CONTINUES = 2    /* Ruhezustand zählt mit */
} nhal_time_suspend;

typedef struct nhal_time_monotonic_v1 {
    uint32_t struct_size;
    nhal_time_suspend suspend;
    uint64_t (*now_ns)(void);
    uint64_t (*resolution_ns)(void);
} nhal_time_monotonic_v1;

static inline const nhal_time_monotonic_v1* nhal_get_time_monotonic_v1(const nhal_context* ctx) {
    return (const nhal_time_monotonic_v1*)nhal_get(ctx, NHAL_FEATURE_TIME_MONOTONIC, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_TIME_MONOTONIC_H */
