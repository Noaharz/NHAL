/*
 * Standard-Allocator für Xernel: malloc/free aus xlibc.
 *
 * xlibc garantiert 16-Byte-Ausrichtung und hat kein aligned_alloc. Deshalb
 * fordert NHAL etwas mehr an, richtet den Zeiger selbst aus und legt den
 * Originalzeiger direkt davor ab. So kommt free() ohne die Ausrichtung aus.
 * xlibc ist einthreadig, das passt zu Xernel (ein Thread pro Prozess).
 */
#include "nhal/nhal_core.h"

#include <stdlib.h>

static void* xernel_alloc(void* user, size_t size, size_t align) {
    (void)user;
    if (align < sizeof(void*)) align = sizeof(void*);
    if ((align & (align - 1)) != 0) return NULL;
    if (size == 0) size = 1;
    const size_t extra = align - 1 + sizeof(void*);
    if (size > SIZE_MAX - extra) return NULL;
    unsigned char* raw = (unsigned char*)malloc(size + extra);
    if (!raw) return NULL;
    const uintptr_t p = ((uintptr_t)(raw + sizeof(void*)) + (align - 1)) & ~(uintptr_t)(align - 1);
    ((void**)p)[-1] = raw;
    return (void*)p;
}

static void xernel_free(void* user, void* ptr, size_t size) {
    (void)user;
    (void)size;
    if (ptr) free(((void**)ptr)[-1]);
}

const nhal_allocator nhal_default_allocator = { xernel_alloc, xernel_free, NULL };
