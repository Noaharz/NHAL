/*
 * sys.shell – Web. Verzeichnisse liegen im virtuellen FS von Emscripten;
 * open() öffnet URLs in einem neuen Tab.
 */
#include "nhal/features/sys/shell.h"
#include "common/sys/shell_util.h"

#include <emscripten.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

/* clang-format off */
EM_JS(int, shell_js_open, (const char* url), {
    if (typeof window === "undefined" || !window.open) return -1;
    window.open(UTF8ToString(url), "_blank", "noopener");
    return 0;
})
/* clang-format on */

static nhal_status program_dir(char* buf, size_t cap) { return nhal_shell_put("/", buf, cap); }

static nhal_status user_data_dir(const char* app, char* buf, size_t cap) {
    if (buf && cap) buf[0] = '\0';
    if (!nhal_shell_valid_app_name(app))
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "app_name leer, zu lang oder mit Schrägstrich");
    char path[256];
    snprintf(path, sizeof path, "/home/web_user/%s", app);
    mkdir("/home", 0777);
    mkdir("/home/web_user", 0777);
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        return nhal_status_error(NHAL_ERR_IO, "Ordner für Nutzerdaten ließ sich nicht anlegen");
    return nhal_shell_put(path, buf, cap);
}

static nhal_status open_target(const char* target) {
    if (!target || !target[0]) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Ziel ist leer");
    if (!nhal_shell_is_url(target))
        return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Im Browser lassen sich nur URLs öffnen, keine Dateien");
    if (shell_js_open(target) != 0) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "Kein Browserfenster (z. B. node)");
    return nhal_status_ok();
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    (void)allocator;
    return nhal_status_ok();
}

static void feature_shutdown(void) {}

static const nhal_sys_shell_v1 table = {
    sizeof(nhal_sys_shell_v1), program_dir, program_dir, user_data_dir, open_target,
};

const nhal_feature_impl nhal_impl_sys_shell = { 1u, feature_init, feature_shutdown, &table };
