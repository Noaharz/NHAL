/*
 * input.state, Version 1: Zustand von Tastatur, Maus und Touch mit Flanken.
 *
 * Plattformunabhängig: Die App füttert Ereignisse hinein (meist aus
 * window.basic, für Tests und Replays auch selbst gebaute) und fragt danach
 * den Zustand ab.
 *
 *     S->begin_frame();
 *     while (W->next_event(&e)) S->feed(&e);
 *     if (S->key_pressed(NHAL_KEY_SPACE)) jump();
 *
 * Semantik:
 *   - key_/mouse_pressed und _released gelten für den Frame seit begin_frame(). Drücken
 *     und Loslassen im selben Frame setzt beides; _down ist dann false.
 *   - Tastenwiederholung (repeat) erzeugt keine neue Flanke.
 *   - WINDOW_BLUR lässt alle Tasten und Maustasten los (mit released-Flanke),
 *     damit nach einem Fensterwechsel nichts "klemmt".
 *   - text() sammelt TEXT_INPUT des Frames (UTF-8, höchstens
 *     NHAL_INPUT_TEXT_MAX - 1 Bytes; der Rest fällt an einer Zeichengrenze weg).
 *   - mouse_delta: Summe der Bewegungen im Frame; wheel: Summe der Raddrehung.
 *   - Touches: bis zu NHAL_INPUT_TOUCH_MAX gleichzeitig, in Reihenfolge des Aufsetzens.
 *   - Ein Zustand für alle Fenster; filter_window() schränkt auf eines ein.
 */
#ifndef NHAL_FEATURES_INPUT_STATE_H
#define NHAL_FEATURES_INPUT_STATE_H

#include "nhal/features/input/events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_INPUT_TEXT_MAX 512u
#define NHAL_INPUT_TOUCH_MAX 10u

typedef struct nhal_touch_point {
    uint64_t id;
    float x, y;
    float pressure;
} nhal_touch_point;

typedef struct nhal_input_state_v1 {
    uint32_t struct_size;

    void (*begin_frame)(void);
    void (*feed)(const nhal_input_event* event);
    void (*reset)(void);                   /* alles los, ohne Flanken */
    void (*filter_window)(uint32_t window); /* 0 = alle Fenster */

    bool (*key_down)(nhal_key key);
    bool (*key_pressed)(nhal_key key);
    bool (*key_released)(nhal_key key);
    uint32_t (*mods)(void);

    bool (*mouse_down)(nhal_mouse_button button);
    bool (*mouse_pressed)(nhal_mouse_button button);
    bool (*mouse_released)(nhal_mouse_button button);
    uint32_t (*mouse_clicks)(nhal_mouse_button button); /* clicks des letzten Drückens im Frame, sonst 0 */
    void (*mouse_position)(float* x, float* y);
    void (*mouse_delta)(float* dx, float* dy);
    void (*wheel)(float* dx, float* dy);
    bool (*mouse_inside)(void);

    const char* (*text)(void);

    uint32_t (*touch_count)(void);
    bool (*touch_at)(uint32_t index, nhal_touch_point* out);
} nhal_input_state_v1;

static inline const nhal_input_state_v1* nhal_get_input_state_v1(const nhal_context* ctx) {
    return (const nhal_input_state_v1*)nhal_get(ctx, NHAL_FEATURE_INPUT_STATE, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_INPUT_STATE_H */
