/*
 * UTF-8 ↔ UTF-16 und Win32-Fehler → nhal_status (intern, Windows-Backends).
 */
#ifndef NHAL_WINDOWS_FS_PATH_H
#define NHAL_WINDOWS_FS_PATH_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nhal/nhal_backend.h"

/* UTF-8-Pfad in einen neuen UTF-16-Puffer; "/" wird zu "\". Freigeben mit
 * nhal_win_free_wide. NULL bei Fehler. */
static inline wchar_t* nhal_win_wide(const nhal_allocator* a, const char* utf8, size_t* bytes) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    *bytes = (size_t)n * sizeof(wchar_t);
    wchar_t* w = (wchar_t*)a->alloc(a->user, *bytes, _Alignof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n);
    for (int i = 0; i < n; ++i) {
        if (w[i] == L'/') w[i] = L'\\';
    }
    return w;
}

static inline void nhal_win_free_wide(const nhal_allocator* a, wchar_t* w, size_t bytes) {
    if (w) a->free(a->user, w, bytes);
}

static inline nhal_status nhal_win_error(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return nhal_status_error(NHAL_ERR_NOT_FOUND, "Datei oder Ordner nicht gefunden");
        case ERROR_ACCESS_DENIED: return nhal_status_error(NHAL_ERR_IO, "Zugriff verweigert");
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return nhal_status_error(NHAL_ERR_IO, "Existiert bereits");
        case ERROR_DIR_NOT_EMPTY: return nhal_status_error(NHAL_ERR_IO, "Ordner ist nicht leer");
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return nhal_status_error(NHAL_ERR_IO, "Kein Platz mehr auf dem Speichermedium");
        case ERROR_TOO_MANY_OPEN_FILES: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Zu viele offene Dateien");
        case ERROR_SHARING_VIOLATION: return nhal_status_error(NHAL_ERR_BUSY, "Datei ist von einem anderen Prozess gesperrt");
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_PARAMETER: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültiger Pfad oder Parameter");
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher");
        default: return nhal_status_error(NHAL_ERR_IO, "Ein-/Ausgabefehler");
    }
}

#endif /* NHAL_WINDOWS_FS_PATH_H */
