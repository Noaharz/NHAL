/*
 * window.basic, Version 1: Fenster und Ereignisse (Tastatur, Maus, Text,
 * Datei-Drops, Touch, Stift, Fokus, Größe, Beenden).
 *
 * Ablauf pro Frame:
 *     W->poll_events();                       // Ereignisse vom System holen
 *     nhal_input_event e;
 *     while (W->next_event(&e)) { ... }       // abarbeiten
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Fenster haben IDs ab 1. Ereignisse ohne Fenster (QUIT) tragen 0.
 *   - Größen in Punkten; die Pixelgröße für gfx steht in size()/RESIZE.
 *   - WINDOW_CLOSE und QUIT sind Bitten: Nichts schließt von selbst.
 *   - Strings in Ereignissen gelten bis zum nächsten next_event/poll_events.
 *   - Die Warteschlange fasst NHAL_WINDOW_QUEUE Ereignisse. Läuft sie über,
 *     gehen neue verloren und dropped_events() zählt mit.
 *   - Text kommt nur, solange text_input(window, true) gilt (Standard: an).
 *
 * Zum Testen und für Replays:
 *   push_event   reiht ein fertiges Ereignis ein (synthetic = true), am
 *                System vorbei.
 *   inject_key   schickt einen Tastendruck durch den Weg der Plattform
 *                (macOS: NSEvent an das Fenster, Web: KeyboardEvent an die
 *                Canvas). Geprüft wird so auch die Übersetzung.
 *
 * Plattformabhängig:
 *   - macOS: alle Funktionen nur im Hauptthread (sonst NHAL_ERR_INVALID_ARGUMENT
 *     bzw. kein Fenster). NHAL richtet NSApplication ein, falls die App das
 *     nicht selbst tut (Menü mit "Beenden", Dock-Symbol). Touch gibt es auf
 *     dem Mac nicht (das Trackpad liefert Maus und Rad), Stifte über
 *     Grafiktabletts schon.
 *   - Web: ein Fenster ist eine Canvas (vorhandene per CSS-Selektor oder neu
 *     angelegte). Titel setzt document.title. wait_events kann nicht
 *     blockieren. QUIT gibt es nicht. Abgelegte Dateien schreibt NHAL ins
 *     virtuelle FS unter /drop/ und meldet dann den Pfad.
 */
#ifndef NHAL_FEATURES_WINDOW_BASIC_H
#define NHAL_FEATURES_WINDOW_BASIC_H

#include "nhal/features/input/events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_WINDOW_QUEUE 256u

enum {
    NHAL_WINDOW_RESIZABLE = 1u << 0,
    NHAL_WINDOW_HIDDEN = 1u << 1,     /* erst mit show() sichtbar */
    NHAL_WINDOW_BORDERLESS = 1u << 2,
    NHAL_WINDOW_HIGH_DPI = 1u << 3    /* volle Pixeldichte (sonst scale 1) */
};

typedef struct nhal_window_desc {
    const char* title;
    uint32_t width, height; /* Punkte; 0 = 1280×720 */
    uint32_t flags;         /* NHAL_WINDOW_* */
    const char* canvas;     /* nur Web: CSS-Selektor einer vorhandenen Canvas; NULL = neu anlegen */
} nhal_window_desc;

/* Werte gleich wie nhal_gfx_surface_kind, damit native() direkt in gfx passt. */
typedef enum nhal_window_native_kind {
    NHAL_WINDOW_NATIVE_NSVIEW = 2, /* handle: NSView* */
    NHAL_WINDOW_NATIVE_CANVAS = 3  /* handle: const char* CSS-Selektor */
} nhal_window_native_kind;

typedef struct nhal_window_native {
    nhal_window_native_kind kind;
    void* handle;
} nhal_window_native;

typedef struct nhal_window_size {
    uint32_t width, height;             /* Punkte */
    uint32_t pixel_width, pixel_height;
    float scale;
} nhal_window_size;

typedef struct nhal_window_basic_v1 {
    uint32_t struct_size;

    nhal_status (*create)(const nhal_window_desc* desc, uint32_t* out_id);
    void (*destroy)(uint32_t window);
    nhal_status (*show)(uint32_t window, bool visible);
    nhal_status (*set_title)(uint32_t window, const char* title);
    nhal_status (*set_size)(uint32_t window, uint32_t width, uint32_t height);
    nhal_status (*size)(uint32_t window, nhal_window_size* out);
    nhal_status (*native)(uint32_t window, nhal_window_native* out);
    bool (*focused)(uint32_t window);

    /* Ereignisse vom System holen (nicht blockierend). */
    void (*poll_events)(void);
    /* Wie poll_events, wartet aber bis zu timeout_ms auf ein Ereignis. true = es kam eins. */
    bool (*wait_events)(uint32_t timeout_ms);
    bool (*next_event)(nhal_input_event* out);
    uint64_t (*dropped_events)(void);

    nhal_status (*text_input)(uint32_t window, bool enabled);
    nhal_status (*set_cursor_visible)(uint32_t window, bool visible);

    nhal_status (*push_event)(const nhal_input_event* event);
    nhal_status (*inject_key)(uint32_t window, nhal_key key, bool down, uint32_t mods);
} nhal_window_basic_v1;

static inline const nhal_window_basic_v1* nhal_get_window_basic_v1(const nhal_context* ctx) {
    return (const nhal_window_basic_v1*)nhal_get(ctx, NHAL_FEATURE_WINDOW_BASIC, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_WINDOW_BASIC_H */
