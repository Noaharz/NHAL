/*
 * input.state und input.actions: gefüttert mit selbst gebauten Ereignissen
 * und einem nachgebauten Gamepad (eigene nhal_input_gamepad_v1-Tabelle).
 * Braucht weder Fenster noch Hardware und läuft deshalb überall.
 */
#include "nhal/features/input/actions.h"
#include "nhal/features/input/state.h"
#include "check.h"

#include <string.h>

static const nhal_input_state_v1* S;
static const nhal_input_actions_v1* A;

static nhal_input_event ev(nhal_event_type t) {
    nhal_input_event e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.window = 1;
    return e;
}

static void key(nhal_key k, bool down, uint32_t mods, bool repeat) {
    nhal_input_event e = ev(down ? NHAL_EVENT_KEY_DOWN : NHAL_EVENT_KEY_UP);
    e.key.key = k;
    e.key.mods = mods;
    e.key.repeat = repeat;
    S->feed(&e);
}

static void mouse(nhal_mouse_button b, bool down, float x, float y, uint32_t clicks) {
    nhal_input_event e = ev(down ? NHAL_EVENT_MOUSE_DOWN : NHAL_EVENT_MOUSE_UP);
    e.mouse.button = b;
    e.mouse.x = x;
    e.mouse.y = y;
    e.mouse.clicks = clicks;
    S->feed(&e);
}

static void text(const char* t) {
    nhal_input_event e = ev(NHAL_EVENT_TEXT_INPUT);
    e.text.text = t;
    S->feed(&e);
}

/* ── Nachgebautes Gamepad ──────────────────────────────────────────────── */

static nhal_gamepad_state g_pad;

static void pad_poll(void) {}
static bool pad_state(uint32_t slot, nhal_gamepad_state* out) {
    if (slot >= NHAL_GAMEPAD_MAX || !out) return false;
    if (slot == 0) *out = g_pad;
    else memset(out, 0, sizeof *out);
    return true;
}
static uint32_t pad_count(void) { return g_pad.connected ? 1u : 0u; }
static const nhal_input_gamepad_v1 k_fake_pad = { sizeof(nhal_input_gamepad_v1), pad_poll, pad_state, pad_count };

/* ── input.state ───────────────────────────────────────────────────────── */

static void test_keys(void) {
    S->reset();
    S->begin_frame();
    key(NHAL_KEY_W, true, 0, false);
    CHECK(S->key_down(NHAL_KEY_W) && S->key_pressed(NHAL_KEY_W) && !S->key_released(NHAL_KEY_W));
    S->begin_frame();
    key(NHAL_KEY_W, true, 0, true); /* Wiederholung: keine neue Flanke */
    CHECK(S->key_down(NHAL_KEY_W) && !S->key_pressed(NHAL_KEY_W));
    S->begin_frame();
    key(NHAL_KEY_W, false, 0, false);
    CHECK(!S->key_down(NHAL_KEY_W) && S->key_released(NHAL_KEY_W));
    S->begin_frame();
    CHECK(!S->key_released(NHAL_KEY_W));

    /* Drücken und Loslassen im selben Frame */
    key(NHAL_KEY_SPACE, true, 0, false);
    key(NHAL_KEY_SPACE, false, 0, false);
    CHECK(S->key_pressed(NHAL_KEY_SPACE) && S->key_released(NHAL_KEY_SPACE) && !S->key_down(NHAL_KEY_SPACE));

    key(NHAL_KEY_LEFT_SHIFT, true, NHAL_MOD_SHIFT, false);
    CHECK(S->mods() == NHAL_MOD_SHIFT);
    CHECK(!S->key_down(NHAL_KEY_UNKNOWN) && !S->key_down(NHAL_KEY_COUNT)); /* Grenzen */

    /* Fokusverlust lässt alles los, mit Flanke */
    S->begin_frame();
    nhal_input_event blur = ev(NHAL_EVENT_WINDOW_BLUR);
    S->feed(&blur);
    CHECK(!S->key_down(NHAL_KEY_LEFT_SHIFT) && S->key_released(NHAL_KEY_LEFT_SHIFT) && S->mods() == 0);
}

static void test_mouse_text_touch(void) {
    S->reset();
    S->begin_frame();
    nhal_input_event mv = ev(NHAL_EVENT_MOUSE_MOVE);
    mv.mouse.x = 10;
    mv.mouse.y = 20;
    mv.mouse.dx = 3;
    mv.mouse.dy = -1;
    S->feed(&mv);
    mv.mouse.x = 12;
    mv.mouse.dx = 2;
    S->feed(&mv);
    float x, y, dx, dy;
    S->mouse_position(&x, &y);
    S->mouse_delta(&dx, &dy);
    CHECK(x == 12 && y == 20 && dx == 5 && dy == -2);
    mouse(NHAL_MOUSE_LEFT, true, 12, 20, 2);
    CHECK(S->mouse_down(NHAL_MOUSE_LEFT) && S->mouse_pressed(NHAL_MOUSE_LEFT) && S->mouse_clicks(NHAL_MOUSE_LEFT) == 2);
    nhal_input_event wh = ev(NHAL_EVENT_MOUSE_WHEEL);
    wh.wheel.dy = 1.5f;
    S->feed(&wh);
    S->feed(&wh);
    S->wheel(&dx, &dy);
    CHECK(dy == 3.0f);
    S->begin_frame();
    S->wheel(&dx, &dy);
    S->mouse_delta(&x, &y);
    CHECK(dy == 0.0f && x == 0.0f && S->mouse_down(NHAL_MOUSE_LEFT) && !S->mouse_pressed(NHAL_MOUSE_LEFT));
    mouse(NHAL_MOUSE_LEFT, false, 12, 20, 0);
    CHECK(S->mouse_released(NHAL_MOUSE_LEFT));

    text("grü");
    text("ß");
    CHECK(strcmp(S->text(), "grüß") == 0);
    S->begin_frame();
    CHECK(S->text()[0] == '\0');
    /* Überlänge: an einer Zeichengrenze gekürzt ("ä" = 2 Bytes) */
    char big[NHAL_INPUT_TEXT_MAX + 8];
    size_t n = 0;
    while (n + 2 < sizeof big - 1) {
        big[n++] = (char)0xC3;
        big[n++] = (char)0xA4;
    }
    big[n] = '\0';
    text(big);
    const size_t len = strlen(S->text());
    CHECK(len < NHAL_INPUT_TEXT_MAX && len % 2 == 0);

    nhal_input_event t = ev(NHAL_EVENT_TOUCH);
    t.touch.id = 7;
    t.touch.phase = NHAL_POINTER_DOWN;
    t.touch.x = 1;
    S->feed(&t);
    t.touch.id = 9;
    S->feed(&t);
    t.touch.id = 7;
    t.touch.phase = NHAL_POINTER_MOVE;
    t.touch.x = 5;
    S->feed(&t);
    nhal_touch_point p;
    CHECK(S->touch_count() == 2 && S->touch_at(0, &p) && p.id == 7 && p.x == 5);
    t.touch.phase = NHAL_POINTER_UP;
    S->feed(&t);
    CHECK(S->touch_count() == 1 && S->touch_at(0, &p) && p.id == 9 && !S->touch_at(1, &p));

    /* Fensterfilter */
    S->reset();
    S->filter_window(2);
    S->begin_frame();
    key(NHAL_KEY_Q, true, 0, false); /* Fenster 1 */
    CHECK(!S->key_down(NHAL_KEY_Q));
    S->filter_window(0);
    key(NHAL_KEY_Q, true, 0, false);
    CHECK(S->key_down(NHAL_KEY_Q));
}

/* ── input.actions ─────────────────────────────────────────────────────── */

static void test_actions(void) {
    S->reset();
    CHECK_OK(A->load_default());
    CHECK(A->count() == 12);
    const int32_t mx = A->find("move_x"), my = A->find("move_y"), jump = A->find("jump"), fire = A->find("fire");
    CHECK(mx >= 0 && my >= 0 && jump >= 0 && fire >= 0);
    CHECK(A->find("gibt_es_nicht") == -1 && A->value(-1) == 0.0f && !A->down(99));
    CHECK(A->define("jump") == jump); /* gleicher Name, gleiche ID */
    CHECK(strcmp(A->name(jump), "jump") == 0);

    memset(&g_pad, 0, sizeof g_pad);
    S->begin_frame();
    key(NHAL_KEY_D, true, 0, false);
    key(NHAL_KEY_W, true, 0, false);
    key(NHAL_KEY_SPACE, true, 0, false);
    A->update(S, &k_fake_pad, 0);
    CHECK(A->value(mx) == 1.0f && A->value(my) == -1.0f);
    CHECK(A->down(jump) && A->pressed(jump));
    A->update(S, &k_fake_pad, 0);
    CHECK(A->down(jump) && !A->pressed(jump));
    key(NHAL_KEY_SPACE, false, 0, false);
    key(NHAL_KEY_A, true, 0, false); /* D und A heben sich auf */
    A->update(S, NULL, 0);
    CHECK(A->released(jump) && A->value(mx) == 0.0f);
    key(NHAL_KEY_D, false, 0, false);
    key(NHAL_KEY_A, false, 0, false);
    key(NHAL_KEY_W, false, 0, false);

    /* Gamepad: Stick mit Totzone, Knopf, Trigger */
    g_pad.connected = true;
    g_pad.axes[NHAL_GAMEPAD_LEFT_X] = 0.1f; /* in der Totzone (0.2) */
    g_pad.axes[NHAL_GAMEPAD_LEFT_Y] = -0.6f;
    g_pad.axes[NHAL_GAMEPAD_RIGHT_TRIGGER] = 1.0f;
    g_pad.buttons = NHAL_GAMEPAD_SOUTH;
    A->update(S, &k_fake_pad, 0);
    CHECK(A->value(mx) == 0.0f);
    CHECK(A->value(my) < -0.49f && A->value(my) > -0.51f); /* (0.6-0.2)/0.8 = 0.5 */
    CHECK(A->down(fire) && A->pressed(jump));
    A->update(S, &k_fake_pad, 1); /* anderer Platz: kein Pad */
    CHECK(!A->down(jump) && A->value(my) == 0.0f);

    /* Eigene Aktion, Mausrad, Grenzen */
    A->clear();
    CHECK(A->count() == 0);
    const int32_t zoom = A->define("zoom");
    const nhal_action_binding wheel = { NHAL_BIND_MOUSE_WHEEL_Y, 0, 0.25f, 0.0f };
    CHECK_OK(A->bind(zoom, &wheel));
    const nhal_action_binding bad_key = { NHAL_BIND_KEY, NHAL_KEY_COUNT, 1.0f, 0.0f };
    CHECK(A->bind(zoom, &bad_key).code == NHAL_ERR_INVALID_ARGUMENT);
    const nhal_action_binding two_bits = { NHAL_BIND_GAMEPAD_BUTTON, NHAL_GAMEPAD_SOUTH | NHAL_GAMEPAD_EAST, 1.0f, 0.0f };
    CHECK(A->bind(zoom, &two_bits).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(A->bind(-1, &wheel).code == NHAL_ERR_NOT_FOUND);
    S->begin_frame();
    nhal_input_event wh = ev(NHAL_EVENT_MOUSE_WHEEL);
    wh.wheel.dy = 2.0f;
    S->feed(&wh);
    A->update(S, NULL, 0);
    CHECK(A->value(zoom) == 0.5f && A->down(zoom));
    CHECK(A->define("") == -1 && A->define("ein_viel_zu_langer_aktionsname_ueber_32") == -1);
    for (int i = 0; i < (int)NHAL_ACTIONS_MAX + 2; ++i) {
        char name[16];
        snprintf(name, sizeof name, "a%d", i);
        A->define(name);
    }
    CHECK(A->count() == NHAL_ACTIONS_MAX);
}

int main(void) {
    const nhal_request req[] = {
        { NHAL_FEATURE_INPUT_STATE, 1u, NHAL_REQUIRED },
        { NHAL_FEATURE_INPUT_ACTIONS, 1u, NHAL_REQUIRED },
    };
    const nhal_manifest m = { req, 2 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    S = nhal_get_input_state_v1(ctx);
    A = nhal_get_input_actions_v1(ctx);
    if (!S || !A) return check_report("input_test");
    test_keys();
    test_mouse_text_touch();
    test_actions();
    nhal_shutdown(ctx);
    return check_report("input_test");
}
