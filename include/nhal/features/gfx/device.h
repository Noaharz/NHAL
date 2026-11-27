/*
 * gfx.device, Version 2: GPU-Gerät, Ressourcen, Pipelines, Befehlslisten.
 *
 * Ein Gerät pro Kontext. Das Modell folgt den modernen APIs (Metal, WebGPU,
 * Vulkan, D3D12): Ressourcen und Pipelines werden vorab angelegt, Arbeit wird
 * in Befehlslisten aufgezeichnet und mit commands_submit() an die GPU gegeben.
 *
 * Neu in Version 2: Bindungsgruppen mit Layouts, Push-Konstanten, Fortschritt
 * über Seriennummern (Frames in Flight), mehrere Farbziele (MRT), MSAA,
 * Scissor, Indirect-Draws und -Dispatches, Occlusion- und Zeitstempel-Abfragen,
 * mehr Pipeline-Zustand (Tiefenvergleich, Blend-Arten, Schreibmasken,
 * Drehsinn der Vorderseite).
 *
 * Bereitschaft:
 *   Auf manchen Plattformen kommt das Gerät asynchron (Web). Nach nhal_init()
 *   pro Frame poll() rufen und ready() prüfen: NHAL_OK = bereit,
 *   NHAL_ERR_BUSY = noch nicht, alles andere = kein Gerät (mit Grund).
 *   Vor der Bereitschaft liefern alle create-Funktionen NHAL_ERR_BUSY.
 *
 * Shader:
 *   Ein Shader-Modul trägt Quelltexte für mehrere Sprachen (MSL, WGSL). Das
 *   Backend nimmt seine; fehlt sie, kommt NHAL_ERR_UNSUPPORTED mit Grund.
 *   info().shader_language sagt, welche Sprache das Backend braucht.
 *
 * Bindungen (gleich auf allen Plattformen):
 *   Jede Ressource hat eine Slot-Nummer 0..NHAL_GFX_MAX_BINDINGS-1, die in
 *   einer Pipeline nur einmal vorkommt – über alle Gruppen hinweg.
 *     WGSL: @group(G) @binding(SLOT)
 *     MSL:  [[buffer(SLOT)]], [[texture(SLOT)]], [[sampler(SLOT)]]
 *   Zwei Wege, die sich kombinieren lassen:
 *   - Slot-Bindungen (bindings in der Pipeline, pass_set_buffer/_texture/
 *     _sampler) bilden Gruppe 0. Bequem für wenige, oft wechselnde Ressourcen.
 *   - Gruppen (groups[g] in der Pipeline, pass_set_group): vorab angelegte,
 *     unveränderliche Sätze von Ressourcen. Günstig für Material- und
 *     Szenendaten. Nutzt eine Pipeline Slot-Bindungen, bleibt groups[0] leer.
 *   Gruppen sind austauschbar, wenn ihre Layouts dieselben Einträge haben.
 *   Vertex-Puffer i liegen in MSL auf [[buffer(16 + i)]] (NHAL_GFX_MSL_VERTEX_BUFFER_BASE).
 *
 * Push-Konstanten:
 *   Bis NHAL_GFX_MAX_CONSTANTS Bytes pro Draw, gesetzt mit pass_set_constants.
 *     WGSL: @group(3) @binding(0) var<uniform> c: Constants;
 *     MSL:  constant Constants& c [[buffer(30)]]
 *   Metal überträgt sie direkt (setBytes). WebGPU hat keine Push-Konstanten:
 *   NHAL schreibt sie in einen Uniform-Ring und bindet sie mit dynamischem
 *   Offset in Gruppe 3. Deshalb gilt überall: höchstens drei eigene Gruppen,
 *   WGSL-Layoutregeln für Uniforms (vec3 auf 16 Bytes ausgerichtet).
 *
 * Ausrichtung (gleich auf allen Plattformen, wegen WebGPU):
 *   Größen und Offsets bei buffer_write, buffer_read, copy_buffer: Vielfache von 4.
 *   Offset einer Uniform- oder Storage-Bindung: Vielfaches von 256.
 *   bytes_per_row bei copy_texture_to_buffer: Vielfaches von 256.
 *   Ziel-Offset bei commands_resolve_queries: Vielfaches von 256.
 *
 * Reihenfolge und Fortschritt:
 *   buffer_write und texture_write wirken vor allen danach abgeschickten
 *   Befehlslisten. Befehlslisten laufen in der Reihenfolge von submit().
 *   Jedes erfolgreiche commands_submit() bekommt eine Seriennummer (1, 2, 3 …).
 *   completed() sagt, bis zu welcher Nummer die GPU fertig ist; wait() wartet
 *   darauf. Frames in Flight: pro Frame die Nummer merken und vor dem
 *   Überschreiben der Ressourcen von Frame N-k auf dessen Nummer warten.
 *   Unterschied: Im Browser kann wait() nicht blockieren
 *   (info().blocking_wait = false); dort completed() pro Frame prüfen.
 *
 * Barrieren:
 *   Gibt es nicht als Funktion. Metal (verfolgte Ressourcen) und WebGPU fügen
 *   sie selbst zwischen Passes und Kopien ein. Innerhalb eines Passes darf eine
 *   Ressource nicht zugleich geschrieben und anderswo gelesen werden.
 *
 * Speicher (Unterschied wird beschrieben, nicht versteckt):
 *   info().unified_memory: CPU und GPU teilen sich den Speicher (Apple Silicon).
 *   info().persistent_mapping: Puffer mit NHAL_GFX_BUFFER_HOST_VISIBLE sind
 *   dauerhaft über buffer_mapped() erreichbar. Wo das fehlt (Web), lehnt
 *   buffer_create HOST_VISIBLE mit NHAL_ERR_UNSUPPORTED ab.
 *   Wer auf einen HOST_VISIBLE-Puffer schreibt, während die GPU ihn noch
 *   liest, bekommt ein Datenrennen – genau dafür gibt es completed()/wait().
 *
 * MSAA:
 *   sample_count 1 oder 4 (überall vorhanden). Eine MSAA-Textur ist nur
 *   Render-Ziel; das Ergebnis landet über resolve in einer normalen Textur
 *   gleicher Größe und gleichen Formats. Pipeline und Pass brauchen dieselbe
 *   Zahl.
 *
 * Abfragen:
 *   OCCLUSION: zählt Samples, die Tiefen- und Scissor-Test bestehen, zwischen
 *   pass_begin_occlusion und pass_end_occlusion. 0 heißt unsichtbar; über
 *   den genauen Wert sonst nichts verlassen (WebGPU garantiert nur "nicht 0").
 *   Ein Render-Pass mit occlusion setzt die Werte des Sets zurück; vor dem
 *   nächsten Pass mit demselben Set auflösen.
 *   TIMESTAMP: GPU-Zeit am Anfang und Ende eines Passes, nur wenn
 *   info().timestamp_queries. Rohwerte in Ticks, mal info().timestamp_period
 *   ergibt Nanosekunden. Browser runden sie aus Sicherheitsgründen.
 *   commands_resolve_queries schreibt je Abfrage einen uint64 in einen Puffer
 *   mit Usage QUERY_RESOLVE.
 *
 * Indirect:
 *   Argumente liegen in einem Puffer mit Usage INDIRECT (Offset Vielfaches von 4):
 *     draw:          uint32 vertex_count, instance_count, first_vertex, first_instance
 *     draw_indexed:  uint32 index_count, instance_count, first_index; int32 base_vertex;
 *                    uint32 first_instance
 *     dispatch:      uint32 x, y, z
 *   first_instance muss 0 sein, außer info().indirect_first_instance.
 *
 * Asynchrone Ergebnisse:
 *   buffer_read() meldet sich nur aus poll(), im Thread des Aufrufers.
 *   Fehler, die erst auf der GPU auffallen (Web-Validierung, abgebrochene
 *   Befehlslisten), sammelt take_error().
 *   Metal übersetzt Shader sofort: Fehler kommen aus shader_create bzw. den
 *   Pipeline-Funktionen. WebGPU übersetzt asynchron: shader_create gelingt,
 *   die Meldung des Browsers kommt später über take_error().
 *
 * Plattformabhängig:
 *   - Web: R32/RG32/RGBA32_FLOAT lassen sich nicht filtern (kein linearer
 *     Sampler). Eine Canvas wird präsentiert, sobald die Seite in die
 *     Ereignisschleife zurückkehrt; surface_present() gibt nur die Textur ab.
 *   - macOS: NSVIEW-Oberflächen nur im Hauptthread anlegen.
 *
 * Fehler beim Aufzeichnen:
 *   pass_*- und commands_copy_*-Funktionen geben nichts zurück. Den ersten
 *   Fehler merkt sich die Befehlsliste; commands_submit() liefert ihn und
 *   verwirft dann die ganze Liste.
 *
 * Threads: alle Funktionen gehören in einen Thread.
 */
#ifndef NHAL_FEATURES_GFX_DEVICE_H
#define NHAL_FEATURES_GFX_DEVICE_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHAL_GFX_MAX_BINDINGS 16u
#define NHAL_GFX_MAX_GROUPS 3u          /* eigene Gruppen 0..2 */
#define NHAL_GFX_CONSTANTS_GROUP 3u     /* WGSL-Gruppe der Push-Konstanten */
#define NHAL_GFX_MAX_CONSTANTS 128u     /* Bytes */
#define NHAL_GFX_MAX_COLOR_TARGETS 4u
#define NHAL_GFX_MAX_VERTEX_BUFFERS 4u
#define NHAL_GFX_MAX_VERTEX_ATTRIBUTES 8u
#define NHAL_GFX_MAX_QUERIES 4096u
#define NHAL_GFX_MSL_VERTEX_BUFFER_BASE 16u
#define NHAL_GFX_MSL_CONSTANTS_BUFFER 30u
#define NHAL_GFX_WAIT_FOREVER UINT64_MAX

typedef struct nhal_gfx_buffer nhal_gfx_buffer;
typedef struct nhal_gfx_texture nhal_gfx_texture;
typedef struct nhal_gfx_sampler nhal_gfx_sampler;
typedef struct nhal_gfx_shader nhal_gfx_shader;
typedef struct nhal_gfx_group_layout nhal_gfx_group_layout;
typedef struct nhal_gfx_group nhal_gfx_group;
typedef struct nhal_gfx_pipeline nhal_gfx_pipeline; /* Render oder Compute */
typedef struct nhal_gfx_query_set nhal_gfx_query_set;
typedef struct nhal_gfx_commands nhal_gfx_commands;
typedef struct nhal_gfx_pass nhal_gfx_pass;
typedef struct nhal_gfx_surface nhal_gfx_surface;

typedef enum nhal_gfx_shader_language {
    NHAL_GFX_SHADER_MSL = 1,
    NHAL_GFX_SHADER_WGSL = 2
} nhal_gfx_shader_language;

typedef struct nhal_gfx_info {
    const char* backend; /* "metal", "webgpu" */
    char device_name[128];
    nhal_gfx_shader_language shader_language;
    bool unified_memory;
    bool persistent_mapping;
    bool blocking_wait;           /* wait() kann blockieren */
    bool timestamp_queries;       /* NHAL_GFX_QUERY_TIMESTAMP vorhanden */
    bool indirect_first_instance; /* first_instance != 0 in Indirect-Draws */
    double timestamp_period;      /* Nanosekunden pro Zeitstempel-Tick */
    uint32_t max_texture_size;    /* Kantenlänge 2D */
    uint64_t max_buffer_size;
} nhal_gfx_info;

/* ── Formate ───────────────────────────────────────────────────────────── */

typedef enum nhal_gfx_format {
    NHAL_GFX_FORMAT_NONE = 0,
    NHAL_GFX_FORMAT_R8_UNORM,
    NHAL_GFX_FORMAT_RGBA8_UNORM,
    NHAL_GFX_FORMAT_RGBA8_SRGB,
    NHAL_GFX_FORMAT_BGRA8_UNORM,
    NHAL_GFX_FORMAT_BGRA8_SRGB,
    NHAL_GFX_FORMAT_R32_FLOAT,
    NHAL_GFX_FORMAT_RG32_FLOAT,
    NHAL_GFX_FORMAT_RGBA16_FLOAT,
    NHAL_GFX_FORMAT_RGBA32_FLOAT,
    NHAL_GFX_FORMAT_DEPTH32_FLOAT
} nhal_gfx_format;

typedef enum nhal_gfx_vertex_format {
    NHAL_GFX_VERTEX_FLOAT32 = 1,
    NHAL_GFX_VERTEX_FLOAT32X2,
    NHAL_GFX_VERTEX_FLOAT32X3,
    NHAL_GFX_VERTEX_FLOAT32X4,
    NHAL_GFX_VERTEX_UNORM8X4,
    NHAL_GFX_VERTEX_UINT32
} nhal_gfx_vertex_format;

/* ── Puffer ────────────────────────────────────────────────────────────── */

enum {
    NHAL_GFX_BUFFER_VERTEX = 1u << 0,
    NHAL_GFX_BUFFER_INDEX = 1u << 1,
    NHAL_GFX_BUFFER_UNIFORM = 1u << 2,
    NHAL_GFX_BUFFER_STORAGE = 1u << 3,
    NHAL_GFX_BUFFER_COPY_SRC = 1u << 4,
    NHAL_GFX_BUFFER_COPY_DST = 1u << 5,
    NHAL_GFX_BUFFER_HOST_VISIBLE = 1u << 6, /* nur mit info().persistent_mapping */
    NHAL_GFX_BUFFER_INDIRECT = 1u << 7,
    NHAL_GFX_BUFFER_QUERY_RESOLVE = 1u << 8
};

typedef struct nhal_gfx_buffer_desc {
    uint64_t size; /* Vielfaches von 4 */
    uint32_t usage; /* NHAL_GFX_BUFFER_* */
} nhal_gfx_buffer_desc;

/* status: Ergebnis; dst ist bei NHAL_OK gefüllt. */
typedef void (*nhal_gfx_read_fn)(void* user, nhal_status status);

/* ── Texturen und Sampler ──────────────────────────────────────────────── */

enum {
    NHAL_GFX_TEXTURE_SAMPLED = 1u << 0,
    NHAL_GFX_TEXTURE_STORAGE = 1u << 1,
    NHAL_GFX_TEXTURE_RENDER_TARGET = 1u << 2,
    NHAL_GFX_TEXTURE_COPY_SRC = 1u << 3,
    NHAL_GFX_TEXTURE_COPY_DST = 1u << 4
};

typedef struct nhal_gfx_texture_desc {
    uint32_t width, height;
    nhal_gfx_format format;
    uint32_t usage;        /* NHAL_GFX_TEXTURE_* */
    uint32_t sample_count; /* 0 oder 1 = ohne MSAA; 4 = MSAA (nur RENDER_TARGET) */
} nhal_gfx_texture_desc;

typedef enum nhal_gfx_filter { NHAL_GFX_FILTER_NEAREST = 0, NHAL_GFX_FILTER_LINEAR = 1 } nhal_gfx_filter;
typedef enum nhal_gfx_address { NHAL_GFX_ADDRESS_CLAMP = 0, NHAL_GFX_ADDRESS_REPEAT = 1 } nhal_gfx_address;

typedef struct nhal_gfx_sampler_desc {
    nhal_gfx_filter filter;
    nhal_gfx_address address;
} nhal_gfx_sampler_desc;

/* ── Shader und Bindungen ──────────────────────────────────────────────── */

typedef struct nhal_gfx_shader_desc {
    const char* msl; /* NULL, wenn nicht vorhanden */
    const char* wgsl;
    /* Für künftige Backends (D3D12, Vulkan); heute ungenutzt. */
    const char* hlsl;
    const void* spirv;
    size_t spirv_size;
} nhal_gfx_shader_desc;

typedef enum nhal_gfx_binding_kind {
    NHAL_GFX_BIND_UNIFORM_BUFFER = 1,
    NHAL_GFX_BIND_STORAGE_BUFFER,      /* lesen und schreiben */
    NHAL_GFX_BIND_READ_STORAGE_BUFFER, /* nur lesen */
    NHAL_GFX_BIND_SAMPLED_TEXTURE,     /* float, 2D */
    NHAL_GFX_BIND_STORAGE_TEXTURE,     /* nur schreiben, 2D */
    NHAL_GFX_BIND_SAMPLER
} nhal_gfx_binding_kind;

enum {
    NHAL_GFX_STAGE_VERTEX = 1u << 0,
    NHAL_GFX_STAGE_FRAGMENT = 1u << 1,
    NHAL_GFX_STAGE_COMPUTE = 1u << 2,
    NHAL_GFX_STAGE_OBJECT = 1u << 3, /* nur mit gfx.mesh_shader */
    NHAL_GFX_STAGE_MESH = 1u << 4    /* nur mit gfx.mesh_shader */
};

typedef struct nhal_gfx_binding_desc {
    uint32_t slot;
    nhal_gfx_binding_kind kind;
    uint32_t stages;              /* NHAL_GFX_STAGE_* */
    nhal_gfx_format storage_format; /* nur STORAGE_TEXTURE */
} nhal_gfx_binding_desc;

typedef struct nhal_gfx_group_layout_desc {
    uint32_t binding_count;
    const nhal_gfx_binding_desc* bindings;
} nhal_gfx_group_layout_desc;

/* Ein Eintrag pro Bindung des Layouts; je nach Art buffer, texture oder sampler. */
typedef struct nhal_gfx_group_entry {
    uint32_t slot;
    nhal_gfx_buffer* buffer;
    uint64_t offset; /* Puffer: Vielfaches von 256 */
    uint64_t size;   /* Puffer: 0 = bis zum Ende */
    nhal_gfx_texture* texture;
    nhal_gfx_sampler* sampler;
} nhal_gfx_group_entry;

typedef struct nhal_gfx_group_desc {
    const nhal_gfx_group_layout* layout;
    uint32_t entry_count;
    const nhal_gfx_group_entry* entries;
} nhal_gfx_group_desc;

/* ── Pipelines ─────────────────────────────────────────────────────────── */

typedef struct nhal_gfx_vertex_attribute {
    nhal_gfx_vertex_format format;
    uint32_t offset;
    uint32_t location; /* WGSL @location(n), MSL [[attribute(n)]] */
} nhal_gfx_vertex_attribute;

typedef struct nhal_gfx_vertex_buffer_layout {
    uint32_t stride;
    bool per_instance;
    uint32_t attribute_count;
    nhal_gfx_vertex_attribute attributes[NHAL_GFX_MAX_VERTEX_ATTRIBUTES];
} nhal_gfx_vertex_buffer_layout;

typedef enum nhal_gfx_topology {
    NHAL_GFX_TRIANGLES = 0,
    NHAL_GFX_TRIANGLE_STRIP,
    NHAL_GFX_LINES,
    NHAL_GFX_POINTS
} nhal_gfx_topology;

typedef enum nhal_gfx_cull { NHAL_GFX_CULL_NONE = 0, NHAL_GFX_CULL_BACK, NHAL_GFX_CULL_FRONT } nhal_gfx_cull;
typedef enum nhal_gfx_front_face { NHAL_GFX_FRONT_CCW = 0, NHAL_GFX_FRONT_CW } nhal_gfx_front_face;

typedef enum nhal_gfx_blend {
    NHAL_GFX_BLEND_NONE = 0,
    NHAL_GFX_BLEND_ALPHA,         /* src·a + dst·(1−a) */
    NHAL_GFX_BLEND_PREMULTIPLIED, /* src + dst·(1−a) */
    NHAL_GFX_BLEND_ADDITIVE,      /* src·a + dst */
    NHAL_GFX_BLEND_MULTIPLY       /* src·dst */
} nhal_gfx_blend;

enum {
    NHAL_GFX_WRITE_R = 1u << 0,
    NHAL_GFX_WRITE_G = 1u << 1,
    NHAL_GFX_WRITE_B = 1u << 2,
    NHAL_GFX_WRITE_A = 1u << 3,
    NHAL_GFX_WRITE_ALL = 0xFu
};

typedef struct nhal_gfx_color_target {
    nhal_gfx_format format;
    nhal_gfx_blend blend;
    uint32_t write_mask; /* NHAL_GFX_WRITE_*; 0 = alle Kanäle */
} nhal_gfx_color_target;

/* Vergleich für den Tiefentest; LESS ist der Standard (0). */
typedef enum nhal_gfx_compare {
    NHAL_GFX_COMPARE_LESS = 0,
    NHAL_GFX_COMPARE_LESS_EQUAL,
    NHAL_GFX_COMPARE_GREATER,
    NHAL_GFX_COMPARE_GREATER_EQUAL,
    NHAL_GFX_COMPARE_EQUAL,
    NHAL_GFX_COMPARE_NOT_EQUAL,
    NHAL_GFX_COMPARE_ALWAYS,
    NHAL_GFX_COMPARE_NEVER
} nhal_gfx_compare;

typedef struct nhal_gfx_render_pipeline_desc {
    const nhal_gfx_shader* shader;
    const char* vertex_entry;
    const char* fragment_entry;
    uint32_t vertex_buffer_count;
    const nhal_gfx_vertex_buffer_layout* vertex_buffers;
    uint32_t binding_count; /* Slot-Bindungen (Gruppe 0) */
    const nhal_gfx_binding_desc* bindings;
    const nhal_gfx_group_layout* groups[NHAL_GFX_MAX_GROUPS]; /* NULL = Gruppe ungenutzt */
    uint32_t constants_size; /* Push-Konstanten in Bytes, Vielfaches von 4, 0 = keine */
    uint32_t color_count;
    nhal_gfx_color_target colors[NHAL_GFX_MAX_COLOR_TARGETS];
    nhal_gfx_format depth_format; /* NONE oder DEPTH32_FLOAT */
    bool depth_test;
    bool depth_write;
    nhal_gfx_compare depth_compare; /* nur mit depth_test */
    nhal_gfx_topology topology;
    nhal_gfx_cull cull;
    nhal_gfx_front_face front_face; /* Standard: gegen den Uhrzeigersinn */
    uint32_t sample_count;          /* 0 oder 1, 4 = MSAA */
} nhal_gfx_render_pipeline_desc;

typedef struct nhal_gfx_compute_pipeline_desc {
    const nhal_gfx_shader* shader;
    const char* entry;
    uint32_t binding_count;
    const nhal_gfx_binding_desc* bindings;
    const nhal_gfx_group_layout* groups[NHAL_GFX_MAX_GROUPS];
    uint32_t constants_size;
    /* Muss zu @workgroup_size im WGSL passen; MSL bekommt sie beim Dispatch. */
    uint32_t workgroup_size[3];
} nhal_gfx_compute_pipeline_desc;

/* ── Abfragen ──────────────────────────────────────────────────────────── */

typedef enum nhal_gfx_query_type { NHAL_GFX_QUERY_OCCLUSION = 1, NHAL_GFX_QUERY_TIMESTAMP } nhal_gfx_query_type;

typedef struct nhal_gfx_query_set_desc {
    nhal_gfx_query_type type;
    uint32_t count; /* 1..NHAL_GFX_MAX_QUERIES */
} nhal_gfx_query_set_desc;

/* Zeitstempel am Anfang und Ende eines Passes; set = NULL: keine. */
typedef struct nhal_gfx_pass_timestamps {
    nhal_gfx_query_set* set;
    uint32_t begin_index;
    uint32_t end_index;
} nhal_gfx_pass_timestamps;

/* ── Passes ────────────────────────────────────────────────────────────── */

typedef enum nhal_gfx_load { NHAL_GFX_LOAD_CLEAR = 0, NHAL_GFX_LOAD_KEEP = 1 } nhal_gfx_load;
typedef enum nhal_gfx_store { NHAL_GFX_STORE_KEEP = 0, NHAL_GFX_STORE_DISCARD = 1 } nhal_gfx_store;

typedef struct nhal_gfx_color_attachment {
    nhal_gfx_texture* texture; /* Render-Target oder Textur aus surface_acquire */
    nhal_gfx_texture* resolve; /* nur bei MSAA: Ziel des Auflösens, sonst NULL */
    nhal_gfx_load load;
    nhal_gfx_store store; /* DISCARD lohnt bei MSAA mit resolve */
    float clear_color[4];
} nhal_gfx_color_attachment;

typedef struct nhal_gfx_render_pass_desc {
    uint32_t color_count;
    nhal_gfx_color_attachment colors[NHAL_GFX_MAX_COLOR_TARGETS];
    nhal_gfx_texture* depth; /* NULL = ohne Tiefe */
    nhal_gfx_load depth_load;
    nhal_gfx_store depth_store;
    float clear_depth;
    nhal_gfx_query_set* occlusion; /* OCCLUSION-Set für pass_begin_occlusion, sonst NULL */
    nhal_gfx_pass_timestamps timestamps;
} nhal_gfx_render_pass_desc;

typedef struct nhal_gfx_compute_pass_desc {
    nhal_gfx_pass_timestamps timestamps;
} nhal_gfx_compute_pass_desc;

typedef enum nhal_gfx_index_format { NHAL_GFX_INDEX_UINT16 = 0, NHAL_GFX_INDEX_UINT32 = 1 } nhal_gfx_index_format;

/* ── Oberflächen (Fenster, Canvas) ─────────────────────────────────────── */

typedef enum nhal_gfx_surface_kind {
    NHAL_GFX_SURFACE_METAL_LAYER = 1, /* handle: CAMetalLayer* */
    NHAL_GFX_SURFACE_NSVIEW,          /* handle: NSView*; NHAL hängt einen CAMetalLayer an */
    NHAL_GFX_SURFACE_CANVAS           /* handle: const char* CSS-Selektor, z. B. "#canvas" */
} nhal_gfx_surface_kind;

typedef struct nhal_gfx_surface_desc {
    nhal_gfx_surface_kind kind;
    void* handle;
} nhal_gfx_surface_desc;

/* ── Funktionstabelle ──────────────────────────────────────────────────── */

typedef struct nhal_gfx_device_v2 {
    uint32_t struct_size;

    nhal_status (*ready)(void);
    void (*poll)(void);
    void (*info)(nhal_gfx_info* out);
    /* Kopiert die älteste asynchrone Fehlermeldung nach buf; false = keine. */
    bool (*take_error)(char* buf, size_t capacity);

    /* initial: NULL (Puffer ist genullt) oder genau desc->size Bytes. */
    nhal_status (*buffer_create)(const nhal_gfx_buffer_desc* desc, const void* initial, nhal_gfx_buffer** out);
    void (*buffer_destroy)(nhal_gfx_buffer* buffer);
    nhal_status (*buffer_write)(nhal_gfx_buffer* buffer, uint64_t offset, const void* data, uint64_t size);
    /* Liest nach Abschluss aller bisher abgeschickten Arbeit; Meldung in poll(). */
    nhal_status (*buffer_read)(nhal_gfx_buffer* buffer, uint64_t offset, uint64_t size, void* dst,
                               nhal_gfx_read_fn done, void* user);
    void* (*buffer_mapped)(nhal_gfx_buffer* buffer); /* NULL ohne HOST_VISIBLE */

    nhal_status (*texture_create)(const nhal_gfx_texture_desc* desc, nhal_gfx_texture** out);
    void (*texture_destroy)(nhal_gfx_texture* texture);
    nhal_status (*texture_write)(nhal_gfx_texture* texture, const void* data, uint32_t bytes_per_row);

    nhal_status (*sampler_create)(const nhal_gfx_sampler_desc* desc, nhal_gfx_sampler** out);
    void (*sampler_destroy)(nhal_gfx_sampler* sampler);

    nhal_status (*shader_create)(const nhal_gfx_shader_desc* desc, nhal_gfx_shader** out);
    void (*shader_destroy)(nhal_gfx_shader* shader);

    nhal_status (*group_layout_create)(const nhal_gfx_group_layout_desc* desc, nhal_gfx_group_layout** out);
    /* Erst freigeben, wenn keine Pipeline und keine Gruppe es mehr braucht. */
    void (*group_layout_destroy)(nhal_gfx_group_layout* layout);
    /* Prüft Usage, Format und Ausrichtung sofort; die Gruppe hält die Ressourcen fest. */
    nhal_status (*group_create)(const nhal_gfx_group_desc* desc, nhal_gfx_group** out);
    void (*group_destroy)(nhal_gfx_group* group);

    nhal_status (*render_pipeline_create)(const nhal_gfx_render_pipeline_desc* desc, nhal_gfx_pipeline** out);
    nhal_status (*compute_pipeline_create)(const nhal_gfx_compute_pipeline_desc* desc, nhal_gfx_pipeline** out);
    void (*pipeline_destroy)(nhal_gfx_pipeline* pipeline);

    nhal_status (*query_set_create)(const nhal_gfx_query_set_desc* desc, nhal_gfx_query_set** out);
    void (*query_set_destroy)(nhal_gfx_query_set* set);

    nhal_status (*commands_begin)(nhal_gfx_commands** out);
    /* Schickt ab und gibt die Liste frei – auch im Fehlerfall. serial (darf NULL
     * sein) bekommt die Seriennummer, bei Fehler 0. */
    nhal_status (*commands_submit)(nhal_gfx_commands* commands, uint64_t* serial);
    /* Höchste Seriennummer, bis zu der alle Arbeit fertig ist. */
    uint64_t (*completed)(void);
    /* NHAL_OK = fertig; NHAL_ERR_BUSY = nach timeout_ns noch nicht (im Web
     * sofort, ohne zu blockieren); NHAL_ERR_INVALID_ARGUMENT = nie abgeschickt. */
    nhal_status (*wait)(uint64_t serial, uint64_t timeout_ns);
    void (*commands_copy_buffer)(nhal_gfx_commands* commands, nhal_gfx_buffer* src, uint64_t src_offset,
                                 nhal_gfx_buffer* dst, uint64_t dst_offset, uint64_t size);
    void (*commands_copy_texture_to_buffer)(nhal_gfx_commands* commands, nhal_gfx_texture* src, nhal_gfx_buffer* dst,
                                            uint64_t dst_offset, uint32_t bytes_per_row);
    /* Schreibt count uint64-Werte ab first; außerhalb eines Passes. */
    void (*commands_resolve_queries)(nhal_gfx_commands* commands, nhal_gfx_query_set* set, uint32_t first,
                                     uint32_t count, nhal_gfx_buffer* dst, uint64_t dst_offset);

    /* Höchstens ein offener Pass pro Befehlsliste; NULL bei Fehler (steht dann in der Liste). */
    nhal_gfx_pass* (*render_pass_begin)(nhal_gfx_commands* commands, const nhal_gfx_render_pass_desc* desc);
    /* desc darf NULL sein. */
    nhal_gfx_pass* (*compute_pass_begin)(nhal_gfx_commands* commands, const nhal_gfx_compute_pass_desc* desc);
    void (*pass_end)(nhal_gfx_pass* pass);

    void (*pass_set_pipeline)(nhal_gfx_pass* pass, nhal_gfx_pipeline* pipeline);
    /* size 0 = bis zum Ende des Puffers. */
    void (*pass_set_buffer)(nhal_gfx_pass* pass, uint32_t slot, nhal_gfx_buffer* buffer, uint64_t offset,
                            uint64_t size);
    void (*pass_set_texture)(nhal_gfx_pass* pass, uint32_t slot, nhal_gfx_texture* texture);
    void (*pass_set_sampler)(nhal_gfx_pass* pass, uint32_t slot, nhal_gfx_sampler* sampler);
    /* index 0..NHAL_GFX_MAX_GROUPS-1; bleibt über Pipeline-Wechsel gesetzt. */
    void (*pass_set_group)(nhal_gfx_pass* pass, uint32_t index, nhal_gfx_group* group);
    /* size: Vielfaches von 4, höchstens NHAL_GFX_MAX_CONSTANTS; gilt für die folgenden Draws. */
    void (*pass_set_constants)(nhal_gfx_pass* pass, const void* data, uint32_t size);
    void (*pass_set_vertex_buffer)(nhal_gfx_pass* pass, uint32_t index, nhal_gfx_buffer* buffer, uint64_t offset);
    void (*pass_set_index_buffer)(nhal_gfx_pass* pass, nhal_gfx_buffer* buffer, nhal_gfx_index_format format,
                                  uint64_t offset);
    void (*pass_set_viewport)(nhal_gfx_pass* pass, float x, float y, float width, float height);
    /* Pixel; muss im Ziel liegen. Standard: ganzes Ziel. */
    void (*pass_set_scissor)(nhal_gfx_pass* pass, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    void (*pass_draw)(nhal_gfx_pass* pass, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                      uint32_t first_instance);
    void (*pass_draw_indexed)(nhal_gfx_pass* pass, uint32_t index_count, uint32_t instance_count,
                              uint32_t first_index, int32_t base_vertex, uint32_t first_instance);
    void (*pass_draw_indirect)(nhal_gfx_pass* pass, nhal_gfx_buffer* args, uint64_t offset);
    void (*pass_draw_indexed_indirect)(nhal_gfx_pass* pass, nhal_gfx_buffer* args, uint64_t offset);
    void (*pass_dispatch)(nhal_gfx_pass* pass, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
    void (*pass_dispatch_indirect)(nhal_gfx_pass* pass, nhal_gfx_buffer* args, uint64_t offset);
    /* Nur im Render-Pass mit occlusion; nicht verschachtelt. */
    void (*pass_begin_occlusion)(nhal_gfx_pass* pass, uint32_t index);
    void (*pass_end_occlusion)(nhal_gfx_pass* pass);

    nhal_status (*surface_create)(const nhal_gfx_surface_desc* desc, nhal_gfx_surface** out);
    void (*surface_destroy)(nhal_gfx_surface* surface);
    /* Größe in Pixeln; vsync = an den Bildschirm takten (Web: immer). */
    nhal_status (*surface_configure)(nhal_gfx_surface* surface, uint32_t width, uint32_t height, bool vsync);
    nhal_gfx_format (*surface_format)(const nhal_gfx_surface* surface);
    /* Textur gilt bis surface_present(). NHAL_ERR_BUSY = gerade keine frei, später erneut. */
    nhal_status (*surface_acquire)(nhal_gfx_surface* surface, nhal_gfx_texture** out);
    /* Nach dem submit() der Befehlsliste, die in die Textur zeichnet. */
    nhal_status (*surface_present)(nhal_gfx_surface* surface);
} nhal_gfx_device_v2;

static inline const nhal_gfx_device_v2* nhal_get_gfx_device_v2(const nhal_context* ctx) {
    return (const nhal_gfx_device_v2*)nhal_get(ctx, NHAL_FEATURE_GFX_DEVICE, 2u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_GFX_DEVICE_H */
