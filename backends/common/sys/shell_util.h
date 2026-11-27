/* Gemeinsame Helfer für sys.shell-Backends. */
#ifndef NHAL_BACKEND_SYS_SHELL_UTIL_H
#define NHAL_BACKEND_SYS_SHELL_UTIL_H

#include "nhal/features/sys/shell.h"
#include "nhal/nhal_backend.h"

#include <string.h>

/* Kopiert s nach buf (ohne abschließenden Schrägstrich, außer bei "/"). */
static inline nhal_status nhal_shell_put(const char* s, char* buf, size_t cap) {
    if (!buf || !cap) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer fehlt");
    buf[0] = '\0';
    if (!s) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Pfad nicht ermittelbar");
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') --n;
    if (n + 1 > cap) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Puffer zu klein für den Pfad");
    memcpy(buf, s, n);
    buf[n] = '\0';
    return nhal_status_ok();
}

static inline bool nhal_shell_valid_app_name(const char* name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    }
    return strlen(name) < 128;
}

static inline bool nhal_shell_is_url(const char* s) {
    const char* colon = strstr(s, "://");
    if (!colon || colon == s) return false;
    for (const char* p = s; p < colon; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.'))
            return false;
    }
    return true;
}

#endif /* NHAL_BACKEND_SYS_SHELL_UTIL_H */
