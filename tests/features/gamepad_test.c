/* input.gamepad: Plätze, Abfragen, Grenzen (ohne angeschlossenes Pad prüfbar). */
#include "nhal/features/input/gamepad.h"
#include "check.h"

int main(void) {
    const nhal_request req = { NHAL_FEATURE_INPUT_GAMEPAD, 1u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    const nhal_input_gamepad_v1* G = nhal_get_input_gamepad_v1(ctx);
    if (!G) return check_report("gamepad_test");

    for (int frame = 0; frame < 3; ++frame) G->poll();
    uint32_t seen = 0;
    for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) {
        nhal_gamepad_state s;
        CHECK(G->state(i, &s));
        if (!s.connected) {
            CHECK(s.buttons == 0);
            continue;
        }
        ++seen;
        printf("gamepad_test: Platz %u: %s (%s)\n", i, s.name, s.standard_mapping ? "Standard" : "unbekannte Belegung");
        for (int a = 0; a < NHAL_GAMEPAD_AXIS_COUNT; ++a) CHECK(s.axes[a] >= -1.0f && s.axes[a] <= 1.0f);
        CHECK(s.axes[NHAL_GAMEPAD_LEFT_TRIGGER] >= 0.0f);
    }
    CHECK(seen == G->connected_count());
    nhal_gamepad_state s;
    CHECK(!G->state(NHAL_GAMEPAD_MAX, &s));
    CHECK(!G->state(0, NULL));
    printf("gamepad_test: %u Gamepad(s) verbunden\n", seen);
    nhal_shutdown(ctx);
    return check_report("gamepad_test");
}
