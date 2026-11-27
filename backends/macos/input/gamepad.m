/*
 * input.gamepad – macOS (GameController-Framework, Objective-C mit ARC).
 */
#include "nhal/features/input/gamepad.h"
#include "nhal/nhal_backend.h"

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include <string.h>

static __strong GCController* g_slots[NHAL_GAMEPAD_MAX];
static nhal_gamepad_state g_state[NHAL_GAMEPAD_MAX];

static void fill(uint32_t slot, GCController* c) {
    nhal_gamepad_state* s = &g_state[slot];
    memset(s, 0, sizeof *s);
    s->connected = true;
    const char* name = c.vendorName.UTF8String;
    if (name) strncpy(s->name, name, sizeof s->name - 1);
    GCExtendedGamepad* g = c.extendedGamepad;
    if (!g) return; /* z. B. Micro Gamepad: verbunden, Belegung unbekannt */
    s->standard_mapping = true;
    uint32_t b = 0;
    if (g.buttonA.pressed) b |= NHAL_GAMEPAD_SOUTH;
    if (g.buttonB.pressed) b |= NHAL_GAMEPAD_EAST;
    if (g.buttonX.pressed) b |= NHAL_GAMEPAD_WEST;
    if (g.buttonY.pressed) b |= NHAL_GAMEPAD_NORTH;
    if (g.leftShoulder.pressed) b |= NHAL_GAMEPAD_LEFT_SHOULDER;
    if (g.rightShoulder.pressed) b |= NHAL_GAMEPAD_RIGHT_SHOULDER;
    if (g.buttonOptions.pressed) b |= NHAL_GAMEPAD_BACK;
    if (g.buttonMenu.pressed) b |= NHAL_GAMEPAD_START;
    if (g.leftThumbstickButton.pressed) b |= NHAL_GAMEPAD_LEFT_STICK;
    if (g.rightThumbstickButton.pressed) b |= NHAL_GAMEPAD_RIGHT_STICK;
    if (g.dpad.up.pressed) b |= NHAL_GAMEPAD_DPAD_UP;
    if (g.dpad.down.pressed) b |= NHAL_GAMEPAD_DPAD_DOWN;
    if (g.dpad.left.pressed) b |= NHAL_GAMEPAD_DPAD_LEFT;
    if (g.dpad.right.pressed) b |= NHAL_GAMEPAD_DPAD_RIGHT;
    if (g.buttonHome.pressed) b |= NHAL_GAMEPAD_GUIDE;
    s->buttons = b;
    s->axes[NHAL_GAMEPAD_LEFT_X] = g.leftThumbstick.xAxis.value;
    s->axes[NHAL_GAMEPAD_LEFT_Y] = -g.leftThumbstick.yAxis.value; /* GameController: oben = +1 */
    s->axes[NHAL_GAMEPAD_RIGHT_X] = g.rightThumbstick.xAxis.value;
    s->axes[NHAL_GAMEPAD_RIGHT_Y] = -g.rightThumbstick.yAxis.value;
    s->axes[NHAL_GAMEPAD_LEFT_TRIGGER] = g.leftTrigger.value;
    s->axes[NHAL_GAMEPAD_RIGHT_TRIGGER] = g.rightTrigger.value;
}

static void gp_poll(void) {
    @autoreleasepool {
        if (pthread_main_np()) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        NSArray<GCController*>* now = GCController.controllers;
        for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) { /* getrennte freigeben */
            if (g_slots[i] && ![now containsObject:g_slots[i]]) {
                g_slots[i] = nil;
                memset(&g_state[i], 0, sizeof g_state[i]);
            }
        }
        for (GCController* c in now) { /* neue einsortieren */
            bool known = false;
            for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX && !known; ++i) known = g_slots[i] == c;
            for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX && !known; ++i) {
                if (!g_slots[i]) {
                    g_slots[i] = c;
                    known = true;
                }
            }
        }
        for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) {
            if (g_slots[i]) fill(i, g_slots[i]);
        }
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
    for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) g_slots[i] = nil;
    if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;
    return nhal_status_ok();
}

static void feature_shutdown(void) {
    for (uint32_t i = 0; i < NHAL_GAMEPAD_MAX; ++i) g_slots[i] = nil;
    memset(g_state, 0, sizeof g_state);
}

static const nhal_input_gamepad_v1 table = {
    sizeof(nhal_input_gamepad_v1), gp_poll, gp_state, gp_connected_count,
};

const nhal_feature_impl nhal_impl_input_gamepad = { 1u, feature_init, feature_shutdown, &table };
