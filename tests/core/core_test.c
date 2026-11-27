/*
 * Tests für den NHAL-Kern: Start, Manifest, Fähigkeiten, Allocator.
 */
#include "nhal/nhal_core.h"
#if NHAL_HAS_TIME_MONOTONIC
#include "nhal/features/time/monotonic.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK(%s) fehlgeschlagen\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static nhal_status init_with(const nhal_request* requests, size_t count, nhal_context** ctx) {
    const nhal_manifest manifest = { requests, count };
    const nhal_init_info info = { sizeof(nhal_init_info), &manifest, NULL };
    return nhal_init(&info, ctx);
}

static void test_default_init(void) {
    nhal_context* ctx = NULL;
    const nhal_status s = nhal_init(NULL, &ctx);
    CHECK(s.code == NHAL_OK);
    CHECK(ctx != NULL);
#if NHAL_HAS_TIME_MONOTONIC
    const nhal_time_monotonic_v1* t = nhal_get_time_monotonic_v1(ctx);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->struct_size == sizeof(nhal_time_monotonic_v1));
        const uint64_t a = t->now_ns();
        const uint64_t b = t->now_ns();
        CHECK(b >= a);
        CHECK(t->resolution_ns() >= 1);
    }
#endif
    nhal_shutdown(ctx);
}

/* Ein Feature, das in diesem Build fehlt: zuerst eines aus dem Katalog
 * (dann gibt es einen Grund aus dem Katalog), sonst eine unbekannte ID. */
static nhal_feature_id missing_feature(const char** name) {
#if !NHAL_HAS_GFX_MESH_SHADER
    *name = "gfx.mesh_shader";
    return NHAL_FEATURE_GFX_MESH_SHADER;
#elif !NHAL_HAS_NET_SOCKET_UDP
    *name = "net.socket_udp";
    return NHAL_FEATURE_NET_SOCKET_UDP;
#elif !NHAL_HAS_GFX_DEVICE
    *name = "gfx.device";
    return NHAL_FEATURE_GFX_DEVICE;
#elif !NHAL_HAS_AUDIO_OUTPUT
    *name = "audio.output";
    return NHAL_FEATURE_AUDIO_OUTPUT;
#else
    *name = "unbekannt";
    return (nhal_feature_id)0x7e57u; /* keine FNV-1a-ID eines Katalogeintrags */
#endif
}

static void test_required_missing_fails(void) {
    const char* name = NULL;
    const nhal_feature_id missing = missing_feature(&name);
    const nhal_request req = { missing, 1u, NHAL_REQUIRED };
    nhal_context* ctx = (nhal_context*)&req; /* muss auf NULL gesetzt werden */
    const nhal_status s = init_with(&req, 1, &ctx);
    CHECK(s.code == NHAL_ERR_UNSUPPORTED);
    CHECK(s.feature == missing);
    CHECK(s.reason != NULL && s.reason[0] != '\0');
    CHECK(ctx == NULL);
}

static void test_optional_missing_is_reported(void) {
    const char* name = NULL;
    const nhal_feature_id missing = missing_feature(&name);
    const nhal_request req = { missing, 1u, NHAL_OPTIONAL };
    nhal_context* ctx = NULL;
    CHECK(init_with(&req, 1, &ctx).code == NHAL_OK);
    CHECK(nhal_get(ctx, missing, 1u) == NULL);
    nhal_feature_info info;
    CHECK(!nhal_query(ctx, missing, &info));
    CHECK(info.support == NHAL_SUPPORT_NONE);
    CHECK(info.status.code == NHAL_ERR_UNSUPPORTED);
    CHECK(strcmp(info.name, name) == 0);
    nhal_shutdown(ctx);
}

static void test_only_requested_features_start(void) {
    nhal_context* ctx = NULL;
    CHECK(init_with(NULL, 0, &ctx).code == NHAL_OK);
    for (size_t i = 0; i < nhal_feature_count(); ++i) {
        nhal_feature_info info;
        CHECK(!nhal_feature_at(ctx, i, &info));
        CHECK(info.status.code == NHAL_ERR_UNAVAILABLE);
    }
#if NHAL_HAS_TIME_MONOTONIC
    CHECK(nhal_get_time_monotonic_v1(ctx) == NULL);
#endif
    nhal_shutdown(ctx);
}

static void test_wrong_version_rejected(void) {
#if NHAL_HAS_TIME_MONOTONIC
    const nhal_request req = { NHAL_FEATURE_TIME_MONOTONIC, 99u, NHAL_REQUIRED };
    nhal_context* ctx = NULL;
    const nhal_status s = init_with(&req, 1, &ctx);
    CHECK(s.code == NHAL_ERR_UNSUPPORTED);
    CHECK(s.feature == NHAL_FEATURE_TIME_MONOTONIC);
    CHECK(ctx == NULL);
#endif
}

static void test_single_context(void) {
    nhal_context* a = NULL;
    nhal_context* b = NULL;
    CHECK(nhal_init(NULL, &a).code == NHAL_OK);
    CHECK(nhal_init(NULL, &b).code == NHAL_ERR_BUSY);
    CHECK(b == NULL);
    nhal_shutdown(a);
    CHECK(nhal_init(NULL, &b).code == NHAL_OK);
    nhal_shutdown(b);
}

/*
 * Eigener Allocator. Die Arena-Größe kommt aus der Registry: Ein Messlauf
 * startet alle Features des Builds über einen malloc-Allocator und summiert,
 * was Kern und Features anfordern (je Anforderung size + align - 1, der
 * schlechteste Fall für die Arena). Danach läuft derselbe Start in einer
 * Arena genau dieser Größe. Eine feste Größe ließ Features mit großen
 * Tabellen (thread.scheduler) still an OUT_OF_MEMORY scheitern, weil ohne
 * Manifest alles optional ist.
 */
typedef struct counting {
    int allocs;
    int frees;
    size_t need; /* Messlauf: Arena-Bedarf */
    unsigned char* buffer;
    size_t capacity;
    size_t used;
} counting;

static void* measure_alloc(void* user, size_t size, size_t align) {
    counting* c = (counting*)user;
    c->need += size + align - 1;
    const size_t a = align < sizeof(void*) ? sizeof(void*) : align;
    unsigned char* raw = (unsigned char*)malloc(size + a + sizeof(void*));
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)(raw + sizeof(void*)) + a - 1) & ~(uintptr_t)(a - 1);
    ((void**)p)[-1] = raw;
    ++c->allocs;
    return (void*)p;
}

static void measure_free(void* user, void* ptr, size_t size) {
    (void)size;
    if (ptr) free(((void**)ptr)[-1]);
    ++((counting*)user)->frees;
}

static void* arena_alloc(void* user, size_t size, size_t align) {
    counting* c = (counting*)user;
    /* Die Adresse ausrichten, nicht nur den Offset: buffer selbst ist beliebig ausgerichtet. */
    const uintptr_t base = (uintptr_t)c->buffer;
    const uintptr_t start = ((base + c->used) + align - 1) & ~(uintptr_t)(align - 1);
    const size_t offset = (size_t)(start - base);
    if (offset > c->capacity || size > c->capacity - offset) return NULL;
    c->used = offset + size;
    ++c->allocs;
    return (void*)start;
}

static void arena_free(void* user, void* ptr, size_t size) {
    (void)ptr;
    (void)size;
    ++((counting*)user)->frees;
}

/* Startet alle Features des Builds mit dem Allocator; merkt sich, welche aktiv sind. */
static void start_all(const nhal_allocator* alloc, bool* active, bool* out_of_memory) {
    const nhal_init_info info = { sizeof(nhal_init_info), NULL, alloc };
    nhal_context* ctx = NULL;
    CHECK(nhal_init(&info, &ctx).code == NHAL_OK);
    *out_of_memory = false;
    for (size_t i = 0; i < nhal_feature_count(); ++i) {
        nhal_feature_info fi;
        active[i] = nhal_feature_at(ctx, i, &fi);
        if (fi.status.code == NHAL_ERR_OUT_OF_MEMORY) {
            fprintf(stderr, "core_test: %s scheitert am Test-Allocator: %s\n", fi.name, fi.status.reason);
            *out_of_memory = true;
        }
    }
    nhal_shutdown(ctx);
}

static void test_custom_allocator(void) {
    const size_t n = nhal_feature_count();
    bool* measured = (bool*)calloc(n ? n : 1, sizeof(bool));
    bool* in_arena = (bool*)calloc(n ? n : 1, sizeof(bool));
    CHECK(measured && in_arena);
    if (!measured || !in_arena) goto done;

    counting m;
    memset(&m, 0, sizeof m);
    const nhal_allocator ma = { measure_alloc, measure_free, &m };
    bool oom = false;
    start_all(&ma, measured, &oom);
    CHECK(!oom);
    CHECK(m.allocs >= 1);
    CHECK(m.allocs == m.frees);

    counting c;
    memset(&c, 0, sizeof c);
    c.capacity = m.need;
    c.buffer = (unsigned char*)malloc(c.capacity ? c.capacity : 1);
    CHECK(c.buffer != NULL);
    if (!c.buffer) goto done;
    const nhal_allocator aa = { arena_alloc, arena_free, &c };
    start_all(&aa, in_arena, &oom);
    CHECK(!oom);
    CHECK(c.allocs == m.allocs);
    CHECK(c.allocs == c.frees);
    CHECK(c.used <= c.capacity);
    for (size_t i = 0; i < n; ++i) CHECK(in_arena[i] == measured[i]);
    free(c.buffer);

done:
    free(measured);
    free(in_arena);
}

static void test_invalid_arguments(void) {
    CHECK(nhal_init(NULL, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    const nhal_init_info small = { 4u, NULL, NULL };
    nhal_context* ctx = NULL;
    CHECK(nhal_init(&small, &ctx).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(nhal_get(NULL, NHAL_FEATURE_TIME_MONOTONIC, 1u) == NULL);
}

static void test_names(void) {
    CHECK(strcmp(nhal_feature_name(NHAL_FEATURE_TIME_MONOTONIC), "time.monotonic") == 0);
    CHECK(strcmp(nhal_feature_name(0xdeadbeefu), "unbekannt") == 0);
    CHECK(strcmp(nhal_platform_name(), NHAL_PLATFORM_NAME) == 0);
    CHECK(strcmp(nhal_code_name(NHAL_ERR_UNSUPPORTED), "unsupported") == 0);
}

int main(void) {
    test_default_init();
    test_required_missing_fails();
    test_optional_missing_is_reported();
    test_only_requested_features_start();
    test_wrong_version_rejected();
    test_single_context();
    test_custom_allocator();
    test_invalid_arguments();
    test_names();
    if (g_failures) {
        fprintf(stderr, "%d Prüfung(en) fehlgeschlagen\n", g_failures);
        return 1;
    }
    printf("core_test: alles in Ordnung (Plattform %s, %zu Features)\n", nhal_platform_name(),
           nhal_feature_count());
    return 0;
}
