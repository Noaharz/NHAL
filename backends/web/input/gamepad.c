/*
 * input.gamepad – Web (Gamepad API über emscripten/html5.h).
 *
 * Platz = Index des Browsers (bleibt gleich, solange das Gerät verbunden ist).
 * Belegung "standard" der Gamepad API: Tasten 0–16, Achsen 0–3.
 */
#include "nhal/features/input/gamepad.h"
#include "nhal/nhal_backend.h"

#include <emscripten/html5.h>
#include <string.h>

static nhal_gamepad_state g_state[NHAL_GAMEPAD_MAX];

static const uint32_t k_standard_buttons[17] = {
    NHAL_GAMEPAD_SOUTH,       NHAL_GAMEPAD_EAST,      NHAL_GAMEPAD_WEST,       NHAL_GAMEPAD_NORTH,
    NHAL_GAMEPAD_LEFT_SHOULDER, NHAL_GAMEPAD_RIGHT_SHOULDER, 0u /* LT: Achse */, 0u /* RT: Achse */,
    NHAL_GAMEPAD_BACK,        NHAL_GAMEPAD_START,     NHAL_GAMEPAD_LEFT_STICK, NHAL_GAMEPAD_RIGHT_STICK,
    NHAL_GAMEPAD_DPAD_UP,     NHAL_GAMEPAD_DPAD_DOWN, NHAL_GAMEPAD_DPAD_LEFT,  NHAL_GAMEPAD_DPAD_RIGHT,
    NHAL_GAMEPAD_GUIDE,
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void gp_poll(void) {
    memset(g_state, 0, sizeof g_state);
    if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) return; /* keine Gamepad API */
    int n = emscripten_get_num_gamepads();
    if (n > (int)NHAL_GAMEPAD_MAX) n = (int)NHAL_GAMEPAD_MAX;
    for (int i = 0; i < n; ++i) {
        EmscriptenGamepadEvent e;
        if (emscripten_get_gamepad_status(i, &e) != EMSCRIPTEN_RESULT_SUCCESS || !e.connected) continue;
        nhal_gamepad_state* s = &g_state[i];
        s->connected = true;
        s->standard_mapping = strcmp(e.mapping, "standard") == 0;
        strncpy(s->name, e.id, sizeof s->name - 1);
        uint32_t b = 0;
        for (int k = 0; k < e.numButtons && k < 17; ++k) {
            if (e.digitalButton[k]) b |= k_standard_buttons[k];
        }
        s->buttons = b;
        if (e.numAxes > 0) s->axes[NHAL_GAMEPAD_LEFT_X] = clampf((float)e.axis[0], -1.f, 1.f);
        if (e.numAxes > 1) s->axes[NHAL_GAMEPAD_LEFT_Y] = clampf((float)e.axis[1], -1.f, 1.f); /* oben = -1 */
        if (e.numAxes > 2) s->axes[NHAL_GAMEPAD_RIGHT_X] = clampf((float)e.axis[2], -1.f, 1.f);
        if (e.numAxes > 3) s->axes[NHAL_GAMEPAD_RIGHT_Y] = clampf((float)e.axis[3], -1.f, 1.f);
        if (e.numButtons > 6) s->axes[NHAL_GAMEPAD_LEFT_TRIGGER] = clampf((float)e.analogButton[6], 0.f, 1.f);
        if (e.numButtons > 7) s->axes[NHAL_GAMEPAD_RIGHT_TRIGGER] = clampf((float)e.analogButton[7], 0.f, 1.f);
    }
}

static bool gp_state(uint32_t slot, nhal_gamepad_state* out) {
    if (slot >= NHAL_GAMEPAD_MAX || !out) return false;
    *out = g_state[slot];
    return true;
}

static uint32_t gp_connected_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) n += g_state[i].connected ? 1u : 0u;
    return n;
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator;
    memset(g_state, 0, sizeof g_state);
    return nhal_status_ok();
}

static void feature_shutdown(void) { memset(g_state, 0, sizeof g_state); }

static const nhal_input_gamepad_v1 table = {
    sizeof(nhal_input_gamepad_v1), gp_poll, gp_state, gp_connected_count,
};

const nhal_feature_impl nhal_impl_input_gamepad = { 1u, feature_init, feature_shutdown, &table };
