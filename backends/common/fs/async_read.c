/*
 * fs.async_read – gemeinsamer Teil: Warteschlangen, Lese-Threads, poll().
 *
 * Das eigentliche Lesen eines Bereichs liefert die Plattform über
 * nhal_platform_read_range() (backends/<plattform>/fs/read_range.c).
 * Ohne Threads (Web) wird in poll() gelesen.
 */
#include "common/fs/read_range.h"
#include "common/threads.h"
#include "nhal/features/fs/async_read.h"
#include "nhal/nhal_backend.h"

#define READ_THREADS 2u

typedef struct request {
    struct request* next;
    char* path;
    size_t path_bytes;
    uint64_t offset;
    size_t size;
    void* dst;
    nhal_read_done_fn done;
    void* user;
    nhal_status status;
    size_t bytes;
} request;

typedef struct queue {
    request* head;
    request* tail;
} queue;

static struct {
    const nhal_allocator* alloc;
    nhal_mutex lock;
    queue todo;      /* eingereicht, noch nicht gelesen */
    queue finished;  /* gelesen, noch nicht ausgeliefert */
    size_t pending;  /* eingereicht, noch nicht ausgeliefert */
#if !defined(NHAL_THREADS_INLINE)
    nhal_cond work;
    nhal_cond ready;
    bool stop;
    nhal_thread threads[READ_THREADS];
    unsigned thread_count;
#endif
} S;

static void push(queue* q, request* r) {
    r->next = NULL;
    if (q->tail) {
        q->tail->next = r;
    } else {
        q->head = r;
    }
    q->tail = r;
}

static request* pop(queue* q) {
    request* r = q->head;
    if (r) {
        q->head = r->next;
        if (!q->head) q->tail = NULL;
    }
    return r;
}

static void release(request* r) {
    S.alloc->free(S.alloc->user, r->path, r->path_bytes);
    S.alloc->free(S.alloc->user, r, sizeof(request));
}

static void perform(request* r) {
    r->status = nhal_platform_read_range(S.alloc, r->path, r->offset, r->dst, r->size, &r->bytes);
}

#if !defined(NHAL_THREADS_INLINE)
NHAL_THREAD_FN(reader_main, arg) {
    (void)arg;
    nhal_mutex_lock(&S.lock);
    for (;;) {
        request* r = pop(&S.todo);
        if (r) {
            nhal_mutex_unlock(&S.lock);
            perform(r);
            nhal_mutex_lock(&S.lock);
            push(&S.finished, r);
            nhal_cond_broadcast(&S.ready);
            continue;
        }
        if (S.stop) break;
        nhal_cond_wait(&S.work, &S.lock);
    }
    nhal_mutex_unlock(&S.lock);
    NHAL_THREAD_RETURN;
}
#endif

static nhal_status ar_submit(const char* path, uint64_t offset, size_t size, void* dst,
                             nhal_read_done_fn done, void* user) {
    if (!path || !*path) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist leer");
    if (!done) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "done ist NULL");
    if (size > 0 && !dst) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "dst ist NULL");

    size_t len = 0;
    while (path[len]) ++len;
    request* r = (request*)S.alloc->alloc(S.alloc->user, sizeof(request), _Alignof(request));
    char* copy = (char*)S.alloc->alloc(S.alloc->user, len + 1, 1);
    if (!r || !copy) {
        if (r) S.alloc->free(S.alloc->user, r, sizeof(request));
        if (copy) S.alloc->free(S.alloc->user, copy, len + 1);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Leseauftrag");
    }
    for (size_t i = 0; i <= len; ++i) copy[i] = path[i];
    r->path = copy;
    r->path_bytes = len + 1;
    r->offset = offset;
    r->size = size;
    r->dst = dst;
    r->done = done;
    r->user = user;
    r->status = nhal_status_ok();
    r->bytes = 0;

    nhal_mutex_lock(&S.lock);
    push(&S.todo, r);
    ++S.pending;
#if !defined(NHAL_THREADS_INLINE)
    nhal_cond_signal(&S.work);
#endif
    nhal_mutex_unlock(&S.lock);
    return nhal_status_ok();
}

static size_t ar_poll(size_t max) {
    size_t delivered = 0;
    while (delivered < max) {
        nhal_mutex_lock(&S.lock);
        request* r = pop(&S.finished);
#if defined(NHAL_THREADS_INLINE)
        if (!r) {
            r = pop(&S.todo);
            if (r) perform(r); /* emuliert: hier lesen */
        }
#endif
        if (r) --S.pending;
        nhal_mutex_unlock(&S.lock);
        if (!r) break;
        r->done(r->user, r->status, r->bytes);
        release(r);
        ++delivered;
    }
    return delivered;
}

static size_t ar_pending(void) {
    nhal_mutex_lock(&S.lock);
    const size_t n = S.pending;
    nhal_mutex_unlock(&S.lock);
    return n;
}

static void ar_drain(void) {
    for (;;) {
        ar_poll((size_t)-1);
        nhal_mutex_lock(&S.lock);
        if (S.pending == 0) {
            nhal_mutex_unlock(&S.lock);
            return;
        }
#if !defined(NHAL_THREADS_INLINE)
        while (!S.finished.head) nhal_cond_wait(&S.ready, &S.lock);
#endif
        nhal_mutex_unlock(&S.lock);
    }
}

static void stop_threads(void) {
#if !defined(NHAL_THREADS_INLINE)
    nhal_mutex_lock(&S.lock);
    S.stop = true;
    nhal_cond_broadcast(&S.work);
    nhal_mutex_unlock(&S.lock);
    for (unsigned i = 0; i < S.thread_count; ++i) nhal_thread_join(S.threads[i]);
    S.thread_count = 0;
#endif
}

static nhal_status init(const nhal_allocator* allocator) {
    S.alloc = allocator;
    S.todo.head = S.todo.tail = NULL;
    S.finished.head = S.finished.tail = NULL;
    S.pending = 0;
    nhal_mutex_init(&S.lock);
#if !defined(NHAL_THREADS_INLINE)
    nhal_cond_init(&S.work);
    nhal_cond_init(&S.ready);
    S.stop = false;
    S.thread_count = 0;
    for (unsigned i = 0; i < READ_THREADS; ++i) {
        if (nhal_thread_start(&S.threads[i], reader_main, NULL) != 0) {
            stop_threads();
            nhal_cond_destroy(&S.ready);
            nhal_cond_destroy(&S.work);
            nhal_mutex_destroy(&S.lock);
            return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Lese-Thread konnte nicht starten");
        }
        S.thread_count = i + 1;
    }
#endif
    return nhal_status_ok();
}

static void shutdown(void) {
    stop_threads(); /* Lese-Threads arbeiten die Warteschlange noch ab */
    request* r;
    while ((r = pop(&S.todo)) != NULL) release(r);
    while ((r = pop(&S.finished)) != NULL) release(r);
    S.pending = 0;
#if !defined(NHAL_THREADS_INLINE)
    nhal_cond_destroy(&S.ready);
    nhal_cond_destroy(&S.work);
#endif
    nhal_mutex_destroy(&S.lock);
}

static const nhal_fs_async_read_v1 table = {
    sizeof(nhal_fs_async_read_v1), ar_submit, ar_poll, ar_drain, ar_pending,
};

const nhal_feature_impl nhal_impl_fs_async_read = { 1u, init, shutdown, &table };
