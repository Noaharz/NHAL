/* Mini-Prüfrahmen für die C-Tests. */
#ifndef NHAL_TEST_CHECK_H
#define NHAL_TEST_CHECK_H

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "%s:%d: CHECK(%s) fehlgeschlagen\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_OK(expr)                                                                              \
    do {                                                                                            \
        nhal_status check_s_ = (expr);                                                              \
        if (check_s_.code != NHAL_OK) {                                                             \
            fprintf(stderr, "%s:%d: %s -> %s (%s)\n", __FILE__, __LINE__, #expr,                    \
                    nhal_code_name(check_s_.code), check_s_.reason);                                \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

static int check_report(const char* name) {
    if (g_failures) {
        fprintf(stderr, "%s: %d Prüfung(en) fehlgeschlagen\n", name, g_failures);
        return 1;
    }
    printf("%s: alles in Ordnung\n", name);
    return 0;
}

#endif
