/*
 * Gemeinsame Eingabe-Typen: Tasten, Maustasten, Ereignisse.
 *
 * Kein eigenes Feature. window.basic erzeugt diese Ereignisse, input.state
 * und input.actions verarbeiten sie.
 *
 * Tasten (nhal_key) bezeichnen die *Lage* auf der Tastatur, nicht das
 * Zeichen: NHAL_KEY_W ist die Taste, die auf einer US-Tastatur "W" trägt –
 * auf AZERTY steht dort "Z". Für Spiele (WASD) ist das richtig. Das getippte
 * Zeichen kommt getrennt als NHAL_EVENT_TEXT_INPUT, mit Tastaturbelegung und
 * Eingabemethode (IME).
 *
 * Koordinaten: in Fensterpunkten (logisch), Ursprung oben links. Pixel =
 * Punkte × scale (siehe NHAL_EVENT_WINDOW_RESIZE).
 */
#ifndef NHAL_FEATURES_INPUT_EVENTS_H
#define NHAL_FEATURES_INPUT_EVENTS_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nhal_key {
    NHAL_KEY_UNKNOWN = 0,
    NHAL_KEY_A, NHAL_KEY_B, NHAL_KEY_C, NHAL_KEY_D, NHAL_KEY_E, NHAL_KEY_F, NHAL_KEY_G, NHAL_KEY_H, NHAL_KEY_I,
    NHAL_KEY_J, NHAL_KEY_K, NHAL_KEY_L, NHAL_KEY_M, NHAL_KEY_N, NHAL_KEY_O, NHAL_KEY_P, NHAL_KEY_Q, NHAL_KEY_R,
    NHAL_KEY_S, NHAL_KEY_T, NHAL_KEY_U, NHAL_KEY_V, NHAL_KEY_W, NHAL_KEY_X, NHAL_KEY_Y, NHAL_KEY_Z,
    NHAL_KEY_0, NHAL_KEY_1, NHAL_KEY_2, NHAL_KEY_3, NHAL_KEY_4, NHAL_KEY_5, NHAL_KEY_6, NHAL_KEY_7, NHAL_KEY_8,
    NHAL_KEY_9,
    NHAL_KEY_F1, NHAL_KEY_F2, NHAL_KEY_F3, NHAL_KEY_F4, NHAL_KEY_F5, NHAL_KEY_F6, NHAL_KEY_F7, NHAL_KEY_F8,
    NHAL_KEY_F9, NHAL_KEY_F10, NHAL_KEY_F11, NHAL_KEY_F12, NHAL_KEY_F13, NHAL_KEY_F14, NHAL_KEY_F15, NHAL_KEY_F16,
    NHAL_KEY_F17, NHAL_KEY_F18, NHAL_KEY_F19, NHAL_KEY_F20,
    NHAL_KEY_ENTER, NHAL_KEY_ESCAPE, NHAL_KEY_BACKSPACE, NHAL_KEY_TAB, NHAL_KEY_SPACE,
    NHAL_KEY_MINUS, NHAL_KEY_EQUAL, NHAL_KEY_LEFT_BRACKET, NHAL_KEY_RIGHT_BRACKET, NHAL_KEY_BACKSLASH,
    NHAL_KEY_SEMICOLON, NHAL_KEY_APOSTROPHE, NHAL_KEY_GRAVE, NHAL_KEY_COMMA, NHAL_KEY_PERIOD, NHAL_KEY_SLASH,
    NHAL_KEY_INTL_BACKSLASH, /* ISO-Taste links neben Z/Y */
    NHAL_KEY_CAPS_LOCK,
    NHAL_KEY_LEFT_SHIFT, NHAL_KEY_RIGHT_SHIFT, NHAL_KEY_LEFT_CTRL, NHAL_KEY_RIGHT_CTRL,
    NHAL_KEY_LEFT_ALT, NHAL_KEY_RIGHT_ALT, NHAL_KEY_LEFT_SUPER, NHAL_KEY_RIGHT_SUPER, /* Super = Cmd/Win */
    NHAL_KEY_UP, NHAL_KEY_DOWN, NHAL_KEY_LEFT, NHAL_KEY_RIGHT,
    NHAL_KEY_HOME, NHAL_KEY_END, NHAL_KEY_PAGE_UP, NHAL_KEY_PAGE_DOWN, NHAL_KEY_INSERT, NHAL_KEY_DELETE,
    NHAL_KEY_PRINT_SCREEN, NHAL_KEY_SCROLL_LOCK, NHAL_KEY_PAUSE, NHAL_KEY_MENU,
    NHAL_KEY_NUM_LOCK, NHAL_KEY_KP_0, NHAL_KEY_KP_1, NHAL_KEY_KP_2, NHAL_KEY_KP_3, NHAL_KEY_KP_4, NHAL_KEY_KP_5,
    NHAL_KEY_KP_6, NHAL_KEY_KP_7, NHAL_KEY_KP_8, NHAL_KEY_KP_9, NHAL_KEY_KP_DECIMAL, NHAL_KEY_KP_DIVIDE,
    NHAL_KEY_KP_MULTIPLY, NHAL_KEY_KP_SUBTRACT, NHAL_KEY_KP_ADD, NHAL_KEY_KP_ENTER, NHAL_KEY_KP_EQUAL,
    NHAL_KEY_COUNT
} nhal_key;

enum {
    NHAL_MOD_SHIFT = 1u << 0,
    NHAL_MOD_CTRL = 1u << 1,
    NHAL_MOD_ALT = 1u << 2,   /* macOS: Option */
    NHAL_MOD_SUPER = 1u << 3, /* macOS: Cmd, Windows: Win */
    NHAL_MOD_CAPS_LOCK = 1u << 4
};

typedef enum nhal_mouse_button {
    NHAL_MOUSE_LEFT = 0,
    NHAL_MOUSE_RIGHT = 1,
    NHAL_MOUSE_MIDDLE = 2,
    NHAL_MOUSE_X1 = 3, /* "Zurück" */
    NHAL_MOUSE_X2 = 4, /* "Vor" */
    NHAL_MOUSE_BUTTON_COUNT = 5
} nhal_mouse_button;

typedef enum nhal_event_type {
    NHAL_EVENT_NONE = 0,
    NHAL_EVENT_QUIT,           /* App soll enden (Cmd+Q, Abmelden); nichts geschieht von selbst */
    NHAL_EVENT_WINDOW_CLOSE,   /* Schließen-Knopf; das Fenster bleibt, bis die App es zerstört */
    NHAL_EVENT_WINDOW_RESIZE,  /* auch bei Wechsel von scale (anderer Bildschirm) */
    NHAL_EVENT_WINDOW_FOCUS,
    NHAL_EVENT_WINDOW_BLUR,
    NHAL_EVENT_KEY_DOWN,
    NHAL_EVENT_KEY_UP,
    NHAL_EVENT_TEXT_INPUT,     /* fertiger Text (UTF-8) */
    NHAL_EVENT_TEXT_EDITING,   /* laufende Komposition der Eingabemethode */
    NHAL_EVENT_MOUSE_MOVE,
    NHAL_EVENT_MOUSE_DOWN,
    NHAL_EVENT_MOUSE_UP,
    NHAL_EVENT_MOUSE_WHEEL,
    NHAL_EVENT_MOUSE_ENTER,
    NHAL_EVENT_MOUSE_LEAVE,
    NHAL_EVENT_FILE_DROP,      /* eine Datei je Ereignis */
    NHAL_EVENT_TOUCH,
    NHAL_EVENT_PEN,
    NHAL_EVENT_TYPE_COUNT
} nhal_event_type;

typedef enum nhal_pointer_phase {
    NHAL_POINTER_DOWN = 0,
    NHAL_POINTER_MOVE,
    NHAL_POINTER_UP,
    NHAL_POINTER_CANCEL
} nhal_pointer_phase;

typedef struct nhal_key_event {
    nhal_key key;
    uint32_t mods;   /* NHAL_MOD_*, Stand nach dem Ereignis */
    bool repeat;     /* Tastenwiederholung */
    uint32_t native; /* Code der Plattform (macOS keyCode, Web: Index in KeyboardEvent.code-Tabelle) */
} nhal_key_event;

typedef struct nhal_text_event {
    const char* text;          /* UTF-8, gilt bis zum nächsten next_event/poll_events */
    int32_t cursor;            /* TEXT_EDITING: Cursor in Bytes, sonst -1 */
    int32_t selection_length;  /* TEXT_EDITING: markierter Teil in Bytes, sonst 0 */
} nhal_text_event;

typedef struct nhal_mouse_event {
    float x, y;       /* Position */
    float dx, dy;     /* MOUSE_MOVE: Bewegung seit dem letzten Ereignis */
    nhal_mouse_button button; /* DOWN/UP */
    uint32_t buttons; /* gedrückte Tasten nach dem Ereignis, Bit = nhal_mouse_button */
    uint32_t clicks;  /* DOWN: 1 = einfach, 2 = doppelt, … */
    uint32_t mods;
} nhal_mouse_event;

typedef struct nhal_wheel_event {
    float dx, dy;   /* dy > 0: vom Nutzer weg (nach oben) */
    bool precise;   /* Trackpad: Pixel; sonst Rasten (Zeilen) */
    uint32_t mods;
    float x, y;
} nhal_wheel_event;

typedef struct nhal_resize_event {
    uint32_t width, height;             /* Punkte */
    uint32_t pixel_width, pixel_height; /* für gfx surface_configure */
    float scale;
} nhal_resize_event;

typedef struct nhal_drop_event {
    const char* path; /* gilt bis zum nächsten next_event/poll_events; Web: Pfad im virtuellen FS */
    float x, y;
    uint32_t index, count; /* Datei index von count im selben Drop */
} nhal_drop_event;

typedef struct nhal_touch_event {
    uint64_t id; /* gleich für einen Finger von DOWN bis UP/CANCEL */
    nhal_pointer_phase phase;
    float x, y;
    float pressure; /* 0..1, ohne Messung 1 */
} nhal_touch_event;

typedef struct nhal_pen_event {
    uint64_t id;
    nhal_pointer_phase phase; /* MOVE ohne Kontakt: schwebt über der Fläche */
    float x, y;
    float pressure;       /* 0..1 */
    float tilt_x, tilt_y; /* -1..1 */
    bool eraser;
    bool contact;
    uint32_t buttons;     /* Stifttasten */
} nhal_pen_event;

typedef struct nhal_input_event {
    nhal_event_type type;
    uint32_t window;  /* window.basic-Fenster-ID, 0 = ohne Fenster (QUIT) */
    double time;      /* Sekunden, monoton; nur Abstände sind aussagekräftig */
    bool synthetic;   /* über push_event eingespeist */
    union {
        nhal_key_event key;
        nhal_text_event text;
        nhal_mouse_event mouse;
        nhal_wheel_event wheel;
        nhal_resize_event resize;
        nhal_drop_event drop;
        nhal_touch_event touch;
        nhal_pen_event pen;
    };
} nhal_input_event;

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_INPUT_EVENTS_H */
