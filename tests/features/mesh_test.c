/*
 * gfx.mesh_shader: Object-Stufe reicht eine Farbe aus einem Uniform-Puffer
 * als Nutzlast weiter, die Mesh-Stufe baut daraus ein Rechteck (linke
 * Bildhälfte). Zurückgelesen und pixelweise geprüft. Läuft nur, wo das
 * Feature gebaut wird (heute macOS); ohne fähige GPU wird der Grund gemeldet.
 */
#include "nhal/features/gfx/mesh_shader.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* k_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Payload { float4 color; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "struct POut { float4 color; };\n"
    "using Mesh = metal::mesh<VOut, POut, 4, 2, topology::triangle>;\n"
    "constant float2 kPos[4] = { float2(-1, -1), float2(0, -1), float2(0, 1), float2(-1, 1) };\n"
    "constant ushort kIdx[6] = { 0, 1, 2, 0, 2, 3 };\n"
    "[[object]] void obj(object_data Payload& payload [[payload]], mesh_grid_properties grid,\n"
    "                    constant float4& color [[buffer(0)]], uint tid [[thread_index_in_threadgroup]]) {\n"
    "    if (tid == 0) { payload.color = color; grid.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"
    "}\n"
    "[[mesh]] void msh(Mesh m, const object_data Payload& payload [[payload]],\n"
    "                  uint tid [[thread_index_in_threadgroup]]) {\n"
    "    if (tid < 4) { VOut v; v.pos = float4(kPos[tid], 0.0, 1.0); m.set_vertex(tid, v); }\n"
    "    if (tid < 6) m.set_index(tid, kIdx[tid]);\n"
    "    if (tid < 2) { POut p; p.color = payload.color; m.set_primitive(tid, p); }\n"
    "    if (tid == 0) m.set_primitive_count(2);\n"
    "}\n"
    "struct FIn { VOut v; POut p; };\n"
    "fragment float4 frag(FIn in [[stage_in]]) { return in.p.color; }\n";

enum { W = 64, H = 64, ROW = 256 };

static int g_pending;
static uint8_t g_out[ROW * H];

static void read_done(void* user, nhal_status s) {
    (void)user;
    CHECK_OK(s);
    --g_pending;
}

static bool pixel_is(const uint8_t* px, int r, int g, int b) {
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1 && px[3] == 255;
}

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_GFX_DEVICE, 2u, NHAL_REQUIRED },
        { NHAL_FEATURE_GFX_MESH_SHADER, 1u, NHAL_OPTIONAL },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    const nhal_status s = nhal_init(&info, &ctx);
    if (s.code == NHAL_ERR_UNAVAILABLE) {
        printf("mesh_test: keine GPU – %s\n", s.reason);
        return 0;
    }
    CHECK_OK(s);
    const nhal_gfx_device_v2* G = nhal_get_gfx_device_v2(ctx);
    const nhal_gfx_mesh_shader_v1* M = nhal_get_gfx_mesh_shader_v1(ctx);
    if (!G) return check_report("mesh_test");
    if (!M) {
        nhal_feature_info fi;
        memset(&fi, 0, sizeof fi);
        nhal_query(ctx, NHAL_FEATURE_GFX_MESH_SHADER, &fi);
        printf("mesh_test: keine Mesh-Shader – %s\n", fi.status.reason ? fi.status.reason : "nicht verfügbar");
        nhal_shutdown(ctx);
        return check_report("mesh_test");
    }

    const nhal_gfx_shader_desc sd = { k_msl, NULL, NULL, NULL, 0 };
    nhal_gfx_shader* sh = NULL;
    CHECK_OK(G->shader_create(&sd, &sh));
    const nhal_gfx_binding_desc ub = { 0, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_OBJECT, NHAL_GFX_FORMAT_NONE };
    nhal_gfx_mesh_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.shader = sh;
    pd.object_entry = "obj";
    pd.mesh_entry = "msh";
    pd.fragment_entry = "frag";
    pd.binding_count = 1;
    pd.bindings = &ub;
    pd.color_count = 1;
    pd.colors[0].format = NHAL_GFX_FORMAT_RGBA8_UNORM;
    pd.cull = NHAL_GFX_CULL_BACK;
    pd.object_threads[0] = pd.object_threads[1] = pd.object_threads[2] = 1;
    pd.mesh_threads[0] = 6;
    pd.mesh_threads[1] = pd.mesh_threads[2] = 1;
    pd.payload_bytes = 16;
    nhal_gfx_pipeline* pipe = NULL;
    CHECK_OK(M->pipeline_create(&pd, &pipe));

    nhal_gfx_mesh_pipeline_desc bad = pd;
    bad.mesh_threads[0] = 0;
    nhal_gfx_pipeline* none = NULL;
    CHECK(M->pipeline_create(&bad, &none).code == NHAL_ERR_INVALID_ARGUMENT && !none);

    const nhal_gfx_texture_desc td = { W, H, NHAL_GFX_FORMAT_RGBA8_UNORM,
                                       NHAL_GFX_TEXTURE_RENDER_TARGET | NHAL_GFX_TEXTURE_COPY_SRC };
    nhal_gfx_texture* target = NULL;
    CHECK_OK(G->texture_create(&td, &target));
    float green[64] = { 0.0f, 1.0f, 0.0f, 1.0f }; /* Startdaten: ganze Puffergröße (256 Bytes) */
    const nhal_gfx_buffer_desc ud = { 256, NHAL_GFX_BUFFER_UNIFORM };
    nhal_gfx_buffer* uniform = NULL;
    CHECK_OK(G->buffer_create(&ud, green, &uniform));
    const nhal_gfx_buffer_desc rd = { ROW * H, NHAL_GFX_BUFFER_COPY_DST | NHAL_GFX_BUFFER_COPY_SRC };
    nhal_gfx_buffer* readback = NULL;
    CHECK_OK(G->buffer_create(&rd, NULL, &readback));

    if (pipe && target && uniform && readback) {
        /* Mesh-Pipeline in pass_draw: muss als Fehler in der Liste landen. */
        nhal_gfx_commands* c = NULL;
        const nhal_gfx_render_pass_desc rp = { .color_count = 1, .colors = { { .texture = target, .clear_color = { 0.0f, 0.0f, 0.0f, 1.0f } } } };
        CHECK_OK(G->commands_begin(&c));
        nhal_gfx_pass* p = G->render_pass_begin(c, &rp);
        G->pass_set_pipeline(p, pipe);
        G->pass_set_buffer(p, 0, uniform, 0, 16);
        G->pass_draw(p, 3, 1, 0, 0);
        G->pass_end(p);
        CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

        CHECK_OK(G->commands_begin(&c));
        p = G->render_pass_begin(c, &rp);
        G->pass_set_pipeline(p, pipe);
        G->pass_set_buffer(p, 0, uniform, 0, 16);
        M->draw(p, 1, 1, 1);
        G->pass_end(p);
        G->commands_copy_texture_to_buffer(c, target, readback, 0, ROW);
        CHECK_OK(G->commands_submit(c, NULL));
        CHECK_OK(G->buffer_read(readback, 0, sizeof g_out, g_out, read_done, NULL));
        ++g_pending;
        for (int i = 0; i < 5000 && g_pending > 0; ++i) {
            G->poll();
            usleep(1000);
        }
        CHECK(g_pending == 0);
        CHECK(pixel_is(&g_out[32 * ROW + 8 * 4], 0, 255, 0));
        CHECK(pixel_is(&g_out[32 * ROW + 56 * 4], 0, 0, 0));
        printf("mesh_test: Object → Mesh → Fragment geprüft\n");
    }

    char msg[256];
    while (G->take_error(msg, sizeof msg)) {
        fprintf(stderr, "mesh_test: GPU meldet: %s\n", msg);
        ++g_failures;
    }
    G->buffer_destroy(readback);
    G->buffer_destroy(uniform);
    G->texture_destroy(target);
    G->pipeline_destroy(pipe);
    G->shader_destroy(sh);
    nhal_shutdown(ctx);
    return check_report("mesh_test");
}
