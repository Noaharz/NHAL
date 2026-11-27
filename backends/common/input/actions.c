/*
 * input.actions – plattformunabhängig. Feste Tabellen, kein Allocator.
 */
#include "nhal/features/input/actions.h"
#include "nhal/nhal_backend.h"

#include <string.h>

typedef struct action {
    char name[NHAL_ACTION_NAME_MAX];
    nhal_action_binding bindings[NHAL_ACTION_BINDINGS_MAX];
    uint32_t binding_count;
    float value;
    bool down, was_down;
} action;

static action g_actions[NHAL_ACTIONS_MAX];
static uint32_t g_count;

static action* get(int32_t id) { return (id >= 0 && (uint32_t)id < g_count) ? &g_actions[id] : NULL; }

static int32_t find(const char* name) {
    if (!name) return -1;
    for (uint32_t i = 0; i < g_count; ++i) {
        if (strcmp(g_actions[i].name, name) == 0) return (int32_t)i;
    }
    return -1;
}

static int32_t define(const char* name) {
    if (!name || !name[0] || strlen(name) >= NHAL_ACTION_NAME_MAX) return -1;
    const int32_t existing = find(name);
    if (existing >= 0) return existing;
    if (g_count == NHAL_ACTIONS_MAX) return -1;
    action* a = &g_actions[g_count];
    memset(a, 0, sizeof *a);
    memcpy(a->name, name, strlen(name) + 1);
    return (int32_t)g_count++;
}

static nhal_status bind(int32_t id, const nhal_action_binding* b) {
    action* a = get(id);
    if (!a) return nhal_status_error(NHAL_ERR_NOT_FOUND, "Unbekannte Aktion");
    if (!b) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindung ist NULL");
    switch (b->kind) {
    case NHAL_BIND_KEY:
        if (b->code <= NHAL_KEY_UNKNOWN || b->code >= NHAL_KEY_COUNT)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Taste");
        break;
    case NHAL_BIND_MOUSE_BUTTON:
        if (b->code >= NHAL_MOUSE_BUTTON_COUNT) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Maustaste");
        break;
    case NHAL_BIND_MOUSE_WHEEL_X:
    case NHAL_BIND_MOUSE_WHEEL_Y: break;
    case NHAL_BIND_GAMEPAD_BUTTON:
        if (!b->code || (b->code & (b->code - 1u)) || b->code > NHAL_GAMEPAD_GUIDE)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gamepad-Knopf muss genau ein NHAL_GAMEPAD_*-Bit sein");
        break;
    case NHAL_BIND_GAMEPAD_AXIS:
        if (b->code >= NHAL_GAMEPAD_AXIS_COUNT) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Achse");
        if (!(b->deadzone >= 0.0f && b->deadzone < 1.0f))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Totzone muss in 0..1 liegen");
        break;
    default: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Bindungsart");
    }
    if (a->binding_count == NHAL_ACTION_BINDINGS_MAX)
        return nhal_status_error(NHAL_ERR_BUSY, "Aktion hat schon NHAL_ACTION_BINDINGS_MAX Bindungen");
    a->bindings[a->binding_count] = *b;
    if (a->bindings[a->binding_count].scale == 0.0f) a->bindings[a->binding_count].scale = 1.0f;
    ++a->binding_count;
    return nhal_status_ok();
}

static void unbind_all(int32_t id) {
    action* a = get(id);
    if (a) a->binding_count = 0;
}

static void clear(void) {
    memset(g_actions, 0, sizeof g_actions);
    g_count = 0;
}

static float axis_value(float v, float deadzone) {
    const float mag = v < 0.0f ? -v : v;
    if (mag <= deadzone) return 0.0f;
    const float scaled = (mag - deadzone) / (1.0f - deadzone);
    return v < 0.0f ? -scaled : scaled;
}

static void update(const nhal_input_state_v1* S, const nhal_input_gamepad_v1* G, uint32_t slot) {
    nhal_gamepad_state pad;
    memset(&pad, 0, sizeof pad);
    const bool have_pad = G && G->state(slot, &pad) && pad.connected;
    float wx = 0.0f, wy = 0.0f;
    if (S) S->wheel(&wx, &wy);
    for (uint32_t i = 0; i < g_count; ++i) {
        action* a = &g_actions[i];
        float v = 0.0f;
        for (uint32_t k = 0; k < a->binding_count; ++k) {
            const nhal_action_binding* b = &a->bindings[k];
            switch (b->kind) {
            case NHAL_BIND_KEY:
                if (S && S->key_down((nhal_key)b->code)) v += b->scale;
                break;
            case NHAL_BIND_MOUSE_BUTTON:
                if (S && S->mouse_down((nhal_mouse_button)b->code)) v += b->scale;
                break;
            case NHAL_BIND_MOUSE_WHEEL_X: v += wx * b->scale; break;
            case NHAL_BIND_MOUSE_WHEEL_Y: v += wy * b->scale; break;
            case NHAL_BIND_GAMEPAD_BUTTON:
                if (have_pad && (pad.buttons & b->code)) v += b->scale;
                break;
            case NHAL_BIND_GAMEPAD_AXIS:
                if (have_pad) v += axis_value(pad.axes[b->code], b->deadzone) * b->scale;
                break;
            }
        }
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        a->was_down = a->down;
        a->value = v;
        a->down = (v < 0.0f ? -v : v) >= 0.5f;
    }
}

static float value(int32_t id) { const action* a = get(id); return a ? a->value : 0.0f; }
static bool down(int32_t id) { const action* a = get(id); return a && a->down; }
static bool pressed(int32_t id) { const action* a = get(id); return a && a->down && !a->was_down; }
static bool released(int32_t id) { const action* a = get(id); return a && !a->down && a->was_down; }
static uint32_t count(void) { return g_count; }
static const char* name(int32_t id) { const action* a = get(id); return a ? a->name : NULL; }

/* ── Standard-Belegung ─────────────────────────────────────────────────── */

static void key(int32_t a, nhal_key k, float scale) {
    const nhal_action_binding b = { NHAL_BIND_KEY, (uint32_t)k, scale, 0.0f };
    bind(a, &b);
}

static void button(int32_t a, uint32_t bit) {
    const nhal_action_binding b = { NHAL_BIND_GAMEPAD_BUTTON, bit, 1.0f, 0.0f };
    bind(a, &b);
}

static void axis(int32_t a, nhal_gamepad_axis ax, float deadzone) {
    const nhal_action_binding b = { NHAL_BIND_GAMEPAD_AXIS, (uint32_t)ax, 1.0f, deadzone };
    bind(a, &b);
}

static void mouse(int32_t a, nhal_mouse_button m) {
    const nhal_action_binding b = { NHAL_BIND_MOUSE_BUTTON, (uint32_t)m, 1.0f, 0.0f };
    bind(a, &b);
}

static nhal_status load_default(void) {
    clear();
    const float stick = 0.2f, trigger = 0.1f;
    int32_t a = define("move_x");
    key(a, NHAL_KEY_D, 1.0f);
    key(a, NHAL_KEY_RIGHT, 1.0f);
    key(a, NHAL_KEY_A, -1.0f);
    key(a, NHAL_KEY_LEFT, -1.0f);
    axis(a, NHAL_GAMEPAD_LEFT_X, stick);
    a = define("move_y");
    key(a, NHAL_KEY_S, 1.0f);
    key(a, NHAL_KEY_DOWN, 1.0f);
    key(a, NHAL_KEY_W, -1.0f);
    key(a, NHAL_KEY_UP, -1.0f);
    axis(a, NHAL_GAMEPAD_LEFT_Y, stick);
    axis(define("look_x"), NHAL_GAMEPAD_RIGHT_X, stick);
    axis(define("look_y"), NHAL_GAMEPAD_RIGHT_Y, stick);
    a = define("jump");
    key(a, NHAL_KEY_SPACE, 1.0f);
    button(a, NHAL_GAMEPAD_SOUTH);
    a = define("confirm");
    key(a, NHAL_KEY_ENTER, 1.0f);
    key(a, NHAL_KEY_KP_ENTER, 1.0f);
    button(a, NHAL_GAMEPAD_SOUTH);
    a = define("cancel");
    key(a, NHAL_KEY_ESCAPE, 1.0f);
    button(a, NHAL_GAMEPAD_EAST);
    a = define("pause");
    key(a, NHAL_KEY_ESCAPE, 1.0f);
    button(a, NHAL_GAMEPAD_START);
    a = define("fire");
    mouse(a, NHAL_MOUSE_LEFT);
    axis(a, NHAL_GAMEPAD_RIGHT_TRIGGER, trigger);
    a = define("aim");
    mouse(a, NHAL_MOUSE_RIGHT);
    axis(a, NHAL_GAMEPAD_LEFT_TRIGGER, trigger);
    a = define("sprint");
    key(a, NHAL_KEY_LEFT_SHIFT, 1.0f);
    button(a, NHAL_GAMEPAD_LEFT_STICK);
    a = define("interact");
    key(a, NHAL_KEY_E, 1.0f);
    button(a, NHAL_GAMEPAD_WEST);
    return g_count == 12u ? nhal_status_ok()
                          : nhal_status_error(NHAL_ERR_UNSUPPORTED, "Standard-Belegung unvollständig");
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator;
    clear();
    return nhal_status_ok();
}

static void feature_shutdown(void) { clear(); }

static const nhal_input_actions_v1 table = {
    sizeof(nhal_input_actions_v1),
    define, find, bind, unbind_all, clear, load_default,
    update,
    value, down, pressed, released,
    count, name,
};

const nhal_feature_impl nhal_impl_input_actions = { 1u, feature_init, feature_shutdown, &table };
