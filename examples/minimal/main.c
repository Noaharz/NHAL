/*
 * Kleinstes NHAL-Programm: starten, Features auflisten, Uhr benutzen.
 */
#include "nhal/nhal_core.h"
#include "nhal/features/time/monotonic.h"

#include <stdio.h>

static const char* support_name(nhal_support s) {
    switch (s) {
        case NHAL_SUPPORT_NATIVE: return "nativ";
        case NHAL_SUPPORT_EMULATED: return "emuliert";
        default: return "fehlt";
    }
}

int main(void) {
    nhal_context* ctx = NULL;
    nhal_status s = nhal_init(NULL, &ctx); /* NULL = Manifest aus nhal.toml */
    if (!nhal_ok(s)) {
        fprintf(stderr, "NHAL-Start fehlgeschlagen: %s – %s\n", nhal_feature_name(s.feature), s.reason);
        return 1;
    }

    printf("NHAL %d.%d.%d auf %s\n", NHAL_VERSION_MAJOR, NHAL_VERSION_MINOR, NHAL_VERSION_PATCH,
           nhal_platform_name());
    for (size_t i = 0; i < nhal_feature_count(); ++i) {
        nhal_feature_info info;
        nhal_feature_at(ctx, i, &info);
        printf("  %-20s v%u  %-8s %s\n", info.name, info.version, support_name(info.support),
               info.active ? "aktiv" : info.status.reason);
    }

    /* Optionales Feature: nicht im Build, aber der Grund ist abfragbar. */
    nhal_feature_info mesh;
    nhal_query(ctx, NHAL_FEATURE_GFX_MESH_SHADER, &mesh);
    printf("  %-20s       %-8s %s\n", mesh.name, support_name(mesh.support), mesh.status.reason);

    /* Tabelle einmal holen, danach direkt aufrufen. */
    const nhal_time_monotonic_v1* clock = nhal_get_time_monotonic_v1(ctx);
    const uint64_t t0 = clock->now_ns();
    volatile uint64_t sum = 0;
    for (uint64_t i = 0; i < 1000000; ++i) sum += i;
    const uint64_t t1 = clock->now_ns();
    printf("Schleife: %.3f ms (Auflösung %llu ns)\n", (double)(t1 - t0) / 1e6,
           (unsigned long long)clock->resolution_ns());

    nhal_shutdown(ctx);
    return 0;
}
