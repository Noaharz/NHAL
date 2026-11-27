/*
 * Ereignis-Warteschlange für window.*-Backends.
 *
 * Jeder Eintrag hat Platz für einen String (Text, Pfad). next_event kopiert
 * den Eintrag nach "current" und biegt den String-Zeiger dorthin um. So gilt
 * er bis zum nächsten next_event/poll_events, wie die Schnittstelle sagt.
 */
#ifndef NHAL_BACKEND_WINDOW_QUEUE_H
#define NHAL_BACKEND_WINDOW_QUEUE_H

#include "nhal/features/window/basic.h"
#include "nhal/nhal_backend.h"

#include <string.h>

#define NHAL_EVQ_STR 1024u

typedef struct nhal_evq_entry {
    nhal_input_event ev;
    char str[NHAL_EVQ_STR];
} nhal_evq_entry;

typedef struct nhal_evq {
    const nhal_allocator* alloc;
    nhal_evq_entry* items;
    uint32_t head, count;
    uint64_t dropped;
    nhal_evq_entry current;
} nhal_evq;

static inline bool nhal_evq_has_string(nhal_event_type t) {
    return t == NHAL_EVENT_TEXT_INPUT || t == NHAL_EVENT_TEXT_EDITING || t == NHAL_EVENT_FILE_DROP;
}

static inline nhal_status nhal_evq_init(nhal_evq* q, const nhal_allocator* alloc) {
    memset(q, 0, sizeof *q);
    q->alloc = alloc;
    q->items = (nhal_evq_entry*)alloc->alloc(alloc->user, sizeof(nhal_evq_entry) * NHAL_WINDOW_QUEUE,
                                             _Alignof(nhal_evq_entry));
    if (!q->items) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Ereignis-Warteschlange");
    return nhal_status_ok();
}

static inline void nhal_evq_free(nhal_evq* q) {
    if (q->items) q->alloc->free(q->alloc->user, q->items, sizeof(nhal_evq_entry) * NHAL_WINDOW_QUEUE);
    q->items = NULL;
    q->count = 0;
}

/* Kopiert ev (und den String, falls die Art einen hat). false = voll oder String zu lang. */
static inline bool nhal_evq_push(nhal_evq* q, const nhal_input_event* ev) {
    const char* s = NULL;
    if (ev->type == NHAL_EVENT_FILE_DROP) s = ev->drop.path;
    else if (nhal_evq_has_string(ev->type)) s = ev->text.text;
    const size_t len = s ? strlen(s) : 0;
    if (!q->items || q->count == NHAL_WINDOW_QUEUE || len >= NHAL_EVQ_STR) {
        ++q->dropped;
        return false;
    }
    nhal_evq_entry* e = &q->items[(q->head + q->count) % NHAL_WINDOW_QUEUE];
    e->ev = *ev;
    if (s) memcpy(e->str, s, len + 1);
    else e->str[0] = '\0';
    ++q->count;
    return true;
}

/* Text an UTF-8-Grenzen in Stücke teilen, die in einen Eintrag passen. */
static inline void nhal_evq_push_text(nhal_evq* q, nhal_input_event ev, const char* text) {
    size_t len = strlen(text);
    char chunk[NHAL_EVQ_STR];
    while (len > 0) {
        size_t n = len < NHAL_EVQ_STR - 1u ? len : NHAL_EVQ_STR - 1u;
        while (n < len && n > 0 && ((unsigned char)text[n] & 0xC0u) == 0x80u) --n; /* nicht mitten im Zeichen */
        memcpy(chunk, text, n);
        chunk[n] = '\0';
        ev.text.text = chunk;
        nhal_evq_push(q, &ev);
        text += n;
        len -= n;
    }
}

static inline bool nhal_evq_pop(nhal_evq* q, nhal_input_event* out) {
    if (!q->count || !out) return false;
    q->current = q->items[q->head];
    q->head = (q->head + 1u) % NHAL_WINDOW_QUEUE;
    --q->count;
    *out = q->current.ev;
    if (out->type == NHAL_EVENT_FILE_DROP) out->drop.path = q->current.str;
    else if (nhal_evq_has_string(out->type)) out->text.text = q->current.str;
    return true;
}

/* Grundprüfung für push_event. */
static inline nhal_status nhal_evq_check_synthetic(const nhal_input_event* ev) {
    if (!ev) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ereignis ist NULL");
    if (ev->type <= NHAL_EVENT_NONE || ev->type >= NHAL_EVENT_TYPE_COUNT)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Ereignisart");
    if ((ev->type == NHAL_EVENT_KEY_DOWN || ev->type == NHAL_EVENT_KEY_UP) &&
        (ev->key.key <= NHAL_KEY_UNKNOWN || ev->key.key >= NHAL_KEY_COUNT))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Taste");
    if ((ev->type == NHAL_EVENT_MOUSE_DOWN || ev->type == NHAL_EVENT_MOUSE_UP) &&
        (unsigned)ev->mouse.button >= NHAL_MOUSE_BUTTON_COUNT)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Maustaste");
    if (ev->type == NHAL_EVENT_FILE_DROP && !ev->drop.path)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "FILE_DROP ohne Pfad");
    if ((ev->type == NHAL_EVENT_TEXT_INPUT || ev->type == NHAL_EVENT_TEXT_EDITING) && !ev->text.text)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Text-Ereignis ohne Text");
    return nhal_status_ok();
}

#endif /* NHAL_BACKEND_WINDOW_QUEUE_H */
