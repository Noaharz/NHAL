/*
 * gfx.raytracing: eine BLAS (ein Dreieck), eine TLAS mit zwei Instanzen
 * (links und rechts verschoben). Ein Compute-Shader schießt pro Pixel einen
 * Strahl entlang −z und schreibt den Instanz-Index + 1 (0 = kein Treffer).
 * Geprüft: Treffer links/rechts, Leerraum, Symmetrie und die Regeln vor der
 * GPU. Läuft nur, wo das Feature gebaut wird (heute macOS).
 */
#include "nhal/features/gfx/raytracing.h"
#include "check.h"

#include <string.h>
#include <unistd.h>

static const char* k_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "using namespace raytracing;\n"
    "kernel void trace(instance_acceleration_structure as [[buffer(0)]], device uint* out [[buffer(1)]],\n"
    "                  uint2 id [[thread_position_in_grid]]) {\n"
    "    if (id.x >= 32 || id.y >= 32) return;\n"
    "    float2 p = (float2(id) + 0.5) / 16.0 - 1.0;\n"
    "    ray r(float3(p.x, -p.y, 1.0), float3(0.0, 0.0, -1.0), 0.0, 10.0);\n"
    "    intersector<triangle_data, instancing> isect;\n"
    "    intersection_result<triangle_data, instancing> res = isect.intersect(r, as);\n"
    "    out[id.y * 32 + id.x] = res.type == intersection_type::triangle ? res.instance_id + 1 : 0;\n"
    "}\n";

enum { S = 32 };

static int g_pending;
static uint32_t g_out[S * S];

static void read_done(void* user, nhal_status s) {
    (void)user;
    CHECK_OK(s);
    --g_pending;
}

static uint32_t at(int x, int y) { return g_out[y * S + x]; }

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_GFX_DEVICE, 2u, NHAL_REQUIRED },
        { NHAL_FEATURE_GFX_RAYTRACING, 1u, NHAL_OPTIONAL },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    const nhal_status s = nhal_init(&info, &ctx);
    if (s.code == NHAL_ERR_UNAVAILABLE) {
        printf("raytracing_test: keine GPU – %s\n", s.reason);
        return 0;
    }
    CHECK_OK(s);
    const nhal_gfx_device_v2* G = nhal_get_gfx_device_v2(ctx);
    const nhal_gfx_raytracing_v1* R = nhal_get_gfx_raytracing_v1(ctx);
    if (!G) return check_report("raytracing_test");
    if (!R) {
        nhal_feature_info fi;
        memset(&fi, 0, sizeof fi);
        nhal_query(ctx, NHAL_FEATURE_GFX_RAYTRACING, &fi);
        printf("raytracing_test: kein Raytracing – %s\n", fi.status.reason ? fi.status.reason : "nicht verfügbar");
        nhal_shutdown(ctx);
        return check_report("raytracing_test");
    }

    const float tri[9] = { -0.4f, -0.4f, 0.0f, 0.4f, -0.4f, 0.0f, 0.0f, 0.4f, 0.0f };
    const nhal_gfx_buffer_desc vd = { sizeof tri, NHAL_GFX_BUFFER_VERTEX };
    nhal_gfx_buffer* vertices = NULL;
    CHECK_OK(G->buffer_create(&vd, tri, &vertices));
    const nhal_gfx_buffer_desc od = { sizeof g_out, NHAL_GFX_BUFFER_STORAGE | NHAL_GFX_BUFFER_COPY_SRC };
    nhal_gfx_buffer* out = NULL;
    CHECK_OK(G->buffer_create(&od, NULL, &out));

    /* Regeln vor der GPU. */
    nhal_gfx_accel* none = NULL;
    nhal_gfx_blas_desc bd = { out, 0, 12, 3, NULL, 0, NHAL_GFX_INDEX_UINT16, 1, true };
    CHECK(R->blas_create(&bd, &none).code == NHAL_ERR_INVALID_ARGUMENT && !none); /* Usage VERTEX fehlt */
    bd.vertices = vertices;
    bd.triangle_count = 2;
    CHECK(R->blas_create(&bd, &none).code == NHAL_ERR_INVALID_ARGUMENT && !none); /* zu wenige Vertices */
    const nhal_gfx_tlas_desc big = { 70000 };
    CHECK(R->tlas_create(&big, &none).code == NHAL_ERR_INVALID_ARGUMENT && !none);

    bd.triangle_count = 1;
    nhal_gfx_accel *blas = NULL, *tlas = NULL;
    CHECK_OK(R->blas_create(&bd, &blas));
    const nhal_gfx_tlas_desc td = { 4 };
    CHECK_OK(R->tlas_create(&td, &tlas));

    const nhal_gfx_shader_desc sd = { k_msl, NULL, NULL, NULL, 0 };
    nhal_gfx_shader* sh = NULL;
    CHECK_OK(G->shader_create(&sd, &sh));
    const nhal_gfx_binding_desc ob = { 1, NHAL_GFX_BIND_STORAGE_BUFFER, NHAL_GFX_STAGE_COMPUTE, NHAL_GFX_FORMAT_NONE };
    nhal_gfx_compute_pipeline_desc cp;
    memset(&cp, 0, sizeof cp);
    cp.shader = sh;
    cp.entry = "trace";
    cp.binding_count = 1;
    cp.bindings = &ob;
    cp.workgroup_size[0] = cp.workgroup_size[1] = 8;
    cp.workgroup_size[2] = 1;
    nhal_gfx_pipeline* pipe = NULL;
    CHECK_OK(G->compute_pipeline_create(&cp, &pipe));

    const nhal_gfx_instance inst[2] = {
        { blas, { 1, 0, 0, -0.5f, 0, 1, 0, 0, 0, 0, 1, 0 }, 0 },
        { blas, { 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, 0 }, 0 },
    };

    if (blas && tlas && pipe && out) {
        /* TLAS vor der BLAS bauen: Fehler in der Liste. */
        nhal_gfx_commands* c = NULL;
        CHECK_OK(G->commands_begin(&c));
        R->commands_build_tlas(c, tlas, inst, 2);
        CHECK(G->commands_submit(c, NULL).code == NHAL_ERR_INVALID_ARGUMENT);

        CHECK_OK(G->commands_begin(&c));
        R->commands_build_blas(c, blas);
        R->commands_build_tlas(c, tlas, inst, 2);
        nhal_gfx_pass* p = G->compute_pass_begin(c, NULL);
        G->pass_set_pipeline(p, pipe);
        R->pass_set_tlas(p, 0, tlas);
        G->pass_set_buffer(p, 1, out, 0, 0);
        G->pass_dispatch(p, S / 8, S / 8, 1);
        G->pass_end(p);
        uint64_t serial = 0;
        CHECK_OK(G->commands_submit(c, &serial));
        CHECK_OK(G->wait(serial, NHAL_GFX_WAIT_FOREVER));
        CHECK_OK(G->buffer_read(out, 0, sizeof g_out, g_out, read_done, NULL));
        ++g_pending;
        for (int i = 0; i < 5000 && g_pending > 0; ++i) {
            G->poll();
            usleep(1000);
        }
        CHECK(g_pending == 0);

        CHECK(at(8, 19) == 1);  /* links: (−0.47, −0.22) */
        CHECK(at(23, 19) == 2); /* rechts: (0.47, −0.22) */
        CHECK(at(0, 0) == 0 && at(15, 19) == 0 && at(31, 31) == 0);
        int left = 0, right = 0;
        for (int i = 0; i < S * S; ++i) {
            left += g_out[i] == 1;
            right += g_out[i] == 2;
        }
        CHECK(left > 0 && left == right);
        printf("raytracing_test: Treffer links %d, rechts %d Pixel\n", left, right);
    }

    char msg[256];
    while (G->take_error(msg, sizeof msg)) {
        fprintf(stderr, "raytracing_test: GPU meldet: %s\n", msg);
        ++g_failures;
    }
    R->accel_destroy(tlas);
    R->accel_destroy(blas);
    G->pipeline_destroy(pipe);
    G->shader_destroy(sh);
    G->buffer_destroy(out);
    G->buffer_destroy(vertices);
    nhal_shutdown(ctx);
    return check_report("raytracing_test");
}
