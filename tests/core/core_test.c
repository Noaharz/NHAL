/*
 * Tests für den NHAL-Kern: Start, Manifest, Fähigkeiten, Allocator.
 */
#include "nhal/nhal_core.h"
#if NHAL_HAS_TIME_MONOTONIC
#include "nhal/features/time/monotonic.h"
#endif

#include <stdio.h>
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

static void test_required_missing_fails(void) {
    /* gfx.mesh_shader ist ein Entwurf und daher nie im Build. */
    const nhal_request req = { NHAL_FEATURE_GFX_MESH_SHADER, 1u, NHAL_REQUIRED };
    nhal_context* ctx = (nhal_context*)&req; /* muss auf NULL gesetzt werden */
    const nhal_status s = init_with(&req, 1, &ctx);
    CHECK(s.code == NHAL_ERR_UNSUPPORTED);
    CHECK(s.feature == NHAL_FEATURE_GFX_MESH_SHADER);
    CHECK(s.reason != NULL && s.reason[0] != '\0');
    CHECK(ctx == NULL);
}

static void test_optional_missing_is_reported(void) {
    const nhal_request req = { NHAL_FEATURE_GFX_MESH_SHADER, 1u, NHAL_OPTIONAL };
    nhal_context* ctx = NULL;
    CHECK(init_with(&req, 1, &ctx).code == NHAL_OK);
    CHECK(nhal_get(ctx, NHAL_FEATURE_GFX_MESH_SHADER, 1u) == NULL);
    nhal_feature_info info;
    CHECK(!nhal_query(ctx, NHAL_FEATURE_GFX_MESH_SHADER, &info));
    CHECK(info.support == NHAL_SUPPORT_NONE);
    CHECK(info.status.code == NHAL_ERR_UNSUPPORTED);
    CHECK(strcmp(info.name, "gfx.mesh_shader") == 0);
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

typedef struct counting {
    int allocs;
    int frees;
    unsigned char buffer[4096];
    size_t used;
} counting;

static void* counting_alloc(void* user, size_t size, size_t align) {
    counting* c = (counting*)user;
    size_t start = (c->used + align - 1) & ~(align - 1);
    if (start + size > sizeof(c->buffer)) return NULL;
    c->used = start + size;
    ++c->allocs;
    return c->buffer + start;
}

static void counting_free(void* user, void* ptr, size_t size) {
    (void)ptr;
    (void)size;
    ++((counting*)user)->frees;
}

static void test_custom_allocator(void) {
    counting c;
    memset(&c, 0, sizeof c);
    const nhal_allocator alloc = { counting_alloc, counting_free, &c };
    const nhal_init_info info = { sizeof(nhal_init_info), NULL, &alloc };
    nhal_context* ctx = NULL;
    CHECK(nhal_init(&info, &ctx).code == NHAL_OK);
    CHECK(c.allocs >= 1);
    nhal_shutdown(ctx);
    CHECK(c.allocs == c.frees);
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
