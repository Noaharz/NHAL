/*
 * mem.pool – plattformunabhängig, freestanding (keine libc).
 *
 * Ein einziger Block: [ Kopf | Objekte ... ]. Freie Objekte bilden eine
 * einfach verkettete Liste, deren Zeiger im Objekt selbst liegt.
 */
#include "nhal/features/mem/pool.h"
#include "nhal/nhal_backend.h"

struct nhal_mem_pool {
    unsigned char* objects;
    void* free_head;
    size_t stride;
    size_t capacity;
    size_t in_use;
    size_t bytes; /* Größe des ganzen Blocks (für den Allocator) */
};

static const nhal_allocator* g_alloc = NULL;

static size_t round_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

static nhal_status pool_create(size_t object_size, size_t object_align, size_t capacity, nhal_mem_pool** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (object_size == 0 || capacity == 0) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Objektgröße und Kapazität müssen > 0 sein");
    }
    if (object_align == 0) object_align = _Alignof(max_align_t);
    if ((object_align & (object_align - 1)) != 0) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ausrichtung muss eine Zweierpotenz sein");
    }
    if (object_align < _Alignof(void*)) object_align = _Alignof(void*);
    if (object_size < sizeof(void*)) object_size = sizeof(void*);

    const size_t stride = round_up(object_size, object_align);
    const size_t head = round_up(sizeof(nhal_mem_pool), object_align);
    if (stride < object_size || capacity > (SIZE_MAX - head) / stride) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pool wäre größer als der Adressraum");
    }
    const size_t bytes = head + stride * capacity;
    const size_t block_align = object_align > _Alignof(nhal_mem_pool) ? object_align : _Alignof(nhal_mem_pool);
    unsigned char* block = (unsigned char*)g_alloc->alloc(g_alloc->user, bytes, block_align);
    if (!block) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Pool");

    nhal_mem_pool* pool = (nhal_mem_pool*)block;
    pool->objects = block + head;
    pool->stride = stride;
    pool->capacity = capacity;
    pool->in_use = 0;
    pool->bytes = bytes;
    pool->free_head = NULL;
    for (size_t i = capacity; i-- > 0;) { /* Liste so, dass alloc vorne beginnt */
        void** slot = (void**)(pool->objects + i * stride);
        *slot = pool->free_head;
        pool->free_head = slot;
    }
    *out = pool;
    return nhal_status_ok();
}

static void pool_destroy(nhal_mem_pool* pool) {
    if (pool) g_alloc->free(g_alloc->user, pool, pool->bytes);
}

static void* pool_alloc(nhal_mem_pool* pool) {
    void** slot = (void**)pool->free_head;
    if (!slot) return NULL;
    pool->free_head = *slot;
    ++pool->in_use;
    return slot;
}

static bool pool_contains(const nhal_mem_pool* pool, const void* object) {
    const unsigned char* p = (const unsigned char*)object;
    if (!pool || p < pool->objects) return false;
    const size_t offset = (size_t)(p - pool->objects);
    return offset < pool->stride * pool->capacity && offset % pool->stride == 0;
}

static bool pool_free(nhal_mem_pool* pool, void* object) {
    if (!pool_contains(pool, object)) return false;
    void** slot = (void**)object;
    *slot = pool->free_head;
    pool->free_head = slot;
    --pool->in_use;
    return true;
}

static size_t pool_capacity(const nhal_mem_pool* pool) { return pool->capacity; }
static size_t pool_in_use(const nhal_mem_pool* pool) { return pool->in_use; }

static nhal_status init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void shutdown(void) { g_alloc = NULL; }

static const nhal_mem_pool_v1 table = {
    sizeof(nhal_mem_pool_v1), pool_create, pool_destroy, pool_alloc, pool_free,
    pool_contains, pool_capacity, pool_in_use,
};

const nhal_feature_impl nhal_impl_mem_pool = { 1u, init, shutdown, &table };
