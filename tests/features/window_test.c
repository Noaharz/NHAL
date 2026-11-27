/*
 * window.basic: Warteschlange (push_event, Strings, Überlauf), Fenster
 * (Größe, nativer Griff, Titel), Tasten über den Weg der Plattform
 * (inject_key mit Text), rohe Plattform-Ereignisse gegen die Tastentabelle
 * und RESIZE nach set_size.
 *
 * Ohne Fensteroberfläche (node) laufen nur die Prüfungen der Warteschlange.
 */
#include "nhal/features/window/basic.h"
#include "check.h"

#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#include <unistd.h>
#endif

static nhal_context* g_ctx;
static const nhal_window_basic_v1* W;
static uint32_t g_win;
static int g_step, g_frames;
static bool g_finished;
static int g_rc;

/* Ereignisse sammeln, die ein Schritt prüft. */
enum { MAX_SEEN = 64 };
static nhal_input_event g_seen[MAX_SEEN];
static char g_seen_text[MAX_SEEN][64];
static int g_nseen;

static void collect(void) {
    W->poll_events();
    nhal_input_event e;
    while (W->next_event(&e)) {
        if (g_nseen >= MAX_SEEN) continue;
        g_seen[g_nseen] = e;
        g_seen_text[g_nseen][0] = '\0';
        if (e.type == NHAL_EVENT_TEXT_INPUT || e.type == NHAL_EVENT_TEXT_EDITING)
            strncpy(g_seen_text[g_nseen], e.text.text, sizeof g_seen_text[0] - 1);
        ++g_nseen;
    }
}

static int find_key(nhal_event_type t, nhal_key k, int from) {
    for (int i = from; i < g_nseen; ++i) {
        if (g_seen[i].type == t && g_seen[i].key.key == k) return i;
    }
    return -1;
}

static int find_text(const char* text) {
    for (int i = 0; i < g_nseen; ++i) {
        if (g_seen[i].type == NHAL_EVENT_TEXT_INPUT && strcmp(g_seen_text[i], text) == 0) return i;
    }
    return -1;
}

static void finish(void) {
    if (W && g_win) W->destroy(g_win);
    nhal_shutdown(g_ctx);
    g_rc = check_report("window_test");
    g_finished = true;
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#endif
}

/* ── Warteschlange (überall) ───────────────────────────────────────────── */

static void test_queue(void) {
    nhal_input_event e;
    memset(&e, 0, sizeof e);
    CHECK(W->push_event(NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    e.type = NHAL_EVENT_KEY_DOWN;
    e.key.key = NHAL_KEY_COUNT;
    CHECK(W->push_event(&e).code == NHAL_ERR_INVALID_ARGUMENT);
    memset(&e, 0, sizeof e); /* Union: key.key läge sonst auf drop.path */
    e.type = NHAL_EVENT_FILE_DROP;
    CHECK(W->push_event(&e).code == NHAL_ERR_INVALID_ARGUMENT); /* ohne Pfad */

    memset(&e, 0, sizeof e);
    e.type = NHAL_EVENT_KEY_DOWN;
    e.key.key = NHAL_KEY_SPACE;
    e.key.mods = NHAL_MOD_SHIFT;
    CHECK_OK(W->push_event(&e));
    char text[16] = "grüß";
    nhal_input_event t;
    memset(&t, 0, sizeof t);
    t.type = NHAL_EVENT_TEXT_INPUT;
    t.text.text = text;
    CHECK_OK(W->push_event(&t));
    text[0] = 'X'; /* NHAL hat kopiert */

    nhal_input_event out;
    CHECK(W->next_event(&out));
    CHECK(out.type == NHAL_EVENT_KEY_DOWN && out.key.key == NHAL_KEY_SPACE && out.key.mods == NHAL_MOD_SHIFT);
    CHECK(out.synthetic && out.time > 0.0);
    CHECK(W->next_event(&out));
    CHECK(out.type == NHAL_EVENT_TEXT_INPUT && strcmp(out.text.text, "grüß") == 0);
    CHECK(!W->next_event(&out));

    /* Überlauf: neue gehen verloren und werden gezählt. */
    const uint64_t before = W->dropped_events();
    nhal_input_event m;
    memset(&m, 0, sizeof m);
    m.type = NHAL_EVENT_MOUSE_MOVE;
    int accepted = 0;
    for (unsigned i = 0; i < NHAL_WINDOW_QUEUE + 10u; ++i) accepted += W->push_event(&m).code == NHAL_OK;
    CHECK(accepted == (int)NHAL_WINDOW_QUEUE);
    CHECK(W->dropped_events() - before == 10u);
    int drained = 0;
    while (W->next_event(&out)) ++drained;
    CHECK(drained == (int)NHAL_WINDOW_QUEUE);
}

/* ── Rohe Plattform-Ereignisse gegen die Tastentabelle ─────────────────── */

static void raw_key(uint32_t native_or_index, const char* code) {
#ifdef __EMSCRIPTEN__
    (void)native_or_index;
    char js[256];
    snprintf(js, sizeof js,
             "(function(){var w=globalThis.nhalWin.wins[%u];var el=w.textInput?w.text:w.canvas;"
             "el.dispatchEvent(new KeyboardEvent('keydown',{code:'%s',key:'%s',bubbles:true}));})()",
             (unsigned)g_win, code, code);
    emscripten_run_script(js);
#elif defined(__APPLE__)
    (void)code;
    nhal_window_native n;
    if (W->native(g_win, &n).code != NHAL_OK) return;
    id view = (id)n.handle;
    id window = ((id(*)(id, SEL))objc_msgSend)(view, sel_registerName("window"));
    long number = ((long (*)(id, SEL))objc_msgSend)(window, sel_registerName("windowNumber"));
    id empty = ((id(*)(Class, SEL, const char*))objc_msgSend)(objc_getClass("NSString"),
                                                                sel_registerName("stringWithUTF8String:"), "");
    typedef struct { double x, y; } point;
    typedef id (*make_fn)(Class, SEL, unsigned long, point, unsigned long, double, long, id, id, id, signed char,
                          unsigned short);
    id ev = ((make_fn)objc_msgSend)(objc_getClass("NSEvent"),
                                    sel_registerName("keyEventWithType:location:modifierFlags:timestamp:windowNumber:"
                                                     "context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
                                    10ul /* NSEventTypeKeyDown */, (point){ 0, 0 }, 0ul, 0.0, number, (id)0, empty,
                                    empty, (signed char)0, (unsigned short)native_or_index);
    ((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("sendEvent:"), ev);
#else
    (void)native_or_index;
    (void)code;
#endif
}

/* ── Schritte ──────────────────────────────────────────────────────────── */

static void step_setup(void) {
    const nhal_window_desc d = { "NHAL-Fenstertest", 320, 200, NHAL_WINDOW_RESIZABLE | NHAL_WINDOW_HIDDEN, NULL };
    const nhal_status s = W->create(&d, &g_win);
    if (s.code == NHAL_ERR_UNAVAILABLE) {
        printf("window_test: keine Fensteroberfläche – %s\n", s.reason);
        finish();
        return;
    }
    CHECK_OK(s);
    if (!g_win) {
        finish();
        return;
    }
    nhal_window_size sz;
    CHECK_OK(W->size(g_win, &sz));
    CHECK(sz.width == 320 && sz.height == 200 && sz.scale >= 1.0f);
    nhal_window_native n;
    CHECK_OK(W->native(g_win, &n));
    CHECK(n.handle != NULL);
#ifdef __EMSCRIPTEN__
    CHECK(n.kind == NHAL_WINDOW_NATIVE_CANVAS);
#else
    CHECK(n.kind == NHAL_WINDOW_NATIVE_NSVIEW);
#endif
    CHECK_OK(W->set_title(g_win, "NHAL – Titel"));
    CHECK(W->size(9999, &sz).code == NHAL_ERR_NOT_FOUND);

    /* Tasten über den Weg der Plattform, mit Text. */
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_W, true, 0));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_W, false, 0));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_A, true, NHAL_MOD_SHIFT));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_A, false, NHAL_MOD_SHIFT));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_LEFT_SHIFT, true, 0));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_LEFT_SHIFT, false, 0));
    CHECK_OK(W->text_input(g_win, false));
    CHECK_OK(W->inject_key(g_win, NHAL_KEY_Z, true, 0)); /* ohne Text */
    CHECK_OK(W->text_input(g_win, true));

    /* Rohe Ereignisse: macOS keyCode bzw. KeyboardEvent.code. */
    raw_key(0x0D, "KeyW");       /* → W */
    raw_key(0x7B, "ArrowLeft");  /* → LEFT */
    raw_key(0x35, "Escape");     /* → ESCAPE */
    g_step = 1;
}

static void step_check_keys(void) {
    collect();
    const int wd = find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_W, 0);
    const int wu = find_key(NHAL_EVENT_KEY_UP, NHAL_KEY_W, 0);
    CHECK(wd >= 0 && wu > wd);
    if (wd >= 0) CHECK(g_seen[wd].window == g_win && !g_seen[wd].synthetic && g_seen[wd].key.mods == 0);
    CHECK(find_text("w") > wd);
    const int ad = find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_A, 0);
    CHECK(ad >= 0 && (g_seen[ad].key.mods & NHAL_MOD_SHIFT));
    CHECK(find_text("A") > ad);
    const int sd = find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_LEFT_SHIFT, 0);
    CHECK(sd >= 0 && find_key(NHAL_EVENT_KEY_UP, NHAL_KEY_LEFT_SHIFT, sd) > sd);
    CHECK(find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_Z, 0) >= 0);
    CHECK(find_text("z") < 0); /* text_input war aus */
    const int raw = find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_W, wu > 0 ? wu : 0);
    CHECK(raw > wu);
    CHECK(find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_LEFT, 0) >= 0);
    CHECK(find_key(NHAL_EVENT_KEY_DOWN, NHAL_KEY_ESCAPE, 0) >= 0);
    printf("window_test: %d Ereignisse aus Tasten geprüft\n", g_nseen);

    g_nseen = 0;
    CHECK_OK(W->set_size(g_win, 400, 300));
    g_step = 2;
    g_frames = 0;
}

static void step_wait_resize(void) {
    collect();
    for (int i = 0; i < g_nseen; ++i) {
        const nhal_input_event* e = &g_seen[i];
        if (e->type == NHAL_EVENT_WINDOW_RESIZE && e->window == g_win && e->resize.width == 400 &&
            e->resize.height == 300) {
            CHECK(e->resize.pixel_width >= 400 && e->resize.scale >= 1.0f);
            nhal_window_size sz;
            CHECK_OK(W->size(g_win, &sz));
            CHECK(sz.width == 400 && sz.height == 300);
            printf("window_test: RESIZE auf 400×300 angekommen\n");
            finish();
            return;
        }
    }
    if (++g_frames > 300) {
        fprintf(stderr, "window_test: kein RESIZE nach set_size\n");
        ++g_failures;
        finish();
    }
}

static void frame(void) {
    if (g_finished) return;
    switch (g_step) {
    case 0: step_setup(); break;
    case 1: step_check_keys(); break;
    default: step_wait_resize(); break;
    }
}

int main(void) {
    const nhal_request req = { NHAL_FEATURE_WINDOW_BASIC, 1u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    CHECK_OK(nhal_init(&info, &g_ctx));
    W = nhal_get_window_basic_v1(g_ctx);
    if (!W) return check_report("window_test");
    test_queue();
    frame();
#ifdef __EMSCRIPTEN__
    if (!g_finished) emscripten_set_main_loop(frame, 0, false);
#else
    while (!g_finished) {
        frame();
        usleep(2000);
    }
#endif
    return g_rc;
}
