/*
 * Interne Strukturen des Metal-Backends (gfx.device, gfx.mesh_shader).
 * Objective-C mit ARC: Felder mit __strong geben ihr Objekt frei, sobald sie
 * auf nil gesetzt werden – deshalb vor jedem free() alles auf nil setzen.
 */
#ifndef NHAL_BACKEND_MACOS_GFX_METAL_H
#define NHAL_BACKEND_MACOS_GFX_METAL_H

#include "common/gfx/validate.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

typedef enum nhal_mtl_pipeline_kind { NHAL_MTL_RENDER = 1, NHAL_MTL_COMPUTE, NHAL_MTL_MESH } nhal_mtl_pipeline_kind;

struct nhal_gfx_buffer {
    __strong id<MTLBuffer> mtl;
    uint64_t size;
    uint32_t usage;
};

struct nhal_gfx_texture {
    __strong id<MTLTexture> mtl;
    uint32_t width, height;
    nhal_gfx_format format;
    uint32_t usage;
    uint32_t samples;
};

struct nhal_gfx_sampler {
    __strong id<MTLSamplerState> mtl;
};

struct nhal_gfx_shader {
    __strong id<MTLLibrary> library;
};

struct nhal_gfx_group_layout {
    nhal_gfx_layout layout;
};

/* Eine Ressource einer Gruppe; je nach kind ist buffer, texture oder sampler gesetzt. */
typedef struct nhal_mtl_group_item {
    uint32_t slot;
    nhal_gfx_binding_kind kind;
    __strong id<MTLBuffer> buffer;
    NSUInteger offset;
    __strong id<MTLTexture> texture;
    __strong id<MTLSamplerState> sampler;
} nhal_mtl_group_item;

struct nhal_gfx_group {
    nhal_gfx_layout layout;
    uint32_t count;
    nhal_mtl_group_item items[NHAL_GFX_MAX_BINDINGS];
};

struct nhal_gfx_pipeline {
    nhal_mtl_pipeline_kind kind;
    __strong id<MTLRenderPipelineState> render;
    __strong id<MTLComputePipelineState> compute;
    __strong id<MTLDepthStencilState> depth;
    MTLPrimitiveType primitive;
    MTLCullMode cull;
    MTLWinding winding;
    nhal_gfx_pass_shape shape;
    MTLSize threads;        /* Compute: Workgroup; Mesh: Threads pro Mesh-Gruppe */
    MTLSize object_threads; /* Mesh: Threads pro Object-Gruppe */
    nhal_gfx_pipeline_layout layout;
};

struct nhal_gfx_query_set {
    nhal_gfx_query_set_desc desc;
    __strong id<MTLBuffer> visibility;              /* OCCLUSION: count × uint64 */
    __strong id<MTLCounterSampleBuffer> counters;   /* TIMESTAMP */
};

typedef enum nhal_mtl_pass_kind { NHAL_MTL_PASS_NONE = 0, NHAL_MTL_PASS_RENDER, NHAL_MTL_PASS_COMPUTE } nhal_mtl_pass_kind;

struct nhal_gfx_pass {
    nhal_gfx_commands* owner;
    nhal_mtl_pass_kind kind;
    __strong id<MTLRenderCommandEncoder> render;
    __strong id<MTLComputeCommandEncoder> compute;
    nhal_gfx_pipeline* pipeline;
    nhal_gfx_slot slots[NHAL_GFX_MAX_BINDINGS];
    nhal_gfx_group* groups[NHAL_GFX_MAX_GROUPS];
    const nhal_gfx_group* applied[NHAL_GFX_MAX_BINDINGS]; /* welche Gruppe auf dem Index liegt; NULL = Slot */
    uint32_t constants_size;
    uint32_t vertex_buffers; /* Bitmaske gesetzter Vertex-Puffer */
    __strong id<MTLBuffer> index;
    uint64_t index_offset;
    MTLIndexType index_type;
    nhal_gfx_pass_shape shape;
    uint32_t width, height;
    bool scissor_empty; /* Scissor ohne Fläche: Draws zeichnen nichts */
    nhal_gfx_query_set* occlusion;
    bool occlusion_open;
};

struct nhal_gfx_commands {
    __strong id<MTLCommandBuffer> cb;
    __strong id<MTLBlitCommandEncoder> blit;
    nhal_status error;
    nhal_gfx_pass pass;
};

/* Gemeinsamer Zustand, gehört gfx.device. */
typedef struct nhal_mtl_shared {
    __strong id<MTLDevice> device;
    __strong id<MTLCommandQueue> queue;
    __strong id<MTLSharedEvent> event;      /* signalisiert die Seriennummern */
    __strong id<MTLCounterSet> timestamps;  /* nil ohne Zeitstempel */
    uint64_t serial;                        /* zuletzt vergebene Nummer */
    MTLTimestamp cpu0, gpu0;                /* Bezugspunkt für timestamp_period */
    const nhal_allocator* alloc;
    bool mesh_capable;
} nhal_mtl_shared;

extern nhal_mtl_shared nhal_mtl;

/* Ersten Fehler einer Befehlsliste merken. */
void nhal_mtl_record(nhal_gfx_commands* c, nhal_status s);
/* Asynchrone Fehlermeldung ablegen (thread-sicher). */
void nhal_mtl_push_error(const char* msg);
/* Gemeinsame Helfer für Pipelines. */
MTLPixelFormat nhal_mtl_pixel_format(nhal_gfx_format f);
id<MTLFunction> nhal_mtl_function(const nhal_gfx_shader* shader, const char* name);
id<MTLDepthStencilState> nhal_mtl_depth_state(bool test, bool write, nhal_gfx_compare compare);
void nhal_mtl_configure_color(MTLRenderPipelineColorAttachmentDescriptor* c, const nhal_gfx_color_target* t);
MTLCullMode nhal_mtl_cull(nhal_gfx_cull c);
/* Layouts der Gruppen einer Pipeline-Beschreibung (NULL, wo keine). */
void nhal_mtl_group_layouts(const nhal_gfx_group_layout* const in[NHAL_GFX_MAX_GROUPS],
                            const nhal_gfx_layout* out[NHAL_GFX_MAX_GROUPS]);
void* nhal_mtl_alloc(size_t size);
void nhal_mtl_free(void* p, size_t size);
/* Prüft vor draw/dispatch Pass, Pipeline, Bindungen und Konstanten und bindet
 * die Gruppen. false = nicht zeichnen (Fehler steht ggf. in der Liste). */
bool nhal_mtl_ready_to_draw(nhal_gfx_pass* p, nhal_mtl_pipeline_kind want);

#endif /* NHAL_BACKEND_MACOS_GFX_METAL_H */
