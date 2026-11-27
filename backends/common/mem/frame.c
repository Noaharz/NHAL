/*
 * mem.frame – plattformunabhängig, freestanding (keine libc).
 *
 * Ein einziger Block: [ Kopf | Puffer ... ]; alloc() schiebt einen Zeiger vor.
 */
#include "nhal/features/mem/frame.h"
#include "nhal/nhal_backend.h"

struct nhal_mem_frame {
    unsigned char* base;
    size_t capacity;
    size_t used;
    size_t high_water;
    size_t bytes;
};

static const nhal_allocator* g_alloc = NULL;

static nhal_status frame_create(size_t capacity, nhal_mem_frame** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (capacity == 0) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kapazität muss > 0 sein");
    const size_t align = _Alignof(max_align_t);
    const size_t head = (sizeof(nhal_mem_frame) + align - 1) / align * align;
    if (capacity > SIZE_MAX - head) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kapazität wäre größer als der Adressraum");
    }
    const size_t bytes = head + capacity;
    unsigned char* block = (unsigned char*)g_alloc->alloc(g_alloc->user, bytes, align);
    if (!block) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Frame-Allocator");
    nhal_mem_frame* f = (nhal_mem_frame*)block;
    f->base = block + head;
    f->capacity = capacity;
    f->used = 0;
    f->high_water = 0;
    f->bytes = bytes;
    *out = f;
    return nhal_status_ok();
}

static void frame_destroy(nhal_mem_frame* f) {
    if (f) g_alloc->free(g_alloc->user, f, f->bytes);
}

static void* frame_alloc(nhal_mem_frame* f, size_t size, size_t align) {
    if (align == 0) align = _Alignof(max_align_t);
    if ((align & (align - 1)) != 0) return NULL;
    const uintptr_t base = (uintptr_t)f->base;
    const uintptr_t cur = base + f->used;
    const uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    const size_t start = (size_t)(aligned - base);
    if (start > f->capacity || size > f->capacity - start) return NULL;
    f->used = start + size;
    if (f->used > f->high_water) f->high_water = f->used;
    return f->base + start;
}

static void frame_reset(nhal_mem_frame* f) { f->used = 0; }
static size_t frame_mark(const nhal_mem_frame* f) { return f->used; }

static void frame_rewind(nhal_mem_frame* f, size_t mark) {
    if (mark <= f->used) f->used = mark;
}

static size_t frame_used(const nhal_mem_frame* f) { return f->used; }
static size_t frame_capacity(const nhal_mem_frame* f) { return f->capacity; }
static size_t frame_high_water(const nhal_mem_frame* f) { return f->high_water; }

static nhal_status init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void shutdown(void) { g_alloc = NULL; }

static const nhal_mem_frame_v1 table = {
    sizeof(nhal_mem_frame_v1), frame_create, frame_destroy, frame_alloc, frame_reset,
    frame_mark, frame_rewind, frame_used, frame_capacity, frame_high_water,
};

const nhal_feature_impl nhal_impl_mem_frame = { 1u, init, shutdown, &table };
