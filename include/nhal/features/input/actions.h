/*
 * input.actions, Version 1: Aktionen statt Tasten ("jump" statt Leertaste).
 *
 * Plattformunabhängig. Eine Aktion hat bis zu NHAL_ACTION_BINDINGS_MAX
 * Bindungen an Tasten, Maustasten, Mausrad und Gamepad-Knöpfe/-Achsen.
 *
 *     A->load_default();
 *     ...pro Frame, nach input.state:
 *     A->update(S, G, 0);              // G darf NULL sein (ohne Gamepad)
 *     if (A->pressed(A->find("jump"))) ...
 *     float mx = A->value(A->find("move_x"));
 *
 * Semantik:
 *   - value() = Summe aller Bindungen, begrenzt auf -1..1.
 *     Taste/Knopf gedrückt → scale; Achse → Wert × scale (nach der Totzone,
 *     danach neu auf 0..1 gestreckt); Mausrad → Raddrehung × scale.
 *   - down() = |value| >= 0.5. pressed/released = Flanken von down()
 *     zwischen zwei update()-Aufrufen.
 *   - Gleicher Name in define() → dieselbe ID. IDs bleiben bis clear().
 *   - Ungültige IDs (z. B. -1 aus find) liefern 0 bzw. false.
 *
 * load_default() legt diese Aktionen an (Gamepad mit Standard-Belegung):
 *   move_x    D/→ +1, A/← −1, linker Stick X
 *   move_y    S/↓ +1, W/↑ −1, linker Stick Y   (oben = −1 wie bei Bildschirmkoordinaten)
 *   look_x    rechter Stick X
 *   look_y    rechter Stick Y
 *   jump      Leertaste, SOUTH
 *   confirm   Enter, Ziffernblock-Enter, SOUTH
 *   cancel    Escape, EAST
 *   pause     Escape, START
 *   fire      linke Maustaste, rechter Trigger
 *   aim       rechte Maustaste, linker Trigger
 *   sprint    linke Umschalttaste, linker Stick gedrückt
 *   interact  E, WEST
 */
#ifndef NHAL_FEATURES_INPUT_ACTIONS_H
#define NHAL_FEATURES_INPUT_ACTIONS_H

#include "nhal/features/input/gamepad.h"
#include "nhal/features/input/state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_ACTIONS_MAX 64u
#define NHAL_ACTION_BINDINGS_MAX 8u
#define NHAL_ACTION_NAME_MAX 32u

typedef enum nhal_binding_kind {
    NHAL_BIND_KEY = 1,        /* code: nhal_key */
    NHAL_BIND_MOUSE_BUTTON,   /* code: nhal_mouse_button */
    NHAL_BIND_MOUSE_WHEEL_X,  /* code ungenutzt */
    NHAL_BIND_MOUSE_WHEEL_Y,
    NHAL_BIND_GAMEPAD_BUTTON, /* code: NHAL_GAMEPAD_* (ein Bit) */
    NHAL_BIND_GAMEPAD_AXIS    /* code: nhal_gamepad_axis */
} nhal_binding_kind;

typedef struct nhal_action_binding {
    nhal_binding_kind kind;
    uint32_t code;
    float scale;    /* 0 wird als 1 genommen */
    float deadzone; /* nur Achsen, 0..1 */
} nhal_action_binding;

typedef struct nhal_input_actions_v1 {
    uint32_t struct_size;

    int32_t (*define)(const char* name); /* -1: Name leer/zu lang oder Tabelle voll */
    int32_t (*find)(const char* name);   /* -1: unbekannt */
    nhal_status (*bind)(int32_t action, const nhal_action_binding* binding);
    void (*unbind_all)(int32_t action);
    void (*clear)(void);
    nhal_status (*load_default)(void); /* ersetzt alle Aktionen */

    void (*update)(const nhal_input_state_v1* state, const nhal_input_gamepad_v1* gamepad, uint32_t gamepad_slot);

    float (*value)(int32_t action);
    bool (*down)(int32_t action);
    bool (*pressed)(int32_t action);
    bool (*released)(int32_t action);

    uint32_t (*count)(void);
    const char* (*name)(int32_t action);
} nhal_input_actions_v1;

static inline const nhal_input_actions_v1* nhal_get_input_actions_v1(const nhal_context* ctx) {
    return (const nhal_input_actions_v1*)nhal_get(ctx, NHAL_FEATURE_INPUT_ACTIONS, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_INPUT_ACTIONS_H */
