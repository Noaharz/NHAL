/*
 * gfx.device v2: Gruppen, Push-Konstanten, Seriennummern, MRT, MSAA mit
 * resolve, Scissor, Schreibmaske, Blend-Arten, Tiefenvergleich, Drehsinn,
 * Indirect (draw, draw_indexed, dispatch), Occlusion- und – wo vorhanden –
 * Zeitstempel-Abfragen. Dazu die Prüfungen, die vor der GPU greifen.
 * Alle Ergebnisse werden zurückgelesen und pixel- bzw. wertweise geprüft.
 *
 * Zustandsmaschine wie gfx_test, weil im Browser Gerät, Lesen und
 * Fortschritt erst in der Ereignisschleife ankommen.
 */
#include "nhal/features/gfx/device.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <unistd.h>
#endif

/* ── Shader ────────────────────────────────────────────────────────────── */

static const char* k_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct C { float4 color; };\n"
    "struct Tint { float4 color; };\n"
    "struct M { float mul; float p0; float p1; float p2; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut tri(uint i [[vertex_id]]) {\n"
    "    float x = float((i << 1) & 2); float y = float(i & 2);\n"
    "    VOut o; o.pos = float4(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.5, 1.0); return o;\n"
    "}\n"
    "struct FOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
    "fragment FOut mrt(constant C& c [[buffer(30)]], constant Tint& t [[buffer(0)]]) {\n"
    "    FOut o; o.c0 = c.color; o.c1 = t.color; return o;\n"
    "}\n"
    "constant float2 kPos[4] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, 1) };\n"
    "vertex VOut rect(uint i [[vertex_id]]) { VOut o; o.pos = float4(kPos[i], 0.5, 1.0); return o; }\n"
    "fragment float4 solid(constant C& c [[buffer(30)]]) { return c.color; }\n"
    "kernel void scale(device const float* addv [[buffer(0)]], device float* data [[buffer(1)]],\n"
    "                  constant M& m [[buffer(30)]], uint id [[thread_position_in_grid]]) {\n"
    "    data[id] = data[id] * m.mul + addv[id];\n"
    "}\n";

static const char* k_wgsl =
    "struct C { color: vec4<f32> };\n"
    "struct Tint { color: vec4<f32> };\n"
    "@group(3) @binding(0) var<uniform> c: C;\n"
    "@group(0) @binding(0) var<uniform> tint: Tint;\n"
    "@vertex fn tri(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
    "    let x = f32((i << 1u) & 2u); let y = f32(i & 2u);\n"
    "    return vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.5, 1.0);\n"
    "}\n"
    "struct FOut { @location(0) c0: vec4<f32>, @location(1) c1: vec4<f32> };\n"
    "@fragment fn mrt() -> FOut { var o: FOut; o.c0 = c.color; o.c1 = tint.color; return o; }\n"
    "@vertex fn rect(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
    "    var p = array<vec2<f32>, 4>(vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),\n"
    "                                vec2<f32>(-1.0, 1.0));\n"
    "    return vec4<f32>(p[i], 0.5, 1.0);\n"
    "}\n"
    "@fragment fn solid() -> @location(0) vec4<f32> { return c.color; }\n";

static const char* k_wgsl_compute =
    "struct M { mul: f32, p0: f32, p1: f32, p2: f32 };\n"
    "@group(0) @binding(0) var<storage, read> addv: array<f32>;\n"
    "@group(1) @binding(1) var<storage, read_write> data: array<f32>;\n"
    "@group(3) @binding(0) var<uniform> m: M;\n"
    "@compute @workgroup_size(64) fn scale(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "    if (id.x < arrayLength(&data)) { data[id.x] = data[id.x] * m.mul + addv[id.x]; }\n"
    "}\n";

/* ── Zustand ───────────────────────────────────────────────────────────── */

enum { N = 256, W = 64, H = 64, SW = 16, ROW = 256 };

static nhal_context* g_ctx;
static const nhal_gfx_device_v2* G;
static nhal_gfx_info g_info;
static int g_step, g_frames, g_pending;
static bool g_finished;
static int g_rc;
static uint64_t g_serials[3];

static float g_compute_out[N];
static uint8_t g_c0[ROW * H], g_c1[ROW * H], g_idx[ROW * SW];
static uint64_t g_queries[64]; /* 0..1 Occlusion, 32..35 Zeitstempel (Offset 256) */

static nhal_gfx_shader *g_sh, *g_csh;
static nhal_gfx_group_layout *g_tint_layout, *g_data_layout;
static nhal_gfx_group *g_tint_group, *g_data_group;
static nhal_gfx_pipeline *g_mrt, *g_solid, *g_scale;
static nhal_gfx_query_set *g_occ, *g_ts;
static nhal_gfx_buffer *g_tint, *g_add, *g_data, *g_args, *g_index, *g_query_buf;
static nhal_gfx_buffer *g_rb_c0, *g_rb_c1, *g_rb_idx;
static nhal_gfx_texture *g_ms0, *g_ms1, *g_msd, *g_res0, *g_res1, *g_small;

static void read_done(void* user, nhal_status s) {
    (void)user;
    CHECK_OK(s);
    --g_pending;
}

static void drain_errors(void) {
    char msg[256];
    while (G->take_error(msg, sizeof msg)) {
        fprintf(stderr, "gfx2_test: GPU meldet: %s\n", msg);
        ++g_failures;
    }
}

static void finish(void) {
    if (G) {
        drain_errors();
        nhal_gfx_buffer* bufs[] = { g_tint, g_add, g_data, g_args, g_index, g_query_buf, g_rb_c0, g_rb_c1, g_rb_idx };
        for (size_t i = 0; i < sizeof bufs / sizeof bufs[0]; ++i) G->buffer_destroy(bufs[i]);
        nhal_gfx_texture* texs[] = { g_ms0, g_ms1, g_msd, g_res0, g_res1, g_small };
        for (size_t i = 0; i < sizeof texs / sizeof texs[0]; ++i) G->texture_destroy(texs[i]);
        G->group_destroy(g_tint_group);
        G->group_destroy(g_data_group);
        G->pipeline_destroy(g_mrt);
        G->pipeline_destroy(g_solid);
        G->pipeline_destroy(g_scale);
        G->group_layout_destroy(g_tint_layout);
        G->group_layout_destroy(g_data_layout);
        G->query_set_destroy(g_occ);
        G->query_set_destroy(g_ts);
        G->shader_destroy(g_sh);
        G->shader_destroy(g_csh);
    }
    nhal_shutdown(g_ctx);
    g_rc = check_report("gfx2_test");
    g_finished = true;
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#endif
}

static nhal_gfx_buffer* make_buffer(uint64_t size, uint32_t usage, const void* init) {
    nhal_gfx_buffer* b = NULL;
    const nhal_gfx_buffer_desc d = { size, usage };
    CHECK_OK(G->buffer_create(&d, init, &b));
    return b;
}

static nhal_gfx_texture* make_texture(uint32_t w, uint32_t h, uint32_t usage, uint32_t samples, nhal_gfx_format f) {
    nhal_gfx_texture* t = NULL;
    const nhal_gfx_texture_desc d = { w, h, f, usage, samples };
    CHECK_OK(G->texture_create(&d, &t));
    return t;
}

/* ── Prüfungen vor der GPU ─────────────────────────────────────────────── */

static void check_rules(void) {
    nhal_gfx_group_layout* l = NULL;
    const nhal_gfx_binding_desc dup[2] = {
        { 2, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE },
        { 2, NHAL_GFX_BIND_SAMPLER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE },
    };
    const nhal_gfx_group_layout_desc dd = { 2, dup };
    CHECK(G->group_layout_create(&dd, &l).code == NHAL_ERR_INVALID_ARGUMENT && !l);

    /* Gruppe: falsche Usage und fehlender Eintrag. */
    nhal_gfx_buffer* storage = make_buffer(256, NHAL_GFX_BUFFER_STORAGE, NULL);
    nhal_gfx_group* g = NULL;
    nhal_gfx_group_entry e = { 0, storage, 0, 0, NULL, NULL };
    const nhal_gfx_group_desc wrong = { g_tint_layout, 1, &e };
    CHECK(G->group_create(&wrong, &g).code == NHAL_ERR_INVALID_ARGUMENT && !g);
    const nhal_gfx_group_desc empty = { g_tint_layout, 0, NULL };
    CHECK(G->group_create(&empty, &g).code == NHAL_ERR_INVALID_ARGUMENT && !g);
    e.buffer = g_tint;
    e.offset = 4; /* nicht auf 256 ausgerichtet */
    CHECK(G->group_create(&wrong, &g).code == NHAL_ERR_INVALID_ARGUMENT && !g);
    G->buffer_destroy(storage);

    /* Pipelines: Gruppe 0 doppelt belegt, Slot doppelt über Gruppen, zu viele Konstanten. */
    const nhal_gfx_binding_desc slot0 = { 0, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE };
    nhal_gfx_render_pipeline_desc rp;
    memset(&rp, 0, sizeof rp);
    rp.shader = g_sh;
    rp.vertex_entry = "tri";
    rp.fragment_entry = "mrt";
    rp.color_count = 1;
    rp.colors[0].format = NHAL_GFX_FORMAT_RGBA8_UNORM;
    rp.binding_count = 1;
    rp.bindings = &slot0;
    rp.groups[0] = g_tint_layout;
    nhal_gfx_pipeline* p = NULL;
    CHECK(G->render_pipeline_create(&rp, &p).code == NHAL_ERR_INVALID_ARGUMENT && !p);
    rp.groups[0] = NULL;
    rp.groups[1] = g_tint_layout; /* auch Slot 0 */
    CHECK(G->render_pipeline_create(&rp, &p).code == NHAL_ERR_INVALID_ARGUMENT && !p);
    rp.groups[1] = NULL;
    rp.constants_size = NHAL_GFX_MAX_CONSTANTS + 4u;
    CHECK(G->render_pipeline_create(&rp, &p).code == NHAL_ERR_INVALID_ARGUMENT && !p);
    rp.constants_size = 16;
    rp.sample_count = 2;
    CHECK(G->render_pipeline_create(&rp, &p).code == NHAL_ERR_INVALID_ARGUMENT && !p);

    /* MSAA-Texturen sind nur Render-Ziel. */
    nhal_gfx_texture* t = NULL;
    const nhal_gfx_texture_desc ms_sampled = { 8, 8, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                               NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_SAMPLED, 4 };
    CHECK(G->texture_create(&ms_sampled, &t).code == NHAL_ERR_INVALID_ARGUMENT && !t);
    const nhal_gfx_texture_desc ms2 = { 8, 8, NHAL_GFX_FORMAT_RGBA8_UNORM, NHAL_GFX_TEXTURE_RENDER_TARGET, 2 };
    CHECK(G->texture_create(&ms2, &t).code == NHAL_ERR_INVALID_ARGUMENT && !t);

    /* Abfragen. */
    nhal_gfx_query_set* q = NULL;
    const nhal_gfx_query_set_desc none = { NHAL_GFX_QUERY_OCCLUSION, 0 };
    CHECK(G->query_set_create(&none, &q).code == NHAL_ERR_INVALID_ARGUMENT && !q);
    if (!g_info.timestamp_queries) {
        const nhal_gfx_query_set_desc ts = { NHAL_GFX_QUERY_TIMESTAMP, 2 };
        CHECK(G->query_set_create(&ts, &q).code == NHAL_ERR_UNSUPPORTED && !q);
    }

    /* Seriennummern. */
    CHECK_OK(G->wait(0, 0));
    CHECK(G->wait(1000000u, 0).code == NHAL_ERR_INVALID_ARGUMENT);
}

/* Fehler beim Aufzeichnen: jede Liste muss bei submit INVALID_ARGUMENT melden. */
static void check_recording_errors(void) {
    const nhal_gfx_render_pass_desc small = { .color_count = 1, .colors = { { .texture = g_small } } };
    uint64_t serial = 1;

    /* 1. Pipeline braucht Konstanten, keine gesetzt. */
    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    nhal_gfx_pass* p = G->render_pass_begin(c, &small);
    G->pass_set_pipeline(p, g_solid);
    G->pass_draw(p, 3, 1, 0, 0);
    G->pass_end(p);
    CHECK(G->commands_submit(c, &serial).code == NHAL_ERR_INVALID_ARGUMENT && serial == 0);

    /* 2. MSAA-Pipeline in einem Pass ohne MSAA. */
    CHECK_OK(G->commands_begin(&c));
    p = G->render_pass_begin(c, &small);
    G->pass_set_pipeline(p, g_mrt);
    G->pass_end(p);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

    /* 3. Scissor außerhalb, Indirect ohne INDIRECT-Usage. */
    CHECK_OK(G->commands_begin(&c));
    p = G->render_pass_begin(c, &small);
    G->pass_set_scissor(p, 8, 8, 16, 16);
    G->pass_end(p);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK_OK(G->commands_begin(&c));
    p = G->render_pass_begin(c, &small);
    G->pass_set_pipeline(p, g_solid);
    const float k[4] = { 1, 1, 1, 1 };
    G->pass_set_constants(p, k, sizeof k);
    G->pass_draw_indirect(p, g_tint, 0);
    G->pass_end(p);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

    /* 4. Resolve in einen Puffer ohne QUERY_RESOLVE; MSAA ohne passendes resolve. */
    CHECK_OK(G->commands_begin(&c));
    G->commands_resolve_queries(c, g_occ, 0, 1, g_rb_c0, 0);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc bad_resolve = { .color_count = 1,
                                                    .colors = { { .texture = g_ms0, .resolve = g_small } } };
    CHECK(G->render_pass_begin(c, &bad_resolve) == NULL);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

    /* 5. Gruppe mit anderem Layout als die Pipeline. */
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc ms = {
        .color_count = 2,
        .colors = { { .texture = g_ms0, .resolve = g_res0 }, { .texture = g_ms1, .resolve = g_res1 } },
        .depth = g_msd,
    };
    p = G->render_pass_begin(c, &ms);
    G->pass_set_pipeline(p, g_mrt);
    G->pass_set_constants(p, k, sizeof k);
    G->pass_set_group(p, 0, g_data_group);
    G->pass_draw(p, 3, 1, 0, 0);
    G->pass_end(p);
    CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);
}

/* ── GPU-Arbeit ────────────────────────────────────────────────────────── */

static void setup(void) {
    const nhal_gfx_shader_desc sd = { k_msl, k_wgsl, NULL, NULL, 0 };
    CHECK_OK(G->shader_create(&sd, &g_sh));
    const nhal_gfx_shader_desc csd = { k_msl, k_wgsl_compute, NULL, NULL, 0 };
    CHECK_OK(G->shader_create(&csd, &g_csh));

    /* Gruppen-Layouts: Tint (Uniform, Slot 0) und Daten (Storage, Slot 1). */
    const nhal_gfx_binding_desc tint_b = { 0, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_FRAGMENT, NHAL_GFX_FORMAT_NONE };
    const nhal_gfx_group_layout_desc tl = { 1, &tint_b };
    CHECK_OK(G->group_layout_create(&tl, &g_tint_layout));
    const nhal_gfx_binding_desc data_b = { 1, NHAL_GFX_BIND_STORAGE_BUFFER, NHAL_GFX_STAGE_COMPUTE, NHAL_GFX_FORMAT_NONE };
    const nhal_gfx_group_layout_desc dl = { 1, &data_b };
    CHECK_OK(G->group_layout_create(&dl, &g_data_layout));

    float tint[64] = { 0.0f, 0.5f, 0.0f, 1.0f }; /* 256 Bytes */
    g_tint = make_buffer(sizeof tint, NHAL_GFX_BUFFER_UNIFORM, tint);
    float data[N], add[N];
    for (int i = 0; i < N; ++i) {
        data[i] = (float)i;
        add[i] = 0.5f * (float)i;
    }
    g_data = make_buffer(sizeof data, NHAL_GFX_BUFFER_STORAGE | NHAL_GFX_BUFFER_COPY_SRC, data);
    g_add = make_buffer(sizeof add, NHAL_GFX_BUFFER_STORAGE, add);
    /* draw {3,1,0,0} | draw_indexed {6,1,0,0,0} | dispatch {N/64,1,1} */
    const uint32_t args[12] = { 3, 1, 0, 0, 6, 1, 0, 0, 0, N / 64, 1, 1 };
    g_args = make_buffer(sizeof args, NHAL_GFX_BUFFER_INDIRECT, args);
    const uint16_t idx[6] = { 0, 3, 2, 0, 2, 1 }; /* im Uhrzeigersinn */
    g_index = make_buffer(sizeof idx, NHAL_GFX_BUFFER_INDEX, idx);
    g_query_buf = make_buffer(512, NHAL_GFX_BUFFER_QUERY_RESOLVE | NHAL_GFX_BUFFER_COPY_SRC, NULL);
    g_rb_c0 = make_buffer(ROW * H, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC, NULL);
    g_rb_c1 = make_buffer(ROW * H, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC, NULL);
    g_rb_idx = make_buffer(ROW * SW, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC, NULL);

    const uint32_t rt = NHAL_GFX_TEXTURE_RENDER_TARGET;
    const uint32_t rt_copy = NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC;
    g_ms0 = make_texture(W, H, rt, 4, NHAL_GFX_FORMAT_RGBA8_UNORM);
    g_ms1 = make_texture(W, H, rt, 4, NHAL_GFX_FORMAT_RGBA8_UNORM);
    g_msd = make_texture(W, H, rt, 4, NHAL_GFX_FORMAT_DEPTH32_FLOAT);
    g_res0 = make_texture(W, H, rt_copy, 1, NHAL_GFX_FORMAT_RGBA8_UNORM);
    g_res1 = make_texture(W, H, rt_copy, 1, NHAL_GFX_FORMAT_RGBA8_UNORM);
    g_small = make_texture(SW, SW, rt_copy, 0, NHAL_GFX_FORMAT_RGBA8_UNORM);

    if (g_tint_layout && g_tint) {
        const nhal_gfx_group_entry e = { 0, g_tint, 0, 16, NULL, NULL };
        const nhal_gfx_group_desc gd = { g_tint_layout, 1, &e };
        CHECK_OK(G->group_create(&gd, &g_tint_group));
    }
    if (g_data_layout && g_data) {
        const nhal_gfx_group_entry e = { 1, g_data, 0, 0, NULL, NULL };
        const nhal_gfx_group_desc gd = { g_data_layout, 1, &e };
        CHECK_OK(G->group_create(&gd, &g_data_group));
    }

    /* MRT + MSAA: Ziel 0 schreibt nur R und A, Ziel 1 addiert. */
    nhal_gfx_render_pipeline_desc rp;
    memset(&rp, 0, sizeof rp);
    rp.shader = g_sh;
    rp.vertex_entry = "tri";
    rp.fragment_entry = "mrt";
    rp.groups[0] = g_tint_layout;
    rp.constants_size = 16;
    rp.color_count = 2;
    rp.colors[0] = (nhal_gfx_color_target){ NHAL_GFX_FORMAT_RGBA8_UNORM, NHAL_GFX_BLEND_NONE,
                                            NHAL_GFX_WRITE_R | NHAL_GFX_WRITE_A };
    rp.colors[1] = (nhal_gfx_color_target){ NHAL_GFX_FORMAT_RGBA8_UNORM, NHAL_GFX_BLEND_ADDITIVE, 0 };
    rp.depth_format = NHAL_GFX_FORMAT_DEPTH32_FLOAT;
    rp.depth_test = true;
    rp.depth_write = true;
    rp.depth_compare = NHAL_GFX_COMPARE_LESS_EQUAL;
    rp.sample_count = 4;
    CHECK_OK(G->render_pipeline_create(&rp, &g_mrt));

    /* Indexiertes Rechteck im Uhrzeigersinn: sichtbar nur mit front_face CW. */
    memset(&rp, 0, sizeof rp);
    rp.shader = g_sh;
    rp.vertex_entry = "rect";
    rp.fragment_entry = "solid";
    rp.constants_size = 16;
    rp.color_count = 1;
    rp.colors[0].format = NHAL_GFX_FORMAT_RGBA8_UNORM;
    rp.cull = NHAL_GFX_CULL_BACK;
    rp.front_face = NHAL_GFX_FRONT_CW;
    CHECK_OK(G->render_pipeline_create(&rp, &g_solid));

    /* Compute: Slot-Bindung (Gruppe 0) + Gruppe 1 + Konstanten. */
    const nhal_gfx_binding_desc add_b = { 0, NHAL_GFX_BIND_READ_STORAGE_BUFFER, NHAL_GFX_STAGE_COMPUTE,
                                          NHAL_GFX_FORMAT_NONE };
    nhal_gfx_compute_pipeline_desc cp;
    memset(&cp, 0, sizeof cp);
    cp.shader = g_csh;
    cp.entry = "scale";
    cp.binding_count = 1;
    cp.bindings = &add_b;
    cp.groups[1] = g_data_layout;
    cp.constants_size = 16;
    cp.workgroup_size[0] = 64;
    cp.workgroup_size[1] = cp.workgroup_size[2] = 1;
    CHECK_OK(G->compute_pipeline_create(&cp, &g_scale));

    const nhal_gfx_query_set_desc od = { NHAL_GFX_QUERY_OCCLUSION, 2 };
    CHECK_OK(G->query_set_create(&od, &g_occ));
    if (g_info.timestamp_queries) {
        const nhal_gfx_query_set_desc td = { NHAL_GFX_QUERY_TIMESTAMP, 4 };
        CHECK_OK(G->query_set_create(&td, &g_ts));
    }
}

static bool all_created(void) {
    return g_mrt && g_solid && g_scale && g_occ && g_tint_group && g_data_group && g_args && g_index &&
           g_query_buf && g_rb_c0 && g_rb_c1 && g_rb_idx && g_ms0 && g_ms1 && g_msd && g_res0 && g_res1 && g_small &&
           (g_ts || !g_info.timestamp_queries);
}

static void record_work(void) {
    /* 1. Compute über Indirect-Dispatch. */
    nhal_gfx_commands* c = NULL;
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_compute_pass_desc cpd = { { g_ts, 2, 3 } };
    nhal_gfx_pass* p = G->compute_pass_begin(c, &cpd);
    CHECK(p != NULL);
    const float m[4] = { 3.0f, 0, 0, 0 };
    G->pass_set_constants(p, m, sizeof m); /* vor der Pipeline: bleibt gesetzt */
    G->pass_set_pipeline(p, g_scale);
    G->pass_set_buffer(p, 0, g_add, 0, 0);
    G->pass_set_group(p, 1, g_data_group);
    G->pass_dispatch_indirect(p, g_args, 36);
    G->pass_end(p);
    CHECK_OK(G->commands_submit(c, &g_serials[0]));

    /* 2. MRT + MSAA + Scissor + Indirect + Occlusion. */
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc rd = {
        .color_count = 2,
        .colors = {
            { .texture = g_ms0, .resolve = g_res0, .store = NHAL_GFX_STORE_DISCARD, .clear_color = { 0, 0, 0.5f, 1 } },
            { .texture = g_ms1, .resolve = g_res1, .store = NHAL_GFX_STORE_DISCARD, .clear_color = { 0.25f, 0, 0, 1 } },
        },
        .depth = g_msd,
        .depth_store = NHAL_GFX_STORE_DISCARD,
        .clear_depth = 1.0f,
        .occlusion = g_occ,
        .timestamps = { g_ts, 0, 1 },
    };
    p = G->render_pass_begin(c, &rd);
    CHECK(p != NULL);
    const float white[4] = { 1, 1, 1, 1 };
    G->pass_set_pipeline(p, g_mrt);
    G->pass_set_constants(p, white, sizeof white);
    G->pass_set_group(p, 0, g_tint_group);
    G->pass_set_scissor(p, 0, 0, W / 2, H);
    G->pass_begin_occlusion(p, 0);
    G->pass_draw_indirect(p, g_args, 0);
    G->pass_end_occlusion(p);
    G->pass_set_scissor(p, 0, 0, 0, 0); /* nichts sichtbar */
    G->pass_begin_occlusion(p, 1);
    G->pass_draw(p, 3, 1, 0, 0);
    G->pass_end_occlusion(p);
    G->pass_end(p);
    G->commands_copy_texture_to_buffer(c, g_res0, g_rb_c0, 0, ROW);
    G->commands_copy_texture_to_buffer(c, g_res1, g_rb_c1, 0, ROW);
    G->commands_resolve_queries(c, g_occ, 0, 2, g_query_buf, 0);
    if (g_ts) G->commands_resolve_queries(c, g_ts, 0, 4, g_query_buf, 256);
    CHECK_OK(G->commands_submit(c, &g_serials[1]));

    /* 3. Indexiert über Indirect, Drehsinn im Uhrzeigersinn. */
    CHECK_OK(G->commands_begin(&c));
    const nhal_gfx_render_pass_desc sd = { .color_count = 1, .colors = { { .texture = g_small } } };
    p = G->render_pass_begin(c, &sd);
    const float green[4] = { 0, 1, 0, 1 };
    G->pass_set_pipeline(p, g_solid);
    G->pass_set_constants(p, green, sizeof green);
    G->pass_set_index_buffer(p, g_index, NHAL_GFX_INDEX_UINT16, 0);
    G->pass_draw_indexed_indirect(p, g_args, 16);
    G->pass_end(p);
    G->commands_copy_texture_to_buffer(c, g_small, g_rb_idx, 0, ROW);
    CHECK_OK(G->commands_submit(c, &g_serials[2]));

    CHECK(g_serials[0] > 0 && g_serials[1] == g_serials[0] + 1u && g_serials[2] == g_serials[1] + 1u);
    if (g_info.blocking_wait) {
        CHECK_OK(G->wait(g_serials[2], NHAL_GFX_WAIT_FOREVER));
        CHECK(G->completed() >= g_serials[2]);
    } else {
        CHECK(G->wait(g_serials[2], NHAL_GFX_WAIT_FOREVER).code == NHAL_ERR_BUSY || G->completed() >= g_serials[2]);
    }

    struct {
        nhal_gfx_buffer* b;
        void* dst;
        uint64_t size;
    } reads[] = {
        { g_data, g_compute_out, sizeof g_compute_out },
        { g_rb_c0, g_c0, sizeof g_c0 },
        { g_rb_c1, g_c1, sizeof g_c1 },
        { g_rb_idx, g_idx, sizeof g_idx },
        { g_query_buf, g_queries, g_ts ? 512u : 16u },
    };
    for (size_t i = 0; i < sizeof reads / sizeof reads[0]; ++i) {
        CHECK_OK(G->buffer_read(reads[i].b, 0, reads[i].size, reads[i].dst, read_done, NULL));
        ++g_pending;
    }
}

/* ── Ergebnisse ────────────────────────────────────────────────────────── */

static bool pixel_is(const uint8_t* px, int r, int g, int b, int a) {
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1 && abs(px[3] - a) <= 1;
}

static void verify(void) {
    int wrong = 0;
    for (int i = 0; i < N; ++i) wrong += g_compute_out[i] != 3.5f * (float)i;
    CHECK(wrong == 0);
    printf("gfx2_test: Compute (Gruppe 1 + Slot + Konstanten, Indirect) %d/%d richtig\n", N - wrong, N);

    const uint8_t* l0 = &g_c0[32 * ROW + 8 * 4];
    const uint8_t* r0 = &g_c0[32 * ROW + 56 * 4];
    const uint8_t* l1 = &g_c1[32 * ROW + 8 * 4];
    const uint8_t* r1 = &g_c1[32 * ROW + 56 * 4];
    CHECK(pixel_is(l0, 255, 0, 128, 255)); /* Schreibmaske R|A */
    CHECK(pixel_is(r0, 0, 0, 128, 255));   /* außerhalb des Scissors */
    CHECK(pixel_is(l1, 64, 128, 0, 255));  /* additiv auf (0.25, 0, 0) */
    CHECK(pixel_is(r1, 64, 0, 0, 255));
    printf("gfx2_test: MRT/MSAA links (%d,%d,%d,%d) (%d,%d,%d,%d), rechts (%d,%d,%d) (%d,%d,%d)\n", l0[0], l0[1],
           l0[2], l0[3], l1[0], l1[1], l1[2], l1[3], r0[0], r0[1], r0[2], r1[0], r1[1], r1[2]);

    CHECK(pixel_is(&g_idx[8 * ROW + 8 * 4], 0, 255, 0, 255));
    CHECK(pixel_is(&g_idx[0], 0, 255, 0, 255));
    printf("gfx2_test: Indexiert/Indirect/Drehsinn geprüft\n");

    CHECK(g_queries[0] > 0);
    CHECK(g_queries[1] == 0);
    printf("gfx2_test: Occlusion sichtbar %llu, verdeckt %llu\n", (unsigned long long)g_queries[0],
           (unsigned long long)g_queries[1]);
    if (g_ts) {
        const uint64_t* t = &g_queries[32];
        CHECK(t[1] >= t[0] && t[3] >= t[2]);
        printf("gfx2_test: Zeitstempel Render %.1f µs, Compute %.1f µs (Periode %.3f ns)\n",
               (double)(t[1] - t[0]) * g_info.timestamp_period / 1000.0,
               (double)(t[3] - t[2]) * g_info.timestamp_period / 1000.0, g_info.timestamp_period);
    } else {
        printf("gfx2_test: Zeitstempel hier nicht vorhanden – übersprungen\n");
    }
    CHECK(G->completed() >= g_serials[2]);
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
                fprintf(stderr, "gfx2_test: Gerät kam nicht\n");
                ++g_failures;
                finish();
            }
            return;
        }
        if (s.code != NHAL_OK) {
            printf("gfx2_test: keine GPU – %s\n", s.reason);
            finish();
            return;
        }
        G->info(&g_info);
        printf("gfx2_test: %s, blockierendes wait: %s, Zeitstempel: %s, first_instance indirekt: %s\n",
               g_info.backend, g_info.blocking_wait ? "ja" : "nein", g_info.timestamp_queries ? "ja" : "nein",
               g_info.indirect_first_instance ? "ja" : "nein");
        setup();
        if (!all_created()) {
            finish();
            return;
        }
        check_rules();
        check_recording_errors();
        record_work();
        g_step = 1;
        g_frames = 0;
        return;
    }
    case 1:
        if (g_pending > 0 || G->completed() < g_serials[2]) {
            if (g_frames > 1200) {
                fprintf(stderr, "gfx2_test: Lesen oder Fortschritt kam nicht (%d offen)\n", g_pending);
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
    if (s.code == NHAL_ERR_UNAVAILABLE) {
        printf("gfx2_test: keine GPU – %s\n", s.reason);
        return 0;
    }
    CHECK_OK(s);
    G = nhal_get_gfx_device_v2(g_ctx);
    if (!G) return check_report("gfx2_test");
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
