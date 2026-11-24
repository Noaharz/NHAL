/*
 * Standard-Allocator für Builds mit libc (nicht in Freestanding-Builds).
 */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nhal/nhal_core.h"

#include <stdlib.h>
#if defined(_WIN32)
#include <malloc.h>
#endif

static void* default_alloc(void* user, size_t size, size_t align) {
    (void)user;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (size == 0) size = 1;
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void* p = NULL;
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
#endif
}

static void default_free(void* user, void* ptr, size_t size) {
    (void)user;
    (void)size;
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

const nhal_allocator nhal_default_allocator = { default_alloc, default_free, NULL };
