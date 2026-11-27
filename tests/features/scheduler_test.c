/* thread.scheduler: Abhängigkeiten, Prioritäten, Warten aus Aufgaben, Last. */
#include "nhal/features/thread/scheduler.h"
#include "check.h"

#include <stdatomic.h>

static const nhal_thread_scheduler_v1* S;

typedef struct order {
    atomic_int next;
    int seen[8];
} order;

typedef struct step {
    order* o;
    int id;
} step;

static void record(void* user) {
    step* s = (step*)user;
    s->o->seen[s->id] = atomic_fetch_add(&s->o->next, 1);
}

static atomic_int g_counter;
static void count(void* user) {
    (void)user;
    atomic_fetch_add(&g_counter, 1);
}

typedef struct nested {
    nhal_task child;
    int child_ran;
} nested;

static void child_fn(void* user) { ((nested*)user)->child_ran = 1; }

static void parent_fn(void* user) {
    nested* n = (nested*)user;
    S->submit(child_fn, n, NHAL_TASK_NORMAL, NULL, 0, &n->child);
    S->wait(n->child); /* Warten innerhalb einer Aufgabe */
}

int main(void) {
    const nhal_request req = { NHAL_FEATURE_THREAD_SCHEDULER, 1u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    S = nhal_get_thread_scheduler_v1(ctx);
    CHECK(S != NULL);
    if (!S) return check_report("scheduler_test");

    /* Kette a -> b -> c und Raute: d hängt von b und c ab. */
    order o;
    atomic_init(&o.next, 0);
    step st[4] = { { &o, 0 }, { &o, 1 }, { &o, 2 }, { &o, 3 } };
    nhal_task a, b, c, d;
    CHECK_OK(S->submit(record, &st[0], NHAL_TASK_LOW, NULL, 0, &a));
    CHECK_OK(S->submit(record, &st[1], NHAL_TASK_HIGH, &a, 1, &b));
    CHECK_OK(S->submit(record, &st[2], NHAL_TASK_NORMAL, &b, 1, &c));
    const nhal_task bc[2] = { b, c };
    CHECK_OK(S->submit(record, &st[3], NHAL_TASK_HIGH, bc, 2, &d));
    S->wait(d);
    CHECK(S->is_done(a) && S->is_done(b) && S->is_done(c) && S->is_done(d));
    CHECK(o.seen[0] < o.seen[1] && o.seen[1] < o.seen[2] && o.seen[2] < o.seen[3]);

    /* Fertige Aufgabe als Abhängigkeit zählt als erfüllt. */
    nhal_task e;
    CHECK_OK(S->submit(count, NULL, NHAL_TASK_NORMAL, &a, 1, &e));
    S->wait(e);

    /* Warten in einer Aufgabe. */
    nested n = { { 0, 0 }, 0 };
    nhal_task parent;
    CHECK_OK(S->submit(parent_fn, &n, NHAL_TASK_NORMAL, NULL, 0, &parent));
    S->wait(parent);
    CHECK(n.child_ran == 1);

    /* Last: viele Aufgaben, jede zehnte hängt von der vorigen ab. */
    atomic_init(&g_counter, 0);
    nhal_task prev = { 0, 0 };
    for (int i = 0; i < 20000; ++i) {
        nhal_task t;
        nhal_status s = S->submit(count, NULL, (nhal_task_priority)(i % 3), i % 10 ? NULL : &prev,
                                  i % 10 ? 0 : 1, &t);
        if (s.code == NHAL_ERR_BUSY) { /* Tabelle voll: abarbeiten, dann weiter */
            S->wait_all();
            s = S->submit(count, NULL, NHAL_TASK_NORMAL, NULL, 0, &t);
        }
        CHECK_OK(s);
        prev = t;
    }
    S->wait_all();
    CHECK(atomic_load(&g_counter) == 20000);

    nhal_task invalid = { 0, 0 };
    CHECK(S->is_done(invalid));
    CHECK(S->submit(NULL, NULL, NHAL_TASK_NORMAL, NULL, 0, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(S->submit(count, NULL, (nhal_task_priority)7, NULL, 0, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(S->capacity() > 0);
    printf("scheduler_test: %u Worker\n", S->worker_count());

    /* Ausstehende Aufgaben laufen beim Herunterfahren noch. */
    atomic_init(&g_counter, 0);
    for (int i = 0; i < 100; ++i) S->submit(count, NULL, NHAL_TASK_LOW, NULL, 0, NULL);
    nhal_shutdown(ctx);
    CHECK(atomic_load(&g_counter) == 100);
    return check_report("scheduler_test");
}
