/*
 * mem.frame, Version 1: linearer Frame-Allocator.
 *
 * Typischer Einsatz: Speicher für die Dauer eines Frames oder Auftrags.
 * Einzelne Objekte werden nie freigegeben; reset() gibt alles auf einmal zurück.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Feste Kapazität, beim Anlegen einmal reserviert. alloc() ist ein
 *     Zeigervorschub in O(1).
 *   - alloc() liefert NULL, wenn der Platz nicht reicht.
 *   - align muss eine Zweierpotenz sein (0 = max_align_t).
 *   - mark()/rewind() geben alles ab einer Marke zurück (Stapel-Prinzip).
 *   - high_water() ist der größte je belegte Stand, zum Bemessen der Kapazität.
 *   - NICHT thread-sicher.
 */
#ifndef NHAL_FEATURES_MEM_FRAME_H
#define NHAL_FEATURES_MEM_FRAME_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_mem_frame nhal_mem_frame;

typedef struct nhal_mem_frame_v1 {
    uint32_t struct_size;
    nhal_status (*create)(size_t capacity, nhal_mem_frame** out);
    void (*destroy)(nhal_mem_frame* frame);
    void* (*alloc)(nhal_mem_frame* frame, size_t size, size_t align);
    void (*reset)(nhal_mem_frame* frame);
    size_t (*mark)(const nhal_mem_frame* frame);
    void (*rewind)(nhal_mem_frame* frame, size_t mark);
    size_t (*used)(const nhal_mem_frame* frame);
    size_t (*capacity)(const nhal_mem_frame* frame);
    size_t (*high_water)(const nhal_mem_frame* frame);
} nhal_mem_frame_v1;

static inline const nhal_mem_frame_v1* nhal_get_mem_frame_v1(const nhal_context* ctx) {
    return (const nhal_mem_frame_v1*)nhal_get(ctx, NHAL_FEATURE_MEM_FRAME, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_MEM_FRAME_H */
