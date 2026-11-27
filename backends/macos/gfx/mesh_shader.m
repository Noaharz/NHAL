/*
 * gfx.mesh_shader – macOS (Metal 3, MTLMeshRenderPipelineDescriptor).
 * Teilt Gerät und Strukturen mit gfx.device (macos/gfx/metal.h).
 */
#include "nhal/features/gfx/mesh_shader.h"
#include "macos/gfx/metal.h"

#import <Foundation/Foundation.h>

static bool threads_ok(const uint32_t t[3]) {
    return t[0] && t[1] && t[2] && (uint64_t)t[0] * t[1] * t[2] <= 1024u;
}

static nhal_status mesh_pipeline_create(const nhal_gfx_mesh_pipeline_desc* d, nhal_gfx_pipeline** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    if (!nhal_mtl.device) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Metal-Gerät");
    const nhal_gfx_layout* groups[NHAL_GFX_MAX_GROUPS];
    nhal_mtl_group_layouts(d->groups, groups);
    nhal_gfx_pipeline_layout layout;
    nhal_status s = nhal_gfx_make_pipeline_layout(d->bindings, d->binding_count, groups, d->constants_size,
                                                  NHAL_GFX_STAGE_OBJECT | NHAL_GFX_STAGE_MESH | NHAL_GFX_STAGE_FRAGMENT,
                                                  "Mesh-Bindungen nur für OBJECT, MESH und FRAGMENT", &layout);
    if (s.code) return s;
    nhal_gfx_pass_shape shape;
    if ((s = nhal_gfx_check_targets(d->color_count, d->colors, d->depth_format, d->fragment_entry, d->depth_test,
                                    d->depth_compare, d->sample_count, &shape))
            .code)
        return s;
    if (!d->shader || !d->mesh_entry) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Shader oder mesh_entry fehlt");
    if (!threads_ok(d->mesh_threads) || (d->object_entry && !threads_ok(d->object_threads)))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Threads je Gruppe: jede Achse > 0, zusammen höchstens 1024");
    if (d->payload_bytes > 16384u) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Nutzlast über 16384 Bytes");

    if (@available(macOS 13.0, *)) {
        MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
        if (d->object_entry) {
            pd.objectFunction = nhal_mtl_function(d->shader, d->object_entry);
            if (!pd.objectFunction) return nhal_status_error(NHAL_ERR_NOT_FOUND, "object_entry fehlt im Shader");
            pd.maxTotalThreadsPerObjectThreadgroup = d->object_threads[0] * d->object_threads[1] * d->object_threads[2];
            pd.payloadMemoryLength = d->payload_bytes;
        }
        pd.meshFunction = nhal_mtl_function(d->shader, d->mesh_entry);
        if (!pd.meshFunction) return nhal_status_error(NHAL_ERR_NOT_FOUND, "mesh_entry fehlt im Shader");
        pd.maxTotalThreadsPerMeshThreadgroup = d->mesh_threads[0] * d->mesh_threads[1] * d->mesh_threads[2];
        if (d->fragment_entry) {
            pd.fragmentFunction = nhal_mtl_function(d->shader, d->fragment_entry);
            if (!pd.fragmentFunction) return nhal_status_error(NHAL_ERR_NOT_FOUND, "fragment_entry fehlt im Shader");
        }
        for (uint32_t i = 0; i < d->color_count; ++i) nhal_mtl_configure_color(pd.colorAttachments[i], &d->colors[i]);
        if (d->depth_format != NHAL_GFX_FORMAT_NONE) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        pd.rasterSampleCount = shape.samples;

        NSError* err = nil;
        id<MTLRenderPipelineState> state = [nhal_mtl.device newRenderPipelineStateWithMeshDescriptor:pd
                                                                                             options:MTLPipelineOptionNone
                                                                                          reflection:nil
                                                                                               error:&err];
        if (!state) {
            nhal_mtl_push_error(err ? err.localizedDescription.UTF8String : "Mesh-Pipeline-Fehler");
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Metal lehnt die Mesh-Pipeline ab (Details: take_error)");
        }
        nhal_gfx_pipeline* p = (nhal_gfx_pipeline*)nhal_mtl_alloc(sizeof *p);
        if (!p) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Pipeline");
        p->kind = NHAL_MTL_MESH;
        p->render = state;
        p->depth = nhal_mtl_depth_state(d->depth_test, d->depth_write, d->depth_compare);
        p->cull = nhal_mtl_cull(d->cull);
        p->winding = d->front_face == NHAL_GFX_FRONT_CW ? MTLWindingClockwise : MTLWindingCounterClockwise;
        p->shape = shape;
        p->threads = MTLSizeMake(d->mesh_threads[0], d->mesh_threads[1], d->mesh_threads[2]);
        p->object_threads = d->object_entry
                                ? MTLSizeMake(d->object_threads[0], d->object_threads[1], d->object_threads[2])
                                : MTLSizeMake(1, 1, 1);
        p->layout = layout;
        *out = p;
        return nhal_status_ok();
    }
    return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Mesh-Shader brauchen macOS 13");
}

static void mesh_draw(nhal_gfx_pass* p, uint32_t x, uint32_t y, uint32_t z) {
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_MESH)) return;
    if (x > 65535u || y > 65535u || z > 65535u) {
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Höchstens 65535 Gruppen je Achse"));
        return;
    }
    if (!x || !y || !z) return;
    if (@available(macOS 13.0, *)) {
        [p->render drawMeshThreadgroups:MTLSizeMake(x, y, z) threadsPerObjectThreadgroup:p->pipeline->object_threads
              threadsPerMeshThreadgroup:p->pipeline->threads];
    }
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator; /* Speicher kommt über gfx.device */
    if (!nhal_mtl.device) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Metal-Gerät");
    if (!nhal_mtl.mesh_capable)
        return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Diese GPU kann keine Mesh-Shader (GPU-Familie Metal 3 nötig)");
    if (@available(macOS 13.0, *)) return nhal_status_ok();
    return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Mesh-Shader brauchen macOS 13");
}

static void feature_shutdown(void) {}

static const nhal_gfx_mesh_shader_v1 table = {
    sizeof(nhal_gfx_mesh_shader_v1),
    mesh_pipeline_create,
    mesh_draw,
};

const nhal_feature_impl nhal_impl_gfx_mesh_shader = { 1u, feature_init, feature_shutdown, &table };
