/*
 * input.state – plattformunabhängig. Reiner Zustandsautomat über
 * nhal_input_event; kein Speicher vom Allocator, kein Betriebssystem.
 */
#include "nhal/features/input/state.h"
#include "nhal/nhal_backend.h"

#include <string.h>

static struct {
    uint8_t key_down[NHAL_KEY_COUNT];
    uint8_t key_pressed[NHAL_KEY_COUNT];
    uint8_t key_released[NHAL_KEY_COUNT];
    uint32_t mods;
    uint8_t mouse_down[NHAL_MOUSE_BUTTON_COUNT];
    uint8_t mouse_pressed[NHAL_MOUSE_BUTTON_COUNT];
    uint8_t mouse_released[NHAL_MOUSE_BUTTON_COUNT];
    uint32_t mouse_clicks[NHAL_MOUSE_BUTTON_COUNT];
    float mx, my, dx, dy, wx, wy;
    bool inside;
    char text[NHAL_INPUT_TEXT_MAX];
    size_t text_len;
    nhal_touch_point touches[NHAL_INPUT_TOUCH_MAX];
    uint32_t touch_count;
    uint32_t window;
} S;

static void begin_frame(void) {
    memset(S.key_pressed, 0, sizeof S.key_pressed);
    memset(S.key_released, 0, sizeof S.key_released);
    memset(S.mouse_pressed, 0, sizeof S.mouse_pressed);
    memset(S.mouse_released, 0, sizeof S.mouse_released);
    memset(S.mouse_clicks, 0, sizeof S.mouse_clicks);
    S.dx = S.dy = S.wx = S.wy = 0.0f;
    S.text[0] = '\0';
    S.text_len = 0;
}

static void reset(void) {
    const uint32_t window = S.window;
    memset(&S, 0, sizeof S);
    S.window = window;
}

static void filter_window(uint32_t window) { S.window = window; }

static void release_all(void) {
    for (int k = 0; k < NHAL_KEY_COUNT; ++k) {
        if (S.key_down[k]) {
            S.key_down[k] = 0;
            S.key_released[k] = 1;
        }
    }
    for (int b = 0; b < NHAL_MOUSE_BUTTON_COUNT; ++b) {
        if (S.mouse_down[b]) {
            S.mouse_down[b] = 0;
            S.mouse_released[b] = 1;
        }
    }
    S.mods &= NHAL_MOD_CAPS_LOCK;
    S.touch_count = 0;
}

static void append_text(const char* t) {
    if (!t) return;
    size_t n = strlen(t);
    const size_t room = NHAL_INPUT_TEXT_MAX - 1u - S.text_len;
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)t[n] & 0xC0u) == 0x80u) --n; /* an Zeichengrenze kürzen */
    }
    memcpy(S.text + S.text_len, t, n);
    S.text_len += n;
    S.text[S.text_len] = '\0';
}

static void touch(const nhal_touch_event* t) {
    uint32_t i = 0;
    while (i < S.touch_count && S.touches[i].id != t->id) ++i;
    if (t->phase == NHAL_POINTER_UP || t->phase == NHAL_POINTER_CANCEL) {
        if (i == S.touch_count) return;
        memmove(&S.touches[i], &S.touches[i + 1], (S.touch_count - i - 1u) * sizeof S.touches[0]);
        --S.touch_count;
        return;
    }
    if (i == S.touch_count) {
        if (S.touch_count == NHAL_INPUT_TOUCH_MAX) return;
        ++S.touch_count;
    }
    S.touches[i] = (nhal_touch_point){ t->id, t->x, t->y, t->pressure };
}

static void feed(const nhal_input_event* e) {
    if (!e) return;
    if (S.window && e->window && e->window != S.window) return;
    switch (e->type) {
    case NHAL_EVENT_KEY_DOWN:
        if (e->key.key > NHAL_KEY_UNKNOWN && e->key.key < NHAL_KEY_COUNT) {
            if (!S.key_down[e->key.key] && !e->key.repeat) S.key_pressed[e->key.key] = 1;
            S.key_down[e->key.key] = 1;
        }
        S.mods = e->key.mods;
        break;
    case NHAL_EVENT_KEY_UP:
        if (e->key.key > NHAL_KEY_UNKNOWN && e->key.key < NHAL_KEY_COUNT && S.key_down[e->key.key]) {
            S.key_down[e->key.key] = 0;
            S.key_released[e->key.key] = 1;
        }
        S.mods = e->key.mods;
        break;
    case NHAL_EVENT_TEXT_INPUT: append_text(e->text.text); break;
    case NHAL_EVENT_MOUSE_DOWN:
    case NHAL_EVENT_MOUSE_UP: {
        const unsigned b = (unsigned)e->mouse.button;
        S.mx = e->mouse.x;
        S.my = e->mouse.y;
        if (b >= NHAL_MOUSE_BUTTON_COUNT) break;
        if (e->type == NHAL_EVENT_MOUSE_DOWN) {
            if (!S.mouse_down[b]) S.mouse_pressed[b] = 1;
            S.mouse_down[b] = 1;
            S.mouse_clicks[b] = e->mouse.clicks ? e->mouse.clicks : 1u;
        } else if (S.mouse_down[b]) {
            S.mouse_down[b] = 0;
            S.mouse_released[b] = 1;
        }
        break;
    }
    case NHAL_EVENT_MOUSE_MOVE:
        S.mx = e->mouse.x;
        S.my = e->mouse.y;
        S.dx += e->mouse.dx;
        S.dy += e->mouse.dy;
        break;
    case NHAL_EVENT_MOUSE_WHEEL:
        S.wx += e->wheel.dx;
        S.wy += e->wheel.dy;
        break;
    case NHAL_EVENT_MOUSE_ENTER: S.inside = true; break;
    case NHAL_EVENT_MOUSE_LEAVE: S.inside = false; break;
    case NHAL_EVENT_WINDOW_BLUR: release_all(); break;
    case NHAL_EVENT_TOUCH: touch(&e->touch); break;
    default: break;
    }
}

static bool valid_key(nhal_key k) { return k > NHAL_KEY_UNKNOWN && k < NHAL_KEY_COUNT; }
static bool valid_button(nhal_mouse_button b) { return (unsigned)b < NHAL_MOUSE_BUTTON_COUNT; }

static bool key_down(nhal_key k) { return valid_key(k) && S.key_down[k]; }
static bool key_pressed(nhal_key k) { return valid_key(k) && S.key_pressed[k]; }
static bool key_released(nhal_key k) { return valid_key(k) && S.key_released[k]; }
static uint32_t mods(void) { return S.mods; }
static bool mouse_down(nhal_mouse_button b) { return valid_button(b) && S.mouse_down[b]; }
static bool mouse_pressed(nhal_mouse_button b) { return valid_button(b) && S.mouse_pressed[b]; }
static bool mouse_released(nhal_mouse_button b) { return valid_button(b) && S.mouse_released[b]; }
static uint32_t mouse_clicks(nhal_mouse_button b) { return valid_button(b) ? S.mouse_clicks[b] : 0u; }

static void mouse_position(float* x, float* y) {
    if (x) *x = S.mx;
    if (y) *y = S.my;
}

static void mouse_delta(float* dx, float* dy) {
    if (dx) *dx = S.dx;
    if (dy) *dy = S.dy;
}

static void wheel(float* dx, float* dy) {
    if (dx) *dx = S.wx;
    if (dy) *dy = S.wy;
}

static bool mouse_inside(void) { return S.inside; }
static const char* text(void) { return S.text; }
static uint32_t touch_count(void) { return S.touch_count; }

static bool touch_at(uint32_t i, nhal_touch_point* out) {
    if (i >= S.touch_count || !out) return false;
    *out = S.touches[i];
    return true;
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator;
    memset(&S, 0, sizeof S);
    return nhal_status_ok();
}

static void feature_shutdown(void) { memset(&S, 0, sizeof S); }

static const nhal_input_state_v1 table = {
    sizeof(nhal_input_state_v1),
    begin_frame, feed, reset, filter_window,
    key_down, key_pressed, key_released, mods,
    mouse_down, mouse_pressed, mouse_released, mouse_clicks, mouse_position, mouse_delta, wheel, mouse_inside,
    text,
    touch_count, touch_at,
};

const nhal_feature_impl nhal_impl_input_state = { 1u, feature_init, feature_shutdown, &table };
