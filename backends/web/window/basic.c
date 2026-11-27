/*
 * window.basic – Web (Canvas und DOM-Ereignisse).
 *
 * Ein Fenster ist eine Canvas. Die DOM-Listener legen Ereignisse in eine
 * JS-Warteschlange; poll_events holt sie in die gemeinsame C-Warteschlange
 * (common/window/queue.h). Maus, Touch und Stift kommen über Pointer Events.
 * Text und IME laufen über ein verstecktes Textfeld, das den Fokus hält,
 * solange text_input an ist. Abgelegte Dateien schreibt NHAL nach /drop/ im
 * virtuellen FS und meldet sie erst, wenn sie dort liegen.
 *
 * inject_key schickt ein KeyboardEvent an das Fenster. Der Browser erzeugt
 * für solche (nicht vertrauenswürdigen) Ereignisse keinen Text; NHAL tut das
 * dann selbst, damit inject_key überall dasselbe liefert wie unter macOS.
 */
#include "nhal/features/window/basic.h"
#include "common/window/queue.h"
#include "nhal/nhal_backend.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define MAX_WINDOWS 16u

typedef struct web_window {
    uint32_t id;
    char selector[128];
} web_window;

static web_window g_win[MAX_WINDOWS];
static uint32_t g_next_id;
static nhal_evq g_q;

/* clang-format off */
EM_JS_DEPS(nhal_window_web, "$stringToUTF8,$UTF8ToString")

EM_JS(int, win_js_init, (void), {
    const codes = [""];
    for (let c = 65; c <= 90; ++c) codes.push("Key" + String.fromCharCode(c));
    for (let d = 0; d <= 9; ++d) codes.push("Digit" + d);
    for (let f = 1; f <= 20; ++f) codes.push("F" + f);
    codes.push("Enter", "Escape", "Backspace", "Tab", "Space", "Minus", "Equal", "BracketLeft", "BracketRight",
               "Backslash", "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash", "IntlBackslash",
               "CapsLock", "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight",
               "MetaLeft", "MetaRight", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End",
               "PageUp", "PageDown", "Insert", "Delete", "PrintScreen", "ScrollLock", "Pause", "ContextMenu",
               "NumLock");
    for (let d = 0; d <= 9; ++d) codes.push("Numpad" + d);
    codes.push("NumpadDecimal", "NumpadDivide", "NumpadMultiply", "NumpadSubtract", "NumpadAdd", "NumpadEnter",
               "NumpadEqual");
    const index = {};
    codes.forEach((c, i) => { if (c) index[c] = i; });
    globalThis.nhalWin = { codes: codes, index: index, wins: {}, q: [] };
    return codes.length;
})

EM_JS(void, win_js_shutdown, (void), {
    const g = globalThis.nhalWin;
    if (g) for (const id in g.wins) g.wins[id].destroy();
    globalThis.nhalWin = null;
})

EM_JS(int, win_js_has_dom, (void), { return typeof document !== "undefined" ? 1 : 0; })

/* Fenster anlegen: großer DOM-Teil in basic.js (--js-library). */
extern int nhal_web_window_create(int id, const char* selector, const char* title, int w, int h, int flags);

EM_JS(void, win_js_destroy, (int id), { const w = globalThis.nhalWin.wins[id]; if (w) w.destroy(); })

EM_JS(void, win_js_show, (int id, int visible), {
    const w = globalThis.nhalWin.wins[id];
    w.canvas.style.display = visible ? "" : "none";
    if (visible) w.focus();
})

EM_JS(void, win_js_set_title, (const char* t), { if (typeof document !== "undefined") document.title = UTF8ToString(t); })

EM_JS(void, win_js_set_size, (int id, int width, int height), {
    const w = globalThis.nhalWin.wins[id];
    w.w = width; w.h = height;
    w.canvas.style.width = width + "px";
    w.canvas.style.height = height + "px";
})

EM_JS(void, win_js_size, (int id, int* out_ints, float* out_scale), {
    const s = globalThis.nhalWin.wins[id].size();
    HEAP32[out_ints >> 2] = s[0]; HEAP32[(out_ints >> 2) + 1] = s[1];
    HEAP32[(out_ints >> 2) + 2] = s[2]; HEAP32[(out_ints >> 2) + 3] = s[3];
    HEAPF32[out_scale >> 2] = s[4];
})

EM_JS(int, win_js_focused, (int id), {
    const w = globalThis.nhalWin.wins[id];
    return w && (document.activeElement === w.canvas || document.activeElement === w.text) ? 1 : 0;
})

EM_JS(void, win_js_text_input, (int id, int on), {
    const w = globalThis.nhalWin.wins[id];
    const had = document.activeElement === w.canvas || document.activeElement === w.text;
    w.textInput = on !== 0;
    if (had) w.focus();
})

EM_JS(void, win_js_cursor, (int id, int visible), {
    globalThis.nhalWin.wins[id].canvas.style.cursor = visible ? "" : "none";
})

EM_JS(int, win_js_take, (int* ints, float* floats, double* time, int* window, char* str, int cap), {
    const g = globalThis.nhalWin;
    if (!g || !g.q.length) return 0;
    const e = g.q.shift();
    for (let k = 0; k < 8; ++k) {
        HEAP32[(ints >> 2) + k] = e.i[k] | 0;
        HEAPF32[(floats >> 2) + k] = e.f[k] || 0;
    }
    HEAPF64[time >> 3] = e.time;
    HEAP32[window >> 2] = e.w;
    if (e.s !== undefined) stringToUTF8(e.s, str, cap); else HEAPU8[str] = 0;
    return e.t;
})

EM_JS(int, win_js_inject_key, (int id, int key, int down, int mods), {
    const g = globalThis.nhalWin;
    const w = g.wins[id];
    const code = g.codes[key];
    if (!w || !code) return 0;
    const shift = (mods & 1) !== 0;
    let ch = "";
    if (code.startsWith("Key")) ch = shift ? code.slice(3) : code.slice(3).toLowerCase();
    else if (code.startsWith("Digit")) ch = shift ? ")!@#$%^&*("[+code.slice(5)] : code.slice(5);
    else if (code === "Space") ch = " ";
    const el = w.textInput ? w.text : w.canvas;
    el.dispatchEvent(new KeyboardEvent(down ? "keydown" : "keyup", {
        code: code, key: ch || code, bubbles: true, cancelable: true,
        shiftKey: shift, ctrlKey: (mods & 2) !== 0, altKey: (mods & 4) !== 0, metaKey: (mods & 8) !== 0
    }));
    return 1;
})
/* clang-format on */

static web_window* find(uint32_t id) {
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (id && g_win[i].id == id) return &g_win[i];
    }
    return NULL;
}

static nhal_status no_window(void) { return nhal_status_error(NHAL_ERR_NOT_FOUND, "Unbekanntes Fenster"); }

static double now_seconds(void) { return emscripten_get_now() / 1000.0; }

static nhal_status win_create(const nhal_window_desc* d, uint32_t* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out_id ist NULL");
    *out = 0;
    if (!win_js_has_dom()) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein DOM (z. B. node) – keine Canvas");
    web_window* w = NULL;
    for (uint32_t i = 0; i < MAX_WINDOWS && !w; ++i) {
        if (!g_win[i].id) w = &g_win[i];
    }
    if (!w) return nhal_status_error(NHAL_ERR_BUSY, "Zu viele Fenster (höchstens 16)");
    const char* sel = d ? d->canvas : NULL;
    if (sel && strlen(sel) >= sizeof w->selector)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Canvas-Selektor zu lang");
    uint32_t id = ++g_next_id;
    if (!id) id = g_next_id = 1;
    const int r = nhal_web_window_create((int)id, sel, d ? d->title : NULL, d && d->width ? (int)d->width : 1280,
                            d && d->height ? (int)d->height : 720, d ? (int)d->flags : 0);
    if (r == -2) return nhal_status_error(NHAL_ERR_NOT_FOUND, "Keine Canvas zum Selektor gefunden");
    if (r != 0) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein DOM (z. B. node) – keine Canvas");
    w->id = id;
    if (sel) snprintf(w->selector, sizeof w->selector, "%s", sel);
    else snprintf(w->selector, sizeof w->selector, "#nhal-window-%u", (unsigned)id);
    *out = id;
    return nhal_status_ok();
}

static void win_destroy(uint32_t id) {
    web_window* w = find(id);
    if (!w) return;
    win_js_destroy((int)id);
    memset(w, 0, sizeof *w);
}

static nhal_status win_show(uint32_t id, bool visible) {
    if (!find(id)) return no_window();
    win_js_show((int)id, visible);
    return nhal_status_ok();
}

static nhal_status win_set_title(uint32_t id, const char* title) {
    if (!find(id)) return no_window();
    win_js_set_title(title ? title : "");
    return nhal_status_ok();
}

static nhal_status win_set_size(uint32_t id, uint32_t width, uint32_t height) {
    if (!find(id)) return no_window();
    if (!width || !height) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Größe 0");
    win_js_set_size((int)id, (int)width, (int)height);
    return nhal_status_ok();
}

static nhal_status win_size(uint32_t id, nhal_window_size* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    if (!find(id)) return no_window();
    int v[4];
    float scale = 1.0f;
    win_js_size((int)id, v, &scale);
    out->width = (uint32_t)v[0];
    out->height = (uint32_t)v[1];
    out->pixel_width = (uint32_t)v[2];
    out->pixel_height = (uint32_t)v[3];
    out->scale = scale;
    return nhal_status_ok();
}

static nhal_status win_native(uint32_t id, nhal_window_native* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    web_window* w = find(id);
    if (!w) return no_window();
    out->kind = NHAL_WINDOW_NATIVE_CANVAS;
    out->handle = w->selector;
    return nhal_status_ok();
}

static bool win_focused(uint32_t id) { return find(id) && win_js_focused((int)id); }

static void win_poll(void) {
    int ints[8];
    float f[8];
    double time = 0.0;
    int window = 0;
    static char str[NHAL_EVQ_STR];
    int t;
    while ((t = win_js_take(ints, f, &time, &window, str, (int)sizeof str)) != 0) {
        nhal_input_event e;
        memset(&e, 0, sizeof e);
        e.type = (nhal_event_type)t;
        e.window = (uint32_t)window;
        e.time = time;
        switch (e.type) {
        case NHAL_EVENT_KEY_DOWN:
        case NHAL_EVENT_KEY_UP:
            e.key.key = (nhal_key)ints[0];
            e.key.mods = (uint32_t)ints[1];
            e.key.repeat = ints[2] != 0;
            e.key.native = (uint32_t)ints[3];
            break;
        case NHAL_EVENT_TEXT_INPUT:
        case NHAL_EVENT_TEXT_EDITING:
            e.text.text = str;
            e.text.cursor = ints[0];
            e.text.selection_length = ints[1];
            break;
        case NHAL_EVENT_MOUSE_MOVE:
        case NHAL_EVENT_MOUSE_DOWN:
        case NHAL_EVENT_MOUSE_UP:
        case NHAL_EVENT_MOUSE_ENTER:
        case NHAL_EVENT_MOUSE_LEAVE:
            e.mouse.x = f[0];
            e.mouse.y = f[1];
            e.mouse.dx = f[2];
            e.mouse.dy = f[3];
            e.mouse.button = (nhal_mouse_button)ints[0];
            e.mouse.buttons = (uint32_t)ints[1];
            e.mouse.clicks = (uint32_t)ints[2];
            e.mouse.mods = (uint32_t)ints[3];
            break;
        case NHAL_EVENT_MOUSE_WHEEL:
            e.wheel.dx = f[0];
            e.wheel.dy = f[1];
            e.wheel.x = f[2];
            e.wheel.y = f[3];
            e.wheel.precise = ints[0] != 0;
            e.wheel.mods = (uint32_t)ints[1];
            break;
        case NHAL_EVENT_WINDOW_RESIZE:
            e.resize.width = (uint32_t)ints[0];
            e.resize.height = (uint32_t)ints[1];
            e.resize.pixel_width = (uint32_t)ints[2];
            e.resize.pixel_height = (uint32_t)ints[3];
            e.resize.scale = f[0];
            break;
        case NHAL_EVENT_FILE_DROP:
            e.drop.path = str;
            e.drop.x = f[0];
            e.drop.y = f[1];
            e.drop.index = (uint32_t)ints[0];
            e.drop.count = (uint32_t)ints[1];
            break;
        case NHAL_EVENT_TOUCH:
            e.touch.id = (uint64_t)(uint32_t)ints[0];
            e.touch.phase = (nhal_pointer_phase)ints[1];
            e.touch.x = f[0];
            e.touch.y = f[1];
            e.touch.pressure = f[2];
            break;
        case NHAL_EVENT_PEN:
            e.pen.id = (uint64_t)(uint32_t)ints[0];
            e.pen.phase = (nhal_pointer_phase)ints[1];
            e.pen.eraser = ints[2] != 0;
            e.pen.contact = ints[3] != 0;
            e.pen.buttons = (uint32_t)ints[4];
            e.pen.x = f[0];
            e.pen.y = f[1];
            e.pen.pressure = f[2];
            e.pen.tilt_x = f[3];
            e.pen.tilt_y = f[4];
            break;
        default: break;
        }
        nhal_evq_push(&g_q, &e);
    }
}

static bool win_wait(uint32_t timeout_ms) {
    (void)timeout_ms; /* der Browser lässt sich nicht blockieren */
    const uint32_t before = g_q.count;
    win_poll();
    return g_q.count > before;
}

static bool win_next(nhal_input_event* out) { return nhal_evq_pop(&g_q, out); }
static uint64_t win_dropped(void) { return g_q.dropped; }

static nhal_status win_text_input(uint32_t id, bool enabled) {
    if (!find(id)) return no_window();
    win_js_text_input((int)id, enabled);
    return nhal_status_ok();
}

static nhal_status win_cursor(uint32_t id, bool visible) {
    if (!find(id)) return no_window();
    win_js_cursor((int)id, visible);
    return nhal_status_ok();
}

static nhal_status win_push(const nhal_input_event* ev) {
    nhal_status s = nhal_evq_check_synthetic(ev);
    if (s.code) return s;
    nhal_input_event e = *ev;
    e.synthetic = true;
    if (e.time == 0.0) e.time = now_seconds();
    if (!nhal_evq_push(&g_q, &e)) return nhal_status_error(NHAL_ERR_BUSY, "Warteschlange voll oder String zu lang");
    return nhal_status_ok();
}

static nhal_status win_inject_key(uint32_t id, nhal_key key, bool down, uint32_t mods) {
    if (!find(id)) return no_window();
    if (key <= NHAL_KEY_UNKNOWN || key >= NHAL_KEY_COUNT) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Taste");
    if (!win_js_inject_key((int)id, (int)key, down, (int)mods))
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Taste hat im Web keinen KeyboardEvent.code");
    return nhal_status_ok();
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    memset(g_win, 0, sizeof g_win);
    g_next_id = 0;
    if (win_js_init() != NHAL_KEY_COUNT)
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Tastentabelle im Web passt nicht zu nhal_key");
    return nhal_evq_init(&g_q, allocator);
}

static void feature_shutdown(void) {
    win_js_shutdown();
    memset(g_win, 0, sizeof g_win);
    nhal_evq_free(&g_q);
}

static const nhal_window_basic_v1 table = {
    sizeof(nhal_window_basic_v1),
    win_create, win_destroy, win_show, win_set_title, win_set_size, win_size, win_native, win_focused,
    win_poll, win_wait, win_next, win_dropped,
    win_text_input, win_cursor,
    win_push, win_inject_key,
};

const nhal_feature_impl nhal_impl_window_basic = { 1u, feature_init, feature_shutdown, &table };
