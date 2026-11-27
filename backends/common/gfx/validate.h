/*
 * Gemeinsame Prüfungen für gfx.*-Backends.
 *
 * Damit gelten auf allen Plattformen dieselben Regeln (Ausrichtung, Usage-
 * Flags, Bindungen), auch wo die native API großzügiger wäre. Eine App, die
 * auf macOS läuft, scheitert so nicht erst im Browser.
 */
#ifndef NHAL_BACKEND_GFX_VALIDATE_H
#define NHAL_BACKEND_GFX_VALIDATE_H

#include "nhal/features/gfx/device.h"
#include "nhal/nhal_backend.h"

#include <string.h>

#define NHAL_GFX_UNIFORM_ALIGN 256u
#define NHAL_GFX_STORAGE_ALIGN 256u
#define NHAL_GFX_ROW_ALIGN 256u

/* Alle Stufen, die eine Bindung sehen darf (Mesh-Stufen kommen aus gfx.mesh_shader). */
#define NHAL_GFX_STAGE_ALL_ (NHAL_GFX_STAGE_VERTEX | NHAL_GFX_STAGE_FRAGMENT | NHAL_GFX_STAGE_COMPUTE | \
                             NHAL_GFX_STAGE_OBJECT | NHAL_GFX_STAGE_MESH)

/* Was in einem Pass auf einem Slot liegt. */
typedef enum nhal_gfx_slot_kind {
    NHAL_GFX_SLOT_EMPTY = 0,
    NHAL_GFX_SLOT_BUFFER,
    NHAL_GFX_SLOT_TEXTURE,
    NHAL_GFX_SLOT_SAMPLER
} nhal_gfx_slot_kind;

typedef struct nhal_gfx_slot {
    nhal_gfx_slot_kind kind;
    uint32_t usage;           /* Puffer- bzw. Textur-Usage */
    nhal_gfx_format format;   /* Texturen */
    uint64_t offset;          /* Puffer */
} nhal_gfx_slot;

/* Kopie der Bindungsbeschreibung, die jede Pipeline mitführt. */
typedef struct nhal_gfx_layout {
    uint32_t count;
    nhal_gfx_binding_desc bindings[NHAL_GFX_MAX_BINDINGS];
} nhal_gfx_layout;

static inline uint32_t nhal_gfx_format_bytes(nhal_gfx_format f) {
    switch (f) {
    case NHAL_GFX_FORMAT_R8_UNORM: return 1;
    case NHAL_GFX_FORMAT_RGBA8_UNORM:
    case NHAL_GFX_FORMAT_RGBA8_SRGB:
    case NHAL_GFX_FORMAT_BGRA8_UNORM:
    case NHAL_GFX_FORMAT_BGRA8_SRGB:
    case NHAL_GFX_FORMAT_R32_FLOAT:
    case NHAL_GFX_FORMAT_DEPTH32_FLOAT: return 4;
    case NHAL_GFX_FORMAT_RG32_FLOAT:
    case NHAL_GFX_FORMAT_RGBA16_FLOAT: return 8;
    case NHAL_GFX_FORMAT_RGBA32_FLOAT: return 16;
    default: return 0;
    }
}

static inline bool nhal_gfx_is_depth(nhal_gfx_format f) { return f == NHAL_GFX_FORMAT_DEPTH32_FLOAT; }
static inline bool nhal_gfx_is_color(nhal_gfx_format f) { return nhal_gfx_format_bytes(f) && !nhal_gfx_is_depth(f); }

static inline nhal_status nhal_gfx_check_buffer_desc(const nhal_gfx_buffer_desc* d) {
    if (!d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pufferbeschreibung fehlt");
    if (d->size == 0 || d->size % 4u)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffergröße muss ein Vielfaches von 4 und > 0 sein");
    if (!d->usage || (d->usage & ~0x1FFu)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Puffer-Usage");
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_range(uint64_t buffer_size, uint64_t offset, uint64_t size) {
    if (offset % 4u || size % 4u)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Offset und Größe müssen Vielfache von 4 sein");
    if (size == 0 || offset > buffer_size || size > buffer_size - offset)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bereich liegt außerhalb des Puffers");
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_texture_desc(const nhal_gfx_texture_desc* d, uint32_t max_size) {
    if (!d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Texturbeschreibung fehlt");
    if (!nhal_gfx_format_bytes(d->format)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültiges Texturformat");
    if (!d->width || !d->height || d->width > max_size || d->height > max_size)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Texturgröße 0 oder über info().max_texture_size");
    if (!d->usage || (d->usage & ~0x1Fu)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Textur-Usage");
    if (nhal_gfx_is_depth(d->format) && (d->usage & (NHAL_GFX_TEXTURE_STORAGE | NHAL_GFX_TEXTURE_COPY_DST)))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Tiefentexturen gehen nicht als Storage oder Kopierziel");
    if ((d->usage & NHAL_GFX_TEXTURE_STORAGE) && d->format != NHAL_GFX_FORMAT_RGBA8_UNORM &&
        d->format != NHAL_GFX_FORMAT_R32_FLOAT && d->format != NHAL_GFX_FORMAT_RGBA16_FLOAT &&
        d->format != NHAL_GFX_FORMAT_RGBA32_FLOAT && d->format != NHAL_GFX_FORMAT_RG32_FLOAT)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Format geht nicht als Storage-Textur");
    if (d->sample_count > 1u) {
        if (d->sample_count != 4u) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "sample_count: 1 oder 4");
        if (d->usage != NHAL_GFX_TEXTURE_RENDER_TARGET)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "MSAA-Texturen sind nur RENDER_TARGET (Ergebnis über resolve)");
    }
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_copy_layout(const nhal_gfx_binding_desc* b, uint32_t n, nhal_gfx_layout* out) {
    if (n > NHAL_GFX_MAX_BINDINGS) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zu viele Bindungen");
    if (n && !b) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "bindings ist NULL");
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (b[i].slot >= NHAL_GFX_MAX_BINDINGS)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungs-Slot über NHAL_GFX_MAX_BINDINGS");
        if (used & (1u << b[i].slot)) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindungs-Slot doppelt");
        used |= 1u << b[i].slot;
        if (b[i].kind < NHAL_GFX_BIND_UNIFORM_BUFFER || b[i].kind > NHAL_GFX_BIND_SAMPLER)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Bindungsart");
        if (!b[i].stages || (b[i].stages & ~(uint32_t)NHAL_GFX_STAGE_ALL_))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Bindung ohne gültige Shader-Stufe");
        if (b[i].kind == NHAL_GFX_BIND_STORAGE_TEXTURE && !nhal_gfx_is_color(b[i].storage_format))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Storage-Textur ohne storage_format");
        if ((b[i].stages & NHAL_GFX_STAGE_VERTEX) &&
            (b[i].kind == NHAL_GFX_BIND_STORAGE_BUFFER || b[i].kind == NHAL_GFX_BIND_STORAGE_TEXTURE))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                     "Vertex-Shader dürfen nicht schreiben (nur READ_STORAGE_BUFFER)");
    }
    out->count = n;
    if (n) memcpy(out->bindings, b, n * sizeof *b);
    for (uint32_t i = 1; i < n; ++i) { /* nach Slot sortiert, damit gleiche Layouts gleich aussehen */
        const nhal_gfx_binding_desc x = out->bindings[i];
        uint32_t j = i;
        for (; j > 0 && out->bindings[j - 1].slot > x.slot; --j) out->bindings[j] = out->bindings[j - 1];
        out->bindings[j] = x;
    }
    return nhal_status_ok();
}

static inline bool nhal_gfx_layout_equal(const nhal_gfx_layout* a, const nhal_gfx_layout* b) {
    if (a->count != b->count) return false;
    for (uint32_t i = 0; i < a->count; ++i) {
        const nhal_gfx_binding_desc* x = &a->bindings[i];
        const nhal_gfx_binding_desc* y = &b->bindings[i];
        if (x->slot != y->slot || x->kind != y->kind || x->stages != y->stages ||
            (x->kind == NHAL_GFX_BIND_STORAGE_TEXTURE && x->storage_format != y->storage_format))
            return false;
    }
    return true;
}

static inline nhal_status nhal_gfx_check_vertex_layouts(const nhal_gfx_vertex_buffer_layout* v, uint32_t n) {
    if (n > NHAL_GFX_MAX_VERTEX_BUFFERS) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zu viele Vertex-Puffer");
    if (n && !v) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "vertex_buffers ist NULL");
    uint32_t locations = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!v[i].stride || v[i].stride % 4u || v[i].stride > 2048u)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertex-Stride muss 4..2048 und durch 4 teilbar sein");
        if (v[i].attribute_count > NHAL_GFX_MAX_VERTEX_ATTRIBUTES)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zu viele Vertex-Attribute");
        for (uint32_t a = 0; a < v[i].attribute_count; ++a) {
            const nhal_gfx_vertex_attribute* at = &v[i].attributes[a];
            if (at->format < NHAL_GFX_VERTEX_FLOAT32 || at->format > NHAL_GFX_VERTEX_UINT32)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültiges Vertex-Format");
            if (at->location >= 16u || (locations & (1u << at->location)))
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertex-Location ungültig oder doppelt");
            locations |= 1u << at->location;
            if (at->offset % 4u || at->offset >= v[i].stride)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Vertex-Offset ungültig");
        }
    }
    return nhal_status_ok();
}

/* Form der Ziele eines Render-Passes; jede Render-Pipeline führt dieselbe mit. */
typedef struct nhal_gfx_pass_shape {
    uint32_t color_count;
    nhal_gfx_format colors[NHAL_GFX_MAX_COLOR_TARGETS];
    nhal_gfx_format depth;
    uint32_t samples; /* 1 oder 4 */
} nhal_gfx_pass_shape;

static inline bool nhal_gfx_shape_equal(const nhal_gfx_pass_shape* a, const nhal_gfx_pass_shape* b) {
    if (a->color_count != b->color_count || a->depth != b->depth || a->samples != b->samples) return false;
    for (uint32_t i = 0; i < a->color_count; ++i) {
        if (a->colors[i] != b->colors[i]) return false;
    }
    return true;
}

static inline uint32_t nhal_gfx_samples(uint32_t sample_count) { return sample_count > 1u ? sample_count : 1u; }

/* Farbziele, Tiefe und Pipeline-Zustand einer Render- oder Mesh-Pipeline. */
static inline nhal_status nhal_gfx_check_targets(uint32_t color_count, const nhal_gfx_color_target* colors,
                                                 nhal_gfx_format depth, const char* fragment, bool depth_test,
                                                 nhal_gfx_compare compare, uint32_t sample_count,
                                                 nhal_gfx_pass_shape* out) {
    if (color_count > NHAL_GFX_MAX_COLOR_TARGETS)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Mehr als NHAL_GFX_MAX_COLOR_TARGETS Farbziele");
    if (color_count && !fragment) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Farbziele ohne fragment_entry");
    for (uint32_t i = 0; i < color_count; ++i) {
        if (!nhal_gfx_is_color(colors[i].format))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Farbziel ohne gültiges Farbformat");
        if (colors[i].blend > NHAL_GFX_BLEND_MULTIPLY)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Blend-Art");
        if (colors[i].write_mask > NHAL_GFX_WRITE_ALL)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Schreibmaske");
    }
    if (depth != NHAL_GFX_FORMAT_NONE && !nhal_gfx_is_depth(depth))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "depth_format muss NONE oder DEPTH32_FLOAT sein");
    if (!color_count && depth == NHAL_GFX_FORMAT_NONE)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline ohne Farb- und Tiefenziel");
    if (depth_test && compare > NHAL_GFX_COMPARE_NEVER)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültiger Tiefenvergleich");
    if (sample_count > 1u && sample_count != 4u) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "sample_count: 1 oder 4");
    memset(out, 0, sizeof *out);
    out->color_count = color_count;
    for (uint32_t i = 0; i < color_count; ++i) out->colors[i] = colors[i].format;
    out->depth = depth;
    out->samples = nhal_gfx_samples(sample_count);
    return nhal_status_ok();
}

/* Alle Bindungen einer Pipeline: Slot-Bindungen (Gruppe 0) und Gruppen-Layouts. */
typedef struct nhal_gfx_pipeline_layout {
    nhal_gfx_layout slots;
    nhal_gfx_layout groups[NHAL_GFX_MAX_GROUPS];
    uint32_t group_mask; /* Bit g: groups[g] wird benutzt */
    uint32_t constants_size;
} nhal_gfx_pipeline_layout;

/* groups[g] zeigt auf das Layout der Gruppe g oder ist NULL. */
static inline nhal_status nhal_gfx_make_pipeline_layout(const nhal_gfx_binding_desc* bindings, uint32_t binding_count,
                                                        const nhal_gfx_layout* const groups[NHAL_GFX_MAX_GROUPS],
                                                        uint32_t constants_size, uint32_t allowed_stages,
                                                        const char* stage_error, nhal_gfx_pipeline_layout* out) {
    memset(out, 0, sizeof *out);
    nhal_status s = nhal_gfx_copy_layout(bindings, binding_count, &out->slots);
    if (s.code) return s;
    if (binding_count && groups[0])
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Slot-Bindungen belegen Gruppe 0 – groups[0] muss NULL sein");
    if (constants_size % 4u || constants_size > NHAL_GFX_MAX_CONSTANTS)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "constants_size: Vielfaches von 4, höchstens 128");
    out->constants_size = constants_size;
    uint32_t used = 0;
    for (uint32_t g = 0; g <= NHAL_GFX_MAX_GROUPS; ++g) {
        const nhal_gfx_layout* l = g == NHAL_GFX_MAX_GROUPS ? &out->slots : groups[g];
        if (!l) continue;
        if (g < NHAL_GFX_MAX_GROUPS) {
            out->groups[g] = *l;
            out->group_mask |= 1u << g;
        }
        for (uint32_t i = 0; i < l->count; ++i) {
            const uint32_t bit = 1u << l->bindings[i].slot;
            if (used & bit)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Slot kommt in der Pipeline doppelt vor (über alle Gruppen)");
            used |= bit;
            if (l->bindings[i].stages & ~allowed_stages) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, stage_error);
        }
    }
    return nhal_status_ok();
}
/* Vor draw/dispatch: Passt das Gebundene zur Pipeline? */
static inline nhal_status nhal_gfx_check_bound(const nhal_gfx_layout* l, const nhal_gfx_slot* slots) {
    for (uint32_t i = 0; i < l->count; ++i) {
        const nhal_gfx_binding_desc* b = &l->bindings[i];
        const nhal_gfx_slot* s = &slots[b->slot];
        switch (b->kind) {
        case NHAL_GFX_BIND_UNIFORM_BUFFER:
            if (s->kind != NHAL_GFX_SLOT_BUFFER || !(s->usage & NHAL_GFX_BUFFER_UNIFORM))
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Uniform-Slot ohne Puffer mit Usage UNIFORM");
            if (s->offset % NHAL_GFX_UNIFORM_ALIGN)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Uniform-Offset muss ein Vielfaches von 256 sein");
            break;
        case NHAL_GFX_BIND_STORAGE_BUFFER:
        case NHAL_GFX_BIND_READ_STORAGE_BUFFER:
            if (s->kind != NHAL_GFX_SLOT_BUFFER || !(s->usage & NHAL_GFX_BUFFER_STORAGE))
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Storage-Slot ohne Puffer mit Usage STORAGE");
            if (s->offset % NHAL_GFX_STORAGE_ALIGN)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Storage-Offset muss ein Vielfaches von 256 sein");
            break;
        case NHAL_GFX_BIND_SAMPLED_TEXTURE:
            if (s->kind != NHAL_GFX_SLOT_TEXTURE || !(s->usage & NHAL_GFX_TEXTURE_SAMPLED) || nhal_gfx_is_depth(s->format))
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Textur-Slot ohne Farbtextur mit Usage SAMPLED");
            break;
        case NHAL_GFX_BIND_STORAGE_TEXTURE:
            if (s->kind != NHAL_GFX_SLOT_TEXTURE || !(s->usage & NHAL_GFX_TEXTURE_STORAGE) ||
                s->format != b->storage_format)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                         "Storage-Textur-Slot ohne Textur mit Usage STORAGE im passenden Format");
            break;
        case NHAL_GFX_BIND_SAMPLER:
            if (s->kind != NHAL_GFX_SLOT_SAMPLER)
                return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Sampler-Slot ohne Sampler");
            break;
        }
    }
    return nhal_status_ok();
}

/* Gruppe anlegen: Das Backend trägt jede Ressource in slots[] ein (Art, Usage,
 * Format, Offset); hier wird geprüft, dass Einträge und Layout zusammenpassen. */
static inline nhal_status nhal_gfx_check_group(const nhal_gfx_layout* l, const nhal_gfx_group_entry* e, uint32_t n,
                                               const nhal_gfx_slot* slots) {
    if (n != l->count) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gruppe braucht genau einen Eintrag pro Bindung");
    if (n && !e) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "entries ist NULL");
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        bool in_layout = false;
        for (uint32_t k = 0; k < l->count; ++k) in_layout |= l->bindings[k].slot == e[i].slot;
        if (e[i].slot >= NHAL_GFX_MAX_BINDINGS || !in_layout || (seen & (1u << e[i].slot)))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gruppen-Eintrag mit fremdem oder doppeltem Slot");
        seen |= 1u << e[i].slot;
    }
    return nhal_gfx_check_bound(l, slots);
}

/* Vor draw/dispatch: Gruppen und Push-Konstanten passen zur Pipeline? */
static inline nhal_status nhal_gfx_check_groups_bound(const nhal_gfx_pipeline_layout* pl,
                                                      const nhal_gfx_layout* const bound[NHAL_GFX_MAX_GROUPS],
                                                      uint32_t constants_set) {
    for (uint32_t g = 0; g < NHAL_GFX_MAX_GROUPS; ++g) {
        if (!(pl->group_mask & (1u << g))) continue;
        if (!bound[g]) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pipeline braucht eine Gruppe, die nicht gesetzt ist");
        if (!nhal_gfx_layout_equal(bound[g], &pl->groups[g]))
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Gesetzte Gruppe hat ein anderes Layout als die Pipeline");
    }
    if (constants_set < pl->constants_size)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Push-Konstanten fehlen oder sind kürzer als constants_size");
    return nhal_status_ok();
}

/* Was ein Backend über eine Textur weiß – für die Prüfung eines Render-Passes. */
typedef struct nhal_gfx_tex_info {
    bool present;
    uint32_t width, height, samples, usage;
    nhal_gfx_format format;
} nhal_gfx_tex_info;

/* Prüft die Ziele eines Render-Passes; out bekommt Form und Größe. */
static inline nhal_status nhal_gfx_check_render_pass(const nhal_gfx_render_pass_desc* d,
                                                     const nhal_gfx_tex_info color[NHAL_GFX_MAX_COLOR_TARGETS],
                                                     const nhal_gfx_tex_info resolve[NHAL_GFX_MAX_COLOR_TARGETS],
                                                     const nhal_gfx_tex_info* depth, nhal_gfx_pass_shape* out,
                                                     uint32_t* width, uint32_t* height) {
    if (!d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Render-Pass ohne Beschreibung");
    if (d->color_count > NHAL_GFX_MAX_COLOR_TARGETS)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Mehr als NHAL_GFX_MAX_COLOR_TARGETS Farbziele");
    if (!d->color_count && !depth->present) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Render-Pass ohne Ziel");
    memset(out, 0, sizeof *out);
    uint32_t w = 0, h = 0, samples = 0;
    for (uint32_t i = 0; i <= d->color_count; ++i) {
        const nhal_gfx_tex_info* t = i < d->color_count ? &color[i] : depth;
        if (!t->present) {
            if (i < d->color_count) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Farbziel ohne Textur");
            continue;
        }
        const bool is_depth = i == d->color_count;
        if (!(t->usage & NHAL_GFX_TEXTURE_RENDER_TARGET) || nhal_gfx_is_depth(t->format) != is_depth)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziel braucht Usage RENDER_TARGET und das passende Format");
        if (!w) {
            w = t->width;
            h = t->height;
            samples = t->samples;
        } else if (t->width != w || t->height != h) {
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziele des Passes sind verschieden groß");
        } else if (t->samples != samples) {
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziele des Passes haben verschiedene sample_count");
        }
        if (!is_depth) out->colors[i] = t->format;
    }
    for (uint32_t i = 0; i < d->color_count; ++i) {
        const nhal_gfx_tex_info* r = &resolve[i];
        if (!r->present) continue;
        if (samples == 1u) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "resolve nur bei MSAA-Zielen");
        if (r->samples != 1u || !(r->usage & NHAL_GFX_TEXTURE_RENDER_TARGET) || r->format != color[i].format ||
            r->width != w || r->height != h)
            return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT,
                                     "resolve braucht eine RENDER_TARGET-Textur ohne MSAA, gleich groß, gleiches Format");
    }
    out->color_count = d->color_count;
    out->depth = depth->present ? depth->format : NHAL_GFX_FORMAT_NONE;
    out->samples = samples;
    *width = w;
    *height = h;
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_query_set_desc(const nhal_gfx_query_set_desc* d, bool timestamps) {
    if (!d) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Beschreibung fehlt");
    if (d->type != NHAL_GFX_QUERY_OCCLUSION && d->type != NHAL_GFX_QUERY_TIMESTAMP)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültige Abfrageart");
    if (!d->count || d->count > NHAL_GFX_MAX_QUERIES)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "count: 1..NHAL_GFX_MAX_QUERIES");
    if (d->type == NHAL_GFX_QUERY_TIMESTAMP && !timestamps)
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Zeitstempel-Abfragen gibt es hier nicht (info().timestamp_queries)");
    return nhal_status_ok();
}

/* set_desc: Beschreibung des Sets aus pass_timestamps (oder NULL ohne Set). */
static inline nhal_status nhal_gfx_check_timestamps(const nhal_gfx_pass_timestamps* t,
                                                    const nhal_gfx_query_set_desc* set_desc) {
    if (!t->set) return nhal_status_ok();
    if (set_desc->type != NHAL_GFX_QUERY_TIMESTAMP)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "timestamps braucht ein TIMESTAMP-Set");
    if (t->begin_index >= set_desc->count || t->end_index >= set_desc->count || t->begin_index == t->end_index)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zeitstempel-Index außerhalb des Sets oder doppelt");
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_resolve(const nhal_gfx_query_set_desc* set, uint32_t first, uint32_t count,
                                                 uint64_t dst_size, uint32_t dst_usage, uint64_t dst_offset) {
    if (!count || first >= set->count || count > set->count - first)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Abfragebereich außerhalb des Sets");
    if (!(dst_usage & NHAL_GFX_BUFFER_QUERY_RESOLVE))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziel braucht Usage QUERY_RESOLVE");
    if (dst_offset % 256u) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziel-Offset muss ein Vielfaches von 256 sein");
    if (dst_offset > dst_size || (uint64_t)count * 8u > dst_size - dst_offset)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Zielpuffer zu klein");
    return nhal_status_ok();
}

/* bytes: 16 (draw), 20 (draw_indexed), 12 (dispatch). */
static inline nhal_status nhal_gfx_check_indirect(uint64_t size, uint32_t usage, uint64_t offset, uint64_t bytes) {
    if (!(usage & NHAL_GFX_BUFFER_INDIRECT))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Indirect braucht einen Puffer mit Usage INDIRECT");
    if (offset % 4u || offset > size || bytes > size - offset)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Indirect-Offset ungültig oder Puffer zu klein");
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t tw,
                                                 uint32_t th) {
    if ((uint64_t)x + w > tw || (uint64_t)y + h > th)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Scissor liegt nicht im Ziel");
    return nhal_status_ok();
}

static inline nhal_status nhal_gfx_check_constants(const void* data, uint32_t size) {
    if (!data || !size || size % 4u || size > NHAL_GFX_MAX_CONSTANTS)
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Push-Konstanten: Daten fehlen oder Größe nicht 4..128 in 4er-Schritten");
    return nhal_status_ok();
}

/* Kleiner Ring für asynchrone Fehlermeldungen (ohne Sperre – die setzt das Backend). */
#define NHAL_GFX_ERROR_RING 8u
#define NHAL_GFX_ERROR_LEN 256u

typedef struct nhal_gfx_error_ring {
    char text[NHAL_GFX_ERROR_RING][NHAL_GFX_ERROR_LEN];
    uint32_t head, count;
} nhal_gfx_error_ring;

static inline void nhal_gfx_error_push(nhal_gfx_error_ring* r, const char* msg) {
    uint32_t idx;
    if (r->count == NHAL_GFX_ERROR_RING) { /* voll: älteste verwerfen */
        idx = r->head;
        r->head = (r->head + 1u) % NHAL_GFX_ERROR_RING;
    } else {
        idx = (r->head + r->count) % NHAL_GFX_ERROR_RING;
        ++r->count;
    }
    strncpy(r->text[idx], msg ? msg : "?", NHAL_GFX_ERROR_LEN - 1u);
    r->text[idx][NHAL_GFX_ERROR_LEN - 1u] = '\0';
}

static inline bool nhal_gfx_error_take(nhal_gfx_error_ring* r, char* buf, size_t cap) {
    if (!r->count) return false;
    if (buf && cap) {
        strncpy(buf, r->text[r->head], cap - 1u);
        buf[cap - 1u] = '\0';
    }
    r->head = (r->head + 1u) % NHAL_GFX_ERROR_RING;
    --r->count;
    return true;
}

#endif /* NHAL_BACKEND_GFX_VALIDATE_H */
