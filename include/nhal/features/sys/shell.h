/*
 * sys.shell, Version 1: Verzeichnisse der App und "im System öffnen".
 *
 * Alle Pfade sind UTF-8 und enden ohne Schrägstrich. Puffer zu klein →
 * NHAL_ERR_INVALID_ARGUMENT, der Puffer bleibt dann ein leerer String.
 *
 *   program_dir    Ordner des laufenden Programms.
 *                  macOS in einem .app-Bündel: Contents/MacOS.
 *                  Web: "/" (Wurzel des virtuellen FS, dorthin legt
 *                  Emscripten vorgeladene Dateien).
 *   resource_dir   Ordner für mitgelieferte Daten. macOS-Bündel:
 *                  Contents/Resources; sonst gleich program_dir.
 *   user_data_dir  Ordner für Spielstände und Einstellungen, wird bei Bedarf
 *                  angelegt. macOS: ~/Library/Application Support/<app>.
 *                  Web: /home/web_user/<app> im virtuellen FS – flüchtig,
 *                  geht beim Neuladen der Seite verloren.
 *   open           URL (mit Schema, z. B. https://…) oder vorhandene Datei/
 *                  Ordner mit dem Standardprogramm des Systems öffnen.
 *                  Kehrt sofort zurück. Web: nur URLs, in einem neuen Tab;
 *                  Browser blockieren das ohne Nutzeraktion oft still.
 */
#ifndef NHAL_FEATURES_SYS_SHELL_H
#define NHAL_FEATURES_SYS_SHELL_H

#include "nhal/nhal_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nhal_sys_shell_v1 {
    uint32_t struct_size;
    nhal_status (*program_dir)(char* buf, size_t capacity);
    nhal_status (*resource_dir)(char* buf, size_t capacity);
    /* app_name: Ordnername, ohne Schrägstriche und nicht "." oder "..". */
    nhal_status (*user_data_dir)(const char* app_name, char* buf, size_t capacity);
    nhal_status (*open)(const char* url_or_path);
} nhal_sys_shell_v1;

static inline const nhal_sys_shell_v1* nhal_get_sys_shell_v1(const nhal_context* ctx) {
    return (const nhal_sys_shell_v1*)nhal_get(ctx, NHAL_FEATURE_SYS_SHELL, 1u);
}

#ifdef __cplusplus
}
#endif

#endif /* NHAL_FEATURES_SYS_SHELL_H */
