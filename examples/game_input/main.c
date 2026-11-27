/*
 * Beispiel: Fenster, Grafik und Eingabe zusammen.
 *
 * Ein Quadrat folgt den Aktionen move_x/move_y (WASD, Pfeiltasten, linker
 * Stick); jump (Leertaste, SOUTH) wechselt die Farbe; cancel (Escape, EAST)
 * oder das Schließen des Fensters beendet. Läuft unverändert auf macOS
 * (Metal) und im Browser (WebGPU).
 *
 * Mit NHAL_DEMO_SELFTEST (so baut es tests/CMakeLists.txt) drückt das
 * Programm selbst D über window.basic.inject_key, prüft, dass das Quadrat
 * nach rechts gewandert ist und die GPU keinen Fehler gemeldet hat, und endet.
 */
#include "nhal/features/gfx/device.h"
#include "nhal/features/input/actions.h"
#include "nhal/features/input/gamepad.h"
#include "nhal/features/input/state.h"
#include "nhal/features/window/basic.h"

#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <unistd.h>
#endif

static const char* k_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct U { float4 rect; float4 color; };\n"
    "constant float2 kCorner[6] = { float2(-1, -1), float2(1, -1), float2(1, 1),\n"
    "                               float2(-1, -1), float2(1, 1), float2(-1, 1) };\n"
    "vertex float4 vs(uint i [[vertex_id]], constant U& u [[buffer(0)]]) {\n"
    "    float2 p = kCorner[i] * u.rect.z;\n"
    "    return float4(u.rect.x + p.x / u.rect.w, u.rect.y + p.y, 0.0, 1.0);\n"
    "}\n"
    "fragment float4 fs(constant U& u [[buffer(0)]]) { return u.color; }\n";

static const char* k_wgsl =
    "struct U { rect: vec4<f32>, color: vec4<f32> };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
    "    var c = array<vec2<f32>, 6>(vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, -1.0), vec2<f32>(1.0, 1.0),\n"
    "                                vec2<f32>(-1.0, -1.0), vec2<f32>(1.0, 1.0), vec2<f32>(-1.0, 1.0));\n"
    "    let p = c[i] * u.rect.z;\n"
    "    return vec4<f32>(u.rect.x + p.x / u.rect.w, u.rect.y + p.y, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs() -> @location(0) vec4<f32> { return u.color; }\n";

static const float k_colors[3][4] = { { 0.95f, 0.55f, 0.15f, 1 }, { 0.2f, 0.75f, 0.45f, 1 }, { 0.35f, 0.5f, 0.95f, 1 } };

static struct {
    nhal_context* ctx;
    const nhal_window_basic_v1* W;
    const nhal_gfx_device_v2* G;
    const nhal_input_state_v1* S;
    const nhal_input_actions_v1* A;
    const nhal_input_gamepad_v1* P; /* optional */
    uint32_t window;
    nhal_gfx_surface* surface;
    nhal_gfx_shader* shader;
    nhal_gfx_pipeline* pipeline;
    nhal_gfx_buffer* uniform;
    int32_t move_x, move_y, jump, cancel;
    float x, y;
    int color;
    uint32_t width, height;
    bool resized;
    bool running;
    int frame;
    int rc;
} D;

static void stop(int rc) {
    D.running = false;
    D.rc = rc;
}

static nhal_status setup_gfx(void) {
    nhal_window_native n;
    nhal_status s = D.W->native(D.window, &n);
    if (s.code) return s;
    const nhal_gfx_surface_desc sd = { (nhal_gfx_surface_kind)n.kind, n.handle }; /* gleiche Werte */
    if ((s = D.G->surface_create(&sd, &D.surface)).code) return s;
    const nhal_gfx_shader_desc shd = { k_msl, k_wgsl, NULL, NULL, 0 };
    if ((s = D.G->shader_create(&shd, &D.shader)).code) return s;
    const nhal_gfx_binding_desc b = { 0, NHAL_GFX_BIND_UNIFORM_BUFFER, NHAL_GFX_STAGE_VERTEX | NHAL_GFX_STAGE_FRAGMENT,
                                      NHAL_GFX_FORMAT_NONE };
    nhal_gfx_render_pipeline_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.shader = D.shader;
    pd.vertex_entry = "vs";
    pd.fragment_entry = "fs";
    pd.binding_count = 1;
    pd.bindings = &b;
    pd.color_count = 1;
    pd.colors[0].format = D.G->surface_format(D.surface);
    if ((s = D.G->render_pipeline_create(&pd, &D.pipeline)).code) return s;
    const nhal_gfx_buffer_desc ud = { 256, NHAL_GFX_BUFFER_UNIFORM | NHAL_GFX_BUFFER_COPY_DST };
    if ((s = D.G->buffer_create(&ud, NULL, &D.uniform)).code) return s;
    D.resized = true;
    return s; /* letzter Aufruf war erfolgreich */
}

static void handle_events(void) {
    D.W->poll_events();
    D.S->begin_frame();
    nhal_input_event e;
    while (D.W->next_event(&e)) {
        D.S->feed(&e);
        if (e.type == NHAL_EVENT_QUIT || e.type == NHAL_EVENT_WINDOW_CLOSE) stop(0);
        if (e.type == NHAL_EVENT_WINDOW_RESIZE) D.resized = true;
    }
    if (D.P) D.P->poll();
    D.A->update(D.S, D.P, 0);
}

static void render(void) {
    if (D.resized) {
        nhal_window_size sz;
        if (D.W->size(D.window, &sz).code == NHAL_OK && sz.pixel_width && sz.pixel_height) {
            D.width = sz.pixel_width;
            D.height = sz.pixel_height;
            D.G->surface_configure(D.surface, D.width, D.height, true);
        }
        D.resized = false;
    }
    const float u[8] = { D.x, D.y, 0.12f, D.height ? (float)D.width / (float)D.height : 1.0f,
                         k_colors[D.color][0], k_colors[D.color][1], k_colors[D.color][2], k_colors[D.color][3] };
    D.G->buffer_write(D.uniform, 0, u, sizeof u);
    nhal_gfx_texture* target = NULL;
    if (D.G->surface_acquire(D.surface, &target).code != NHAL_OK) return; /* z. B. BUSY: nächster Frame */
    nhal_gfx_commands* c = NULL;
    if (D.G->commands_begin(&c).code != NHAL_OK) return;
    const nhal_gfx_render_pass_desc rp = { .color_count = 1, .colors = { { .texture = target, .clear_color = { 0.08f, 0.09f, 0.11f, 1.0f } } } };
    nhal_gfx_pass* p = D.G->render_pass_begin(c, &rp);
    D.G->pass_set_pipeline(p, D.pipeline);
    D.G->pass_set_buffer(p, 0, D.uniform, 0, 32);
    D.G->pass_draw(p, 6, 1, 0, 0);
    D.G->pass_end(p);
    const nhal_status s = D.G->commands_submit(c, NULL);
    if (s.code) {
        fprintf(stderr, "game_input: submit: %s\n", s.reason);
        stop(1);
    }
    D.G->surface_present(D.surface);
}

static void selftest(void) {
#ifdef NHAL_DEMO_SELFTEST
    if (D.frame == 5) D.W->inject_key(D.window, NHAL_KEY_D, true, 0);
    if (D.frame == 35) D.W->inject_key(D.window, NHAL_KEY_D, false, 0);
    if (D.frame == 36) D.W->inject_key(D.window, NHAL_KEY_SPACE, true, 0);
    if (D.frame == 40) {
        char msg[256];
        int errors = 0;
        while (D.G->take_error(msg, sizeof msg)) {
            fprintf(stderr, "game_input: GPU meldet: %s\n", msg);
            ++errors;
        }
        const bool moved = D.x > 0.3f && D.y == 0.0f;
        if (!moved) fprintf(stderr, "game_input: Quadrat steht bei %.3f/%.3f statt rechts\n", D.x, D.y);
        if (D.color != 1) fprintf(stderr, "game_input: Farbe %d statt 1 nach jump\n", D.color);
        const bool ok = moved && D.color == 1 && errors == 0;
        printf(ok ? "game_input: alles in Ordnung (x = %.2f)\n" : "game_input: 1 Prüfung(en) fehlgeschlagen\n", D.x);
        stop(ok ? 0 : 1);
    }
#endif
}

static void frame(void) {
    if (!D.running) return;
    ++D.frame;
    handle_events();
    const nhal_status r = D.G->ready();
    if (r.code == NHAL_ERR_BUSY) return; /* Web: Gerät kommt noch */
    if (r.code != NHAL_OK) {
        printf("game_input: keine GPU – %s\n", r.reason);
#ifdef NHAL_DEMO_SELFTEST
        printf("game_input: alles in Ordnung (ohne GPU übersprungen)\n");
#endif
        stop(0);
        return;
    }
    if (!D.surface) {
        const nhal_status s = setup_gfx();
        if (s.code) {
            fprintf(stderr, "game_input: Grafik: %s\n", s.reason);
            stop(1);
            return;
        }
    }
    const float speed = 1.5f / 60.0f;
    D.x += D.A->value(D.move_x) * speed;
    D.y -= D.A->value(D.move_y) * speed; /* move_y oben = −1, NDC oben = +1 */
    if (D.x > 1.0f) D.x = 1.0f;
    if (D.x < -1.0f) D.x = -1.0f;
    if (D.y > 1.0f) D.y = 1.0f;
    if (D.y < -1.0f) D.y = -1.0f;
    if (D.A->pressed(D.jump)) D.color = (D.color + 1) % 3;
    if (D.A->pressed(D.cancel)) stop(0);
    selftest();
    render();
}

static void shutdown_all(void) {
    if (D.G) {
        D.G->buffer_destroy(D.uniform);
        D.G->pipeline_destroy(D.pipeline);
        D.G->shader_destroy(D.shader);
        D.G->surface_destroy(D.surface);
    }
    if (D.W && D.window) D.W->destroy(D.window);
    nhal_shutdown(D.ctx);
}

#ifdef __EMSCRIPTEN__
static void web_frame(void) {
    frame();
    if (!D.running) {
        emscripten_cancel_main_loop();
        shutdown_all();
    }
}
#endif

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_WINDOW_BASIC, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_GFX_DEVICE, 2u, NHAL_REQUIRED },
        { NHAL_FEATURE_INPUT_STATE, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_INPUT_ACTIONS, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_INPUT_GAMEPAD, 1u, NHAL_OPTIONAL },
    };
    const nhal_manifest m = { req, sizeof req / sizeof req[0] };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_status s = nhal_init(&info, &D.ctx);
    if (s.code) {
        fprintf(stderr, "game_input: %s\n", s.reason);
        return s.code == NHAL_ERR_UNAVAILABLE ? 0 : 1;
    }
    D.W = nhal_get_window_basic_v1(D.ctx);
    D.G = nhal_get_gfx_device_v2(D.ctx);
    D.S = nhal_get_input_state_v1(D.ctx);
    D.A = nhal_get_input_actions_v1(D.ctx);
    D.P = nhal_get_input_gamepad_v1(D.ctx);

    uint32_t flags = NHAL_WINDOW_RESIZABLE | NHAL_WINDOW_HIGH_DPI;
#ifdef NHAL_DEMO_SELFTEST
    flags |= NHAL_WINDOW_HIDDEN;
#endif
    const nhal_window_desc wd = { "NHAL – Fenster, Grafik, Eingabe", 640, 400, flags, NULL };
    s = D.W->create(&wd, &D.window);
    if (s.code) {
        printf("game_input: kein Fenster – %s\n", s.reason);
#ifdef NHAL_DEMO_SELFTEST
        printf("game_input: alles in Ordnung (ohne Fenster übersprungen)\n");
#endif
        shutdown_all();
        return s.code == NHAL_ERR_UNAVAILABLE ? 0 : 1;
    }
    D.A->load_default();
    D.move_x = D.A->find("move_x");
    D.move_y = D.A->find("move_y");
    D.jump = D.A->find("jump");
    D.cancel = D.A->find("cancel");
    D.running = true;
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(web_frame, 0, false);
    return 0;
#else
    while (D.running) {
        frame();
        if (!D.surface) usleep(1000);
    }
    shutdown_all();
    return D.rc;
#endif
}
