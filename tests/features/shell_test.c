/*
 * sys.shell: Verzeichnisse und die Fehlerwege von open().
 *
 * open() wird bewusst nicht mit einem echten Ziel aufgerufen – das würde bei
 * jedem Testlauf ein Programm bzw. einen Tab öffnen. Der Ordner für
 * Nutzerdaten bekommt einen Testnamen und wird am Ende wieder entfernt.
 */
#include "nhal/features/sys/shell.h"
#include "check.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool is_dir(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void) {
    const nhal_request req = { NHAL_FEATURE_SYS_SHELL, 1u, NHAL_REQUIRED };
    const nhal_manifest m = { &req, 1 };
    const nhal_init_info info = { sizeof info, &m, NULL };
    nhal_context* ctx = NULL;
    CHECK_OK(nhal_init(&info, &ctx));
    const nhal_sys_shell_v1* Sh = nhal_get_sys_shell_v1(ctx);
    if (!Sh) return check_report("shell_test");

    char dir[1024];
    CHECK_OK(Sh->program_dir(dir, sizeof dir));
    CHECK(dir[0] == '/' && is_dir(dir));
    const size_t n = strlen(dir);
    CHECK(n == 1 || dir[n - 1] != '/');
    printf("shell_test: program_dir = %s\n", dir);
    char small[2];
    CHECK(Sh->program_dir(small, sizeof small).code == NHAL_ERR_INVALID_ARGUMENT || n == 1);
    CHECK_OK(Sh->resource_dir(dir, sizeof dir));
    CHECK(is_dir(dir));

    CHECK(Sh->user_data_dir("../x", dir, sizeof dir).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(Sh->user_data_dir("", dir, sizeof dir).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK_OK(Sh->user_data_dir("nhal-shell-test", dir, sizeof dir));
    CHECK(is_dir(dir) && strstr(dir, "nhal-shell-test") != NULL);
    rmdir(dir); /* nur den leeren Testordner, nichts sonst */

    CHECK(Sh->open(NULL).code == NHAL_ERR_INVALID_ARGUMENT);
    CHECK(Sh->open("").code == NHAL_ERR_INVALID_ARGUMENT);
#ifdef __EMSCRIPTEN__
    CHECK(Sh->open("/etc/gibt-es-nicht").code == NHAL_ERR_UNSUPPORTED);
#else
    CHECK(Sh->open("/gibt/es/sicher/nicht.nhal").code == NHAL_ERR_NOT_FOUND);
#endif
    nhal_shutdown(ctx);
    return check_report("shell_test");
}
