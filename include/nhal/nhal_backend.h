/*
 * NHAL – Schnittstelle für Backends.
 *
 * Nur für Code, der ein Feature implementiert, nicht für Apps.
 *
 * Jede Implementierung definiert genau ein Symbol
 *
 *     const nhal_feature_impl nhal_impl_<gruppe>_<name>;
 *
 * (Punkte im Feature-Namen werden zu Unterstrichen). Welche Quelldatei das
 * Symbol liefert, steht im Katalog unter [platforms.<id>].sources. Die
 * Registry erzeugt tools/nhalgen.
 */
#ifndef NHAL_BACKEND_H
#define NHAL_BACKEND_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_feature_impl {
    uint32_t version; /* muss zur Katalog-Version passen */
    /* Wird beim Start gerufen, nachdem alle Abhängigkeiten laufen. Hier
     * prüft das Backend auch, ob das Gerät das Feature wirklich kann.
     * NULL = nichts zu tun. */
    nhal_status (*init)(const nhal_allocator* allocator);
    void (*shutdown)(void); /* NULL = nichts zu tun */
    const void* table;      /* Funktionstabelle, z. B. nhal_time_monotonic_v1 */
} nhal_feature_impl;

/* Von nhalgen erzeugt. */
typedef struct nhal_registry_entry {
    nhal_feature_id id;
    const char* name;
    uint32_t version;
    nhal_support support;
    const nhal_feature_impl* impl;
    const uint16_t* deps; /* Indizes in nhal_registry, stehen immer davor */
    uint16_t dep_count;
} nhal_registry_entry;

typedef struct nhal_catalog_entry {
    nhal_feature_id id;
    const char* name;
    uint32_t version;
} nhal_catalog_entry;

extern const nhal_registry_entry nhal_registry[];
extern const size_t nhal_registry_count;
extern const nhal_catalog_entry nhal_catalog[];
extern const size_t nhal_catalog_count;
extern const nhal_manifest nhal_build_manifest_data;

static inline nhal_status nhal_status_ok(void) {
    nhal_status s = { NHAL_OK, NHAL_FEATURE_NONE, "ok" };
    return s;
}

static inline nhal_status nhal_status_error(nhal_code code, const char* reason) {
    nhal_status s = { code, NHAL_FEATURE_NONE, reason ? reason : "ohne Angabe" };
    return s;
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_BACKEND_H */
