/*
 * gfx.device: Compute, Render (Vertex/Index/Uniform), Textur+Sampler,
 * Fehler beim Aufzeichnen, Mapping-Regeln und – wo vorhanden – Oberfläche.
 * Ergebnisse werden von der GPU zurückgelesen und Pixel für Pixel geprüft.
 *
 * Läuft als Zustandsmaschine, weil im Browser das Gerät und das Lesen erst
 * in der Ereignisschleife ankommen. Ohne GPU (node, CI ohne Grafik) meldet
 * der Test den Grund und gilt als bestanden.
 */
#include "nhal/features/gfx/device.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#include <unistd.h>
#endif

/* ── Shader ────────────────────────────────────────────────────────────── */

static const char* k_compute_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void double_it(device float* data [[buffer(0)]], uint id [[thread_position_in_grid]]) {\n"
    "    data[id] = data[id] * 2.0;\n"
    "}\n";

static const char* k_compute_wgsl =
    "@group(0) @binding(0) var<storage, read_write> data: array<f32>;\n"
    "@compute @workgroup_size(64) fn double_it(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "    if (id.x < arrayLength(&data)) { data[id.x] = data[id.x] * 2.0; }\n"
    "}\n";

static const char* k_render_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float2 p [[attribute(0)]]; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vs(VIn in [[stage_in]]) { VOut o; o.pos = float4(in.p, 0.0, 1.0); return o; }\n"
    "fragment float4 fs(constant float4& color [[buffer(0)]]) { return color; }\n"
    "struct TOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex TOut tvs(uint i [[vertex_id]]) {\n"
    "    float x = float((i << 1) & 2); float y = float(i & 2);\n"
    "    TOut o; o.pos = float4(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0); o.uv = float2(x, y); return o;\n"
    "}\n"
    "fragment float4 tfs(TOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(1)]]) {\n"
    "    return tex.sample(smp, in.uv);\n"
    "}\n";

static const char* k_render_wgsl =
    "struct Params { color: vec4<f32> };\n"
    "@group(0) @binding(0) var<uniform> params: Params;\n"
    "@vertex fn vs(@location(0) p: vec2<f32>) -> @builtin(position) vec4<f32> { return vec4<f32>(p, 0.0, 1.0); }\n"
    "@fragment fn fs() -> @location(0) vec4<f32> { return params.color; }\n"
    "struct TOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };\n"
    "@vertex fn tvs(@builtin(vertex_index) i: u32) -> TOut {\n"
    "    let x = f32((i << 1u) & 2u); let y = f32(i & 2u);\n"
    "    var o: TOut; o.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0); o.uv = vec2<f32>(x, y); return o;\n"
    "}\n"
    "@group(0) @binding(0) var tex: texture_2d<f32>;\n"
    "@group(0) @binding(1) var smp: sampler;\n"
    "@fragment fn tfs(v: TOut) -> @location(0) vec4<f32> { return textureSample(tex, smp, v.uv); }\n";

/* ── Zustand ───────────────────────────────────────────────────────────── */

enum { N = 256, W = 64, H = 64, TW = 4, ROW = 256 };

static nhal_context* g_ctx;
static const nhal_gfx_device_v2* G;
static int g_step;
static int g_frames;
static int g_pending;
static bool g_finished;
static int g_rc;

static float g_compute_out[N];
static uint8_t g_render_out[ROW * H];
static uint8_t g_tex_out[ROW * TW];
static uint8_t g_surface_out[ROW];
static bool g_have_surface;
static nhal_gfx_format g_surface_format;
static uint8_t g_checker[TW * TW * 4];

static nhal_gfx_buffer *g_data, *g_readback, *g_vertices, *g_indices, *g_uniform, *g_tex_readback, *g_surf_readback;
static nhal_gfx_texture *g_target, *g_checker_tex;
static nhal_gfx_sampler* g_sampler;
static nhal_gfx_shader *g_cs, *g_rs;
static nhal_gfx_pipeline *g_cp, *g_rp, *g_tp;
static nhal_gfx_surface* g_surface;
static void* g_layer;

static void read_done(void* user, nhal_status s) {
    (void)user;
    CHECK_OK(s);
    --g_pending;
}

static void drain_errors(void) {
    char msg[256];
    while (G->take_error(msg, sizeof msg)) {
        fprintf(stderr, "gfx_test: GPU meldet: %s\n", msg);
        ++g_failures;
    }
}

static void finish(void) {
    if (G) {
        drain_errors();
        G->buffer_destroy(g_data);
        G->buffer_destroy(g_readback);
        G->buffer_destroy(g_vertices);
        G->buffer_destroy(g_indices);
        G->buffer_destroy(g_uniform);
        G->buffer_destroy(g_tex_readback);
        G->buffer_destroy(g_surf_readback);
        G->texture_destroy(g_target);
        G->texture_destroy(g_checker_tex);
        G->sampler_destroy(g_sampler);
        G->pipeline_destroy(g_cp);
        G->pipeline_destroy(g_rp);
        G->pipeline_destroy(g_tp);
        G->shader_destroy(g_cs);
        G->shader_destroy(g_rs);
        G->surface_destroy(g_surface);
    }
    nhal_shutdown(g_ctx);
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    if (g_layer) ((void (*)(id, SEL))objc_msgSend)((id)g_layer, sel_registerName("release"));
#endif
    g_rc = check_report("gfx_test");
    g_finished = true;
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#endif
}

/* ── Prüfungen ohne GPU-Arbeit ─────────────────────────────────────────── */

static void check_rules(const nhal_gfx_info* info) {
    nhal_gfx_buffer* b = NULL;
    const nhal_gfx_buffer_desc odd = { 6, NHAL_GFX_BUFFER_STORAGE };
    CHECK(G->buffer_create(&odd, NULL, &b).code == NHAL_ERR_INVALID_ARGUMENT && !b);
    const nhal_gfx_buffer_desc host = { 64, NHAL_GFX_BUFFER_HOST_VISIBLE | NHAL_GFX_BUFFER_COPY_SRC };
    const nhal_status hs = G->buffer_create(&host, NULL, &b);
    if (info->persistent_mapping) {
        CHECK_OK(hs);
        uint32_t* p = (uint32_t*)G->buffer_mapped(b);
        CHECK(p != NULL);
        if (p) {
            p[0] = 0xC0FFEEu;
            CHECK(((uint32_t*)G->buffer_mapped(b))[0] == 0xC0FFEEu);
        }
        G->buffer_destroy(b);
    } else {
        CHECK(hs.code == NHAL_ERR_UNSUPPORTED && !b);
    }
    const nhal_gfx_shader_desc none = { NULL, NULL, NULL, NULL, 0 };
    nhal_gfx_shader* sh = NULL;
    CHECK(G->shader_create(&none, &sh).code == NHAL_ERR_UNSUPPORTED);

    const nhal_gfx_texture_desc big = { info->max_texture_size + 1u, 1, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                        NHAL_GFX_TEXTURE_SAMPLED };
    nhal_gfx_texture* t = NULL;
    CHECK(G->texture_create(&big, &t).code == NHAL_ERR_INVALID_ARGUMENT);
}

/* ── GPU-Arbeit aufzeichnen ────────────────────────────────────────────── */

static void setup_compute(void) {
    float init[N];
    for (int i = 0; i < N; ++i) init[i] = (float)i;
    const nhal_gfx_buffer_desc dd = { sizeof init, NHAL_GFX_BUFFER_STORAGE | NHAL_GFX_BUFFER_COPY_SRC };
    CHECK_OK(G->buffer_create(&dd, init, &g_data));
    const nhal_gfx_buffer_desc rd = { sizeof init, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC };
    CHECK_OK(G->buffer_create(&rd, NULL, &g_readback));
    const nhal_gfx_shader_desc sd = { k_compute_msl, k_compute_wgsl, NULL, NULL, 0 };
    CHECK_OK(G->shader_create(&sd, &g_cs));
    const nhal_gfx_binding_desc b = { 0, NHAL_GFX_BIND_STORAGE_BUFFER, NHAL_GFX_STAGE_COMPUTE, NHAL_GFX_FORMAT_NONE };
    const nhal_gfx_compute_pipeline_desc cd = { .shader = g_cs, .entry = "double_it", .binding_count = 1,
                                                .bindings = &b, .workgroup_size = { 64, 1, 1 } };
    CHECK_OK(G->compute_pipeline_create(&cd, &g_cp));
    if (!g_data || !g_readback || !g_cp) return;

    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    nhal_gfx_pass* p = G->compute_pass_begin(c, NULL);
    CHECK(p != NULL);
    G->pass_set_pipeline(p, g_cp);
    G->pass_set_buffer(p, 0, g_data, 0, 0);
    G->pass_dispatch(p, N / 64, 1, 1);
    G->pass_end(p);
    G->commands_copy_buffer(c, g_data, 0, g_readback, 0, sizeof init);
    CHECK_OK(G->commands_submit(c, NULL));
    CHECK_OK(G->buffer_read(g_readback, 0, sizeof g_compute_out, g_compute_out, read_done, NULL));
    ++g_pending;
}

static void setup_render(void) {
    const nhal_gfx_texture_desc td = { W, H, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                       NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC };
    CHECK_OK(G->texture_create(&td, &g_target));
    /* Rechteck links: x -1..0, gegen den Uhrzeigersinn (Vorderseite). */
    const float quad[] = { -1.0f, -1.0f, 0.0f, -1.0f, 0.0f, 1.0f, -1.0f, 1.0f };
    const uint16_t idx[] = { 0, 1, 2, 0, 2, 3 };
    const nhal_gfx_buffer_desc vd = { sizeof quad, NHAL_GFX_BUFFER_VERTEX };
    CHECK_OK(G->buffer_create(&vd, quad, &g_vertices));
    const nhal_gfx_buffer_desc id = { sizeof idx, NHAL_GFX_BUFFER_INDEX };
    CHECK_OK(G->buffer_create(&id, idx, &g_indices));
    const nhal_gfx_buffer_desc ud = { 512, NHAL_GFX_BUFFER_UNIFORM | NHAL_GFX_BUFFER_COPY_DST };
    CHECK_OK(G->buffer_create(&ud, NULL, &g_uniform));
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    CHECK_OK(G->buffer_write(g_uniform, 256, red, sizeof red)); /* zweiter 256er-Block */
    CHECK(G->buffer_write(g_uniform, 2, red, sizeof red).code == NHAL_ERR_INVALID_ARGUMENT);
    const nhal_gfx_buffer_desc rd = { ROW * H, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC };
    CHECK_OK(G->buffer_create(&rd, NULL, &g_tex_readback));

    const nhal_gfx_shader_desc sd = { k_render_msl, k_render_wgsl, NULL, NULL, 0 };
    CHECK_OK(G->shader_create(&sd, &g_rs));
    const nhal_gfx_vertex_buffer_layout vl = { 8, false, 1, { { NHAL_GFX_VERTEX_FLOAT32X2, 0, 0 } } };
    const nhal_gfx_binding_desc ub = { 0, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE };
    nhal_gfx_render_pipeline_desc rp;
    memset(&rp, 0, sizeof rp);
    rp.shader = g_rs;
    rp.vertex_entry = "vs";
    rp.fragment_entry = "fs";
    rp.vertex_buffer_count = 1;
    rp.vertex_buffers = &vl;
    rp.binding_count = 1;
    rp.bindings = &ub;
    rp.color_count = 1;
    rp.colors[0].format = NHAL_GFX_FORMAT_RGBA8_UNORM;
    rp.topology = NHAL_GFX_TRIANGLES;
    rp.cull = NHAL_GFX_CULL_BACK; /* prüft, dass CCW überall vorne ist */
    CHECK_OK(G->render_pipeline_create(&rp, &g_rp));
    if (!g_target || !g_rp || !g_tex_readback) return;

    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc pd = { .color_count = 1, .colors = { { .texture = g_target, .clear_color = { 0.0f, 0.0f, 1.0f, 1.0f } } } };
    nhal_gfx_pass* p = G->render_pass_begin(c, &pd);
    CHECK(p != NULL);
    G->pass_set_pipeline(p, g_rp);
    G->pass_set_vertex_buffer(p, 0, g_vertices, 0);
    G->pass_set_index_buffer(p, g_indices, NHAL_GFX_INDEX_UINT16, 0);
    G->pass_set_buffer(p, 0, g_uniform, 256, 16);
    G->pass_draw_indexed(p, 6, 1, 0, 0, 0);
    G->pass_end(p);
    G->commands_copy_texture_to_buffer(c, g_target, g_tex_readback, 0, ROW);
    CHECK_OK(G->commands_submit(c, NULL));
    CHECK_OK(G->buffer_read(g_tex_readback, 0, sizeof g_render_out, g_render_out, read_done, NULL));
    ++g_pending;
}

static void setup_texture(void) {
    for (int y = 0; y < TW; ++y) {
        for (int x = 0; x < TW; ++x) {
            uint8_t* px = &g_checker[(y * TW + x) * 4];
            px[0] = (uint8_t)(x * 60);
            px[1] = (uint8_t)(y * 60);
            px[2] = (uint8_t)(((x + y) & 1) ? 255 : 0);
            px[3] = 255;
        }
    }
    const nhal_gfx_texture_desc cd = { TW, TW, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                       NHAL_GFX_TEXTURE_SAMPLED | NHAL_GFX_TEXTURE_COPY_DST };
    CHECK_OK(G->texture_create(&cd, &g_checker_tex));
    if (g_checker_tex) CHECK_OK(G->texture_write(g_checker_tex, g_checker, TW * 4));
    const nhal_gfx_sampler_desc sm = { NHAL_GFX_FILTER_NEAREST, NHAL_GFX_ADDRESS_CLAMP };
    CHECK_OK(G->sampler_create(&sm, &g_sampler));
    const nhal_gfx_binding_desc bs[2] = {
        { 0, NHAL_GFX_BIND_SAMPLED_TEXTURE, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE },
        { 1, NHAL_GFX_BIND_SAMPLER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE },
    };
    nhal_gfx_render_pipeline_desc rp;
    memset(&rp, 0, sizeof rp);
    rp.shader = g_rs;
    rp.vertex_entry = "tvs";
    rp.fragment_entry = "tfs";
    rp.binding_count = 2;
    rp.bindings = bs;
    rp.color_count = 1;
    rp.colors[0].format = NHAL_GFX_FORMAT_RGBA8_UNORM;
    CHECK_OK(G->render_pipeline_create(&rp, &g_tp));

    const nhal_gfx_texture_desc td = { TW, TW, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                       NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC };
    nhal_gfx_texture* small = NULL;
    CHECK_OK(G->texture_create(&td, &small));
    if (!g_tp || !small || !g_sampler || !g_checker_tex) {
        G->texture_destroy(small);
        return;
    }

    /* Fehlende Bindung: Die Liste merkt sich den Fehler, submit liefert ihn. */
    nhal_gfx_commands* bad = NULL;
    CHECK_OK(G->commands_begin(&bad));
    const nhal_gfx_render_pass_desc pd = { .color_count = 1, .colors = { { .texture = small, .clear_color = { 0, 0, 0, 0 } } } };
    nhal_gfx_pass* p = G->render_pass_begin(bad, &pd);
    G->pass_set_pipeline(p, g_tp);
    G->pass_set_texture(p, 0, g_checker_tex);
    G->pass_draw(p, 3, 1, 0, 0);
    G->pass_end(p);
    CHECK(G->commands_submit(bad, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    p = G->render_pass_begin(c, &pd);
    G->pass_set_pipeline(p, g_tp);
    G->pass_set_texture(p, 0, g_checker_tex);
    G->pass_set_sampler(p, 1, g_sampler);
    G->pass_draw(p, 3, 1, 0, 0);
    G->pass_end(p);
    G->commands_copy_texture_to_buffer(c, small, g_tex_readback, 0, ROW);
    CHECK_OK(G->commands_submit(c, NULL));
    /* Liest nach dem Render-Ergebnis oben aus demselben Puffer: Reihenfolge der Listen. */
    CHECK_OK(G->buffer_read(g_tex_readback, 0, sizeof g_tex_out, g_tex_out, read_done, NULL));
    ++g_pending;
    G->texture_destroy(small); /* GPU-Arbeit läuft weiter, das Backend hält die Textur */
}

static void setup_surface(void) {
    nhal_gfx_surface_desc sd = { 0, NULL };
#ifdef __EMSCRIPTEN__
    sd.kind = NHAL_GFX_SURFACE_CANVAS;
    sd.handle = (void*)"#nhal-canvas";
#elif defined(__APPLE__)
    g_layer = ((void* (*)(Class, SEL))objc_msgSend)(objc_getClass("CAMetalLayer"), sel_registerName("new"));
    sd.kind = NHAL_GFX_SURFACE_METAL_LAYER;
    sd.handle = g_layer;
#endif
    const nhal_status s = G->surface_create(&sd, &g_surface);
    if (s.code != NHAL_OK) {
        printf("gfx_test: Oberfläche übersprungen – %s\n", s.reason);
        return;
    }
    CHECK_OK(G->surface_configure(g_surface, 32, 16, true));
    g_surface_format = G->surface_format(g_surface);
    CHECK(g_surface_format == NHAL_GFX_FORMAT_BGRA8_UNORM || g_surface_format == NHAL_GFX_FORMAT_RGBA8_UNORM);
    nhal_gfx_texture* frame = NULL;
    const nhal_status as = G->surface_acquire(g_surface, &frame);
    if (as.code == NHAL_ERR_BUSY) {
        printf("gfx_test: Oberfläche hat gerade keine Textur frei – übersprungen\n");
        return;
    }
    CHECK_OK(as);
    if (!frame) return;
    const nhal_gfx_buffer_desc rd = { ROW * 16, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC };
    CHECK_OK(G->buffer_create(&rd, NULL, &g_surf_readback));
    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc pd = { .color_count = 1, .colors = { { .texture = frame, .clear_color = { 0.0f, 1.0f, 0.0f, 1.0f } } } };
    G->pass_end(G->render_pass_begin(c, &pd));
    G->commands_copy_texture_to_buffer(c, frame, g_surf_readback, 0, ROW);
    CHECK_OK(G->commands_submit(c, NULL));
    CHECK_OK(G->surface_present(g_surface));
    CHECK_OK(G->buffer_read(g_surf_readback, 0, sizeof g_surface_out, g_surface_out, read_done, NULL));
    ++g_pending;
    g_have_surface = true;
}

/* ── Ergebnisse ────────────────────────────────────────────────────────── */

static bool pixel_is(const uint8_t* px, int r, int g, int b) {
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1 && px[3] == 255;
}

static void verify(void) {
    if (g_readback) {
        int wrong = 0;
        for (int i = 0; i < N; ++i) wrong += g_compute_out[i] != (float)(2 * i);
        CHECK(wrong == 0);
        printf("gfx_test: Compute %d/%d Werte richtig\n", N - wrong, N);
    }
    if (g_tex_readback) {
        /* g_render_out wurde vor dem Textur-Test gelesen: links rot, rechts blau. */
        CHECK(pixel_is(&g_render_out[32 * ROW + 8 * 4], 255, 0, 0));
        CHECK(pixel_is(&g_render_out[32 * ROW + 56 * 4], 0, 0, 255));
        CHECK(pixel_is(&g_render_out[0 * ROW + 0 * 4], 255, 0, 0));
        CHECK(pixel_is(&g_render_out[63 * ROW + 63 * 4], 0, 0, 255));
        int wrong = 0;
        for (int y = 0; y < TW; ++y) wrong += memcmp(&g_tex_out[y * ROW], &g_checker[y * TW * 4], TW * 4) != 0;
        CHECK(wrong == 0);
        printf("gfx_test: Render und Textur geprüft\n");
    }
    if (g_have_surface) {
        const uint8_t* px = &g_surface_out[4 * 4];
        CHECK(px[1] == 255 && px[0] == 0 && px[2] == 0 && px[3] == 255); /* grün in RGBA und BGRA gleich */
        printf("gfx_test: Oberfläche %s geprüft\n",
               g_surface_format == NHAL_GFX_FORMAT_BGRA8_UNORM ? "(BGRA8)" : "(RGBA8)");
    }
}

static void frame(void) {
    if (g_finished) return;
    ++g_frames;
    G->poll();
    switch (g_step) {
    case 0: {
        const nhal_status s = G->ready();
        if (s.code == NHAL_ERR_BUSY) {
            if (g_frames > 1200) {
                fprintf(stderr, "gfx_test: Gerät kam nicht\n");
                ++g_failures;
                finish();
            }
            return;
        }
        if (s.code != NHAL_OK) {
            printf("gfx_test: keine GPU – %s\n", s.reason);
            finish();
            return;
        }
        nhal_gfx_info info;
        G->info(&info);
        printf("gfx_test: %s auf %s, Unified Memory: %s, dauerhaftes Mapping: %s\n", info.backend, info.device_name,
               info.unified_memory ? "ja" : "nein", info.persistent_mapping ? "ja" : "nein");
        check_rules(&info);
        setup_compute();
        setup_render();
        setup_texture();
        setup_surface();
        g_step = 1;
        g_frames = 0;
        return;
    }
    case 1:
        if (g_pending > 0) {
            if (g_frames > 1200) {
                fprintf(stderr, "gfx_test: %d Lesevorgang/-vorgänge kamen nicht zurück\n", g_pending);
                ++g_failures;
                finish();
            }
            return;
        }
        verify();
        finish();
        return;
    }
}

int main(void) {
    const nhal_request req = { NHAL_FEATURE_GFX_DEVICE, 2u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    const nhal_status s = nhal_init(&info, &g_ctx);
    if (s.code == NHAL_ERR_UNAVAILABLE) { /* z. B. kein Metal-Gerät */
        printf("gfx_test: keine GPU – %s\n", s.reason);
        return 0;
    }
    CHECK_OK(s);
    G = nhal_get_gfx_device_v2(g_ctx);
    if (!G) return check_report("gfx_test");
    frame();
#ifdef __EMSCRIPTEN__
    if (!g_finished) emscripten_set_main_loop(frame, 0, false);
#else
    while (!g_finished) {
        frame();
        usleep(1000);
    }
#endif
    return g_rc;
}
