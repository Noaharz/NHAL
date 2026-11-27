/*
 * gfx.device – macOS (Metal, Objective-C mit ARC).
 *
 * Speicher: GPU-Ressourcen liegen privat (MTLStorageModePrivate), nur
 * HOST_VISIBLE-Puffer liegen geteilt. Uploads (buffer_write, texture_write,
 * Startdaten) sammeln sich in einer eigenen Befehlsliste, die vor jedem
 * submit, Lesen und Present abgeschickt wird. Damit gilt dieselbe Reihenfolge
 * wie bei WebGPU: Schreiben wirkt vor allen danach abgeschickten Listen.
 */
#include "nhal/features/gfx/device.h"
#include "macos/gfx/metal.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

nhal_mtl_shared nhal_mtl;

/* Fehler in der Befehlsliste merken und die void-Funktion verlassen. */
#define REC(cmds, status)               \
    do {                                \
        nhal_mtl_record((cmds), status); \
        return;                         \
    } while (0)

static os_unfair_lock g_error_lock = OS_UNFAIR_LOCK_INIT;
static nhal_gfx_error_ring g_errors;
static __strong id<MTLCommandBuffer> g_upload;
static __strong id<MTLBlitCommandEncoder> g_upload_blit;

/* Offene Lesevorgänge; der Completion-Handler setzt done, poll() liefert aus. */
typedef struct read_request {
    struct read_request* next;
    __strong id<MTLBuffer> staging;
    void* dst;
    uint64_t size;
    nhal_gfx_read_fn fn;
    void* user;
    _Atomic int state; /* 0 läuft, 1 fertig, 2 Fehler */
} read_request;
static read_request* g_reads;

/* ── Helfer ────────────────────────────────────────────────────────────── */

void* nhal_mtl_alloc(size_t size) {
    void* p = nhal_mtl.alloc->alloc(nhal_mtl.alloc->user, size, 16);
    if (p) memset(p, 0, size);
    return p;
}

void nhal_mtl_free(void* p, size_t size) {
    if (p) nhal_mtl.alloc->free(nhal_mtl.alloc->user, p, size);
}

void nhal_mtl_push_error(const char* msg) {
    os_unfair_lock_lock(&g_error_lock);
    nhal_gfx_error_push(&g_errors, msg);
    os_unfair_lock_unlock(&g_error_lock);
}

void nhal_mtl_record(nhal_gfx_commands* c, nhal_status s) {
    if (c && c->error.code == NHAL_OK && s.code != NHAL_OK) c->error = s;
}

MTLPixelFormat nhal_mtl_pixel_format(nhal_gfx_format f) {
    switch (f) {
    case NHAL_GFX_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case NHAL_GFX_FORMAT_RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case NHAL_GFX_FORMAT_RGBA8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case NHAL_GFX_FORMAT_BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case NHAL_GFX_FORMAT_BGRA8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case NHAL_GFX_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
    case NHAL_GFX_FORMAT_RG32_FLOAT: return MTLPixelFormatRG32Float;
    case NHAL_GFX_FORMAT_RGBA16_FLOAT: return MTLPixelFormatRGBA16Float;
    case NHAL_GFX_FORMAT_RGBA32_FLOAT: return MTLPixelFormatRGBA32Float;
    case NHAL_GFX_FORMAT_DEPTH32_FLOAT: return MTLPixelFormatDepth32Float;
    default: return MTLPixelFormatInvalid;
    }
}

static MTLVertexFormat vertex_format(nhal_gfx_vertex_format f) {
    switch (f) {
    case NHAL_GFX_VERTEX_FLOAT32: return MTLVertexFormatFloat;
    case NHAL_GFX_VERTEX_FLOAT32X2: return MTLVertexFormatFloat2;
    case NHAL_GFX_VERTEX_FLOAT32X3: return MTLVertexFormatFloat3;
    case NHAL_GFX_VERTEX_FLOAT32X4: return MTLVertexFormatFloat4;
    case NHAL_GFX_VERTEX_UNORM8X4: return MTLVertexFormatUChar4Normalized;
    case NHAL_GFX_VERTEX_UINT32: return MTLVertexFormatUInt;
    default: return MTLVertexFormatInvalid;
    }
}

id<MTLFunction> nhal_mtl_function(const nhal_gfx_shader* shader, const char* name) {
    if (!shader || !name) return nil;
    return [shader->library newFunctionWithName:[NSString stringWithUTF8String:name]];
}

static MTLCompareFunction compare_fn(nhal_gfx_compare c) {
    switch (c) {
    case NHAL_GFX_COMPARE_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case NHAL_GFX_COMPARE_GREATER: return MTLCompareFunctionGreater;
    case NHAL_GFX_COMPARE_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    case NHAL_GFX_COMPARE_EQUAL: return MTLCompareFunctionEqual;
    case NHAL_GFX_COMPARE_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case NHAL_GFX_COMPARE_ALWAYS: return MTLCompareFunctionAlways;
    case NHAL_GFX_COMPARE_NEVER: return MTLCompareFunctionNever;
    default: return MTLCompareFunctionLess;
    }
}

id<MTLDepthStencilState> nhal_mtl_depth_state(bool test, bool write, nhal_gfx_compare compare) {
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = test ? compare_fn(compare) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = write;
    return [nhal_mtl.device newDepthStencilStateWithDescriptor:d];
}

void nhal_mtl_configure_color(MTLRenderPipelineColorAttachmentDescriptor* c, const nhal_gfx_color_target* t) {
    c.pixelFormat = nhal_mtl_pixel_format(t->format);
    const uint32_t m = t->write_mask ? t->write_mask : NHAL_GFX_WRITE_ALL;
    c.writeMask = ((m & NHAL_GFX_WRITE_R) ? MTLColorWriteMaskRed : 0) | ((m & NHAL_GFX_WRITE_G) ? MTLColorWriteMaskGreen : 0) |
                  ((m & NHAL_GFX_WRITE_B) ? MTLColorWriteMaskBlue : 0) | ((m & NHAL_GFX_WRITE_A) ? MTLColorWriteMaskAlpha : 0);
    if (t->blend == NHAL_GFX_BLEND_NONE) return;
    c.blendingEnabled = YES;
    c.rgbBlendOperation = MTLBlendOperationAdd;
    c.alphaBlendOperation = MTLBlendOperationAdd;
    switch (t->blend) {
    case NHAL_GFX_BLEND_PREMULTIPLIED:
        c.sourceRGBBlendFactor = MTLBlendFactorOne;
        c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    case NHAL_GFX_BLEND_ADDITIVE:
        c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor = MTLBlendFactorOne;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOne;
        break;
    case NHAL_GFX_BLEND_MULTIPLY:
        c.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
        c.destinationRGBBlendFactor = MTLBlendFactorZero;
        c.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
        c.destinationAlphaBlendFactor = MTLBlendFactorZero;
        break;
    default: /* ALPHA */
        c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    }
}

MTLCullMode nhal_mtl_cull(nhal_gfx_cull c) {
    return c == NHAL_GFX_CULL_BACK ? MTLCullModeBack : c == NHAL_GFX_CULL_FRONT ? MTLCullModeFront : MTLCullModeNone;
}

void nhal_mtl_group_layouts(const nhal_gfx_group_layout* const in[NHAL_GFX_MAX_GROUPS],
                            const nhal_gfx_layout* out[NHAL_GFX_MAX_GROUPS]) {
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) out[g] = in[g] ? &in[g]->layout : NULL;
}

static nhal_status not_ready(void) {
    return nhal_mtl.device ? nhal_status_ok() : nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Metal-Gerät");
}

static id<MTLBlitCommandEncoder> upload_blit(void) {
    if (!g_upload) {
        g_upload = [nhal_mtl.queue commandBuffer];
        g_upload.label = @"nhal.upload";
        g_upload_blit = [g_upload blitCommandEncoder];
    }
    return g_upload_blit;
}

static void flush_uploads(void) {
    if (!g_upload) return;
    [g_upload_blit endEncoding];
    [g_upload commit];
    g_upload_blit = nil;
    g_upload = nil;
}

/* Staging-Puffer mit Daten; Kopie auf die GPU über die Upload-Liste. */
static id<MTLBuffer> staging_with(const void* data, uint64_t size) {
    return [nhal_mtl.device newBufferWithBytes:data length:(NSUInteger)size options:MTLResourceStorageModeShared];
}

static bool has_timestamps(void);

/* ── Gerät ─────────────────────────────────────────────────────────────── */

static nhal_status gfx_ready(void) { return not_ready(); }

static void gfx_poll(void) {
    flush_uploads();
    read_request** link = &g_reads;
    while (*link) {
        read_request* r = *link;
        const int st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (st == 0) {
            link = &r->next;
            continue;
        }
        *link = r->next;
        if (st == 1) memcpy(r->dst, r->staging.contents, (size_t)r->size);
        r->staging = nil;
        nhal_gfx_read_fn fn = r->fn;
        void* user = r->user;
        nhal_mtl_free(r, sizeof *r);
        fn(user, st == 1 ? nhal_status_ok()
                         : nhal_status_error(NHAL_ERR_IO, "Lesen von der GPU fehlgeschlagen (Details: take_error)"));
    }
}

static void gfx_info(nhal_gfx_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->backend = "metal";
    out->shader_language = NHAL_GFX_SHADER_MSL;
    if (!nhal_mtl.device) return;
    strncpy(out->device_name, nhal_mtl.device.name.UTF8String, sizeof out->device_name - 1);
    out->unified_memory = nhal_mtl.device.hasUnifiedMemory;
    out->persistent_mapping = true;
    out->blocking_wait = true;
    out->timestamp_queries = has_timestamps();
    out->indirect_first_instance = true;
    out->timestamp_period = 1.0;
    if (out->timestamp_queries) { /* Ticks gegen CPU-Nanosekunden seit dem Start messen */
        MTLTimestamp cpu = 0, gpu = 0;
        [nhal_mtl.device sampleTimestamps:&cpu gpuTimestamp:&gpu];
        if (gpu > nhal_mtl.gpu0 && cpu - nhal_mtl.cpu0 > 1000000u)
            out->timestamp_period = (double)(cpu - nhal_mtl.cpu0) / (double)(gpu - nhal_mtl.gpu0);
    }
    out->max_texture_size = 16384;
    out->max_buffer_size = nhal_mtl.device.maxBufferLength;
}

static bool gfx_take_error(char* buf, size_t cap) {
    os_unfair_lock_lock(&g_error_lock);
    const bool any = nhal_gfx_error_take(&g_errors, buf, cap);
    os_unfair_lock_unlock(&g_error_lock);
    return any;
}

/* ── Puffer ────────────────────────────────────────────────────────────── */

static nhal_status buffer_create(const nhal_gfx_buffer_desc* d, const void* initial, nhal_gfx_buffer** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_buffer_desc(d)).code) return s;
    if (d->size > nhal_mtl.device.maxBufferLength)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer größer als info().max_buffer_size");
    const bool host = (d->usage & NHAL_GFX_BUFFER_HOST_VISIBLE) != 0;
    nhal_gfx_buffer* b = (nhal_gfx_buffer*)nhal_mtl_alloc(sizeof *b);
    if (!b) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Puffer");
    b->size = d->size;
    b->usage = d->usage;
    const MTLResourceOptions opt = host ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
    if (host && initial) {
        b->mtl = [nhal_mtl.device newBufferWithBytes:initial length:(NSUInteger)d->size options:opt];
    } else {
        b->mtl = [nhal_mtl.device newBufferWithLength:(NSUInteger)d->size options:opt];
        if (b->mtl && !host) { /* GPU-Speicher ist nicht genullt – wie bei WebGPU nullen */
            if (initial) {
                [upload_blit() copyFromBuffer:staging_with(initial, d->size) sourceOffset:0 toBuffer:b->mtl
                            destinationOffset:0 size:(NSUInteger)d->size];
            } else {
                [upload_blit() fillBuffer:b->mtl range:NSMakeRange(0, (NSUInteger)d->size) value:0];
            }
        }
    }
    if (!b->mtl) {
        nhal_mtl_free(b, sizeof *b);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Metal konnte den Puffer nicht anlegen");
    }
    *out = b;
    return nhal_status_ok();
}

static void buffer_destroy(nhal_gfx_buffer* b) {
    if (!b) return;
    b->mtl = nil; /* läuft noch Arbeit, hält Metal den Puffer selbst fest */
    nhal_mtl_free(b, sizeof *b);
}

static nhal_status buffer_write(nhal_gfx_buffer* b, uint64_t offset, const void* data, uint64_t size) {
    if (!b || !data) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer oder Daten fehlen");
    if (!(b->usage & NHAL_GFX_BUFFER_COPY_DST))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "buffer_write braucht Usage COPY_DST");
    nhal_status s = nhal_gfx_check_range(b->size, offset, size);
    if (s.code) return s;
    [upload_blit() copyFromBuffer:staging_with(data, size) sourceOffset:0 toBuffer:b->mtl
                destinationOffset:(NSUInteger)offset size:(NSUInteger)size];
    return nhal_status_ok();
}

static nhal_status buffer_read(nhal_gfx_buffer* b, uint64_t offset, uint64_t size, void* dst, nhal_gfx_read_fn fn,
                               void* user) {
    if (!b || !dst || !fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer, Ziel oder Rückruf fehlt");
    if (!(b->usage & NHAL_GFX_BUFFER_COPY_SRC))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "buffer_read braucht Usage COPY_SRC");
    nhal_status s = nhal_gfx_check_range(b->size, offset, size);
    if (s.code) return s;
    read_request* r = (read_request*)nhal_mtl_alloc(sizeof *r);
    if (!r) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Lesevorgang");
    r->staging = [nhal_mtl.device newBufferWithLength:(NSUInteger)size options:MTLResourceStorageModeShared];
    if (!r->staging) {
        nhal_mtl_free(r, sizeof *r);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Lesepuffer");
    }
    r->dst = dst;
    r->size = size;
    r->fn = fn;
    r->user = user;
    flush_uploads();
    id<MTLCommandBuffer> cb = [nhal_mtl.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:b->mtl sourceOffset:(NSUInteger)offset toBuffer:r->staging destinationOffset:0
                    size:(NSUInteger)size];
    [blit endEncoding];
    read_request* req = r;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const bool ok = done.status == MTLCommandBufferStatusCompleted;
      if (!ok) nhal_mtl_push_error(done.error.localizedDescription.UTF8String);
      atomic_store_explicit(&req->state, ok ? 1 : 2, memory_order_release);
    }];
    r->next = g_reads;
    g_reads = r;
    [cb commit];
    return nhal_status_ok();
}

static void* buffer_mapped(nhal_gfx_buffer* b) {
    return (b && (b->usage & NHAL_GFX_BUFFER_HOST_VISIBLE)) ? b->mtl.contents : NULL;
}

/* ── Texturen, Sampler ─────────────────────────────────────────────────── */

static nhal_status texture_create(const nhal_gfx_texture_desc* d, nhal_gfx_texture** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_texture_desc(d, 16384)).code) return s;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:nhal_mtl_pixel_format(d->format)
                                                                                  width:d->width
                                                                                 height:d->height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    const uint32_t samples = nhal_gfx_samples(d->sample_count);
    if (samples > 1u) {
        td.textureType = MTLTextureType2DMultisample;
        td.sampleCount = samples;
    }
    MTLTextureUsage u = 0;
    if (d->usage & NHAL_GFX_TEXTURE_SAMPLED) u |= MTLTextureUsageShaderRead;
    if (d->usage & NHAL_GFX_TEXTURE_STORAGE) u |= MTLTextureUsageShaderWrite;
    if (d->usage & NHAL_GFX_TEXTURE_RENDER_TARGET) u |= MTLTextureUsageRenderTarget;
    td.usage = u ? u : MTLTextureUsageShaderRead;
    nhal_gfx_texture* t = (nhal_gfx_texture*)nhal_mtl_alloc(sizeof *t);
    if (!t) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Textur");
    t->mtl = [nhal_mtl.device newTextureWithDescriptor:td];
    if (!t->mtl) {
        nhal_mtl_free(t, sizeof *t);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Metal konnte die Textur nicht anlegen");
    }
    t->width = d->width;
    t->height = d->height;
    t->format = d->format;
    t->usage = d->usage;
    t->samples = samples;
    *out = t;
    return nhal_status_ok();
}

static void texture_destroy(nhal_gfx_texture* t) {
    if (!t) return;
    t->mtl = nil;
    nhal_mtl_free(t, sizeof *t);
}

static nhal_status texture_write(nhal_gfx_texture* t, const void* data, uint32_t bytes_per_row) {
    if (!t || !data) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Textur oder Daten fehlen");
    if (!(t->usage & NHAL_GFX_TEXTURE_COPY_DST))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "texture_write braucht Usage COPY_DST");
    const uint32_t bpp = nhal_gfx_format_bytes(t->format);
    if (bytes_per_row < t->width * bpp || bytes_per_row % bpp)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "bytes_per_row zu klein oder nicht durch Pixelgröße teilbar");
    const uint64_t size = (uint64_t)bytes_per_row * t->height;
    [upload_blit() copyFromBuffer:staging_with(data, size) sourceOffset:0 sourceBytesPerRow:bytes_per_row
               sourceBytesPerImage:(NSUInteger)size sourceSize:MTLSizeMake(t->width, t->height, 1) toTexture:t->mtl
                  destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    return nhal_status_ok();
}

static nhal_status sampler_create(const nhal_gfx_sampler_desc* d, nhal_gfx_sampler** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    const MTLSamplerMinMagFilter f = d->filter == NHAL_GFX_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
                                                                         : MTLSamplerMinMagFilterNearest;
    sd.minFilter = f;
    sd.magFilter = f;
    const MTLSamplerAddressMode a = d->address == NHAL_GFX_ADDRESS_REPEAT ? MTLSamplerAddressModeRepeat
                                                                          : MTLSamplerAddressModeClampToEdge;
    sd.sAddressMode = a;
    sd.tAddressMode = a;
    nhal_gfx_sampler* sm = (nhal_gfx_sampler*)nhal_mtl_alloc(sizeof *sm);
    if (!sm) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Sampler");
    sm->mtl = [nhal_mtl.device newSamplerStateWithDescriptor:sd];
    *out = sm;
    return nhal_status_ok();
}

static void sampler_destroy(nhal_gfx_sampler* s) {
    if (!s) return;
    s->mtl = nil;
    nhal_mtl_free(s, sizeof *s);
}

/* ── Shader, Pipelines ─────────────────────────────────────────────────── */

static nhal_status shader_create(const nhal_gfx_shader_desc* d, nhal_gfx_shader** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if (!d->msl) return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Shader hat keinen MSL-Quelltext (Metal braucht MSL)");
    NSError* err = nil;
    id<MTLLibrary> lib = [nhal_mtl.device newLibraryWithSource:[NSString stringWithUTF8String:d->msl] options:nil
                                                         error:&err];
    if (!lib) {
        nhal_mtl_push_error(err.localizedDescription.UTF8String);
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "MSL ließ sich nicht übersetzen (Details: take_error)");
    }
    nhal_gfx_shader* sh = (nhal_gfx_shader*)nhal_mtl_alloc(sizeof *sh);
    if (!sh) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Shader");
    sh->library = lib;
    *out = sh;
    return nhal_status_ok();
}

static void shader_destroy(nhal_gfx_shader* s) {
    if (!s) return;
    s->library = nil;
    nhal_mtl_free(s, sizeof *s);
}

static nhal_status pipeline_failed(NSError* err) {
    nhal_mtl_push_error(err ? err.localizedDescription.UTF8String : "Pipeline-Fehler");
    return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Metal lehnt die Pipeline ab (Details: take_error)");
}

static nhal_status render_pipeline_create(const nhal_gfx_render_pipeline_desc* d, nhal_gfx_pipeline** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    const nhal_gfx_layout* groups[NHAL_GFX_MAX_GROUPS];
    nhal_mtl_group_layouts(d->groups, groups);
    nhal_gfx_pipeline_layout layout;
    if ((s = nhal_gfx_make_pipeline_layout(d->bindings, d->binding_count, groups, d->constants_size,
                                           NHAL_GFX_STAGE_VERTEX | NHAL_GFX_STAGE_FRAGMENT,
                                           "Render-Bindungen nur für VERTEX und FRAGMENT", &layout))
            .code)
        return s;
    if ((s = nhal_gfx_check_vertex_layouts(d->vertex_buffers, d->vertex_buffer_count)).code) return s;
    nhal_gfx_pass_shape shape;
    if ((s = nhal_gfx_check_targets(d->color_count, d->colors, d->depth_format, d->fragment_entry, d->depth_test,
                                    d->depth_compare, d->sample_count, &shape))
            .code)
        return s;
    if (!d->shader || !d->vertex_entry) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Shader oder vertex_entry fehlt");

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = nhal_mtl_function(d->shader, d->vertex_entry);
    if (!pd.vertexFunction) return nhal_status_error(NHAL_ERR_NOT_FOUND, "vertex_entry fehlt im Shader");
    if (d->fragment_entry) {
        pd.fragmentFunction = nhal_mtl_function(d->shader, d->fragment_entry);
        if (!pd.fragmentFunction) return nhal_status_error(NHAL_ERR_NOT_FOUND, "fragment_entry fehlt im Shader");
    }
    if (d->vertex_buffer_count) {
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < d->vertex_buffer_count; ++i) {
            const nhal_gfx_vertex_buffer_layout* vb = &d->vertex_buffers[i];
            const NSUInteger index = NHAL_GFX_MSL_VERTEX_BUFFER_BASE + i;
            vd.layouts[index].stride = vb->stride;
            vd.layouts[index].stepFunction = vb->per_instance ? MTLVertexStepFunctionPerInstance
                                                              : MTLVertexStepFunctionPerVertex;
            for (uint32_t a = 0; a < vb->attribute_count; ++a) {
                const nhal_gfx_vertex_attribute* at = &vb->attributes[a];
                vd.attributes[at->location].format = vertex_format(at->format);
                vd.attributes[at->location].offset = at->offset;
                vd.attributes[at->location].bufferIndex = index;
            }
        }
        pd.vertexDescriptor = vd;
    }
    for (uint32_t i = 0; i < d->color_count; ++i) nhal_mtl_configure_color(pd.colorAttachments[i], &d->colors[i]);
    if (d->depth_format != NHAL_GFX_FORMAT_NONE) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.rasterSampleCount = shape.samples;
    switch (d->topology) {
    case NHAL_GFX_LINES: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
    case NHAL_GFX_POINTS: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
    default: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> state = [nhal_mtl.device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!state) return pipeline_failed(err);

    nhal_gfx_pipeline* p = (nhal_gfx_pipeline*)nhal_mtl_alloc(sizeof *p);
    if (!p) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Pipeline");
    p->kind = NHAL_MTL_RENDER;
    p->render = state;
    p->depth = nhal_mtl_depth_state(d->depth_test, d->depth_write, d->depth_compare);
    switch (d->topology) {
    case NHAL_GFX_TRIANGLE_STRIP: p->primitive = MTLPrimitiveTypeTriangleStrip; break;
    case NHAL_GFX_LINES: p->primitive = MTLPrimitiveTypeLine; break;
    case NHAL_GFX_POINTS: p->primitive = MTLPrimitiveTypePoint; break;
    default: p->primitive = MTLPrimitiveTypeTriangle; break;
    }
    p->cull = nhal_mtl_cull(d->cull);
    p->winding = d->front_face == NHAL_GFX_FRONT_CW ? MTLWindingClockwise : MTLWindingCounterClockwise;
    p->shape = shape;
    p->layout = layout;
    *out = p;
    return nhal_status_ok();
}

static nhal_status compute_pipeline_create(const nhal_gfx_compute_pipeline_desc* d, nhal_gfx_pipeline** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    const nhal_gfx_layout* groups[NHAL_GFX_MAX_GROUPS];
    nhal_mtl_group_layouts(d->groups, groups);
    nhal_gfx_pipeline_layout layout;
    if ((s = nhal_gfx_make_pipeline_layout(d->bindings, d->binding_count, groups, d->constants_size,
                                           NHAL_GFX_STAGE_COMPUTE, "Compute-Bindungen brauchen stages = COMPUTE",
                                           &layout))
            .code)
        return s;
    const uint32_t* wg = d->workgroup_size;
    if (!wg[0] || !wg[1] || !wg[2] || (uint64_t)wg[0] * wg[1] * wg[2] > 256u)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "workgroup_size: jede Achse > 0, zusammen höchstens 256");
    id<MTLFunction> fn = nhal_mtl_function(d->shader, d->entry);
    if (!fn) return nhal_status_error(NHAL_ERR_NOT_FOUND, "Compute-Einstiegspunkt fehlt im Shader");
    NSError* err = nil;
    id<MTLComputePipelineState> state = [nhal_mtl.device newComputePipelineStateWithFunction:fn error:&err];
    if (!state) return pipeline_failed(err);
    nhal_gfx_pipeline* p = (nhal_gfx_pipeline*)nhal_mtl_alloc(sizeof *p);
    if (!p) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Pipeline");
    p->kind = NHAL_MTL_COMPUTE;
    p->compute = state;
    p->threads = MTLSizeMake(wg[0], wg[1], wg[2]);
    p->layout = layout;
    *out = p;
    return nhal_status_ok();
}

static void pipeline_destroy(nhal_gfx_pipeline* p) {
    if (!p) return;
    p->render = nil;
    p->compute = nil;
    p->depth = nil;
    nhal_mtl_free(p, sizeof *p);
}

/* ── Gruppen ───────────────────────────────────────────────────────────── */

static nhal_status group_layout_create(const nhal_gfx_group_layout_desc* d, nhal_gfx_group_layout** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    nhal_gfx_layout layout;
    if ((s = nhal_gfx_copy_layout(d->bindings, d->binding_count, &layout)).code) return s;
    nhal_gfx_group_layout* l = (nhal_gfx_group_layout*)nhal_mtl_alloc(sizeof *l);
    if (!l) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für das Layout");
    l->layout = layout;
    *out = l;
    return nhal_status_ok();
}

static void group_layout_destroy(nhal_gfx_group_layout* l) { nhal_mtl_free(l, sizeof *l); }

static void free_group(nhal_gfx_group* g) {
    for (uint32_t i = 0; i < NHAL_GFX_MAX_BINDINGS; ++i) {
        g->items[i].buffer = nil;
        g->items[i].texture = nil;
        g->items[i].sampler = nil;
    }
    nhal_mtl_free(g, sizeof *g);
}

static nhal_status group_create(const nhal_gfx_group_desc* d, nhal_gfx_group** out) {
    if (!out || !d || !d->layout) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc, layout oder out fehlt");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    const nhal_gfx_layout* l = &d->layout->layout;
    nhal_gfx_slot slots[NHAL_GFX_MAX_BINDINGS];
    memset(slots, 0, sizeof slots);
    nhal_gfx_group* g = (nhal_gfx_group*)nhal_mtl_alloc(sizeof *g);
    if (!g) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Gruppe");
    g->layout = *l;
    for (uint32_t i = 0; i < d->entry_count && i < NHAL_GFX_MAX_BINDINGS && d->entries; ++i) {
        const nhal_gfx_group_entry* e = &d->entries[i];
        if (e->slot >= NHAL_GFX_MAX_BINDINGS) break; /* meldet nhal_gfx_check_group */
        nhal_gfx_binding_kind kind = (nhal_gfx_binding_kind)0;
        for (uint32_t k = 0; k < l->count; ++k) {
            if (l->bindings[k].slot == e->slot) kind = l->bindings[k].kind;
        }
        nhal_mtl_group_item* it = &g->items[g->count++];
        it->slot = e->slot;
        it->kind = kind;
        if (kind >= NHAL_GFX_BIND_UNIFORM_BUFFER && kind <= NHAL_GFX_BIND_READ_STORAGE_BUFFER && e->buffer) {
            if (e->offset >= e->buffer->size || (e->size && e->size > e->buffer->size - e->offset)) {
                free_group(g);
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungsbereich außerhalb des Puffers");
            }
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_BUFFER, e->buffer->usage, NHAL_GFX_FORMAT_NONE, e->offset };
            it->buffer = e->buffer->mtl;
            it->offset = (NSUInteger)e->offset;
        } else if ((kind == NHAL_GFX_BIND_SAMPLED_TEXTURE || kind == NHAL_GFX_BIND_STORAGE_TEXTURE) && e->texture) {
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_TEXTURE, e->texture->usage, e->texture->format, 0 };
            it->texture = e->texture->mtl;
        } else if (kind == NHAL_GFX_BIND_SAMPLER && e->sampler) {
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_SAMPLER, 0, NHAL_GFX_FORMAT_NONE, 0 };
            it->sampler = e->sampler->mtl;
        }
    }
    if ((s = nhal_gfx_check_group(l, d->entries, d->entry_count, slots)).code) {
        free_group(g);
        return s;
    }
    *out = g;
    return nhal_status_ok();
}

static void group_destroy(nhal_gfx_group* g) {
    if (g) free_group(g);
}

/* ── Abfragen ──────────────────────────────────────────────────────────── */

static bool has_timestamps(void) {
    return nhal_mtl.timestamps && [nhal_mtl.device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
}

static nhal_status query_set_create(const nhal_gfx_query_set_desc* d, nhal_gfx_query_set** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_query_set_desc(d, has_timestamps())).code) return s;
    nhal_gfx_query_set* q = (nhal_gfx_query_set*)nhal_mtl_alloc(sizeof *q);
    if (!q) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Abfragen");
    q->desc = *d;
    if (d->type == NHAL_GFX_QUERY_OCCLUSION) {
        q->visibility = [nhal_mtl.device newBufferWithLength:(NSUInteger)d->count * 8u options:MTLResourceStorageModeShared];
        if (q->visibility) memset(q->visibility.contents, 0, (size_t)d->count * 8u);
    } else {
        MTLCounterSampleBufferDescriptor* cd = [MTLCounterSampleBufferDescriptor new];
        cd.counterSet = nhal_mtl.timestamps;
        cd.sampleCount = d->count;
        cd.storageMode = MTLStorageModeShared;
        NSError* err = nil;
        q->counters = [nhal_mtl.device newCounterSampleBufferWithDescriptor:cd error:&err];
        if (!q->counters && err) nhal_mtl_push_error(err.localizedDescription.UTF8String);
    }
    if (!q->visibility && !q->counters) {
        nhal_mtl_free(q, sizeof *q);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Metal konnte die Abfragen nicht anlegen");
    }
    *out = q;
    return nhal_status_ok();
}

static void query_set_destroy(nhal_gfx_query_set* q) {
    if (!q) return;
    q->visibility = nil;
    q->counters = nil;
    nhal_mtl_free(q, sizeof *q);
}

/* ── Befehlslisten ─────────────────────────────────────────────────────── */

static nhal_status commands_begin(nhal_gfx_commands** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    nhal_gfx_commands* c = (nhal_gfx_commands*)nhal_mtl_alloc(sizeof *c);
    if (!c) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Befehlsliste");
    c->cb = [nhal_mtl.queue commandBuffer];
    c->error = nhal_status_ok();
    c->pass.owner = c;
    *out = c;
    return nhal_status_ok();
}

static void end_blit(nhal_gfx_commands* c) {
    if (c->blit) {
        [c->blit endEncoding];
        c->blit = nil;
    }
}

static void free_commands(nhal_gfx_commands* c) {
    c->pass.render = nil;
    c->pass.compute = nil;
    c->pass.index = nil;
    c->blit = nil;
    c->cb = nil;
    nhal_mtl_free(c, sizeof *c);
}

static void end_encoders(nhal_gfx_pass* p) {
    if (p->render) [p->render endEncoding];
    if (p->compute) [p->compute endEncoding];
    p->render = nil;
    p->compute = nil;
    p->index = nil;
    p->kind = NHAL_MTL_PASS_NONE;
}

static nhal_status commands_submit(nhal_gfx_commands* c, uint64_t* serial) {
    if (serial) *serial = 0;
    if (!c) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Befehlsliste ist NULL");
    if (c->pass.kind != NHAL_MTL_PASS_NONE) {
        end_encoders(&c->pass);
        nhal_mtl_record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "submit mit offenem Pass"));
    }
    end_blit(c);
    const nhal_status s = c->error;
    if (s.code == NHAL_OK) {
        flush_uploads();
        const uint64_t value = ++nhal_mtl.serial;
        id<MTLSharedEvent> event = nhal_mtl.event;
        [c->cb encodeSignalEvent:event value:value];
        [c->cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
          if (done.status == MTLCommandBufferStatusError) {
              nhal_mtl_push_error(done.error.localizedDescription.UTF8String);
              /* Abgebrochene Listen signalisieren nicht – sonst wartete wait() ewig. */
              if (event.signaledValue < value) event.signaledValue = value;
          }
        }];
        [c->cb commit];
        if (serial) *serial = value;
    }
    free_commands(c); /* nicht abgeschickte Listen gibt ARC einfach frei */
    return s;
}

static uint64_t gfx_completed(void) { return nhal_mtl.event ? nhal_mtl.event.signaledValue : 0; }

static nhal_status gfx_wait(uint64_t serial, uint64_t timeout_ns) {
    if (serial > nhal_mtl.serial) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Seriennummer wurde nie vergeben");
    if (gfx_completed() >= serial) return nhal_status_ok();
    if (timeout_ns == 0) return nhal_status_error(NHAL_ERR_BUSY, "GPU noch nicht so weit");
    if (@available(macOS 12.0, *)) {
        const uint64_t ms = timeout_ns == NHAL_GFX_WAIT_FOREVER ? UINT64_MAX : (timeout_ns + 999999u) / 1000000u;
        if ([nhal_mtl.event waitUntilSignaledValue:serial timeoutMS:ms]) return nhal_status_ok();
    } else {
        const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        while (gfx_completed() < serial) {
            if (timeout_ns != NHAL_GFX_WAIT_FOREVER && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start >= timeout_ns) break;
            usleep(50);
        }
    }
    return gfx_completed() >= serial ? nhal_status_ok()
                                     : nhal_status_error(NHAL_ERR_BUSY, "Zeit abgelaufen, GPU noch nicht so weit");
}

static bool recording(nhal_gfx_commands* c) {
    if (!c) return false;
    if (c->pass.kind != NHAL_MTL_PASS_NONE) {
        nhal_mtl_record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kopieren geht nicht in einem offenen Pass"));
        return false;
    }
    if (!c->blit) c->blit = [c->cb blitCommandEncoder];
    return true;
}

static void commands_copy_buffer(nhal_gfx_commands* c, nhal_gfx_buffer* src, uint64_t so, nhal_gfx_buffer* dst,
                                 uint64_t dof, uint64_t size) {
    if (!c) return;
    if (!src || !dst) REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Quelle oder Ziel fehlt"));
    if (!(src->usage & NHAL_GFX_BUFFER_COPY_SRC) || !(dst->usage & NHAL_GFX_BUFFER_COPY_DST))
        REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "copy_buffer braucht COPY_SRC/COPY_DST"));
    nhal_status s = nhal_gfx_check_range(src->size, so, size);
    if (!s.code) s = nhal_gfx_check_range(dst->size, dof, size);
    if (s.code) REC(c, s);
    if (src == dst) REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "copy_buffer im selben Puffer"));
    if (!recording(c)) return;
    [c->blit copyFromBuffer:src->mtl sourceOffset:(NSUInteger)so toBuffer:dst->mtl destinationOffset:(NSUInteger)dof
                       size:(NSUInteger)size];
}

static void commands_copy_texture_to_buffer(nhal_gfx_commands* c, nhal_gfx_texture* src, nhal_gfx_buffer* dst,
                                            uint64_t offset, uint32_t bpr) {
    if (!c) return;
    if (!src || !dst) REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Quelle oder Ziel fehlt"));
    if (!(src->usage & NHAL_GFX_TEXTURE_COPY_SRC) || !(dst->usage & NHAL_GFX_BUFFER_COPY_DST))
        REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "copy_texture_to_buffer braucht COPY_SRC/COPY_DST"));
    if (nhal_gfx_is_depth(src->format))
        REC(c, nhal_status_error(NHAL_ERR_UNSUPPORTED, "Tiefentexturen lassen sich nicht kopieren"));
    const uint32_t bpp = nhal_gfx_format_bytes(src->format);
    if (bpr % NHAL_GFX_ROW_ALIGN || bpr < src->width * bpp)
        REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "bytes_per_row: Vielfaches von 256 und mindestens eine Zeile"));
    const uint64_t need = (uint64_t)bpr * (src->height - 1u) + (uint64_t)src->width * bpp;
    if (offset % 4u || offset > dst->size || need > dst->size - offset)
        REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zielpuffer zu klein"));
    if (!recording(c)) return;
    [c->blit copyFromTexture:src->mtl sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(src->width, src->height, 1) toBuffer:dst->mtl
           destinationOffset:(NSUInteger)offset destinationBytesPerRow:bpr
         destinationBytesPerImage:(NSUInteger)bpr * src->height];
}

static void commands_resolve_queries(nhal_gfx_commands* c, nhal_gfx_query_set* q, uint32_t first, uint32_t count,
                                     nhal_gfx_buffer* dst, uint64_t offset) {
    if (!c) return;
    if (!q || !dst) REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Abfragen oder Ziel fehlen"));
    const nhal_status s = nhal_gfx_check_resolve(&q->desc, first, count, dst->size, dst->usage, offset);
    if (s.code) REC(c, s);
    if (!recording(c)) return;
    if (q->visibility) {
        [c->blit copyFromBuffer:q->visibility sourceOffset:(NSUInteger)first * 8u toBuffer:dst->mtl
              destinationOffset:(NSUInteger)offset size:(NSUInteger)count * 8u];
    } else {
        [c->blit resolveCounters:q->counters inRange:NSMakeRange(first, count) destinationBuffer:dst->mtl
               destinationOffset:(NSUInteger)offset];
    }
}

/* ── Passes ────────────────────────────────────────────────────────────── */

static nhal_gfx_pass* open_pass(nhal_gfx_commands* c) {
    if (!c) return NULL;
    if (c->pass.kind != NHAL_MTL_PASS_NONE) {
        nhal_mtl_record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Es ist schon ein Pass offen"));
        return NULL;
    }
    end_blit(c);
    nhal_gfx_pass* p = &c->pass;
    memset(p->slots, 0, sizeof p->slots);
    memset(p->groups, 0, sizeof p->groups);
    memset(p->applied, 0, sizeof p->applied);
    memset(&p->shape, 0, sizeof p->shape);
    p->pipeline = NULL;
    p->constants_size = 0;
    p->vertex_buffers = 0;
    p->index = nil;
    p->width = p->height = 0;
    p->scissor_empty = false;
    p->occlusion = NULL;
    p->occlusion_open = false;
    return p;
}

static nhal_gfx_tex_info tex_info(const nhal_gfx_texture* t) {
    nhal_gfx_tex_info i = { false, 0, 0, 0, 0, NHAL_GFX_FORMAT_NONE };
    if (t) i = (nhal_gfx_tex_info){ true, t->width, t->height, t->samples ? t->samples : 1u, t->usage, t->format };
    return i;
}

static nhal_gfx_pass* fail(nhal_gfx_commands* c, nhal_status s) {
    nhal_mtl_record(c, s);
    return NULL;
}

static void apply_sample_buffer(MTLRenderPassDescriptor* rp, const nhal_gfx_pass_timestamps* t) {
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = t->set->counters;
    a.startOfVertexSampleIndex = t->begin_index;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = MTLCounterDontSample;
    a.endOfFragmentSampleIndex = t->end_index;
}

static nhal_gfx_pass* render_pass_begin(nhal_gfx_commands* c, const nhal_gfx_render_pass_desc* d) {
    nhal_gfx_pass* p = open_pass(c);
    if (!p) return NULL;
    if (!d) return fail(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Render-Pass ohne Beschreibung"));
    nhal_gfx_tex_info color[NHAL_GFX_MAX_COLOR_TARGETS], resolve[NHAL_GFX_MAX_COLOR_TARGETS];
    const uint32_t n = d->color_count < NHAL_GFX_MAX_COLOR_TARGETS ? d->color_count : NHAL_GFX_MAX_COLOR_TARGETS;
    for (uint32_t i = 0; i < NHAL_GFX_MAX_COLOR_TARGETS; ++i) {
        color[i] = tex_info(i < n ? d->colors[i].texture : NULL);
        resolve[i] = tex_info(i < n ? d->colors[i].resolve : NULL);
    }
    const nhal_gfx_tex_info depth = tex_info(d->depth);
    nhal_status s = nhal_gfx_check_render_pass(d, color, resolve, &depth, &p->shape, &p->width, &p->height);
    if (s.code) return fail(c, s);
    if (d->occlusion && d->occlusion->desc.type != NHAL_GFX_QUERY_OCCLUSION)
        return fail(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "occlusion braucht ein OCCLUSION-Set"));
    if ((s = nhal_gfx_check_timestamps(&d->timestamps, d->timestamps.set ? &d->timestamps.set->desc : NULL)).code)
        return fail(c, s);

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (uint32_t i = 0; i < n; ++i) {
        const nhal_gfx_color_attachment* a = &d->colors[i];
        MTLRenderPassColorAttachmentDescriptor* ca = rp.colorAttachments[i];
        ca.texture = a->texture->mtl;
        ca.loadAction = a->load == NHAL_GFX_LOAD_KEEP ? MTLLoadActionLoad : MTLLoadActionClear;
        ca.clearColor = MTLClearColorMake(a->clear_color[0], a->clear_color[1], a->clear_color[2], a->clear_color[3]);
        const bool keep = a->store == NHAL_GFX_STORE_KEEP;
        if (a->resolve) {
            ca.resolveTexture = a->resolve->mtl;
            ca.storeAction = keep ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
        } else {
            ca.storeAction = keep ? MTLStoreActionStore : MTLStoreActionDontCare;
        }
    }
    if (d->depth) {
        rp.depthAttachment.texture = d->depth->mtl;
        rp.depthAttachment.loadAction = d->depth_load == NHAL_GFX_LOAD_KEEP ? MTLLoadActionLoad : MTLLoadActionClear;
        rp.depthAttachment.clearDepth = d->clear_depth;
        rp.depthAttachment.storeAction = d->depth_store == NHAL_GFX_STORE_KEEP ? MTLStoreActionStore
                                                                               : MTLStoreActionDontCare;
    }
    if (d->occlusion) { /* Werte wie dokumentiert zurücksetzen, dann zählen */
        id<MTLBlitCommandEncoder> blit = [c->cb blitCommandEncoder];
        [blit fillBuffer:d->occlusion->visibility range:NSMakeRange(0, d->occlusion->desc.count * 8u) value:0];
        [blit endEncoding];
        rp.visibilityResultBuffer = d->occlusion->visibility;
        p->occlusion = d->occlusion;
    }
    if (d->timestamps.set) apply_sample_buffer(rp, &d->timestamps);
    p->render = [c->cb renderCommandEncoderWithDescriptor:rp];
    [p->render setFrontFacingWinding:MTLWindingCounterClockwise];
    p->kind = NHAL_MTL_PASS_RENDER;
    return p;
}

static nhal_gfx_pass* compute_pass_begin(nhal_gfx_commands* c, const nhal_gfx_compute_pass_desc* d) {
    nhal_gfx_pass* p = open_pass(c);
    if (!p) return NULL;
    if (d && d->timestamps.set) {
        const nhal_status s = nhal_gfx_check_timestamps(&d->timestamps, &d->timestamps.set->desc);
        if (s.code) return fail(c, s);
        MTLComputePassDescriptor* cp = [MTLComputePassDescriptor computePassDescriptor];
        cp.sampleBufferAttachments[0].sampleBuffer = d->timestamps.set->counters;
        cp.sampleBufferAttachments[0].startOfEncoderSampleIndex = d->timestamps.begin_index;
        cp.sampleBufferAttachments[0].endOfEncoderSampleIndex = d->timestamps.end_index;
        p->compute = [c->cb computeCommandEncoderWithDescriptor:cp];
    } else {
        p->compute = [c->cb computeCommandEncoder];
    }
    p->kind = NHAL_MTL_PASS_COMPUTE;
    return p;
}

static void pass_end(nhal_gfx_pass* p) {
    if (!p || p->kind == NHAL_MTL_PASS_NONE) return;
    if (p->occlusion_open) {
        [p->render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "pass_end mit offener Occlusion-Abfrage"));
    }
    end_encoders(p);
}

static bool pass_open(nhal_gfx_pass* p) {
    if (!p) return false;
    if (p->kind == NHAL_MTL_PASS_NONE) {
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pass ist schon beendet"));
        return false;
    }
    return true;
}

static void pass_set_pipeline(nhal_gfx_pass* p, nhal_gfx_pipeline* pl) {
    if (!pass_open(p)) return;
    if (!pl) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline ist NULL"));
    const bool compute = pl->kind == NHAL_MTL_COMPUTE;
    if (compute != (p->kind == NHAL_MTL_PASS_COMPUTE))
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline-Art passt nicht zum Pass"));
    if (compute) {
        [p->compute setComputePipelineState:pl->compute];
    } else {
        if (!nhal_gfx_shape_equal(&pl->shape, &p->shape))
            REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                            "Formate oder sample_count der Pipeline passen nicht zu den Zielen"));
        [p->render setRenderPipelineState:pl->render];
        [p->render setDepthStencilState:pl->depth];
        [p->render setCullMode:pl->cull];
        [p->render setFrontFacingWinding:pl->winding];
    }
    p->pipeline = pl;
}

static bool check_slot(nhal_gfx_pass* p, uint32_t slot) {
    if (!pass_open(p)) return false;
    if (slot >= NHAL_GFX_MAX_BINDINGS) {
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Slot über NHAL_GFX_MAX_BINDINGS"));
        return false;
    }
    return true;
}

/* Bindet auf allen Stufen des Passes (Compute bzw. Vertex, Fragment, Object, Mesh). */
static void bind_buffer(nhal_gfx_pass* p, uint32_t slot, id<MTLBuffer> b, NSUInteger off) {
    if (p->compute) {
        [p->compute setBuffer:b offset:off atIndex:slot];
        return;
    }
    [p->render setVertexBuffer:b offset:off atIndex:slot];
    [p->render setFragmentBuffer:b offset:off atIndex:slot];
    if (nhal_mtl.mesh_capable) {
        if (@available(macOS 13.0, *)) {
            [p->render setObjectBuffer:b offset:off atIndex:slot];
            [p->render setMeshBuffer:b offset:off atIndex:slot];
        }
    }
}

static void bind_texture(nhal_gfx_pass* p, uint32_t slot, id<MTLTexture> t) {
    if (p->compute) {
        [p->compute setTexture:t atIndex:slot];
        return;
    }
    [p->render setVertexTexture:t atIndex:slot];
    [p->render setFragmentTexture:t atIndex:slot];
    if (nhal_mtl.mesh_capable) {
        if (@available(macOS 13.0, *)) {
            [p->render setObjectTexture:t atIndex:slot];
            [p->render setMeshTexture:t atIndex:slot];
        }
    }
}

static void bind_sampler(nhal_gfx_pass* p, uint32_t slot, id<MTLSamplerState> s) {
    if (p->compute) {
        [p->compute setSamplerState:s atIndex:slot];
        return;
    }
    [p->render setVertexSamplerState:s atIndex:slot];
    [p->render setFragmentSamplerState:s atIndex:slot];
    if (nhal_mtl.mesh_capable) {
        if (@available(macOS 13.0, *)) {
            [p->render setObjectSamplerState:s atIndex:slot];
            [p->render setMeshSamplerState:s atIndex:slot];
        }
    }
}

static void pass_set_buffer(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_buffer* b, uint64_t offset, uint64_t size) {
    if (!check_slot(p, slot)) return;
    if (!b) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer ist NULL"));
    if (offset >= b->size || (size && size > b->size - offset))
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungsbereich außerhalb des Puffers"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_BUFFER, b->usage, NHAL_GFX_FORMAT_NONE, offset };
    p->applied[slot] = NULL;
    bind_buffer(p, slot, b->mtl, (NSUInteger)offset);
}

static void pass_set_texture(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_texture* t) {
    if (!check_slot(p, slot)) return;
    if (!t) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Textur ist NULL"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_TEXTURE, t->usage, t->format, 0 };
    p->applied[slot] = NULL;
    bind_texture(p, slot, t->mtl);
}

static void pass_set_sampler(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_sampler* s) {
    if (!check_slot(p, slot)) return;
    if (!s) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Sampler ist NULL"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_SAMPLER, 0, NHAL_GFX_FORMAT_NONE, 0 };
    p->applied[slot] = NULL;
    bind_sampler(p, slot, s->mtl);
}

static void pass_set_group(nhal_gfx_pass* p, uint32_t index, nhal_gfx_group* g) {
    if (!pass_open(p)) return;
    if (index >= NHAL_GFX_MAX_GROUPS || !g)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gruppe fehlt oder Index über NHAL_GFX_MAX_GROUPS"));
    p->groups[index] = g; /* gebunden wird beim nächsten Draw */
}

static void pass_set_constants(nhal_gfx_pass* p, const void* data, uint32_t size) {
    if (!pass_open(p)) return;
    const nhal_status s = nhal_gfx_check_constants(data, size);
    if (s.code) REC(p->owner, s);
    const NSUInteger i = NHAL_GFX_MSL_CONSTANTS_BUFFER;
    if (p->compute) {
        [p->compute setBytes:data length:size atIndex:i];
    } else {
        [p->render setVertexBytes:data length:size atIndex:i];
        [p->render setFragmentBytes:data length:size atIndex:i];
        if (nhal_mtl.mesh_capable) {
            if (@available(macOS 13.0, *)) {
                [p->render setObjectBytes:data length:size atIndex:i];
                [p->render setMeshBytes:data length:size atIndex:i];
            }
        }
    }
    p->constants_size = size;
}

static bool render_only(nhal_gfx_pass* p, const char* what) {
    if (!pass_open(p)) return false;
    if (p->kind != NHAL_MTL_PASS_RENDER) {
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, what));
        return false;
    }
    return true;
}

static void pass_set_vertex_buffer(nhal_gfx_pass* p, uint32_t index, nhal_gfx_buffer* b, uint64_t offset) {
    if (!render_only(p, "Vertex-Puffer gibt es nur im Render-Pass")) return;
    if (index >= NHAL_GFX_MAX_VERTEX_BUFFERS || !b || !(b->usage & NHAL_GFX_BUFFER_VERTEX) || offset % 4u ||
        offset >= b->size)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertex-Puffer: Index, Usage VERTEX oder Offset ungültig"));
    [p->render setVertexBuffer:b->mtl offset:(NSUInteger)offset atIndex:NHAL_GFX_MSL_VERTEX_BUFFER_BASE + index];
    p->vertex_buffers |= 1u << index;
}

static void pass_set_index_buffer(nhal_gfx_pass* p, nhal_gfx_buffer* b, nhal_gfx_index_format f, uint64_t offset) {
    if (!render_only(p, "Index-Puffer gibt es nur im Render-Pass")) return;
    const uint64_t isz = f == NHAL_GFX_INDEX_UINT32 ? 4u : 2u;
    if (!b || !(b->usage & NHAL_GFX_BUFFER_INDEX) || offset % isz || offset >= b->size)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Index-Puffer: Usage INDEX oder Offset ungültig"));
    p->index = b->mtl;
    p->index_offset = offset;
    p->index_type = f == NHAL_GFX_INDEX_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

static void pass_set_viewport(nhal_gfx_pass* p, float x, float y, float w, float h) {
    if (!render_only(p, "Viewport gibt es nur im Render-Pass")) return;
    if (!(w > 0.0f) || !(h > 0.0f)) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Viewport ohne Fläche"));
    [p->render setViewport:(MTLViewport){ x, y, w, h, 0.0, 1.0 }];
}

static void pass_set_scissor(nhal_gfx_pass* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!render_only(p, "Scissor gibt es nur im Render-Pass")) return;
    const nhal_status s = nhal_gfx_check_scissor(x, y, w, h, p->width, p->height);
    if (s.code) REC(p->owner, s);
    p->scissor_empty = !w || !h; /* Metal kennt kein leeres Rechteck – dann zeichnet NHAL nichts */
    if (!p->scissor_empty) [p->render setScissorRect:(MTLScissorRect){ x, y, w, h }];
}

bool nhal_mtl_ready_to_draw(nhal_gfx_pass* p, nhal_mtl_pipeline_kind want) {
    if (!pass_open(p)) return false;
    if (!p->pipeline || p->pipeline->kind != want) {
        nhal_mtl_record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine oder eine unpassende Pipeline gesetzt"));
        return false;
    }
    const nhal_gfx_pipeline_layout* pl = &p->pipeline->layout;
    const nhal_gfx_layout* bound[NHAL_GFX_MAX_GROUPS];
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) bound[g] = p->groups[g] ? &p->groups[g]->layout : NULL;
    nhal_status s = nhal_gfx_check_bound(&pl->slots, p->slots);
    if (!s.code) s = nhal_gfx_check_groups_bound(pl, bound, p->constants_size);
    if (s.code) {
        nhal_mtl_record(p->owner, s);
        return false;
    }
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) {
        const nhal_gfx_group* grp = p->groups[g];
        if (!(pl->group_mask & (1u << g))) continue;
        for (uint32_t i = 0; i < grp->count; ++i) {
            const nhal_mtl_group_item* it = &grp->items[i];
            if (p->applied[it->slot] == grp) continue;
            if (it->buffer) bind_buffer(p, it->slot, it->buffer, it->offset);
            if (it->texture) bind_texture(p, it->slot, it->texture);
            if (it->sampler) bind_sampler(p, it->slot, it->sampler);
            p->applied[it->slot] = grp;
        }
    }
    return !p->scissor_empty;
}

static void pass_draw(nhal_gfx_pass* p, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_RENDER) || !vc || !ic) return;
    [p->render drawPrimitives:p->pipeline->primitive vertexStart:fv vertexCount:vc instanceCount:ic baseInstance:fi];
}

static void pass_draw_indexed(nhal_gfx_pass* p, uint32_t count, uint32_t ic, uint32_t first, int32_t base,
                              uint32_t fi) {
    if (p && p->kind == NHAL_MTL_PASS_RENDER && !p->index)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kein Index-Puffer gesetzt"));
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_RENDER) || !count || !ic) return;
    const NSUInteger isz = p->index_type == MTLIndexTypeUInt32 ? 4u : 2u;
    [p->render drawIndexedPrimitives:p->pipeline->primitive indexCount:count indexType:p->index_type
                         indexBuffer:p->index indexBufferOffset:(NSUInteger)p->index_offset + first * isz
                       instanceCount:ic baseVertex:base baseInstance:fi];
}

static bool indirect_ok(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset, uint64_t bytes) {
    if (!pass_open(p)) return false;
    const nhal_status s = b ? nhal_gfx_check_indirect(b->size, b->usage, offset, bytes)
                            : nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Argumentpuffer ist NULL");
    if (s.code) nhal_mtl_record(p->owner, s);
    return s.code == NHAL_OK;
}

static void pass_draw_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 16u) || !nhal_mtl_ready_to_draw(p, NHAL_MTL_RENDER)) return;
    [p->render drawPrimitives:p->pipeline->primitive indirectBuffer:b->mtl indirectBufferOffset:(NSUInteger)offset];
}

static void pass_draw_indexed_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 20u)) return;
    if (p->kind == NHAL_MTL_PASS_RENDER && !p->index)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kein Index-Puffer gesetzt"));
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_RENDER)) return;
    [p->render drawIndexedPrimitives:p->pipeline->primitive indexType:p->index_type indexBuffer:p->index
                   indexBufferOffset:(NSUInteger)p->index_offset indirectBuffer:b->mtl
                indirectBufferOffset:(NSUInteger)offset];
}

static void pass_dispatch(nhal_gfx_pass* p, uint32_t x, uint32_t y, uint32_t z) {
    if (!pass_open(p)) return;
    if (p->kind != NHAL_MTL_PASS_COMPUTE)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "dispatch nur im Compute-Pass"));
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_COMPUTE)) return;
    if (x > 65535u || y > 65535u || z > 65535u)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Höchstens 65535 Gruppen je Achse"));
    if (!x || !y || !z) return;
    [p->compute dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:p->pipeline->threads];
}

static void pass_dispatch_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 12u)) return;
    if (p->kind != NHAL_MTL_PASS_COMPUTE)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "dispatch nur im Compute-Pass"));
    if (!nhal_mtl_ready_to_draw(p, NHAL_MTL_COMPUTE)) return;
    [p->compute dispatchThreadgroupsWithIndirectBuffer:b->mtl indirectBufferOffset:(NSUInteger)offset
                                 threadsPerThreadgroup:p->pipeline->threads];
}

static void pass_begin_occlusion(nhal_gfx_pass* p, uint32_t index) {
    if (!render_only(p, "Occlusion-Abfragen gibt es nur im Render-Pass")) return;
    if (!p->occlusion) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Render-Pass ohne occlusion-Set"));
    if (p->occlusion_open) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Occlusion-Abfragen nicht verschachteln"));
    if (index >= p->occlusion->desc.count)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Abfrage-Index außerhalb des Sets"));
    [p->render setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)index * 8u];
    p->occlusion_open = true;
}

static void pass_end_occlusion(nhal_gfx_pass* p) {
    if (!render_only(p, "Occlusion-Abfragen gibt es nur im Render-Pass")) return;
    if (!p->occlusion_open) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine Occlusion-Abfrage offen"));
    [p->render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    p->occlusion_open = false;
}

/* ── Oberflächen ───────────────────────────────────────────────────────── */

struct nhal_gfx_surface {
    __strong CAMetalLayer* layer;
    __strong id<CAMetalDrawable> drawable;
    nhal_gfx_texture texture; /* zeigt auf drawable.texture */
    uint32_t width, height;
};

static nhal_status surface_create(const nhal_gfx_surface_desc* d, nhal_gfx_surface** out) {
    if (!out || !d || !d->handle) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc, handle oder out fehlt");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    CAMetalLayer* layer = nil;
    if (d->kind == NHAL_GFX_SURFACE_METAL_LAYER) {
        id obj = (__bridge id)d->handle;
        if (![obj isKindOfClass:[CAMetalLayer class]])
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "handle ist kein CAMetalLayer");
        layer = obj;
    } else if (d->kind == NHAL_GFX_SURFACE_NSVIEW) {
        id obj = (__bridge id)d->handle;
        if (![obj isKindOfClass:[NSView class]]) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "handle ist kein NSView");
        NSView* view = obj;
        if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
            layer = (CAMetalLayer*)view.layer;
        } else {
            layer = [CAMetalLayer layer];
            view.wantsLayer = YES;
            view.layer = layer;
        }
    } else {
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Oberflächenart gibt es auf macOS nicht (METAL_LAYER oder NSVIEW)");
    }
    nhal_gfx_surface* sf = (nhal_gfx_surface*)nhal_mtl_alloc(sizeof *sf);
    if (!sf) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Oberfläche");
    layer.device = nhal_mtl.device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO; /* wie Web: RENDER_TARGET | COPY_SRC */
    layer.allowsNextDrawableTimeout = YES;
    sf->layer = layer;
    *out = sf;
    return nhal_status_ok();
}

static void surface_destroy(nhal_gfx_surface* sf) {
    if (!sf) return;
    sf->texture.mtl = nil;
    sf->drawable = nil;
    sf->layer = nil;
    nhal_mtl_free(sf, sizeof *sf);
}

static nhal_status surface_configure(nhal_gfx_surface* sf, uint32_t w, uint32_t h, bool vsync) {
    if (!sf || !w || !h || w > 16384 || h > 16384) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Größe");
    sf->layer.drawableSize = CGSizeMake(w, h);
    sf->layer.displaySyncEnabled = vsync;
    sf->width = w;
    sf->height = h;
    return nhal_status_ok();
}

static nhal_gfx_format surface_format(const nhal_gfx_surface* sf) {
    (void)sf;
    return NHAL_GFX_FORMAT_BGRA8_UNORM;
}

static nhal_status surface_acquire(nhal_gfx_surface* sf, nhal_gfx_texture** out) {
    if (!sf || !out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Oberfläche oder out fehlt");
    *out = NULL;
    if (!sf->width) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Oberfläche ist nicht konfiguriert");
    if (sf->drawable) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vorherige Textur noch nicht präsentiert");
    @autoreleasepool {
        sf->drawable = [sf->layer nextDrawable];
    }
    if (!sf->drawable) return nhal_status_error(NHAL_ERR_BUSY, "Gerade keine Textur frei");
    sf->texture.mtl = sf->drawable.texture;
    sf->texture.width = (uint32_t)sf->texture.mtl.width;
    sf->texture.height = (uint32_t)sf->texture.mtl.height;
    sf->texture.format = NHAL_GFX_FORMAT_BGRA8_UNORM;
    sf->texture.usage = NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC;
    sf->texture.samples = 1;
    *out = &sf->texture;
    return nhal_status_ok();
}

static nhal_status surface_present(nhal_gfx_surface* sf) {
    if (!sf || !sf->drawable) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine Textur zum Präsentieren");
    flush_uploads();
    id<MTLCommandBuffer> cb = [nhal_mtl.queue commandBuffer];
    [cb presentDrawable:sf->drawable];
    [cb commit];
    sf->texture.mtl = nil;
    sf->drawable = nil;
    return nhal_status_ok();
}

/* ── Feature ───────────────────────────────────────────────────────────── */

static nhal_status feature_init(const nhal_allocator* allocator) {
    memset(&g_errors, 0, sizeof g_errors);
    g_reads = NULL;
    nhal_mtl.alloc = allocator;
    @autoreleasepool {
        nhal_mtl.device = MTLCreateSystemDefaultDevice();
        if (!nhal_mtl.device) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Metal-Gerät gefunden");
        nhal_mtl.queue = [nhal_mtl.device newCommandQueue];
        nhal_mtl.mesh_capable = [nhal_mtl.device supportsFamily:MTLGPUFamilyMetal3];
        nhal_mtl.event = [nhal_mtl.device newSharedEvent];
        nhal_mtl.serial = 0;
        nhal_mtl.timestamps = nil;
        for (id<MTLCounterSet> set in nhal_mtl.device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) nhal_mtl.timestamps = set;
        }
        [nhal_mtl.device sampleTimestamps:&nhal_mtl.cpu0 gpuTimestamp:&nhal_mtl.gpu0];
        if (!nhal_mtl.event) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Metal liefert kein MTLSharedEvent");
    }
    return nhal_status_ok();
}

static void feature_shutdown(void) {
    flush_uploads();
    if (nhal_mtl.queue) { /* auf alles warten, damit offene Lesevorgänge nicht ins Leere schreiben */
        id<MTLCommandBuffer> cb = [nhal_mtl.queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    while (g_reads) {
        read_request* r = g_reads;
        g_reads = r->next;
        r->staging = nil;
        nhal_mtl_free(r, sizeof *r);
    }
    nhal_mtl.queue = nil;
    nhal_mtl.event = nil;
    nhal_mtl.timestamps = nil;
    nhal_mtl.serial = 0;
    nhal_mtl.device = nil;
    nhal_mtl.alloc = NULL;
    nhal_mtl.mesh_capable = false;
}

static const nhal_gfx_device_v2 table = {
    sizeof(nhal_gfx_device_v2),
    gfx_ready, gfx_poll, gfx_info, gfx_take_error,
    buffer_create, buffer_destroy, buffer_write, buffer_read, buffer_mapped,
    texture_create, texture_destroy, texture_write,
    sampler_create, sampler_destroy,
    shader_create, shader_destroy,
    group_layout_create, group_layout_destroy, group_create, group_destroy,
    render_pipeline_create, compute_pipeline_create, pipeline_destroy,
    query_set_create, query_set_destroy,
    commands_begin, commands_submit, gfx_completed, gfx_wait,
    commands_copy_buffer, commands_copy_texture_to_buffer, commands_resolve_queries,
    render_pass_begin, compute_pass_begin, pass_end,
    pass_set_pipeline, pass_set_buffer, pass_set_texture, pass_set_sampler, pass_set_group, pass_set_constants,
    pass_set_vertex_buffer, pass_set_index_buffer, pass_set_viewport, pass_set_scissor,
    pass_draw, pass_draw_indexed, pass_draw_indirect, pass_draw_indexed_indirect,
    pass_dispatch, pass_dispatch_indirect, pass_begin_occlusion, pass_end_occlusion,
    surface_create, surface_destroy, surface_configure, surface_format, surface_acquire, surface_present,
};

const nhal_feature_impl nhal_impl_gfx_device = { 2u, feature_init, feature_shutdown, &table };
