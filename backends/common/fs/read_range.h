/*
 * Plattform-Hook für fs.async_read (intern): einen Dateibereich synchron
 * lesen. Läuft auf einem Lese-Thread (Web: in poll()).
 */
#ifndef NHAL_COMMON_FS_READ_RANGE_H
#define NHAL_COMMON_FS_READ_RANGE_H

#include "nhal/nhal_backend.h"

/* Liest bis zu `size` Bytes ab `offset` nach `dst`; *bytes < size am Dateiende. */
nhal_status nhal_platform_read_range(const nhal_allocator* alloc, const char* path, uint64_t offset,
                                     void* dst, size_t size, size_t* bytes);

#endif /* NHAL_COMMON_FS_READ_RANGE_H */
