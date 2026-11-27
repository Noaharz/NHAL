/*
 * fs.async_read – Lesen eines Bereichs, POSIX (open + pread).
 */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

#include "common/fs/read_range.h"
#include "posix/fs/errors.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

nhal_status nhal_platform_read_range(const nhal_allocator* alloc, const char* path, uint64_t offset,
                                     void* dst, size_t size, size_t* bytes) {
    (void)alloc;
    *bytes = 0;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd;
    do {
        fd = open(path, flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return nhal_posix_error(errno);
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist ein Ordner");
    }
    size_t total = 0;
    nhal_status status = nhal_status_ok();
    while (total < size) {
        const ssize_t n = pread(fd, (unsigned char*)dst + total, size - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            status = nhal_posix_error(errno);
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    *bytes = total;
    return status;
}
