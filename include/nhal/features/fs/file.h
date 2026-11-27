/*
 * fs.file, Version 1: Dateien und Ordner, synchron.
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Pfade sind UTF-8. "/" ist überall als Trenner erlaubt.
 *   - read() liest, bis `size` Bytes da sind oder das Dateiende erreicht ist;
 *     *done < size heißt Dateiende, kein Fehler. write() schreibt alles oder
 *     meldet einen Fehler.
 *   - Positionen und Größen sind 64 Bit.
 *   - sync() wartet, bis die Daten auf dem Speichermedium sind (teuer).
 *   - Ein nhal_file darf nicht gleichzeitig aus mehreren Threads benutzt werden;
 *     verschiedene Dateien schon.
 *   - list_dir() liefert "." und ".." nicht; die Reihenfolge ist unbestimmt.
 *   - remove() löscht Dateien und leere Ordner.
 *
 * Plattformabhängig (in der Plattform-Matrix beschrieben):
 *   - Web: Dateien liegen im virtuellen Dateisystem von Emscripten im
 *     Speicher, nicht auf der Platte des Rechners.
 */
#ifndef NHAL_FEATURES_FS_FILE_H
#define NHAL_FEATURES_FS_FILE_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_file nhal_file;

enum {
    NHAL_FILE_READ = 1u << 0,
    NHAL_FILE_WRITE = 1u << 1,
    NHAL_FILE_CREATE = 1u << 2,   /* anlegen, falls nicht vorhanden */
    NHAL_FILE_TRUNCATE = 1u << 3, /* auf 0 Bytes kürzen */
    NHAL_FILE_APPEND = 1u << 4,   /* jedes write() ans Ende */
    NHAL_FILE_EXCLUSIVE = 1u << 5 /* mit CREATE: Fehler, wenn es die Datei schon gibt */
};

typedef enum nhal_seek_origin {
    NHAL_SEEK_SET = 0,
    NHAL_SEEK_CUR = 1,
    NHAL_SEEK_END = 2
} nhal_seek_origin;

typedef struct nhal_file_stat {
    bool exists;
    bool is_dir;
    uint64_t size; /* Bytes, 0 bei Ordnern */
} nhal_file_stat;

typedef struct nhal_dir_entry {
    const char* name; /* nur der Name, gilt nur während des Rückrufs */
    bool is_dir;
    uint64_t size;
} nhal_dir_entry;

/* Rückgabe false bricht list_dir ab (Ergebnis trotzdem NHAL_OK). */
typedef bool (*nhal_dir_fn)(void* user, const nhal_dir_entry* entry);

typedef struct nhal_fs_file_v1 {
    uint32_t struct_size;
    nhal_status (*open)(const char* path, uint32_t flags, nhal_file** out);
    void (*close)(nhal_file* file);
    nhal_status (*read)(nhal_file* file, void* dst, size_t size, size_t* done);
    nhal_status (*write)(nhal_file* file, const void* src, size_t size);
    nhal_status (*seek)(nhal_file* file, int64_t offset, nhal_seek_origin origin, uint64_t* position);
    nhal_status (*size)(nhal_file* file, uint64_t* size);
    nhal_status (*sync)(nhal_file* file);
    /* Gibt es den Pfad nicht, ist das kein Fehler: exists = false. */
    nhal_status (*stat)(const char* path, nhal_file_stat* out);
    nhal_status (*list_dir)(const char* path, nhal_dir_fn fn, void* user);
    nhal_status (*make_dir)(const char* path);
    nhal_status (*remove)(const char* path);
} nhal_fs_file_v1;

static inline const nhal_fs_file_v1* nhal_get_fs_file_v1(const nhal_context* ctx) {
    return (const nhal_fs_file_v1*)nhal_get(ctx, NHAL_FEATURE_FS_FILE, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_FS_FILE_H */
