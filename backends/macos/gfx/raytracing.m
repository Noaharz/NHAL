/*
 * gfx.raytracing – macOS (Metal, MTLAccelerationStructure).
 * Teilt Gerät und Strukturen mit gfx.device (macos/gfx/metal.h).
 */
#include "nhal/features/gfx/raytracing.h"
#include "macos/gfx/metal.h"

#import <Foundation/Foundation.h>
#include <string.h>

#define MAX_INSTANCES 65536u

struct nhal_gfx_accel {
    bool top;
    bool built;
    uint32_t max_instances;
    __strong id<MTLAccelerationStructure> mtl;
    __strong MTLPrimitiveAccelerationStructureDescriptor* blas_desc;
    __strong NSArray<id<MTLAccelerationStructure>>* blases; /* TLAS: beim letzten Bau benutzte BLAS */
    NSUInteger scratch;
};

static void record(nhal_gfx_commands* c, nhal_status s) { nhal_mtl_record(c, s); }

static nhal_status make_accel(MTLAccelerationStructureDescriptor* d, nhal_gfx_accel* a) {
    const MTLAccelerationStructureSizes sizes = [nhal_mtl.device accelerationStructureSizesWithDescriptor:d];
    a->mtl = [nhal_mtl.device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    a->scratch = sizes.buildScratchBufferSize;
    if (!a->mtl) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Metal konnte die Beschleunigungsstruktur nicht anlegen");
    return nhal_status_ok();
}

static void free_accel(nhal_gfx_accel* a) {
    a->mtl = nil;
    a->blas_desc = nil;
    a->blases = nil;
    nhal_mtl_free(a, sizeof *a);
}

static nhal_status blas_create(const nhal_gfx_blas_desc* d, nhal_gfx_accel** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    if (!d->vertices || !(d->vertices->usage & NHAL_GFX_BUFFER_VERTEX))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "BLAS braucht einen Vertex-Puffer mit Usage VERTEX");
    if (d->vertex_stride < 12u || d->vertex_stride % 4u || d->vertex_offset % 4u || !d->vertex_count)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "vertex_stride ≥ 12 und Vielfaches von 4, Offset durch 4 teilbar");
    const uint64_t vbytes = (uint64_t)d->vertex_stride * (d->vertex_count - 1u) + 12u;
    if (d->vertex_offset > d->vertices->size || vbytes > d->vertices->size - d->vertex_offset)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertices reichen über den Puffer hinaus");
    if (!d->triangle_count) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "triangle_count ist 0");
    if (d->indices) {
        const uint64_t isz = d->index_format == NHAL_GFX_INDEX_UINT32 ? 4u : 2u;
        if (!(d->indices->usage & NHAL_GFX_BUFFER_INDEX) || d->index_offset % isz ||
            d->index_offset > d->indices->size || (uint64_t)d->triangle_count * 3u * isz > d->indices->size - d->index_offset)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Index-Puffer: Usage INDEX, Offset oder Größe ungültig");
    } else if ((uint64_t)d->triangle_count * 3u > d->vertex_count) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ohne Indizes braucht jedes Dreieck drei eigene Vertices");
    }
    MTLAccelerationStructureTriangleGeometryDescriptor* geo = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    geo.vertexBuffer = d->vertices->mtl;
    geo.vertexBufferOffset = (NSUInteger)d->vertex_offset;
    geo.vertexStride = d->vertex_stride;
    if (d->indices) {
        geo.indexBuffer = d->indices->mtl;
        geo.indexBufferOffset = (NSUInteger)d->index_offset;
        geo.indexType = d->index_format == NHAL_GFX_INDEX_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    }
    geo.triangleCount = d->triangle_count;
    geo.opaque = d->opaque;
    MTLPrimitiveAccelerationStructureDescriptor* pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    pd.geometryDescriptors = @[ geo ];
    nhal_gfx_accel* a = (nhal_gfx_accel*)nhal_mtl_alloc(sizeof *a);
    if (!a) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die BLAS");
    a->blas_desc = pd;
    const nhal_status s = make_accel(pd, a);
    if (s.code) {
        free_accel(a);
        return s;
    }
    *out = a;
    return nhal_status_ok();
}

static MTLInstanceAccelerationStructureDescriptor* tlas_descriptor(uint32_t count) {
    MTLInstanceAccelerationStructureDescriptor* d = [MTLInstanceAccelerationStructureDescriptor descriptor];
    d.instanceCount = count;
    d.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeDefault;
    return d;
}

static nhal_status tlas_create(const nhal_gfx_tlas_desc* d, nhal_gfx_accel** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    if (!d->max_instances || d->max_instances > MAX_INSTANCES)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "max_instances: 1..65536");
    nhal_gfx_accel* a = (nhal_gfx_accel*)nhal_mtl_alloc(sizeof *a);
    if (!a) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die TLAS");
    a->top = true;
    a->max_instances = d->max_instances;
    const nhal_status s = make_accel(tlas_descriptor(d->max_instances), a);
    if (s.code) {
        free_accel(a);
        return s;
    }
    *out = a;
    return nhal_status_ok();
}

static void accel_destroy(nhal_gfx_accel* a) {
    if (a) free_accel(a); /* läuft noch Arbeit, hält Metal die Struktur selbst fest */
}

/* Bereitet die Liste auf einen Bau vor: kein offener Pass, Blit-Encoder beendet. */
static bool begin_build(nhal_gfx_commands* c) {
    if (!c) return false;
    if (c->pass.kind != NHAL_MTL_PASS_NONE) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bauen geht nicht in einem offenen Pass"));
        return false;
    }
    if (c->blit) {
        [c->blit endEncoding];
        c->blit = nil;
    }
    return true;
}

static void build(nhal_gfx_commands* c, nhal_gfx_accel* a, MTLAccelerationStructureDescriptor* d) {
    id<MTLBuffer> scratch = [nhal_mtl.device newBufferWithLength:a->scratch ? a->scratch : 16u
                                                         options:MTLResourceStorageModePrivate];
    id<MTLAccelerationStructureCommandEncoder> enc = [c->cb accelerationStructureCommandEncoder];
    [enc buildAccelerationStructure:a->mtl descriptor:d scratchBuffer:scratch scratchBufferOffset:0];
    [enc endEncoding];
    a->built = true;
}

static void commands_build_blas(nhal_gfx_commands* c, nhal_gfx_accel* a) {
    if (!c) return;
    if (!a || a->top) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "build_blas braucht eine BLAS"));
        return;
    }
    if (!begin_build(c)) return;
    build(c, a, a->blas_desc);
}

static void commands_build_tlas(nhal_gfx_commands* c, nhal_gfx_accel* t, const nhal_gfx_instance* in, uint32_t n) {
    if (!c) return;
    if (!t || !t->top) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "build_tlas braucht eine TLAS"));
        return;
    }
    if (!n || n > t->max_instances || !in) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Instanzen fehlen oder mehr als max_instances"));
        return;
    }
    NSMutableArray<id<MTLAccelerationStructure>>* blases = [NSMutableArray array];
    NSMutableData* data = [NSMutableData dataWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) * n];
    MTLAccelerationStructureInstanceDescriptor* out = (MTLAccelerationStructureInstanceDescriptor*)data.mutableBytes;
    for (uint32_t i = 0; i < n; ++i) {
        const nhal_gfx_accel* b = in[i].blas;
        if (!b || b->top || !b->built) {
            record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Instanz ohne gebaute BLAS (erst build_blas)"));
            return;
        }
        NSUInteger index = [blases indexOfObjectIdenticalTo:b->mtl];
        if (index == NSNotFound) {
            index = blases.count;
            [blases addObject:b->mtl];
        }
        for (int col = 0; col < 4; ++col) { /* MTLPackedFloat4x3: vier Spalten zu je drei Zeilen */
            out[i].transformationMatrix.columns[col].x = in[i].transform[0 * 4 + col];
            out[i].transformationMatrix.columns[col].y = in[i].transform[1 * 4 + col];
            out[i].transformationMatrix.columns[col].z = in[i].transform[2 * 4 + col];
        }
        out[i].options = MTLAccelerationStructureInstanceOptionNone;
        out[i].mask = in[i].mask ? (in[i].mask & 0xFFu) : 0xFFu;
        out[i].intersectionFunctionTableOffset = 0;
        out[i].accelerationStructureIndex = (uint32_t)index;
    }
    if (!begin_build(c)) return;
    MTLInstanceAccelerationStructureDescriptor* d = tlas_descriptor(n);
    d.instancedAccelerationStructures = blases;
    d.instanceDescriptorBuffer = [nhal_mtl.device newBufferWithBytes:data.bytes length:data.length
                                                             options:MTLResourceStorageModeShared];
    t->blases = blases;
    build(c, t, d);
}

static void pass_set_tlas(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_accel* t) {
    if (!p) return;
    if (p->kind == NHAL_MTL_PASS_NONE) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pass ist schon beendet"));
        return;
    }
    if (!t || !t->top || !t->built || slot >= NHAL_GFX_MAX_BINDINGS) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                           "pass_set_tlas braucht eine gebaute TLAS und einen Slot unter 16"));
        return;
    }
    p->applied[slot] = NULL;
    if (p->compute) {
        [p->compute setAccelerationStructure:t->mtl atBufferIndex:slot];
        for (id<MTLAccelerationStructure> b in t->blases) [p->compute useResource:b usage:MTLResourceUsageRead];
        return;
    }
    [p->render setVertexAccelerationStructure:t->mtl atBufferIndex:slot];
    [p->render setFragmentAccelerationStructure:t->mtl atBufferIndex:slot];
    for (id<MTLAccelerationStructure> b in t->blases)
        [p->render useResource:b usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator; /* Speicher kommt über gfx.device */
    if (!nhal_mtl.device) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Metal-Gerät");
    if (!nhal_mtl.device.supportsRaytracing)
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Diese GPU kann kein Raytracing (supportsRaytracing)");
    return nhal_status_ok();
}

static void feature_shutdown(void) {}

static const nhal_gfx_raytracing_v1 table = {
    sizeof(nhal_gfx_raytracing_v1), blas_create, tlas_create, accel_destroy,
    commands_build_blas, commands_build_tlas, pass_set_tlas,
};

const nhal_feature_impl nhal_impl_gfx_raytracing = { 1u, feature_init, feature_shutdown, &table };
