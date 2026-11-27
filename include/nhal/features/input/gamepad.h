/*
 * input.gamepad, Version 1: Gamepads mit einheitlicher Belegung.
 *
 * Ablauf: einmal pro Frame poll(), danach state(slot) für jeden Platz lesen.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Bis zu NHAL_GAMEPAD_MAX Plätze. Ein Gamepad behält seinen Platz, solange
 *     es verbunden ist. Freie Plätze werden wiederverwendet.
 *   - Tasten nach Lage benannt (SOUTH = unten, auf Xbox-Pads "A").
 *   - Achsen: Sticks -1..1, Y nach oben negativ (wie Bildschirmkoordinaten).
 *     Trigger 0..1. Eine Totzone gibt es nicht; die setzt die App.
 *   - standard_mapping = false heißt: Das Gerät ist verbunden, aber seine
 *     Belegung ist unbekannt. Tasten und Achsen sind dann nach bestem Wissen
 *     belegt und können abweichen.
 *   - poll() und state() gehören in denselben Thread.
 *
 * Plattformabhängig:
 *   - macOS: GameController-Framework. Neue Geräte meldet das System über
 *     die Main-Runloop. poll() im Hauptthread verarbeitet anstehende
 *     Meldungen selbst, damit es auch ohne Cocoa-App geht. Eingaben kommen
 *     auch an, wenn die App nicht im Vordergrund ist.
 *   - Web: Gamepad API. Browser melden ein Gamepad erst nach dem ersten
 *     Tastendruck darauf.
 */
#ifndef NHAL_FEATURES_INPUT_GAMEPAD_H
#define NHAL_FEATURES_INPUT_GAMEPAD_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_GAMEPAD_MAX 8u

enum {
    NHAL_GAMEPAD_SOUTH = 1u << 0,
    NHAL_GAMEPAD_EAST = 1u << 1,
    NHAL_GAMEPAD_WEST = 1u << 2,
    NHAL_GAMEPAD_NORTH = 1u << 3,
    NHAL_GAMEPAD_LEFT_SHOULDER = 1u << 4,
    NHAL_GAMEPAD_RIGHT_SHOULDER = 1u << 5,
    NHAL_GAMEPAD_BACK = 1u << 6,
    NHAL_GAMEPAD_START = 1u << 7,
    NHAL_GAMEPAD_LEFT_STICK = 1u << 8,
    NHAL_GAMEPAD_RIGHT_STICK = 1u << 9,
    NHAL_GAMEPAD_DPAD_UP = 1u << 10,
    NHAL_GAMEPAD_DPAD_DOWN = 1u << 11,
    NHAL_GAMEPAD_DPAD_LEFT = 1u << 12,
    NHAL_GAMEPAD_DPAD_RIGHT = 1u << 13,
    NHAL_GAMEPAD_GUIDE = 1u << 14
};

typedef enum nhal_gamepad_axis {
    NHAL_GAMEPAD_LEFT_X = 0,
    NHAL_GAMEPAD_LEFT_Y = 1,
    NHAL_GAMEPAD_RIGHT_X = 2,
    NHAL_GAMEPAD_RIGHT_Y = 3,
    NHAL_GAMEPAD_LEFT_TRIGGER = 4,
    NHAL_GAMEPAD_RIGHT_TRIGGER = 5,
    NHAL_GAMEPAD_AXIS_COUNT = 6
} nhal_gamepad_axis;

typedef struct nhal_gamepad_state {
    bool connected;
    bool standard_mapping;
    uint32_t buttons; /* NHAL_GAMEPAD_* */
    float axes[NHAL_GAMEPAD_AXIS_COUNT];
    char name[64]; /* UTF-8, vom System gemeldet */
} nhal_gamepad_state;

typedef struct nhal_input_gamepad_v1 {
    uint32_t struct_size;
    void (*poll)(void);
    /* false bei ungültigem Platz; sonst gefüllt (auch wenn nicht verbunden). */
    bool (*state)(uint32_t slot, nhal_gamepad_state* out);
    uint32_t (*connected_count)(void);
} nhal_input_gamepad_v1;

static inline const nhal_input_gamepad_v1* nhal_get_input_gamepad_v1(const nhal_context* ctx) {
    return (const nhal_input_gamepad_v1*)nhal_get(ctx, NHAL_FEATURE_INPUT_GAMEPAD, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_INPUT_GAMEPAD_H */
