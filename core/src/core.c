/*
 * NHAL – Kern. Freestanding: keine libc, nur der Allocator des Aufrufers
 * (bzw. der Standard-Allocator, wo es eine libc gibt).
 */
#include "nhal/nhal_backend.h"

#if NHAL_HAS_DEFAULT_ALLOCATOR
extern const nhal_allocator nhal_default_allocator;
#endif

#define NPOS ((size_t)-1)

enum { WANT_OPTIONAL = 1u, WANT_REQUIRED = 2u };

typedef struct nhal_slot {
    nhal_status status;
    uint8_t wanted;
    uint8_t active;
} nhal_slot;

struct nhal_context {
    nhal_allocator allocator;
    size_t bytes;
    nhal_slot* slots; /* je Registry-Eintrag einer, liegen direkt hinter dem Kontext */
};

/* Platz für den Kontext, aufgerundet auf die Ausrichtung der Slots. */
#define CONTEXT_HEAD \
    ((sizeof(nhal_context) + _Alignof(nhal_slot) - 1) / _Alignof(nhal_slot) * _Alignof(nhal_slot))

static bool g_context_alive = false;

static nhal_status make_status(nhal_code code, nhal_feature_id feature, const char* reason) {
    nhal_status s = { code, feature, reason };
    return s;
}

static size_t registry_index(nhal_feature_id id) {
    for (size_t i = 0; i < nhal_registry_count; ++i) {
        if (nhal_registry[i].id == id) return i;
    }
    return NPOS;
}

static const nhal_catalog_entry* catalog_entry(nhal_feature_id id) {
    for (size_t i = 0; i < nhal_catalog_count; ++i) {
        if (nhal_catalog[i].id == id) return &nhal_catalog[i];
    }
    return NULL;
}

const char* nhal_code_name(nhal_code code) {
    switch (code) {
        case NHAL_OK: return "ok";
        case NHAL_ERR_INVALID_ARGUMENT: return "invalid_argument";
        case NHAL_ERR_UNSUPPORTED: return "unsupported";
        case NHAL_ERR_UNAVAILABLE: return "unavailable";
        case NHAL_ERR_OUT_OF_MEMORY: return "out_of_memory";
        case NHAL_ERR_IO: return "io";
        case NHAL_ERR_NOT_FOUND: return "not_found";
        case NHAL_ERR_BUSY: return "busy";
    }
    return "unknown";
}

const char* nhal_feature_name(nhal_feature_id feature) {
    const nhal_catalog_entry* e = catalog_entry(feature);
    return e ? e->name : "unbekannt";
}

const char* nhal_platform_name(void) { return NHAL_PLATFORM_NAME; }

const nhal_manifest* nhal_build_manifest(void) { return &nhal_build_manifest_data; }

size_t nhal_feature_count(void) { return nhal_registry_count; }

/* Aktive Features in umgekehrter Start-Reihenfolge beenden und Speicher freigeben. */
static void teardown(nhal_context* ctx) {
    for (size_t i = nhal_registry_count; i-- > 0;) {
        if (ctx->slots[i].active && nhal_registry[i].impl->shutdown) {
            nhal_registry[i].impl->shutdown();
        }
        ctx->slots[i].active = 0;
    }
    nhal_allocator a = ctx->allocator;
    a.free(a.user, ctx, ctx->bytes);
    g_context_alive = false;
}

static nhal_status fail(nhal_context* ctx, nhal_status status) {
    teardown(ctx);
    return status;
}

nhal_status nhal_init(const nhal_init_info* info, nhal_context** out) {
    if (!out) return make_status(NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE, "out ist NULL");
    *out = NULL;
    if (info && info->struct_size < sizeof(nhal_init_info)) {
        return make_status(NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE,
                           "nhal_init_info.struct_size ist zu klein");
    }
    if (g_context_alive) {
        return make_status(NHAL_ERR_BUSY, NHAL_FEATURE_NONE, "Es ist bereits ein NHAL-Kontext aktiv");
    }

    nhal_allocator allocator;
    if (info && info->allocator) {
        allocator = *info->allocator;
    } else {
#if NHAL_HAS_DEFAULT_ALLOCATOR
        allocator = nhal_default_allocator;
#else
        return make_status(NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE,
                           "Plattform ohne Standard-Allocator: nhal_init_info.allocator ist Pflicht");
#endif
    }
    if (!allocator.alloc || !allocator.free) {
        return make_status(NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE, "Allocator ist unvollständig");
    }

    const nhal_manifest* manifest = (info && info->manifest) ? info->manifest : &nhal_build_manifest_data;
    if (manifest->count > 0 && !manifest->requests) {
        return make_status(NHAL_ERR_INVALID_ARGUMENT, NHAL_FEATURE_NONE, "Manifest ohne Einträge");
    }

    const size_t bytes = CONTEXT_HEAD + nhal_registry_count * sizeof(nhal_slot);
    nhal_context* ctx = (nhal_context*)allocator.alloc(allocator.user, bytes, _Alignof(nhal_context));
    if (!ctx) {
        return make_status(NHAL_ERR_OUT_OF_MEMORY, NHAL_FEATURE_NONE, "Kein Speicher für den Kontext");
    }
    g_context_alive = true;
    ctx->allocator = allocator;
    ctx->bytes = bytes;
    ctx->slots = (nhal_slot*)((unsigned char*)ctx + CONTEXT_HEAD);
    for (size_t i = 0; i < nhal_registry_count; ++i) {
        ctx->slots[i].status = make_status(NHAL_ERR_UNAVAILABLE, nhal_registry[i].id,
                                           "Nicht im Manifest angefordert");
        ctx->slots[i].wanted = 0;
        ctx->slots[i].active = 0;
    }

    /* 1. Anforderungen eintragen. */
    for (size_t r = 0; r < manifest->count; ++r) {
        const nhal_request* req = &manifest->requests[r];
        const bool required = req->need == NHAL_REQUIRED;
        const size_t i = registry_index(req->feature);
        if (i == NPOS) {
            if (required) {
                return fail(ctx, make_status(NHAL_ERR_UNSUPPORTED, req->feature,
                                             "Feature ist in diesem Build nicht enthalten "
                                             "(Manifest und Plattform-Matrix prüfen)"));
            }
            continue;
        }
        if (nhal_registry[i].version != req->version) {
            nhal_status s = make_status(NHAL_ERR_UNSUPPORTED, req->feature,
                                        "Build enthält eine andere Version dieses Features");
            if (required) return fail(ctx, s);
            ctx->slots[i].status = s;
            continue;
        }
        ctx->slots[i].wanted |= required ? WANT_REQUIRED : WANT_OPTIONAL;
    }

    /* 2. Abhängigkeiten erben den Bedarf (sie stehen immer vor ihren Nutzern). */
    for (size_t i = nhal_registry_count; i-- > 0;) {
        if (!ctx->slots[i].wanted) continue;
        for (uint16_t d = 0; d < nhal_registry[i].dep_count; ++d) {
            ctx->slots[nhal_registry[i].deps[d]].wanted |= ctx->slots[i].wanted;
        }
    }

    /* 3. Starten, in Abhängigkeits-Reihenfolge. */
    for (size_t i = 0; i < nhal_registry_count; ++i) {
        nhal_slot* slot = &ctx->slots[i];
        const nhal_registry_entry* e = &nhal_registry[i];
        if (!slot->wanted) continue;

        bool deps_ok = true;
        for (uint16_t d = 0; d < e->dep_count; ++d) {
            if (!ctx->slots[e->deps[d]].active) deps_ok = false;
        }
        if (!deps_ok) {
            slot->status = make_status(NHAL_ERR_UNAVAILABLE, e->id, "Eine Abhängigkeit ist nicht aktiv");
            continue;
        }
        if (!e->impl || e->impl->version != e->version || !e->impl->table) {
            slot->status = make_status(NHAL_ERR_UNSUPPORTED, e->id,
                                       "Backend passt nicht zur Katalog-Version");
            continue;
        }
        nhal_status s = e->impl->init ? e->impl->init(&ctx->allocator) : nhal_status_ok();
        if (s.feature == NHAL_FEATURE_NONE) s.feature = e->id;
        if (!s.reason) s.reason = "ohne Angabe";
        if (s.code == NHAL_OK) {
            slot->active = 1;
            s.reason = "aktiv";
        }
        slot->status = s;
    }

    /* 4. Pflicht-Features müssen laufen. */
    for (size_t i = 0; i < nhal_registry_count; ++i) {
        if ((ctx->slots[i].wanted & WANT_REQUIRED) && !ctx->slots[i].active) {
            return fail(ctx, ctx->slots[i].status);
        }
    }

    *out = ctx;
    return nhal_status_ok();
}

void nhal_shutdown(nhal_context* ctx) {
    if (ctx) teardown(ctx);
}

const void* nhal_get(const nhal_context* ctx, nhal_feature_id feature, uint32_t version) {
    if (!ctx) return NULL;
    const size_t i = registry_index(feature);
    if (i == NPOS || !ctx->slots[i].active || nhal_registry[i].version != version) return NULL;
    return nhal_registry[i].impl->table;
}

static void fill_info(const nhal_context* ctx, size_t i, nhal_feature_info* info) {
    const nhal_registry_entry* e = &nhal_registry[i];
    info->id = e->id;
    info->name = e->name;
    info->version = e->version;
    info->support = e->support;
    if (ctx) {
        info->active = ctx->slots[i].active != 0;
        info->status = ctx->slots[i].status;
    } else {
        info->active = false;
        info->status = make_status(NHAL_ERR_UNAVAILABLE, e->id, "Kein Kontext");
    }
}

bool nhal_query(const nhal_context* ctx, nhal_feature_id feature, nhal_feature_info* info) {
    if (!info) return false;
    const size_t i = registry_index(feature);
    if (i != NPOS) {
        fill_info(ctx, i, info);
        return info->active;
    }
    const nhal_catalog_entry* c = catalog_entry(feature);
    info->id = feature;
    info->name = c ? c->name : "unbekannt";
    info->version = 0;
    info->support = NHAL_SUPPORT_NONE;
    info->active = false;
    info->status = make_status(NHAL_ERR_UNSUPPORTED, feature,
                               c ? "Nicht in diesem Build (Manifest oder Plattform)"
                                 : "Unbekannte Feature-ID");
    return false;
}

bool nhal_feature_at(const nhal_context* ctx, size_t index, nhal_feature_info* info) {
    if (!info || index >= nhal_registry_count) return false;
    fill_info(ctx, index, info);
    return info->active;
}
