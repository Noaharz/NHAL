/*
 * fs.async_read, Version 1: Dateibereiche asynchron lesen.
 *
 * Ablauf:
 *   submit(path, offset, size, dst, done, user)  // kehrt sofort zurück
 *   ...                                           // Lesen läuft im Hintergrund
 *   poll(max)                                     // ruft done() für fertige Aufträge
 *
 * Semantik (gleich auf allen Plattformen):
 *   - Der Aufrufer stellt den Zielpuffer `dst` (mindestens `size` Bytes). Er
 *     muss gültig bleiben, bis done() gelaufen ist. NHAL allokiert pro Auftrag
 *     nur seine Verwaltung.
 *   - done() läuft NIE innerhalb von submit() und NIE auf einem fremden
 *     Thread, sondern immer in poll() bzw. drain() des Aufrufers. Damit braucht
 *     die App für ihre Daten keine eigenen Sperren.
 *   - `bytes` in done() ist < size, wenn die Datei vorher endet; das ist kein
 *     Fehler. Ein Offset hinter dem Ende liefert bytes = 0.
 *   - submit() und poll() dürfen aus beliebigen Threads kommen; done() läuft
 *     auf dem Thread, der poll() aufruft.
 *   - Beim nhal_shutdown() werden fertige, nicht abgeholte Aufträge OHNE
 *     done() verworfen. Wer alle Ergebnisse braucht, ruft vorher drain().
 *
 * Plattformabhängig:
 *   - Desktop: zwei Lese-Threads im Hintergrund.
 *   - Web (ohne pthreads), emuliert: gelesen wird erst in poll()/drain(),
 *     synchron aus dem virtuellen Dateisystem.
 */
#ifndef NHAL_FEATURES_FS_ASYNC_READ_H
#define NHAL_FEATURES_FS_ASYNC_READ_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nhal_read_done_fn)(void* user, nhal_status status, size_t bytes);

typedef struct nhal_fs_async_read_v1 {
    uint32_t struct_size;
    nhal_status (*submit)(const char* path, uint64_t offset, size_t size, void* dst,
                          nhal_read_done_fn done, void* user);
    /* Liefert bis zu `max` fertige Aufträge aus; gibt die Anzahl zurück. */
    size_t (*poll)(size_t max);
    /* Wartet, bis alle eingereichten Aufträge ausgeliefert sind. */
    void (*drain)(void);
    /* Eingereicht, aber noch nicht ausgeliefert. */
    size_t (*pending)(void);
} nhal_fs_async_read_v1;

static inline const nhal_fs_async_read_v1* nhal_get_fs_async_read_v1(const nhal_context* ctx) {
    return (const nhal_fs_async_read_v1*)nhal_get(ctx, NHAL_FEATURE_FS_ASYNC_READ, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_FS_ASYNC_READ_H */
