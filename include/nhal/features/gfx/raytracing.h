/*
 * gfx.raytracing, Version 1: Beschleunigungsstrukturen und Ray Queries.
 *
 * Baut auf gfx.device (Version 2) auf. Zwei Stufen wie in Metal, DXR und
 * Vulkan:
 *   BLAS (unten): Dreiecke aus einem Vertex- und optional einem Index-Puffer.
 *   TLAS (oben):  Instanzen von BLAS mit je einer 3×4-Transformation.
 * Beide werden in einer Befehlsliste gebaut (außerhalb eines Passes) und
 * danach in Shadern abgefragt (Ray Queries, z. B. in einem Compute-Shader).
 *
 * Bindung im Shader (eine TLAS pro Slot):
 *   MSL: instance_acceleration_structure as [[buffer(SLOT)]]
 *   Den Slot setzt pass_set_tlas; er darf in der Pipeline nicht zusätzlich
 *   als Puffer, Textur oder Sampler belegt sein.
 *   Instanz-Index im Treffer = Position der Instanz im Array von build_tlas.
 *
 * Reihenfolge: build_blas vor build_tlas (in derselben oder einer früheren
 * Liste); eine TLAS gilt bis zum nächsten build_tlas.
 *
 * Verfügbarkeit: nhal_init meldet NHAL_ERR_UNAVAILABLE ohne fähige GPU
 * (Metal: supportsRaytracing, Apple Silicon). WebGPU kennt kein Raytracing;
 * im Web wird das Feature beim Bauen abgelehnt.
 */
#ifndef NHAL_FEATURES_GFX_RAYTRACING_H
#define NHAL_FEATURES_GFX_RAYTRACING_H

#include "nhal/features/gfx/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_gfx_accel nhal_gfx_accel; /* BLAS oder TLAS */

typedef struct nhal_gfx_blas_desc {
    nhal_gfx_buffer* vertices; /* Usage VERTEX; je Vertex drei float (x, y, z) */
    uint64_t vertex_offset;    /* Vielfaches von 4 */
    uint32_t vertex_stride;    /* mindestens 12, Vielfaches von 4 */
    uint32_t vertex_count;
    nhal_gfx_buffer* indices; /* NULL = je drei Vertices ein Dreieck; sonst Usage INDEX */
    uint64_t index_offset;
    nhal_gfx_index_format index_format;
    uint32_t triangle_count;
    bool opaque; /* true: keine Any-Hit-Prüfung */
} nhal_gfx_blas_desc;

typedef struct nhal_gfx_tlas_desc {
    uint32_t max_instances; /* 1..65536 */
} nhal_gfx_tlas_desc;

typedef struct nhal_gfx_instance {
    const nhal_gfx_accel* blas;
    float transform[12]; /* 3×4, zeilenweise: x' = t[0]x + t[1]y + t[2]z + t[3] … */
    uint32_t mask;       /* 8 Bit; 0 = 0xFF (für alle Strahlen sichtbar) */
} nhal_gfx_instance;

typedef struct nhal_gfx_raytracing_v1 {
    uint32_t struct_size;
    /* Legt Speicher an; Inhalt kommt erst mit commands_build_blas. */
    nhal_status (*blas_create)(const nhal_gfx_blas_desc* desc, nhal_gfx_accel** out);
    nhal_status (*tlas_create)(const nhal_gfx_tlas_desc* desc, nhal_gfx_accel** out);
    void (*accel_destroy)(nhal_gfx_accel* accel);
    /* Außerhalb eines Passes; Fehler landen wie üblich in der Befehlsliste. */
    void (*commands_build_blas)(nhal_gfx_commands* commands, nhal_gfx_accel* blas);
    void (*commands_build_tlas)(nhal_gfx_commands* commands, nhal_gfx_accel* tlas,
                                const nhal_gfx_instance* instances, uint32_t count);
    /* Im Compute- oder Render-Pass; MSL [[buffer(slot)]]. */
    void (*pass_set_tlas)(nhal_gfx_pass* pass, uint32_t slot, nhal_gfx_accel* tlas);
} nhal_gfx_raytracing_v1;

static inline const nhal_gfx_raytracing_v1* nhal_get_gfx_raytracing_v1(const nhal_context* ctx) {
    return (const nhal_gfx_raytracing_v1*)nhal_get(ctx, NHAL_FEATURE_GFX_RAYTRACING, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_GFX_RAYTRACING_H */
