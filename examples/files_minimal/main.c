/*
 * files_minimal: liest alle Dateien eines Ordners asynchron und berechnet
 * ihre Prüfsummen parallel.
 *
 *   fs.file          Ordner auflisten
 *   mem.frame        ein Puffer für alle Dateien, am Ende auf einmal frei
 *   fs.async_read    alle Dateien gleichzeitig lesen
 *   thread.scheduler Prüfsummen auf den Worker-Threads
 *   time.monotonic   (optional) Laufzeit messen
 *
 * Aufruf: files_minimal [ordner]
 */
#include "nhal/features/fs/async_read.h"
#include "nhal/features/fs/file.h"
#include "nhal/features/mem/frame.h"
#include "nhal/features/thread/scheduler.h"
#if NHAL_HAS_TIME_MONOTONIC
#include "nhal/features/time/monotonic.h"
#endif

#include <stdio.h>
#include <string.h>

#define MAX_FILES 256
#define MAX_TOTAL (64u * 1024u * 1024u)

typedef struct job {
    char path[512];
    unsigned char* data;
    size_t size;
    size_t read;
    uint32_t checksum;
    nhal_status status;
} job;

static job g_jobs[MAX_FILES];
static size_t g_count = 0;
static size_t g_total = 0;
static const char* g_dir = ".";

static bool collect(void* user, const nhal_dir_entry* e) {
    (void)user;
    if (e->is_dir || g_count == MAX_FILES || g_total + e->size > MAX_TOTAL) return true;
    job* j = &g_jobs[g_count++];
    snprintf(j->path, sizeof j->path, "%s/%s", g_dir, e->name);
    j->size = (size_t)e->size;
    g_total += j->size;
    return true;
}

static void checksum_task(void* user) { /* FNV-1a, läuft auf einem Worker */
    job* j = (job*)user;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < j->read; ++i) h = (h ^ j->data[i]) * 0x01000193u;
    j->checksum = h;
}

typedef struct read_ctx {
    job* j;
    const nhal_thread_scheduler_v1* sched;
} read_ctx;

static read_ctx g_reads[MAX_FILES];

static void on_read(void* user, nhal_status status, size_t bytes) { /* läuft in poll() */
    read_ctx* r = (read_ctx*)user;
    r->j->status = status;
    r->j->read = bytes;
    if (nhal_ok(status)) r->sched->submit(checksum_task, r->j, NHAL_TASK_NORMAL, NULL, 0, NULL);
}

int main(int argc, char** argv) {
    if (argc > 1) g_dir = argv[1];

    nhal_context* ctx = NULL;
    nhal_status s = nhal_init(NULL, &ctx);
    if (!nhal_ok(s)) {
        fprintf(stderr, "NHAL: %s – %s\n", nhal_feature_name(s.feature), s.reason);
        return 1;
    }
    const nhal_fs_file_v1* fs = nhal_get_fs_file_v1(ctx);
    const nhal_fs_async_read_v1* ar = nhal_get_fs_async_read_v1(ctx);
    const nhal_mem_frame_v1* mem = nhal_get_mem_frame_v1(ctx);
    const nhal_thread_scheduler_v1* sched = nhal_get_thread_scheduler_v1(ctx);
#if NHAL_HAS_TIME_MONOTONIC
    const nhal_time_monotonic_v1* clock = nhal_get_time_monotonic_v1(ctx);
    const uint64_t t0 = clock ? clock->now_ns() : 0;
#endif

    s = fs->list_dir(g_dir, collect, NULL);
    if (!nhal_ok(s)) {
        fprintf(stderr, "%s: %s\n", g_dir, s.reason);
        nhal_shutdown(ctx);
        return 1;
    }

    nhal_mem_frame* buffer = NULL;
    s = mem->create(g_total + g_count * 16 + 16, &buffer);
    if (!nhal_ok(s)) {
        fprintf(stderr, "Speicher: %s\n", s.reason);
        nhal_shutdown(ctx);
        return 1;
    }
    for (size_t i = 0; i < g_count; ++i) {
        job* j = &g_jobs[i];
        j->data = (unsigned char*)mem->alloc(buffer, j->size ? j->size : 1, 16);
        g_reads[i].j = j;
        g_reads[i].sched = sched;
        ar->submit(j->path, 0, j->size, j->data, on_read, &g_reads[i]);
    }
    ar->drain();      /* Rückrufe laufen hier, im Hauptthread */
    sched->wait_all(); /* Prüfsummen fertig */

    for (size_t i = 0; i < g_count; ++i) {
        const job* j = &g_jobs[i];
        if (nhal_ok(j->status)) {
            printf("%08x  %10zu  %s\n", (unsigned)j->checksum, j->read, j->path);
        } else {
            printf("--------  %10s  %s (%s)\n", "Fehler", j->path, j->status.reason);
        }
    }
    printf("%zu Dateien, %zu Bytes, %u Worker", g_count, g_total, sched->worker_count());
#if NHAL_HAS_TIME_MONOTONIC
    if (clock) printf(", %.2f ms", (double)(clock->now_ns() - t0) / 1e6);
#endif
    printf("\n");

    mem->destroy(buffer);
    nhal_shutdown(ctx);
    return 0;
}
