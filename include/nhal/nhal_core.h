/*
 * NHAL – Kern-Schnittstelle.
 *
 * Der Kern ist der einzige Pflichtteil von NHAL. Er kennt keine Hardware,
 * sondern nur:
 *   - Statuscodes mit Klartext-Grund,
 *   - das Manifest (welche Features eine App braucht),
 *   - die Fähigkeiten-Abfrage (was ist in diesem Build und auf diesem Gerät aktiv),
 *   - den Zugriff auf die Funktionstabellen der Features.
 *
 * Reines C11, nutzt nur freestanding-Header (stddef, stdint, stdbool) und
 * läuft damit auch ohne libc (z. B. auf Xernel, solange es dort keine gibt).
 *
 * Grundsatz: Kann ein Backend etwas nicht, antwortet NHAL mit einem Fehler
 * und nennt den Grund. Es gibt keinen stillen Ersatz.
 *
 * Hot Path: nhal_get() sucht linear und ist für den Start gedacht. Die App
 * holt jede Tabelle einmal und ruft danach direkt über die Funktionszeiger.
 */
#ifndef NHAL_CORE_H
#define NHAL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nhal/nhal_features.h" /* generiert: Feature-IDs, NHAL_HAS_* */

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_VERSION_MAJOR 0
#define NHAL_VERSION_MINOR 1
#define NHAL_VERSION_PATCH 0

/* ── Status ─────────────────────────────────────────────────────────────── */

typedef enum nhal_code {
    NHAL_OK = 0,
    NHAL_ERR_INVALID_ARGUMENT, /* Aufruf ergibt so keinen Sinn */
    NHAL_ERR_UNSUPPORTED,      /* Build oder Plattform kann das nicht */
    NHAL_ERR_UNAVAILABLE,      /* gerade nicht möglich (Gerät fehlt, Abhängigkeit inaktiv) */
    NHAL_ERR_OUT_OF_MEMORY,
    NHAL_ERR_IO,
    NHAL_ERR_NOT_FOUND,
    NHAL_ERR_BUSY              /* z. B. schon ein Kontext aktiv */
} nhal_code;

typedef struct nhal_status {
    nhal_code code;
    nhal_feature_id feature; /* betroffenes Feature, sonst NHAL_FEATURE_NONE */
    const char* reason;      /* statischer Klartext, nie NULL */
} nhal_status;

static inline bool nhal_ok(nhal_status s) { return s.code == NHAL_OK; }

const char* nhal_code_name(nhal_code code);

/* ── Allocator ──────────────────────────────────────────────────────────── */

/* Speicher für den Kontext und für Features. In Freestanding-Builds Pflicht,
 * sonst gibt es einen Standard-Allocator. align ist eine Zweierpotenz. */
typedef struct nhal_allocator {
    void* (*alloc)(void* user, size_t size, size_t align);
    void (*free)(void* user, void* ptr, size_t size);
    void* user;
} nhal_allocator;

/* ── Manifest ───────────────────────────────────────────────────────────── */

typedef enum nhal_need {
    NHAL_REQUIRED = 0, /* fehlt es, schlägt nhal_init fehl */
    NHAL_OPTIONAL = 1  /* fehlt es, läuft die App ohne; Grund per nhal_query */
} nhal_need;

typedef struct nhal_request {
    nhal_feature_id feature;
    uint32_t version; /* Version der Funktionstabelle, muss exakt passen */
    nhal_need need;
} nhal_request;

typedef struct nhal_manifest {
    const nhal_request* requests;
    size_t count;
} nhal_manifest;

/* Das Manifest, mit dem dieser Build erzeugt wurde (aus nhal.toml). */
const nhal_manifest* nhal_build_manifest(void);

/* ── Kontext ────────────────────────────────────────────────────────────── */

typedef struct nhal_context nhal_context;

typedef struct nhal_init_info {
    uint32_t struct_size;            /* sizeof(nhal_init_info) */
    const nhal_manifest* manifest;   /* NULL = nhal_build_manifest() */
    const nhal_allocator* allocator; /* NULL = Standard (nicht in Freestanding-Builds) */
} nhal_init_info;

/* Startet NHAL. info darf NULL sein (alles Standard). Initialisiert werden
 * nur die angeforderten Features und ihre Abhängigkeiten. Fehlt ein
 * Pflicht-Feature, ist *out danach NULL und der Status nennt Feature und Grund.
 * Es darf nur ein Kontext zur Zeit leben; nhal_init und nhal_shutdown nicht
 * gleichzeitig aus mehreren Threads aufrufen. */
nhal_status nhal_init(const nhal_init_info* info, nhal_context** out);
void nhal_shutdown(nhal_context* ctx);

/* Funktionstabelle eines aktiven Features, sonst NULL. Einmal holen, dann
 * behalten. Die Feature-Header bieten typisierte Varianten
 * (z. B. nhal_get_time_monotonic_v1). */
const void* nhal_get(const nhal_context* ctx, nhal_feature_id feature, uint32_t version);

/* ── Fähigkeiten ────────────────────────────────────────────────────────── */

typedef enum nhal_support {
    NHAL_SUPPORT_NONE = 0,     /* nicht in diesem Build */
    NHAL_SUPPORT_EMULATED = 1, /* Ersatz mit dokumentierten Unterschieden */
    NHAL_SUPPORT_NATIVE = 2
} nhal_support;

typedef struct nhal_feature_info {
    nhal_feature_id id;
    const char* name;     /* z. B. "time.monotonic" */
    uint32_t version;     /* 0, wenn nicht im Build */
    nhal_support support; /* so ist es in diesem Build gebaut */
    bool active;          /* läuft im Kontext */
    nhal_status status;   /* Grund, wenn nicht aktiv */
} nhal_feature_info;

/* Zustand eines beliebigen Katalog-Features. Gibt info->active zurück. */
bool nhal_query(const nhal_context* ctx, nhal_feature_id feature, nhal_feature_info* info);

/* Alle Features dieses Builds durchgehen (0 <= index < nhal_feature_count()). */
size_t nhal_feature_count(void);
bool nhal_feature_at(const nhal_context* ctx, size_t index, nhal_feature_info* info);

/* Name eines Katalog-Features, "unbekannt" für fremde IDs. */
const char* nhal_feature_name(nhal_feature_id feature);

/* Plattform dieses Builds, z. B. "macos" oder "xernel". */
const char* nhal_platform_name(void);

#ifdef __cplusplus
}
#endif

#endif /* NHAL_CORE_H */
