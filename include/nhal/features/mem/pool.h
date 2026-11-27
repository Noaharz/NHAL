/*
 * mem.pool, Version 1: Pool für Objekte fester Größe.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Feste Kapazität, beim Anlegen einmal reserviert. alloc() und free()
 *     allokieren nie und laufen in O(1), also im Hot Path nutzbar.
 *   - alloc() liefert NULL, wenn der Pool voll ist (kein Wachsen, kein Ersatz).
 *   - Jedes Objekt ist auf object_align ausgerichtet (0 = max_align_t).
 *   - Ein Pool ist NICHT thread-sicher. Mehrere Threads brauchen eigene Pools
 *     oder eine Sperre beim Aufrufer.
 *   - free() eines fremden Zeigers wird erkannt und liefert false. Doppeltes
 *     free() wird nicht erkannt.
 *   - Speicher kommt vom Allocator des NHAL-Kontexts.
 */
#ifndef NHAL_FEATURES_MEM_POOL_H
#define NHAL_FEATURES_MEM_POOL_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_mem_pool nhal_mem_pool;

typedef struct nhal_mem_pool_v1 {
    uint32_t struct_size;
    nhal_status (*create)(size_t object_size, size_t object_align, size_t capacity, nhal_mem_pool** out);
    void (*destroy)(nhal_mem_pool* pool);
    void* (*alloc)(nhal_mem_pool* pool);
    bool (*free)(nhal_mem_pool* pool, void* object);
    bool (*contains)(const nhal_mem_pool* pool, const void* object);
    size_t (*capacity)(const nhal_mem_pool* pool);
    size_t (*in_use)(const nhal_mem_pool* pool);
} nhal_mem_pool_v1;

static inline const nhal_mem_pool_v1* nhal_get_mem_pool_v1(const nhal_context* ctx) {
    return (const nhal_mem_pool_v1*)nhal_get(ctx, NHAL_FEATURE_MEM_POOL, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_MEM_POOL_H */
