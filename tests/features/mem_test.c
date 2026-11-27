/* mem.pool und mem.frame */
#include "nhal/features/mem/frame.h"
#include "nhal/features/mem/pool.h"
#include "check.h"

#include <stdint.h>

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_MEM_POOL, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_MEM_FRAME, 1u, NHAL_REQUIRED },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    const nhal_mem_pool_v1* pool = nhal_get_mem_pool_v1(ctx);
    const nhal_mem_frame_v1* frame = nhal_get_mem_frame_v1(ctx);
    CHECK(pool && frame);
    if (!pool || !frame) return check_report("mem_test");

    /* ── Pool ── */
    nhal_mem_pool* p = NULL;
    CHECK_OK(pool->create(24, 64, 8, &p));
    void* objs[8];
    for (int i = 0; i < 8; ++i) {
        objs[i] = pool->alloc(p);
        CHECK(objs[i] != NULL);
        CHECK(((uintptr_t)objs[i] % 64) == 0);
        CHECK(pool->contains(p, objs[i]));
    }
    CHECK(pool->alloc(p) == NULL); /* voll: kein Wachsen */
    CHECK(pool->in_use(p) == 8);
    int local = 0;
    CHECK(!pool->free(p, &local)); /* fremder Zeiger */
    CHECK(!pool->free(p, (char*)objs[0] + 1));
    CHECK(pool->free(p, objs[3]));
    CHECK(pool->alloc(p) == objs[3]); /* wiederverwendet */
    for (int i = 0; i < 8; ++i) CHECK(pool->free(p, objs[i]));
    CHECK(pool->in_use(p) == 0);
    CHECK(pool->capacity(p) == 8);
    pool->destroy(p);

    nhal_mem_pool* bad = NULL;
    CHECK(pool->create(16, 3, 4, &bad).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(pool->create(0, 8, 4, &bad).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(pool->create(16, 8, SIZE_MAX / 2, &bad).code != NHAL_OK);
    CHECK(bad == NULL);

    /* ── Frame ── */
    nhal_mem_frame* f = NULL;
    CHECK_OK(frame->create(1024, &f));
    void* a = frame->alloc(f, 10, 1);
    void* b = frame->alloc(f, 8, 64);
    CHECK(a && b);
    CHECK(((uintptr_t)b % 64) == 0);
    const size_t mark = frame->mark(f);
    CHECK(frame->alloc(f, 100, 8) != NULL);
    frame->rewind(f, mark);
    CHECK(frame->used(f) == mark);
    CHECK(frame->alloc(f, 2000, 8) == NULL); /* zu groß */
    CHECK(frame->alloc(f, 4, 3) == NULL);    /* keine Zweierpotenz */
    const size_t high = frame->high_water(f);
    frame->reset(f);
    CHECK(frame->used(f) == 0);
    CHECK(frame->high_water(f) == high);
    CHECK(frame->alloc(f, 1024, 1) != NULL); /* exakt voll */
    CHECK(frame->alloc(f, 1, 1) == NULL);
    CHECK(frame->capacity(f) == 1024);
    frame->destroy(f);

    nhal_shutdown(ctx);
    return check_report("mem_test");
}
