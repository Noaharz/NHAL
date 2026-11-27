/*
 * gfx.mesh_shader, Version 1: Mesh- und Object-/Task-Shader.
 *
 * Baut auf gfx.device auf: Pipelines entstehen hier, gesetzt werden sie mit
 * pass_set_pipeline in einem Render-Pass, freigegeben mit pipeline_destroy.
 * Statt pass_draw zeichnet draw() Gruppen von Mesh-Threads.
 *
 * Stufen:
 *   Object (Metal) bzw. Task (D3D12/Vulkan): optional; legt fest, wie viele
 *   Mesh-Gruppen laufen, und reicht eine Nutzlast (payload) weiter.
 *   Mesh: erzeugt Vertices und Primitive direkt, ohne Vertex-Puffer.
 * Bindungen, Gruppen, Push-Konstanten und Farbziele wie bei gfx.device
 * (Version 2), mit den Stufen NHAL_GFX_STAGE_OBJECT, NHAL_GFX_STAGE_MESH und
 * NHAL_GFX_STAGE_FRAGMENT.
 *
 * Verfügbarkeit: nhal_init meldet NHAL_ERR_UNAVAILABLE, wenn die GPU keine
 * Mesh-Shader kann (Metal: GPU-Familie Metal 3). WebGPU kennt keine
 * Mesh-Shader; im Web wird das Feature beim Bauen abgelehnt.
 */
#ifndef NHAL_FEATURES_GFX_MESH_SHADER_H
#define NHAL_FEATURES_GFX_MESH_SHADER_H

#include "nhal/features/gfx/device.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_gfx_mesh_pipeline_desc {
    const nhal_gfx_shader* shader;
    const char* object_entry; /* NULL = ohne Object-Stufe */
    const char* mesh_entry;
    const char* fragment_entry;
    uint32_t binding_count;
    const nhal_gfx_binding_desc* bindings;
    const nhal_gfx_group_layout* groups[NHAL_GFX_MAX_GROUPS];
    uint32_t constants_size;
    uint32_t color_count;
    nhal_gfx_color_target colors[NHAL_GFX_MAX_COLOR_TARGETS];
    nhal_gfx_format depth_format; /* NONE oder DEPTH32_FLOAT */
    bool depth_test;
    bool depth_write;
    nhal_gfx_compare depth_compare;
    nhal_gfx_cull cull;
    nhal_gfx_front_face front_face;
    uint32_t sample_count;
    uint32_t object_threads[3]; /* Threads je Object-Gruppe (ohne Object-Stufe ignoriert) */
    uint32_t mesh_threads[3];   /* Threads je Mesh-Gruppe */
    uint32_t payload_bytes;     /* Nutzlast von Object an Mesh, höchstens 16384 */
} nhal_gfx_mesh_pipeline_desc;

typedef struct nhal_gfx_mesh_shader_v1 {
    uint32_t struct_size;
    nhal_status (*pipeline_create)(const nhal_gfx_mesh_pipeline_desc* desc, nhal_gfx_pipeline** out);
    /* Mit Object-Stufe: Object-Gruppen; ohne: Mesh-Gruppen. */
    void (*draw)(nhal_gfx_pass* pass, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
} nhal_gfx_mesh_shader_v1;

static inline const nhal_gfx_mesh_shader_v1* nhal_get_gfx_mesh_shader_v1(const nhal_context* ctx) {
    return (const nhal_gfx_mesh_shader_v1*)nhal_get(ctx, NHAL_FEATURE_GFX_MESH_SHADER, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_GFX_MESH_SHADER_H */
