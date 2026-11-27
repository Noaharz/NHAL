/*
 * gfx.device – Web (WebGPU, direkt über EM_JS, ohne fremde Bibliothek).
 *
 * Die WebGPU-Objekte liegen in einer JS-Tabelle (globalThis.nhalGpu.objs),
 * die C-Strukturen halten nur deren Nummer und die Daten für die Prüfungen.
 * Geprüft wird in C mit denselben Regeln wie unter Metal (common/gfx/validate.h).
 *
 * Asynchron: Gerät (ready), Lesen (buffer_read) und Validierungsfehler des
 * Browsers (take_error). Ergebnisse kommen erst an, wenn die Seite wieder in
 * die Ereignisschleife zurückkehrt – die App braucht also eine Hauptschleife
 * (emscripten_set_main_loop o. ä.) und ruft dort poll().
 *
 * Bindungen: Slot-Bindungen bilden Bind Group 0 und werden beim ersten draw/
 * dispatch nach einer Änderung neu gebaut. Eigene Gruppen sind fertige Bind
 * Groups. Push-Konstanten liegen in einem Uniform-Ring (Gruppe 3, dynamischer
 * Offset), der beim submit mit writeBuffer hochgeladen wird.
 *
 * Der größere JS-Teil steht in device.js (über js_libraries gelinkt).
 */
#include "nhal/features/gfx/device.h"
#include "common/gfx/validate.h"

#include <emscripten.h>
#include <string.h>

/* clang-format off */
extern void nhal_web_gpu_init(void); /* device.js */

EM_JS_DEPS(nhal_gfx_web, "$stringToUTF8,$UTF8ToString")

EM_JS(void, gpu_js_shutdown, (void), {
    const g = globalThis.nhalGpu;
    if (g && g.device) g.device.destroy();
    globalThis.nhalGpu = null;
})

EM_JS(int, gpu_js_state, (void), { return globalThis.nhalGpu.state; })
EM_JS(int, gpu_js_reason, (void), { return globalThis.nhalGpu.reason; })
EM_JS(void, gpu_js_name, (char* out, int cap), { stringToUTF8(globalThis.nhalGpu.name, out, cap); })
EM_JS(int, gpu_js_max_texture, (void), { return globalThis.nhalGpu.device.limits.maxTextureDimension2D; })
EM_JS(double, gpu_js_max_buffer, (void), { return globalThis.nhalGpu.device.limits.maxBufferSize; })
EM_JS(int, gpu_js_features, (void), { const g = globalThis.nhalGpu; return (g.ts ? 1 : 0) | (g.fi ? 2 : 0); })
EM_JS(double, gpu_js_completed, (void), { const g = globalThis.nhalGpu; return g ? g.completed : 0; })

EM_JS(int, gpu_js_take_error, (char* out, int cap), {
    const g = globalThis.nhalGpu;
    if (!g || !g.errors.length) return 0;
    const m = g.errors.shift();
    if (cap > 0) stringToUTF8(m, out, cap);
    return 1;
})

EM_JS(void, gpu_js_destroy, (int id), {
    const g = globalThis.nhalGpu;
    const o = g.objs[id];
    if (o && typeof o.destroy === "function") o.destroy();
    g.del(id);
})

EM_JS(void, gpu_js_forget, (int id), { globalThis.nhalGpu.del(id); })

EM_JS(int, gpu_js_buffer_create, (double size, int usage, const void* init), {
    const g = globalThis.nhalGpu;
    let u = 0;
    if (usage & 1) u |= GPUBufferUsage.VERTEX;
    if (usage & 2) u |= GPUBufferUsage.INDEX;
    if (usage & 4) u |= GPUBufferUsage.UNIFORM;
    if (usage & 8) u |= GPUBufferUsage.STORAGE;
    if (usage & 16) u |= GPUBufferUsage.COPY_SRC;
    if (usage & 32) u |= GPUBufferUsage.COPY_DST;
    if (usage & 128) u |= GPUBufferUsage.INDIRECT;
    if (usage & 256) u |= GPUBufferUsage.QUERY_RESOLVE;
    const b = g.device.createBuffer({ size: size, usage: u, mappedAtCreation: init !== 0 });
    if (init) {
        new Uint8Array(b.getMappedRange()).set(HEAPU8.subarray(init, init + size));
        b.unmap();
    }
    return g.add(b);
})

EM_JS(void, gpu_js_buffer_write, (int id, double offset, const void* data, double size), {
    const g = globalThis.nhalGpu;
    g.device.queue.writeBuffer(g.objs[id], offset, HEAPU8, data, size);
})

EM_JS(void, gpu_js_buffer_read, (int id, double offset, double size, void* dst, int req), {
    const g = globalThis.nhalGpu;
    const st = g.device.createBuffer({ size: size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const e = g.device.createCommandEncoder();
    e.copyBufferToBuffer(g.objs[id], offset, st, 0, size);
    g.device.queue.submit([e.finish()]);
    st.mapAsync(GPUMapMode.READ).then(() => {
        if (globalThis.nhalGpu === g) HEAPU8.set(new Uint8Array(st.getMappedRange()), dst);
        st.unmap(); st.destroy();
        g.done.push(req * 2);
    }, (err) => {
        g.errors.push("Lesen fehlgeschlagen: " + String(err));
        st.destroy();
        g.done.push(req * 2 + 1);
    });
})

EM_JS(int, gpu_js_take_done, (void), {
    const g = globalThis.nhalGpu;
    return g.done.length ? g.done.shift() : -1;
})

EM_JS(int, gpu_js_texture_create, (int w, int h, int format, int usage, int samples), {
    const g = globalThis.nhalGpu;
    let u = 0;
    if (usage & 1) u |= GPUTextureUsage.TEXTURE_BINDING;
    if (usage & 2) u |= GPUTextureUsage.STORAGE_BINDING;
    if (usage & 4) u |= GPUTextureUsage.RENDER_ATTACHMENT;
    if (usage & 8) u |= GPUTextureUsage.COPY_SRC;
    if (usage & 16) u |= GPUTextureUsage.COPY_DST;
    return g.add(g.device.createTexture({ size: [w, h], format: g.formats[format], usage: u, sampleCount: samples }));
})

EM_JS(void, gpu_js_texture_write, (int id, const void* data, int bpr, int w, int h), {
    const g = globalThis.nhalGpu;
    g.device.queue.writeTexture({ texture: g.objs[id] }, HEAPU8, { offset: data, bytesPerRow: bpr, rowsPerImage: h },
                                { width: w, height: h });
})

EM_JS(int, gpu_js_sampler_create, (int linear, int repeat), {
    const g = globalThis.nhalGpu;
    const f = linear ? "linear" : "nearest";
    const a = repeat ? "repeat" : "clamp-to-edge";
    return g.add(g.device.createSampler({ magFilter: f, minFilter: f, addressModeU: a, addressModeV: a }));
})

EM_JS(int, gpu_js_shader_create, (const char* code), {
    const g = globalThis.nhalGpu;
    const m = g.device.createShaderModule({ code: UTF8ToString(code) });
    if (m.getCompilationInfo) {
        m.getCompilationInfo().then((info) => {
            for (const msg of info.messages) {
                if (msg.type === "error") g.errors.push("WGSL " + msg.lineNum + ":" + msg.linePos + ": " + msg.message);
            }
        });
    }
    return g.add(m);
})

EM_JS(int, gpu_js_render_pipeline_create, (const uint32_t* desc), { return globalThis.nhalGpu.renderPipeline(desc); })
EM_JS(int, gpu_js_compute_pipeline_create, (const uint32_t* desc), { return globalThis.nhalGpu.computePipeline(desc); })

EM_JS(int, gpu_js_group_layout_create, (const uint32_t* words, int count), {
    const g = globalThis.nhalGpu;
    return g.add({ bgl: g.bgl(HEAPU32, words >> 2, count).bgl });
})

/* Je Eintrag: slot, kind, id, offset (lo, hi), size (lo, hi). */
EM_JS(int, gpu_js_group_create, (int layout, const uint32_t* words, int count), {
    const g = globalThis.nhalGpu;
    const u = HEAPU32;
    let at = words >> 2;
    const entries = [];
    for (let i = 0; i < count; ++i) {
        const slot = u[at], kind = u[at + 1], id = u[at + 2];
        const offset = u[at + 3] + u[at + 4] * 4294967296, size = u[at + 5] + u[at + 6] * 4294967296;
        entries.push({ binding: slot, resource: g.resource(kind, id, offset, size) });
        at += 7;
    }
    return g.add({ bg: g.device.createBindGroup({ layout: g.objs[layout].bgl, entries: entries }) });
})

EM_JS(int, gpu_js_query_set_create, (int timestamp, int count), {
    const g = globalThis.nhalGpu;
    return g.add(g.device.createQuerySet({ type: timestamp ? "timestamp" : "occlusion", count: count }));
})

EM_JS(int, gpu_js_cmd_begin, (void), {
    const g = globalThis.nhalGpu;
    return g.add({ enc: g.device.createCommandEncoder(), chunks: [] });
})

EM_JS(void, gpu_js_cmd_submit, (int id, double serial), { globalThis.nhalGpu.submit(id, serial); })
EM_JS(void, gpu_js_cmd_drop, (int id), { globalThis.nhalGpu.drop(id); })

EM_JS(void, gpu_js_cmd_copy_buffer, (int enc, int src, double so, int dst, double dof, double size), {
    const g = globalThis.nhalGpu;
    g.objs[enc].enc.copyBufferToBuffer(g.objs[src], so, g.objs[dst], dof, size);
})

EM_JS(void, gpu_js_cmd_copy_texture, (int enc, int tex, int buf, double offset, int bpr, int w, int h), {
    const g = globalThis.nhalGpu;
    g.objs[enc].enc.copyTextureToBuffer({ texture: g.objs[tex] },
                                    { buffer: g.objs[buf], offset: offset, bytesPerRow: bpr, rowsPerImage: h },
                                    { width: w, height: h });
})

EM_JS(void, gpu_js_cmd_resolve, (int enc, int set, int first, int count, int buf, double offset), {
    const g = globalThis.nhalGpu;
    g.objs[enc].enc.resolveQuerySet(g.objs[set], first, count, g.objs[buf], offset);
})

EM_JS(int, gpu_js_render_pass_begin, (const uint32_t* desc), { return globalThis.nhalGpu.renderPassBegin(desc); })

EM_JS(int, gpu_js_compute_pass_begin, (int enc, int ts, int begin, int end), {
    const g = globalThis.nhalGpu;
    const cmd = g.objs[enc];
    const d = {};
    if (ts) d.timestampWrites = { querySet: g.objs[ts], beginningOfPassWriteIndex: begin, endOfPassWriteIndex: end };
    return g.add(g.newPass(cmd, cmd.enc.beginComputePass(d)));
})

EM_JS(void, gpu_js_pass_end, (int pass), {
    const g = globalThis.nhalGpu;
    g.objs[pass].enc.end();
    g.del(pass);
})

EM_JS(void, gpu_js_pass_pipeline, (int pass, int pipeline), {
    const g = globalThis.nhalGpu;
    const p = g.objs[pass];
    p.pipe = g.objs[pipeline];
    p.enc.setPipeline(p.pipe.pipe);
    p.dirty = true;
    p.applied = [];
})

EM_JS(void, gpu_js_pass_bind, (int pass, int slot, int id, double offset, double size), {
    const p = globalThis.nhalGpu.objs[pass];
    p.bound[slot] = { id: id, offset: offset, size: size };
    p.dirty = true;
})

EM_JS(void, gpu_js_pass_group, (int pass, int index, int group), {
    const g = globalThis.nhalGpu;
    g.objs[pass].groups[index] = g.objs[group].bg;
})

EM_JS(void, gpu_js_pass_constants, (int pass, const void* data, int size), {
    const g = globalThis.nhalGpu;
    g.constants(g.objs[pass], data, size);
})

EM_JS(void, gpu_js_pass_scissor, (int pass, int x, int y, int w, int h), {
    globalThis.nhalGpu.objs[pass].enc.setScissorRect(x, y, w, h);
})

EM_JS(void, gpu_js_pass_indirect, (int pass, int what, int buf, double offset), {
    const g = globalThis.nhalGpu;
    const p = g.objs[pass];
    g.flush(p);
    const b = g.objs[buf];
    if (what === 0) p.enc.drawIndirect(b, offset);
    else if (what === 1) p.enc.drawIndexedIndirect(b, offset);
    else p.enc.dispatchWorkgroupsIndirect(b, offset);
})

EM_JS(void, gpu_js_pass_occlusion, (int pass, int index), {
    const p = globalThis.nhalGpu.objs[pass];
    if (index >= 0) p.enc.beginOcclusionQuery(index);
    else p.enc.endOcclusionQuery();
})

EM_JS(void, gpu_js_pass_vertex, (int pass, int index, int buf, double offset), {
    const g = globalThis.nhalGpu;
    g.objs[pass].enc.setVertexBuffer(index, g.objs[buf], offset);
})

EM_JS(void, gpu_js_pass_index, (int pass, int buf, int u32, double offset), {
    const g = globalThis.nhalGpu;
    g.objs[pass].enc.setIndexBuffer(g.objs[buf], u32 ? "uint32" : "uint16", offset);
})

EM_JS(void, gpu_js_pass_viewport, (int pass, float x, float y, float w, float h), {
    globalThis.nhalGpu.objs[pass].enc.setViewport(x, y, w, h, 0, 1);
})

EM_JS(void, gpu_js_pass_draw, (int pass, int vc, int ic, int fv, int fi), {
    const g = globalThis.nhalGpu;
    const p = g.objs[pass];
    g.flush(p);
    p.enc.draw(vc >>> 0, ic >>> 0, fv >>> 0, fi >>> 0);
})

EM_JS(void, gpu_js_pass_draw_indexed, (int pass, int n, int ic, int first, int base, int fi), {
    const g = globalThis.nhalGpu;
    const p = g.objs[pass];
    g.flush(p);
    p.enc.drawIndexed(n >>> 0, ic >>> 0, first >>> 0, base, fi >>> 0);
})

EM_JS(void, gpu_js_pass_dispatch, (int pass, int x, int y, int z), {
    const g = globalThis.nhalGpu;
    const p = g.objs[pass];
    g.flush(p);
    p.enc.dispatchWorkgroups(x, y, z);
})

EM_JS(int, gpu_js_surface_create, (const char* selector), {
    const g = globalThis.nhalGpu;
    if (typeof document === "undefined") return -1;
    const canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return 0;
    const ctx = canvas.getContext("webgpu");
    if (!ctx) return -2;
    return g.add({ canvas: canvas, ctx: ctx });
})

EM_JS(int, gpu_js_surface_format, (void), {
    const f = navigator.gpu.getPreferredCanvasFormat();
    return f === "rgba8unorm" ? 2 : 4;
})

EM_JS(void, gpu_js_surface_configure, (int id, int w, int h, int format), {
    const g = globalThis.nhalGpu;
    const s = g.objs[id];
    s.canvas.width = w;
    s.canvas.height = h;
    s.ctx.configure({ device: g.device, format: g.formats[format],
                      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC, alphaMode: "opaque" });
})

EM_JS(int, gpu_js_surface_acquire, (int id), {
    const g = globalThis.nhalGpu;
    return g.add(g.objs[id].ctx.getCurrentTexture());
})

EM_JS(void, gpu_js_surface_destroy, (int id), {
    const g = globalThis.nhalGpu;
    const s = g.objs[id];
    if (s && s.ctx.unconfigure) s.ctx.unconfigure();
    g.del(id);
})
/* clang-format on */

/* ── Strukturen ────────────────────────────────────────────────────────── */

struct nhal_gfx_buffer {
    int id;
    uint64_t size;
    uint32_t usage;
};

struct nhal_gfx_texture {
    int id;
    uint32_t width, height;
    nhal_gfx_format format;
    uint32_t usage;
    uint32_t samples;
    bool surface; /* gehört der Oberfläche */
};

struct nhal_gfx_sampler {
    int id;
};

struct nhal_gfx_shader {
    int id;
};

struct nhal_gfx_group_layout {
    int id;
    nhal_gfx_layout layout;
};

struct nhal_gfx_group {
    int id;
    nhal_gfx_layout layout;
};

struct nhal_gfx_pipeline {
    int id;
    bool compute;
    nhal_gfx_pass_shape shape;
    nhal_gfx_pipeline_layout layout;
};

struct nhal_gfx_query_set {
    int id;
    nhal_gfx_query_set_desc desc;
};

typedef enum pass_kind { PASS_NONE = 0, PASS_RENDER, PASS_COMPUTE } pass_kind;

struct nhal_gfx_pass {
    nhal_gfx_commands* owner;
    int id;
    pass_kind kind;
    nhal_gfx_pipeline* pipeline;
    nhal_gfx_slot slots[NHAL_GFX_MAX_BINDINGS];
    nhal_gfx_group* groups[NHAL_GFX_MAX_GROUPS];
    uint32_t constants_size;
    bool has_index;
    nhal_gfx_pass_shape shape;
    uint32_t width, height;
    nhal_gfx_query_set* occlusion;
    bool occlusion_open;
};

struct nhal_gfx_commands {
    int id;
    nhal_status error;
    nhal_gfx_pass pass;
};

struct nhal_gfx_surface {
    int id;
    uint32_t width, height;
    nhal_gfx_format format;
    nhal_gfx_texture texture;
};

typedef struct read_request {
    struct read_request* next;
    int id;
    nhal_gfx_read_fn fn;
    void* user;
} read_request;

static const nhal_allocator* g_alloc;
static read_request* g_reads;
static int g_next_read = 1;
static uint64_t g_serial; /* zuletzt vergebene Seriennummer */
static nhal_gfx_error_ring g_errors; /* eigene Fehler; die des Browsers kommen aus JS */

#define REC(cmds, status)          \
    do {                           \
        record((cmds), status);    \
        return;                    \
    } while (0)

static void* zalloc(size_t size) {
    void* p = g_alloc->alloc(g_alloc->user, size, 16);
    if (p) memset(p, 0, size);
    return p;
}

static void zfree(void* p, size_t size) {
    if (p) g_alloc->free(g_alloc->user, p, size);
}

static void record(nhal_gfx_commands* c, nhal_status s) {
    if (c && c->error.code == NHAL_OK && s.code != NHAL_OK) c->error = s;
}

static nhal_status not_ready(void) {
    switch (gpu_js_state()) {
    case 1: return nhal_status_ok();
    case 0: return nhal_status_error(NHAL_ERR_BUSY, "WebGPU-Gerät wird noch angefordert (poll() und ready())");
    default: break;
    }
    switch (gpu_js_reason()) {
    case 1: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Dieser Browser bzw. diese Umgebung hat kein WebGPU");
    case 2: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein passender GPU-Adapter");
    case 3: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "GPU-Gerät verloren (Details: take_error)");
    default: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "WebGPU-Gerät ließ sich nicht anlegen (Details: take_error)");
    }
}

static uint32_t max_texture(void) { return (uint32_t)gpu_js_max_texture(); }

/* ── Gerät ─────────────────────────────────────────────────────────────── */

static nhal_status gfx_ready(void) { return not_ready(); }

static void gfx_poll(void) {
    int done;
    while ((done = gpu_js_take_done()) >= 0) {
        const int id = done >> 1;
        read_request** link = &g_reads;
        while (*link && (*link)->id != id) link = &(*link)->next;
        read_request* r = *link;
        if (!r) continue;
        *link = r->next;
        nhal_gfx_read_fn fn = r->fn;
        void* user = r->user;
        zfree(r, sizeof *r);
        fn(user, (done & 1) ? nhal_status_error(NHAL_ERR_IO, "Lesen von der GPU fehlgeschlagen (Details: take_error)")
                            : nhal_status_ok());
    }
}

static void gfx_info(nhal_gfx_info* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->backend = "webgpu";
    out->shader_language = NHAL_GFX_SHADER_WGSL;
    if (gpu_js_state() != 1) return;
    gpu_js_name(out->device_name, (int)sizeof out->device_name);
    out->unified_memory = false; /* WebGPU gibt das nicht preis */
    out->persistent_mapping = false;
    out->blocking_wait = false; /* der Browser kann nicht blockierend warten */
    const int f = gpu_js_features();
    out->timestamp_queries = (f & 1) != 0;
    out->indirect_first_instance = (f & 2) != 0;
    out->timestamp_period = 1.0; /* WebGPU liefert Nanosekunden */
    out->max_texture_size = max_texture();
    out->max_buffer_size = (uint64_t)gpu_js_max_buffer();
}

static bool gfx_take_error(char* buf, size_t cap) {
    if (nhal_gfx_error_take(&g_errors, buf, cap)) return true;
    return gpu_js_take_error(buf, (int)(cap > 0x7fffffff ? 0x7fffffff : cap)) != 0;
}

/* ── Puffer ────────────────────────────────────────────────────────────── */

static nhal_status buffer_create(const nhal_gfx_buffer_desc* d, const void* initial, nhal_gfx_buffer** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_buffer_desc(d)).code) return s;
    if (d->usage & NHAL_GFX_BUFFER_HOST_VISIBLE)
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "WebGPU kann Puffer nicht dauerhaft mappen (HOST_VISIBLE)");
    if (d->size > (uint64_t)gpu_js_max_buffer())
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer größer als info().max_buffer_size");
    nhal_gfx_buffer* b = (nhal_gfx_buffer*)zalloc(sizeof *b);
    if (!b) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Puffer");
    b->id = gpu_js_buffer_create((double)d->size, (int)d->usage, initial);
    b->size = d->size;
    b->usage = d->usage;
    *out = b;
    return nhal_status_ok();
}

static void buffer_destroy(nhal_gfx_buffer* b) {
    if (!b) return;
    gpu_js_destroy(b->id);
    zfree(b, sizeof *b);
}

static nhal_status buffer_write(nhal_gfx_buffer* b, uint64_t offset, const void* data, uint64_t size) {
    if (!b || !data) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer oder Daten fehlen");
    if (!(b->usage & NHAL_GFX_BUFFER_COPY_DST))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "buffer_write braucht Usage COPY_DST");
    nhal_status s = nhal_gfx_check_range(b->size, offset, size);
    if (s.code) return s;
    gpu_js_buffer_write(b->id, (double)offset, data, (double)size);
    return nhal_status_ok();
}

static nhal_status buffer_read(nhal_gfx_buffer* b, uint64_t offset, uint64_t size, void* dst, nhal_gfx_read_fn fn,
                               void* user) {
    if (!b || !dst || !fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer, Ziel oder Rückruf fehlt");
    if (!(b->usage & NHAL_GFX_BUFFER_COPY_SRC))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "buffer_read braucht Usage COPY_SRC");
    nhal_status s = nhal_gfx_check_range(b->size, offset, size);
    if (s.code) return s;
    read_request* r = (read_request*)zalloc(sizeof *r);
    if (!r) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Lesevorgang");
    r->id = g_next_read++;
    if (g_next_read > 0x3fffffff) g_next_read = 1;
    r->fn = fn;
    r->user = user;
    r->next = g_reads;
    g_reads = r;
    gpu_js_buffer_read(b->id, (double)offset, (double)size, dst, r->id);
    return nhal_status_ok();
}

static void* buffer_mapped(nhal_gfx_buffer* b) {
    (void)b;
    return NULL;
}

/* ── Texturen, Sampler, Shader ─────────────────────────────────────────── */

static nhal_status texture_create(const nhal_gfx_texture_desc* d, nhal_gfx_texture** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_texture_desc(d, max_texture())).code) return s;
    nhal_gfx_texture* t = (nhal_gfx_texture*)zalloc(sizeof *t);
    if (!t) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Textur");
    t->samples = nhal_gfx_samples(d->sample_count);
    t->id = gpu_js_texture_create((int)d->width, (int)d->height, (int)d->format, (int)d->usage, (int)t->samples);
    t->width = d->width;
    t->height = d->height;
    t->format = d->format;
    t->usage = d->usage;
    *out = t;
    return nhal_status_ok();
}

static void texture_destroy(nhal_gfx_texture* t) {
    if (!t || t->surface) return;
    gpu_js_destroy(t->id);
    zfree(t, sizeof *t);
}

static nhal_status texture_write(nhal_gfx_texture* t, const void* data, uint32_t bpr) {
    if (!t || !data) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Textur oder Daten fehlen");
    if (!(t->usage & NHAL_GFX_TEXTURE_COPY_DST))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "texture_write braucht Usage COPY_DST");
    const uint32_t bpp = nhal_gfx_format_bytes(t->format);
    if (bpr < t->width * bpp || bpr % bpp)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "bytes_per_row zu klein oder nicht durch Pixelgröße teilbar");
    gpu_js_texture_write(t->id, data, (int)bpr, (int)t->width, (int)t->height);
    return nhal_status_ok();
}

static nhal_status sampler_create(const nhal_gfx_sampler_desc* d, nhal_gfx_sampler** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    nhal_gfx_sampler* sm = (nhal_gfx_sampler*)zalloc(sizeof *sm);
    if (!sm) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Sampler");
    sm->id = gpu_js_sampler_create(d->filter == NHAL_GFX_FILTER_LINEAR, d->address == NHAL_GFX_ADDRESS_REPEAT);
    *out = sm;
    return nhal_status_ok();
}

static void sampler_destroy(nhal_gfx_sampler* s) {
    if (!s) return;
    gpu_js_forget(s->id);
    zfree(s, sizeof *s);
}

static nhal_status shader_create(const nhal_gfx_shader_desc* d, nhal_gfx_shader** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if (!d->wgsl) return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Shader hat keinen WGSL-Quelltext (WebGPU braucht WGSL)");
    nhal_gfx_shader* sh = (nhal_gfx_shader*)zalloc(sizeof *sh);
    if (!sh) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für den Shader");
    sh->id = gpu_js_shader_create(d->wgsl);
    *out = sh;
    return nhal_status_ok();
}

static void shader_destroy(nhal_gfx_shader* s) {
    if (!s) return;
    gpu_js_forget(s->id);
    zfree(s, sizeof *s);
}

/* ── Gruppen ───────────────────────────────────────────────────────────── */

static uint32_t put_bindings(uint32_t* w, uint32_t n, const nhal_gfx_layout* l) {
    for (uint32_t i = 0; i < l->count; ++i) {
        w[n++] = l->bindings[i].slot;
        w[n++] = (uint32_t)l->bindings[i].kind;
        w[n++] = l->bindings[i].stages;
        w[n++] = (uint32_t)l->bindings[i].storage_format;
    }
    return n;
}

static nhal_status group_layout_create(const nhal_gfx_group_layout_desc* d, nhal_gfx_group_layout** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    nhal_gfx_layout layout;
    if ((s = nhal_gfx_copy_layout(d->bindings, d->binding_count, &layout)).code) return s;
    nhal_gfx_group_layout* l = (nhal_gfx_group_layout*)zalloc(sizeof *l);
    if (!l) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für das Layout");
    uint32_t w[4u * NHAL_GFX_MAX_BINDINGS];
    put_bindings(w, 0, &layout);
    l->id = gpu_js_group_layout_create(w, (int)layout.count);
    l->layout = layout;
    *out = l;
    return nhal_status_ok();
}

static void group_layout_destroy(nhal_gfx_group_layout* l) {
    if (!l) return;
    gpu_js_forget(l->id);
    zfree(l, sizeof *l);
}

static nhal_status group_create(const nhal_gfx_group_desc* d, nhal_gfx_group** out) {
    if (!out || !d || !d->layout) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc, layout oder out fehlt");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    const nhal_gfx_layout* l = &d->layout->layout;
    nhal_gfx_slot slots[NHAL_GFX_MAX_BINDINGS];
    memset(slots, 0, sizeof slots);
    uint32_t w[7u * NHAL_GFX_MAX_BINDINGS];
    uint32_t n = 0, count = 0;
    for (uint32_t i = 0; i < d->entry_count && i < NHAL_GFX_MAX_BINDINGS && d->entries; ++i) {
        const nhal_gfx_group_entry* e = &d->entries[i];
        if (e->slot >= NHAL_GFX_MAX_BINDINGS) break; /* meldet nhal_gfx_check_group */
        nhal_gfx_binding_kind kind = (nhal_gfx_binding_kind)0;
        for (uint32_t k = 0; k < l->count; ++k) {
            if (l->bindings[k].slot == e->slot) kind = l->bindings[k].kind;
        }
        int id = 0;
        if (kind >= NHAL_GFX_BIND_UNIFORM_BUFFER && kind <= NHAL_GFX_BIND_READ_STORAGE_BUFFER && e->buffer) {
            if (e->offset >= e->buffer->size || (e->size && e->size > e->buffer->size - e->offset))
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungsbereich außerhalb des Puffers");
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_BUFFER, e->buffer->usage, NHAL_GFX_FORMAT_NONE, e->offset };
            id = e->buffer->id;
        } else if ((kind == NHAL_GFX_BIND_SAMPLED_TEXTURE || kind == NHAL_GFX_BIND_STORAGE_TEXTURE) && e->texture) {
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_TEXTURE, e->texture->usage, e->texture->format, 0 };
            id = e->texture->id;
        } else if (kind == NHAL_GFX_BIND_SAMPLER && e->sampler) {
            slots[e->slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_SAMPLER, 0, NHAL_GFX_FORMAT_NONE, 0 };
            id = e->sampler->id;
        }
        w[n++] = e->slot;
        w[n++] = (uint32_t)kind;
        w[n++] = (uint32_t)id;
        w[n++] = (uint32_t)e->offset;
        w[n++] = (uint32_t)(e->offset >> 32);
        w[n++] = (uint32_t)e->size;
        w[n++] = (uint32_t)(e->size >> 32);
        ++count;
    }
    if ((s = nhal_gfx_check_group(l, d->entries, d->entry_count, slots)).code) return s;
    nhal_gfx_group* g = (nhal_gfx_group*)zalloc(sizeof *g);
    if (!g) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Gruppe");
    g->id = gpu_js_group_create(d->layout->id, w, (int)count);
    g->layout = *l;
    *out = g;
    return nhal_status_ok();
}

static void group_destroy(nhal_gfx_group* g) {
    if (!g) return;
    gpu_js_forget(g->id);
    zfree(g, sizeof *g);
}

/* ── Pipelines ─────────────────────────────────────────────────────────── */

/* Platz für die flache Beschreibung, die JS liest. */
#define LAYOUT_WORDS (5u + 4u * NHAL_GFX_MAX_BINDINGS)
#define DESC_WORDS (16u + 3u * NHAL_GFX_MAX_COLOR_TARGETS + \
                    NHAL_GFX_MAX_VERTEX_BUFFERS * (3u + 3u * NHAL_GFX_MAX_VERTEX_ATTRIBUTES) + LAYOUT_WORDS)

static void layouts_of(const nhal_gfx_group_layout* const in[NHAL_GFX_MAX_GROUPS],
                       const nhal_gfx_layout* out[NHAL_GFX_MAX_GROUPS]) {
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) out[g] = in[g] ? &in[g]->layout : NULL;
}

/* g0 g1 g2 (JS-Nummern der Layouts), constants, Slot-Anzahl, Slot-Vierer. */
static uint32_t put_layout(uint32_t* w, uint32_t n, const nhal_gfx_group_layout* const groups[NHAL_GFX_MAX_GROUPS],
                           const nhal_gfx_pipeline_layout* pl) {
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) w[n++] = groups[g] ? (uint32_t)groups[g]->id : 0u;
    w[n++] = pl->constants_size ? 1u : 0u;
    w[n++] = pl->slots.count;
    return put_bindings(w, n, &pl->slots);
}

static nhal_status render_pipeline_create(const nhal_gfx_render_pipeline_desc* d, nhal_gfx_pipeline** out) {
    if (!out || !d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc oder out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    const nhal_gfx_layout* groups[NHAL_GFX_MAX_GROUPS];
    layouts_of(d->groups, groups);
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

    uint32_t w[DESC_WORDS];
    uint32_t n = 0;
    w[n++] = (uint32_t)d->shader->id;
    w[n++] = (uint32_t)(uintptr_t)d->vertex_entry;
    w[n++] = (uint32_t)(uintptr_t)d->fragment_entry;
    w[n++] = d->color_count;
    for (uint32_t i = 0; i < d->color_count; ++i) {
        w[n++] = (uint32_t)d->colors[i].format;
        w[n++] = (uint32_t)d->colors[i].blend;
        w[n++] = d->colors[i].write_mask;
    }
    w[n++] = (uint32_t)d->depth_format;
    w[n++] = d->depth_test;
    w[n++] = d->depth_write;
    w[n++] = (uint32_t)d->depth_compare;
    w[n++] = (uint32_t)d->topology;
    w[n++] = (uint32_t)d->cull;
    w[n++] = d->front_face == NHAL_GFX_FRONT_CW ? 1u : 0u;
    w[n++] = shape.samples;
    w[n++] = d->vertex_buffer_count;
    for (uint32_t i = 0; i < d->vertex_buffer_count; ++i) {
        const nhal_gfx_vertex_buffer_layout* vb = &d->vertex_buffers[i];
        w[n++] = vb->stride;
        w[n++] = vb->per_instance;
        w[n++] = vb->attribute_count;
        for (uint32_t a = 0; a < vb->attribute_count; ++a) {
            w[n++] = (uint32_t)vb->attributes[a].format;
            w[n++] = vb->attributes[a].offset;
            w[n++] = vb->attributes[a].location;
        }
    }
    put_layout(w, n, d->groups, &layout);

    nhal_gfx_pipeline* p = (nhal_gfx_pipeline*)zalloc(sizeof *p);
    if (!p) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Pipeline");
    p->id = gpu_js_render_pipeline_create(w);
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
    layouts_of(d->groups, groups);
    nhal_gfx_pipeline_layout layout;
    if ((s = nhal_gfx_make_pipeline_layout(d->bindings, d->binding_count, groups, d->constants_size,
                                           NHAL_GFX_STAGE_COMPUTE, "Compute-Bindungen brauchen stages = COMPUTE",
                                           &layout))
            .code)
        return s;
    const uint32_t* wg = d->workgroup_size;
    if (!wg[0] || !wg[1] || !wg[2] || (uint64_t)wg[0] * wg[1] * wg[2] > 256u)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "workgroup_size: jede Achse > 0, zusammen höchstens 256");
    if (!d->shader || !d->entry) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Shader oder entry fehlt");
    uint32_t w[2u + LAYOUT_WORDS];
    w[0] = (uint32_t)d->shader->id;
    w[1] = (uint32_t)(uintptr_t)d->entry;
    put_layout(w, 2, d->groups, &layout);
    nhal_gfx_pipeline* p = (nhal_gfx_pipeline*)zalloc(sizeof *p);
    if (!p) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Pipeline");
    p->id = gpu_js_compute_pipeline_create(w);
    p->compute = true;
    p->layout = layout;
    *out = p;
    return nhal_status_ok();
}

static void pipeline_destroy(nhal_gfx_pipeline* p) {
    if (!p) return;
    gpu_js_forget(p->id);
    zfree(p, sizeof *p);
}

/* ── Abfragen ──────────────────────────────────────────────────────────── */

static nhal_status query_set_create(const nhal_gfx_query_set_desc* d, nhal_gfx_query_set** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if ((s = nhal_gfx_check_query_set_desc(d, (gpu_js_features() & 1) != 0)).code) return s;
    nhal_gfx_query_set* q = (nhal_gfx_query_set*)zalloc(sizeof *q);
    if (!q) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Abfragen");
    q->id = gpu_js_query_set_create(d->type == NHAL_GFX_QUERY_TIMESTAMP, (int)d->count);
    q->desc = *d;
    *out = q;
    return nhal_status_ok();
}

static void query_set_destroy(nhal_gfx_query_set* q) {
    if (!q) return;
    gpu_js_destroy(q->id);
    zfree(q, sizeof *q);
}

/* ── Befehlslisten ─────────────────────────────────────────────────────── */

static nhal_status commands_begin(nhal_gfx_commands** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    nhal_gfx_commands* c = (nhal_gfx_commands*)zalloc(sizeof *c);
    if (!c) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Befehlsliste");
    c->id = gpu_js_cmd_begin();
    c->error = nhal_status_ok();
    c->pass.owner = c;
    *out = c;
    return nhal_status_ok();
}

static nhal_status commands_submit(nhal_gfx_commands* c, uint64_t* serial) {
    if (serial) *serial = 0;
    if (!c) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Befehlsliste ist NULL");
    if (c->pass.kind != PASS_NONE) {
        gpu_js_pass_end(c->pass.id);
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "submit mit offenem Pass"));
    }
    const nhal_status s = c->error;
    if (s.code == NHAL_OK) {
        gpu_js_cmd_submit(c->id, (double)++g_serial);
        if (serial) *serial = g_serial;
    } else {
        gpu_js_cmd_drop(c->id); /* nie abgeschlossen, verfällt */
    }
    zfree(c, sizeof *c);
    return s;
}

static uint64_t gfx_completed(void) { return gpu_js_state() == 1 ? (uint64_t)gpu_js_completed() : 0u; }

static nhal_status gfx_wait(uint64_t serial, uint64_t timeout_ns) {
    (void)timeout_ns; /* der Browser kann nicht blockieren; Fortschritt kommt in der Ereignisschleife */
    if (serial > g_serial) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Seriennummer wurde nie vergeben");
    if (gfx_completed() >= serial) return nhal_status_ok();
    return nhal_status_error(NHAL_ERR_BUSY, "GPU noch nicht so weit (im Browser wartet wait() nicht)");
}

static bool can_copy(nhal_gfx_commands* c) {
    if (c->pass.kind != PASS_NONE) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kopieren geht nicht in einem offenen Pass"));
        return false;
    }
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
    if (!can_copy(c)) return;
    gpu_js_cmd_copy_buffer(c->id, src->id, (double)so, dst->id, (double)dof, (double)size);
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
    if (!can_copy(c)) return;
    gpu_js_cmd_copy_texture(c->id, src->id, dst->id, (double)offset, (int)bpr, (int)src->width, (int)src->height);
}

static void commands_resolve_queries(nhal_gfx_commands* c, nhal_gfx_query_set* q, uint32_t first, uint32_t count,
                                     nhal_gfx_buffer* dst, uint64_t offset) {
    if (!c) return;
    if (!q || !dst) REC(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Abfragen oder Ziel fehlen"));
    const nhal_status s = nhal_gfx_check_resolve(&q->desc, first, count, dst->size, dst->usage, offset);
    if (s.code) REC(c, s);
    if (!can_copy(c)) return;
    gpu_js_cmd_resolve(c->id, q->id, (int)first, (int)count, dst->id, (double)offset);
}

/* ── Passes ────────────────────────────────────────────────────────────── */

static nhal_gfx_pass* open_pass(nhal_gfx_commands* c) {
    if (!c) return NULL;
    if (c->pass.kind != PASS_NONE) {
        record(c, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Es ist schon ein Pass offen"));
        return NULL;
    }
    nhal_gfx_pass* p = &c->pass;
    memset(p->slots, 0, sizeof p->slots);
    memset(p->groups, 0, sizeof p->groups);
    memset(&p->shape, 0, sizeof p->shape);
    p->pipeline = NULL;
    p->constants_size = 0;
    p->has_index = false;
    p->width = p->height = 0;
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
    record(c, s);
    return NULL;
}

static uint32_t float_word(float f) {
    uint32_t w;
    memcpy(&w, &f, sizeof w);
    return w;
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

    uint32_t w[2u + 8u * NHAL_GFX_MAX_COLOR_TARGETS + 8u];
    uint32_t k = 0;
    w[k++] = (uint32_t)c->id;
    w[k++] = n;
    for (uint32_t i = 0; i < n; ++i) {
        const nhal_gfx_color_attachment* a = &d->colors[i];
        w[k++] = (uint32_t)a->texture->id;
        w[k++] = a->resolve ? (uint32_t)a->resolve->id : 0u;
        w[k++] = a->load == NHAL_GFX_LOAD_KEEP;
        w[k++] = a->store == NHAL_GFX_STORE_DISCARD;
        for (int ch = 0; ch < 4; ++ch) w[k++] = float_word(a->clear_color[ch]);
    }
    w[k++] = d->depth ? (uint32_t)d->depth->id : 0u;
    w[k++] = d->depth_load == NHAL_GFX_LOAD_KEEP;
    w[k++] = d->depth_store == NHAL_GFX_STORE_DISCARD;
    w[k++] = float_word(d->clear_depth);
    w[k++] = d->occlusion ? (uint32_t)d->occlusion->id : 0u;
    w[k++] = d->timestamps.set ? (uint32_t)d->timestamps.set->id : 0u;
    w[k++] = d->timestamps.begin_index;
    w[k++] = d->timestamps.end_index;
    p->id = gpu_js_render_pass_begin(w);
    p->occlusion = d->occlusion;
    p->kind = PASS_RENDER;
    return p;
}

static nhal_gfx_pass* compute_pass_begin(nhal_gfx_commands* c, const nhal_gfx_compute_pass_desc* d) {
    nhal_gfx_pass* p = open_pass(c);
    if (!p) return NULL;
    const nhal_gfx_pass_timestamps* t = d ? &d->timestamps : NULL;
    if (t && t->set) {
        const nhal_status s = nhal_gfx_check_timestamps(t, &t->set->desc);
        if (s.code) return fail(c, s);
    }
    p->id = gpu_js_compute_pass_begin(c->id, t && t->set ? t->set->id : 0, t ? (int)t->begin_index : 0,
                                      t ? (int)t->end_index : 0);
    p->kind = PASS_COMPUTE;
    return p;
}

static void pass_end(nhal_gfx_pass* p) {
    if (!p || p->kind == PASS_NONE) return;
    if (p->occlusion_open) {
        gpu_js_pass_occlusion(p->id, -1);
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "pass_end mit offener Occlusion-Abfrage"));
    }
    gpu_js_pass_end(p->id);
    p->kind = PASS_NONE;
}

static bool pass_open(nhal_gfx_pass* p) {
    if (!p) return false;
    if (p->kind == PASS_NONE) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pass ist schon beendet"));
        return false;
    }
    return true;
}

static void pass_set_pipeline(nhal_gfx_pass* p, nhal_gfx_pipeline* pl) {
    if (!pass_open(p)) return;
    if (!pl) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline ist NULL"));
    if (pl->compute != (p->kind == PASS_COMPUTE))
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline-Art passt nicht zum Pass"));
    if (!pl->compute && !nhal_gfx_shape_equal(&pl->shape, &p->shape))
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                        "Formate oder sample_count der Pipeline passen nicht zu den Zielen"));
    gpu_js_pass_pipeline(p->id, pl->id);
    p->pipeline = pl;
}

static bool check_slot(nhal_gfx_pass* p, uint32_t slot) {
    if (!pass_open(p)) return false;
    if (slot >= NHAL_GFX_MAX_BINDINGS) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Slot über NHAL_GFX_MAX_BINDINGS"));
        return false;
    }
    return true;
}

static void pass_set_buffer(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_buffer* b, uint64_t offset, uint64_t size) {
    if (!check_slot(p, slot)) return;
    if (!b) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer ist NULL"));
    if (offset >= b->size || (size && size > b->size - offset))
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungsbereich außerhalb des Puffers"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_BUFFER, b->usage, NHAL_GFX_FORMAT_NONE, offset };
    gpu_js_pass_bind(p->id, (int)slot, b->id, (double)offset, (double)size);
}

static void pass_set_texture(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_texture* t) {
    if (!check_slot(p, slot)) return;
    if (!t) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Textur ist NULL"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_TEXTURE, t->usage, t->format, 0 };
    gpu_js_pass_bind(p->id, (int)slot, t->id, 0, 0);
}

static void pass_set_sampler(nhal_gfx_pass* p, uint32_t slot, nhal_gfx_sampler* s) {
    if (!check_slot(p, slot)) return;
    if (!s) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Sampler ist NULL"));
    p->slots[slot] = (nhal_gfx_slot){ NHAL_GFX_SLOT_SAMPLER, 0, NHAL_GFX_FORMAT_NONE, 0 };
    gpu_js_pass_bind(p->id, (int)slot, s->id, 0, 0);
}

static void pass_set_group(nhal_gfx_pass* p, uint32_t index, nhal_gfx_group* g) {
    if (!pass_open(p)) return;
    if (index >= NHAL_GFX_MAX_GROUPS || !g)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gruppe fehlt oder Index über NHAL_GFX_MAX_GROUPS"));
    p->groups[index] = g;
    gpu_js_pass_group(p->id, (int)index, g->id); /* gesetzt wird beim nächsten Draw */
}

static void pass_set_constants(nhal_gfx_pass* p, const void* data, uint32_t size) {
    if (!pass_open(p)) return;
    const nhal_status s = nhal_gfx_check_constants(data, size);
    if (s.code) REC(p->owner, s);
    gpu_js_pass_constants(p->id, data, (int)size);
    p->constants_size = size;
}

static bool render_only(nhal_gfx_pass* p, const char* what) {
    if (!pass_open(p)) return false;
    if (p->kind != PASS_RENDER) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, what));
        return false;
    }
    return true;
}

static void pass_set_vertex_buffer(nhal_gfx_pass* p, uint32_t index, nhal_gfx_buffer* b, uint64_t offset) {
    if (!render_only(p, "Vertex-Puffer gibt es nur im Render-Pass")) return;
    if (index >= NHAL_GFX_MAX_VERTEX_BUFFERS || !b || !(b->usage & NHAL_GFX_BUFFER_VERTEX) || offset % 4u ||
        offset >= b->size)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertex-Puffer: Index, Usage VERTEX oder Offset ungültig"));
    gpu_js_pass_vertex(p->id, (int)index, b->id, (double)offset);
}

static void pass_set_index_buffer(nhal_gfx_pass* p, nhal_gfx_buffer* b, nhal_gfx_index_format f, uint64_t offset) {
    if (!render_only(p, "Index-Puffer gibt es nur im Render-Pass")) return;
    const uint64_t isz = f == NHAL_GFX_INDEX_UINT32 ? 4u : 2u;
    if (!b || !(b->usage & NHAL_GFX_BUFFER_INDEX) || offset % isz || offset >= b->size)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Index-Puffer: Usage INDEX oder Offset ungültig"));
    gpu_js_pass_index(p->id, b->id, f == NHAL_GFX_INDEX_UINT32, (double)offset);
    p->has_index = true;
}

static void pass_set_viewport(nhal_gfx_pass* p, float x, float y, float w, float h) {
    if (!render_only(p, "Viewport gibt es nur im Render-Pass")) return;
    if (!(w > 0.0f) || !(h > 0.0f)) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Viewport ohne Fläche"));
    gpu_js_pass_viewport(p->id, x, y, w, h);
}

static void pass_set_scissor(nhal_gfx_pass* p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!render_only(p, "Scissor gibt es nur im Render-Pass")) return;
    const nhal_status s = nhal_gfx_check_scissor(x, y, w, h, p->width, p->height);
    if (s.code) REC(p->owner, s);
    gpu_js_pass_scissor(p->id, (int)x, (int)y, (int)w, (int)h);
}

static bool ready_to_draw(nhal_gfx_pass* p, bool compute) {
    if (!pass_open(p)) return false;
    if (!p->pipeline || p->pipeline->compute != compute) {
        record(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine oder eine unpassende Pipeline gesetzt"));
        return false;
    }
    const nhal_gfx_pipeline_layout* pl = &p->pipeline->layout;
    const nhal_gfx_layout* bound[NHAL_GFX_MAX_GROUPS];
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) bound[g] = p->groups[g] ? &p->groups[g]->layout : NULL;
    nhal_status s = nhal_gfx_check_bound(&pl->slots, p->slots);
    if (!s.code) s = nhal_gfx_check_groups_bound(pl, bound, p->constants_size);
    if (s.code) {
        record(p->owner, s);
        return false;
    }
    return true;
}

static void pass_draw(nhal_gfx_pass* p, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (!ready_to_draw(p, false) || !vc || !ic) return;
    gpu_js_pass_draw(p->id, (int)vc, (int)ic, (int)fv, (int)fi);
}

static void pass_draw_indexed(nhal_gfx_pass* p, uint32_t count, uint32_t ic, uint32_t first, int32_t base,
                              uint32_t fi) {
    if (!ready_to_draw(p, false)) return;
    if (!p->has_index) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kein Index-Puffer gesetzt"));
    if (!count || !ic) return;
    gpu_js_pass_draw_indexed(p->id, (int)count, (int)ic, (int)first, base, (int)fi);
}

static bool indirect_ok(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset, uint64_t bytes) {
    if (!pass_open(p)) return false;
    const nhal_status s = b ? nhal_gfx_check_indirect(b->size, b->usage, offset, bytes)
                            : nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Argumentpuffer ist NULL");
    if (s.code) record(p->owner, s);
    return s.code == NHAL_OK;
}

static void pass_draw_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 16u) || !ready_to_draw(p, false)) return;
    gpu_js_pass_indirect(p->id, 0, b->id, (double)offset);
}

static void pass_draw_indexed_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 20u) || !ready_to_draw(p, false)) return;
    if (!p->has_index) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Kein Index-Puffer gesetzt"));
    gpu_js_pass_indirect(p->id, 1, b->id, (double)offset);
}

static void pass_dispatch(nhal_gfx_pass* p, uint32_t x, uint32_t y, uint32_t z) {
    if (!pass_open(p)) return;
    if (p->kind != PASS_COMPUTE) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "dispatch nur im Compute-Pass"));
    if (!ready_to_draw(p, true)) return;
    if (x > 65535u || y > 65535u || z > 65535u)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Höchstens 65535 Gruppen je Achse"));
    if (!x || !y || !z) return;
    gpu_js_pass_dispatch(p->id, (int)x, (int)y, (int)z);
}

static void pass_dispatch_indirect(nhal_gfx_pass* p, nhal_gfx_buffer* b, uint64_t offset) {
    if (!indirect_ok(p, b, offset, 12u)) return;
    if (p->kind != PASS_COMPUTE) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "dispatch nur im Compute-Pass"));
    if (!ready_to_draw(p, true)) return;
    gpu_js_pass_indirect(p->id, 2, b->id, (double)offset);
}

static void pass_begin_occlusion(nhal_gfx_pass* p, uint32_t index) {
    if (!render_only(p, "Occlusion-Abfragen gibt es nur im Render-Pass")) return;
    if (!p->occlusion) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Render-Pass ohne occlusion-Set"));
    if (p->occlusion_open) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Occlusion-Abfragen nicht verschachteln"));
    if (index >= p->occlusion->desc.count)
        REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Abfrage-Index außerhalb des Sets"));
    gpu_js_pass_occlusion(p->id, (int)index);
    p->occlusion_open = true;
}

static void pass_end_occlusion(nhal_gfx_pass* p) {
    if (!render_only(p, "Occlusion-Abfragen gibt es nur im Render-Pass")) return;
    if (!p->occlusion_open) REC(p->owner, nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine Occlusion-Abfrage offen"));
    gpu_js_pass_occlusion(p->id, -1);
    p->occlusion_open = false;
}

/* ── Oberflächen ───────────────────────────────────────────────────────── */

static nhal_status surface_create(const nhal_gfx_surface_desc* d, nhal_gfx_surface** out) {
    if (!out || !d || !d->handle) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "desc, handle oder out fehlt");
    *out = NULL;
    nhal_status s = not_ready();
    if (s.code) return s;
    if (d->kind != NHAL_GFX_SURFACE_CANVAS)
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Im Web gibt es nur NHAL_GFX_SURFACE_CANVAS");
    const int id = gpu_js_surface_create((const char*)d->handle);
    if (id == -1) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein DOM (z. B. node) – keine Canvas");
    if (id == 0) return nhal_status_error(NHAL_ERR_NOT_FOUND, "Kein Element zum Selektor gefunden");
    if (id < 0) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Canvas liefert keinen WebGPU-Kontext");
    nhal_gfx_surface* sf = (nhal_gfx_surface*)zalloc(sizeof *sf);
    if (!sf) {
        gpu_js_surface_destroy(id);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Oberfläche");
    }
    sf->id = id;
    sf->format = gpu_js_surface_format() == 2 ? NHAL_GFX_FORMAT_RGBA8_UNORM : NHAL_GFX_FORMAT_BGRA8_UNORM;
    *out = sf;
    return nhal_status_ok();
}

static void surface_destroy(nhal_gfx_surface* sf) {
    if (!sf) return;
    if (sf->texture.id) gpu_js_forget(sf->texture.id);
    gpu_js_surface_destroy(sf->id);
    zfree(sf, sizeof *sf);
}

static nhal_status surface_configure(nhal_gfx_surface* sf, uint32_t w, uint32_t h, bool vsync) {
    (void)vsync; /* der Browser taktet immer an den Bildschirm */
    if (!sf || !w || !h || w > max_texture() || h > max_texture())
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Größe");
    gpu_js_surface_configure(sf->id, (int)w, (int)h, (int)sf->format);
    sf->width = w;
    sf->height = h;
    return nhal_status_ok();
}

static nhal_gfx_format surface_format(const nhal_gfx_surface* sf) {
    return sf ? sf->format : NHAL_GFX_FORMAT_NONE;
}

static nhal_status surface_acquire(nhal_gfx_surface* sf, nhal_gfx_texture** out) {
    if (!sf || !out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Oberfläche oder out fehlt");
    *out = NULL;
    if (!sf->width) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Oberfläche ist nicht konfiguriert");
    if (sf->texture.id) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vorherige Textur noch nicht präsentiert");
    sf->texture.id = gpu_js_surface_acquire(sf->id);
    sf->texture.width = sf->width;
    sf->texture.height = sf->height;
    sf->texture.format = sf->format;
    sf->texture.usage = NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC;
    sf->texture.surface = true;
    sf->texture.samples = 1;
    *out = &sf->texture;
    return nhal_status_ok();
}

static nhal_status surface_present(nhal_gfx_surface* sf) {
    if (!sf || !sf->texture.id) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Keine Textur zum Präsentieren");
    /* Der Browser zeigt das Bild, sobald die Seite in die Ereignisschleife zurückkehrt. */
    gpu_js_forget(sf->texture.id);
    sf->texture.id = 0;
    return nhal_status_ok();
}

/* ── Feature ───────────────────────────────────────────────────────────── */

static nhal_status feature_init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    g_reads = NULL;
    memset(&g_errors, 0, sizeof g_errors);
    g_serial = 0;
    nhal_web_gpu_init(); /* fordert das Gerät an; Ergebnis über ready() */
    return nhal_status_ok();
}

static void feature_shutdown(void) {
    while (g_reads) {
        read_request* r = g_reads;
        g_reads = r->next;
        zfree(r, sizeof *r);
    }
    gpu_js_shutdown();
    g_alloc = NULL;
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
