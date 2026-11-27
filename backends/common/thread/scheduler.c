/*
 * thread.scheduler – POSIX und Windows (Worker-Threads), Web ohne pthreads
 * emuliert (Aufgaben laufen sofort im aufrufenden Thread).
 *
 * Aufbau: feste Tabelle von Task-Plätzen und Abhängigkeits-Kanten, eine
 * Sperre für alles. Jede Aufgabe kennt die Liste der Aufgaben, die auf sie
 * warten. Wird sie fertig, zählt sie bei denen herunter und reiht die bereit
 * gewordenen ein.
 */
#include "nhal/features/thread/scheduler.h"
#include "nhal/nhal_backend.h"

#include "common/threads.h"

#define TASK_CAPACITY 4096u
#define EDGE_CAPACITY (TASK_CAPACITY * 4u)
#define MAX_WORKERS 64u
#define NONE UINT32_MAX

#if defined(NHAL_THREADS_INLINE)

/* ── Emuliert: keine Threads ─────────────────────────────────────────────── */

static nhal_status sched_submit(nhal_task_fn fn, void* user, nhal_task_priority priority,
                                const nhal_task* deps, uint32_t dep_count, nhal_task* out) {
    /* Gleiche Prüfungen wie mit Threads; alle früheren Aufgaben sind schon fertig. */
    if (!fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "fn ist NULL");
    if (dep_count && !deps) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "deps ist NULL");
    if ((unsigned)priority > NHAL_TASK_LOW) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Unbekannte Priorität");
    }
    fn(user);
    if (out) {
        out->index = 0;
        out->generation = 0;
    }
    return nhal_status_ok();
}

static bool sched_is_done(nhal_task task) {
    (void)task;
    return true;
}
static void sched_wait(nhal_task task) { (void)task; }
static void sched_wait_all(void) {}
static uint32_t sched_worker_count(void) { return 0; }
static uint32_t sched_capacity(void) { return TASK_CAPACITY; }
static nhal_status init(const nhal_allocator* allocator) {
    (void)allocator;
    return nhal_status_ok();
}
static void shutdown(void) {}

#else

/* ── Mit Worker-Threads ─────────────────────────────────────────────────── */

enum { SLOT_FREE, SLOT_WAITING, SLOT_READY, SLOT_RUNNING };

typedef struct slot {
    nhal_task_fn fn;
    void* user;
    uint32_t generation;
    uint32_t pending;         /* offene Abhängigkeiten */
    uint32_t first_dependent; /* Kante */
    uint32_t next;            /* Freiliste oder Warteschlange */
    uint8_t priority;
    uint8_t state;
} slot;

typedef struct edge {
    uint32_t task;
    uint32_t next;
} edge;

static struct {
    const nhal_allocator* alloc;
    nhal_mutex lock;
    nhal_cond work; /* Worker warten auf Arbeit */
    nhal_cond done; /* Wartende warten auf Abschlüsse */
    slot* slots;
    edge* edges;
    uint32_t free_slot;
    uint32_t free_edge;
    uint32_t free_edges;
    uint32_t ready_head[3];
    uint32_t ready_tail[3];
    uint32_t outstanding;
    bool stop;
    nhal_thread threads[MAX_WORKERS];
    uint32_t worker_count;
} S;

static void push_ready(uint32_t i) {
    const uint8_t p = S.slots[i].priority;
    S.slots[i].state = SLOT_READY;
    S.slots[i].next = NONE;
    if (S.ready_tail[p] == NONE) {
        S.ready_head[p] = i;
    } else {
        S.slots[S.ready_tail[p]].next = i;
    }
    S.ready_tail[p] = i;
}

static uint32_t pop_ready(void) {
    for (int p = 0; p < 3; ++p) {
        const uint32_t i = S.ready_head[p];
        if (i == NONE) continue;
        S.ready_head[p] = S.slots[i].next;
        if (S.ready_head[p] == NONE) S.ready_tail[p] = NONE;
        return i;
    }
    return NONE;
}

static bool done_locked(nhal_task t) {
    return t.generation == 0 || t.index >= TASK_CAPACITY || S.slots[t.index].generation != t.generation ||
           S.slots[t.index].state == SLOT_FREE;
}

/* Bei gehaltener Sperre: eine bereite Aufgabe ausführen. false = keine da. */
static bool run_one_locked(void) {
    const uint32_t i = pop_ready();
    if (i == NONE) return false;
    slot* s = &S.slots[i];
    s->state = SLOT_RUNNING;
    nhal_task_fn fn = s->fn;
    void* user = s->user;
    nhal_mutex_unlock(&S.lock);
    fn(user);
    nhal_mutex_lock(&S.lock);

    bool readied = false;
    for (uint32_t e = s->first_dependent; e != NONE;) {
        const uint32_t next = S.edges[e].next;
        slot* d = &S.slots[S.edges[e].task];
        if (--d->pending == 0) {
            push_ready(S.edges[e].task);
            readied = true;
        }
        S.edges[e].next = S.free_edge;
        S.free_edge = e;
        ++S.free_edges;
        e = next;
    }
    s->first_dependent = NONE;
    s->state = SLOT_FREE;
    s->fn = NULL;
    s->user = NULL;
    if (++s->generation == 0) s->generation = 1;
    s->next = S.free_slot;
    S.free_slot = i;
    --S.outstanding;
    if (readied) nhal_cond_broadcast(&S.work);
    nhal_cond_broadcast(&S.done);
    return true;
}

NHAL_THREAD_FN(worker_main, arg) {
    (void)arg;
    nhal_mutex_lock(&S.lock);
    for (;;) {
        if (run_one_locked()) continue;
        if (S.stop) break;
        nhal_cond_wait(&S.work, &S.lock);
    }
    nhal_mutex_unlock(&S.lock);
    NHAL_THREAD_RETURN;
}

static nhal_status sched_submit(nhal_task_fn fn, void* user, nhal_task_priority priority,
                                const nhal_task* deps, uint32_t dep_count, nhal_task* out) {
    if (!fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "fn ist NULL");
    if (dep_count && !deps) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "deps ist NULL");
    if ((unsigned)priority > NHAL_TASK_LOW) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Unbekannte Priorität");
    }
    nhal_mutex_lock(&S.lock);
    if (S.free_slot == NONE) {
        nhal_mutex_unlock(&S.lock);
        return nhal_status_error(NHAL_ERR_BUSY, "Task-Tabelle ist voll");
    }
    if (dep_count > S.free_edges) {
        nhal_mutex_unlock(&S.lock);
        return nhal_status_error(NHAL_ERR_BUSY, "Zu viele offene Abhängigkeiten");
    }
    const uint32_t i = S.free_slot;
    slot* s = &S.slots[i];
    S.free_slot = s->next;
    s->fn = fn;
    s->user = user;
    s->priority = (uint8_t)priority;
    s->state = SLOT_WAITING;
    s->pending = 0;
    s->first_dependent = NONE;
    for (uint32_t d = 0; d < dep_count; ++d) {
        if (done_locked(deps[d])) continue;
        const uint32_t e = S.free_edge;
        S.free_edge = S.edges[e].next;
        --S.free_edges;
        S.edges[e].task = i;
        S.edges[e].next = S.slots[deps[d].index].first_dependent;
        S.slots[deps[d].index].first_dependent = e;
        ++s->pending;
    }
    ++S.outstanding;
    if (s->pending == 0) {
        push_ready(i);
        nhal_cond_signal(&S.work);
    }
    if (out) {
        out->index = i;
        out->generation = s->generation;
    }
    nhal_mutex_unlock(&S.lock);
    return nhal_status_ok();
}

static bool sched_is_done(nhal_task task) {
    nhal_mutex_lock(&S.lock);
    const bool d = done_locked(task);
    nhal_mutex_unlock(&S.lock);
    return d;
}

static void sched_wait(nhal_task task) {
    nhal_mutex_lock(&S.lock);
    while (!done_locked(task)) {
        if (!run_one_locked()) nhal_cond_wait(&S.done, &S.lock);
    }
    nhal_mutex_unlock(&S.lock);
}

static void sched_wait_all(void) {
    nhal_mutex_lock(&S.lock);
    while (S.outstanding > 0) {
        if (!run_one_locked()) nhal_cond_wait(&S.done, &S.lock);
    }
    nhal_mutex_unlock(&S.lock);
}

static uint32_t sched_worker_count(void) { return S.worker_count; }
static uint32_t sched_capacity(void) { return TASK_CAPACITY; }

static void free_tables(void) {
    if (S.slots) S.alloc->free(S.alloc->user, S.slots, sizeof(slot) * TASK_CAPACITY);
    if (S.edges) S.alloc->free(S.alloc->user, S.edges, sizeof(edge) * EDGE_CAPACITY);
    S.slots = NULL;
    S.edges = NULL;
}

static void stop_workers(void) {
    nhal_mutex_lock(&S.lock);
    S.stop = true;
    nhal_cond_broadcast(&S.work);
    nhal_mutex_unlock(&S.lock);
    for (uint32_t w = 0; w < S.worker_count; ++w) nhal_thread_join(S.threads[w]);
    S.worker_count = 0;
}

static nhal_status init(const nhal_allocator* allocator) {
    S.alloc = allocator;
    S.slots = (slot*)allocator->alloc(allocator->user, sizeof(slot) * TASK_CAPACITY, _Alignof(slot));
    S.edges = (edge*)allocator->alloc(allocator->user, sizeof(edge) * EDGE_CAPACITY, _Alignof(edge));
    if (!S.slots || !S.edges) {
        free_tables();
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Task-Tabelle");
    }
    for (uint32_t i = 0; i < TASK_CAPACITY; ++i) {
        S.slots[i].fn = NULL;
        S.slots[i].user = NULL;
        S.slots[i].generation = 1;
        S.slots[i].pending = 0;
        S.slots[i].first_dependent = NONE;
        S.slots[i].state = SLOT_FREE;
        S.slots[i].priority = 0;
        S.slots[i].next = i + 1 < TASK_CAPACITY ? i + 1 : NONE;
    }
    for (uint32_t e = 0; e < EDGE_CAPACITY; ++e) S.edges[e].next = e + 1 < EDGE_CAPACITY ? e + 1 : NONE;
    S.free_slot = 0;
    S.free_edge = 0;
    S.free_edges = EDGE_CAPACITY;
    for (int p = 0; p < 3; ++p) S.ready_head[p] = S.ready_tail[p] = NONE;
    S.outstanding = 0;
    S.stop = false;
    nhal_mutex_init(&S.lock);
    nhal_cond_init(&S.work);
    nhal_cond_init(&S.done);

    unsigned workers = nhal_cpu_count();
    workers = workers > 1 ? workers - 1 : 1;
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    S.worker_count = 0;
    for (unsigned w = 0; w < workers; ++w) {
        if (nhal_thread_start(&S.threads[w], worker_main, NULL) != 0) {
            stop_workers();
            nhal_cond_destroy(&S.done);
            nhal_cond_destroy(&S.work);
            nhal_mutex_destroy(&S.lock);
            free_tables();
            return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Worker-Thread konnte nicht starten");
        }
        S.worker_count = w + 1;
    }
    return nhal_status_ok();
}

static void shutdown(void) {
    sched_wait_all(); /* ausstehende Aufgaben laufen noch */
    stop_workers();
    nhal_cond_destroy(&S.done);
    nhal_cond_destroy(&S.work);
    nhal_mutex_destroy(&S.lock);
    free_tables();
}

#endif

static const nhal_thread_scheduler_v1 table = {
    sizeof(nhal_thread_scheduler_v1), sched_submit, sched_is_done, sched_wait,
    sched_wait_all, sched_worker_count, sched_capacity,
};

const nhal_feature_impl nhal_impl_thread_scheduler = { 1u, init, shutdown, &table };
