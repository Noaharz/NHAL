/*
 * thread.scheduler, Version 1: Task-Scheduler mit Abhängigkeiten und Prioritäten.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - submit() reiht eine Aufgabe ein. Sie startet erst, wenn alle `deps`
 *     fertig sind. Ein Handle einer schon fertigen Aufgabe zählt als erfüllt.
 *   - Bereite Aufgaben laufen nach Priorität (HIGH vor NORMAL vor LOW),
 *     innerhalb einer Priorität in Einreihungs-Reihenfolge.
 *   - wait() und wait_all() blockieren nicht untätig, sondern führen
 *     währenddessen selbst bereite Aufgaben aus. Warten innerhalb einer Aufgabe
 *     ist daher erlaubt.
 *   - Die Task-Tabelle hat eine feste Größe (capacity()). Ist sie voll, liefert
 *     submit() NHAL_ERR_BUSY. Es wird nichts still verworfen.
 *   - Handles sind Werte. Nach Abschluss wird der Platz wiederverwendet, ein
 *     altes Handle meldet dann weiterhin "fertig".
 *   - nhal_shutdown() führt noch ausstehende Aufgaben aus, bevor die Worker enden.
 *   - submit(), is_done(), wait() und wait_all() sind aus jedem Thread aufrufbar.
 *
 * Plattformabhängig:
 *   - worker_count(): Anzahl eigener Worker-Threads (Kerne - 1, mindestens 1).
 *     0 heißt emuliert: Aufgaben laufen sofort im aufrufenden Thread (Web ohne
 *     pthreads). Abhängigkeiten sind dann immer schon erfüllt.
 */
#ifndef NHAL_FEATURES_THREAD_SCHEDULER_H
#define NHAL_FEATURES_THREAD_SCHEDULER_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_task {
    uint32_t index;
    uint32_t generation; /* 0 = ungültiges Handle (zählt als fertig) */
} nhal_task;

typedef enum nhal_task_priority {
    NHAL_TASK_HIGH = 0,
    NHAL_TASK_NORMAL = 1,
    NHAL_TASK_LOW = 2
} nhal_task_priority;

typedef void (*nhal_task_fn)(void* user);

typedef struct nhal_thread_scheduler_v1 {
    uint32_t struct_size;
    nhal_status (*submit)(nhal_task_fn fn, void* user, nhal_task_priority priority,
                          const nhal_task* deps, uint32_t dep_count, nhal_task* out);
    bool (*is_done)(nhal_task task);
    void (*wait)(nhal_task task);
    void (*wait_all)(void);
    uint32_t (*worker_count)(void);
    uint32_t (*capacity)(void);
} nhal_thread_scheduler_v1;

static inline const nhal_thread_scheduler_v1* nhal_get_thread_scheduler_v1(const nhal_context* ctx) {
    return (const nhal_thread_scheduler_v1*)nhal_get(ctx, NHAL_FEATURE_THREAD_SCHEDULER, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_THREAD_SCHEDULER_H */
