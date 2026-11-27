/*
 * errno → nhal_status (intern, POSIX-Backends). Gründe sind statische Texte.
 */
#ifndef NHAL_POSIX_FS_ERRORS_H
#define NHAL_POSIX_FS_ERRORS_H

#include "nhal/nhal_backend.h"

#include <errno.h>

static inline nhal_status nhal_posix_error(int err) {
    switch (err) {
        case ENOENT: return nhal_status_error(NHAL_ERR_NOT_FOUND, "Datei oder Ordner nicht gefunden");
        case ENOTDIR: return nhal_status_error(NHAL_ERR_NOT_FOUND, "Ein Teil des Pfades ist kein Ordner");
        case EACCES:
        case EPERM: return nhal_status_error(NHAL_ERR_IO, "Zugriff verweigert");
        case EEXIST: return nhal_status_error(NHAL_ERR_IO, "Existiert bereits");
        case EISDIR: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist ein Ordner");
        case ENOTEMPTY: return nhal_status_error(NHAL_ERR_IO, "Ordner ist nicht leer");
        case ENOSPC: return nhal_status_error(NHAL_ERR_IO, "Kein Platz mehr auf dem Speichermedium");
        case EROFS: return nhal_status_error(NHAL_ERR_IO, "Dateisystem ist schreibgeschützt");
        case EMFILE:
        case ENFILE: return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Zu viele offene Dateien");
        case ENAMETOOLONG: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist zu lang");
        case EINVAL: return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ungültiges Argument");
        case ENOMEM: return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher");
        default: return nhal_status_error(NHAL_ERR_IO, "Ein-/Ausgabefehler");
    }
}

#endif /* NHAL_POSIX_FS_ERRORS_H */
