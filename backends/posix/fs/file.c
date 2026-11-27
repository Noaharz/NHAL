/*
 * fs.file – POSIX (macOS, Linux, Web/Emscripten).
 */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

#include "nhal/features/fs/file.h"
#include "posix/fs/errors.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct nhal_file {
    int fd;
};

static const nhal_allocator* g_alloc = NULL;

static nhal_status file_open(const char* path, uint32_t flags, nhal_file** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (!path || !*path) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist leer");
    const bool r = (flags & NHAL_FILE_READ) != 0;
    const bool w = (flags & (NHAL_FILE_WRITE | NHAL_FILE_APPEND)) != 0;
    if (!r && !w) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Weder READ noch WRITE angegeben");
    if ((flags & (NHAL_FILE_CREATE | NHAL_FILE_TRUNCATE)) && !w) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "CREATE/TRUNCATE nur zusammen mit WRITE");
    }
    int o = r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY;
    if (flags & NHAL_FILE_CREATE) o |= O_CREAT;
    if (flags & NHAL_FILE_TRUNCATE) o |= O_TRUNC;
    if (flags & NHAL_FILE_APPEND) o |= O_APPEND;
    if (flags & NHAL_FILE_EXCLUSIVE) o |= O_EXCL;
#ifdef O_CLOEXEC
    o |= O_CLOEXEC;
#endif
    nhal_file* f = (nhal_file*)g_alloc->alloc(g_alloc->user, sizeof(nhal_file), _Alignof(nhal_file));
    if (!f) return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Datei");
    int fd;
    do {
        fd = open(path, o, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const nhal_status s = nhal_posix_error(errno);
        g_alloc->free(g_alloc->user, f, sizeof(nhal_file));
        return s;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        g_alloc->free(g_alloc->user, f, sizeof(nhal_file));
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist ein Ordner");
    }
    f->fd = fd;
    *out = f;
    return nhal_status_ok();
}

static void file_close(nhal_file* f) {
    if (!f) return;
    close(f->fd);
    g_alloc->free(g_alloc->user, f, sizeof(nhal_file));
}

static nhal_status file_read(nhal_file* f, void* dst, size_t size, size_t* done) {
    size_t total = 0;
    while (total < size) {
        const ssize_t n = read(f->fd, (unsigned char*)dst + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (done) *done = total;
            return nhal_posix_error(errno);
        }
        if (n == 0) break; /* Dateiende */
        total += (size_t)n;
    }
    if (done) *done = total;
    return nhal_status_ok();
}

static nhal_status file_write(nhal_file* f, const void* src, size_t size) {
    size_t total = 0;
    while (total < size) {
        const ssize_t n = write(f->fd, (const unsigned char*)src + total, size - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return nhal_posix_error(errno);
        }
        total += (size_t)n;
    }
    return nhal_status_ok();
}

static nhal_status file_seek(nhal_file* f, int64_t offset, nhal_seek_origin origin, uint64_t* position) {
    const int whence = origin == NHAL_SEEK_SET ? SEEK_SET : origin == NHAL_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if ((unsigned)origin > NHAL_SEEK_END) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Unbekannter Ursprung");
    const off_t pos = lseek(f->fd, (off_t)offset, whence);
    if (pos < 0) return nhal_posix_error(errno);
    if (position) *position = (uint64_t)pos;
    return nhal_status_ok();
}

static nhal_status file_size(nhal_file* f, uint64_t* size) {
    struct stat st;
    if (fstat(f->fd, &st) != 0) return nhal_posix_error(errno);
    *size = (uint64_t)st.st_size;
    return nhal_status_ok();
}

static nhal_status file_sync(nhal_file* f) {
    if (fsync(f->fd) != 0) return nhal_posix_error(errno);
    return nhal_status_ok();
}

static nhal_status file_stat(const char* path, nhal_file_stat* out) {
    if (!path || !out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "path oder out ist NULL");
    struct stat st;
    out->exists = false;
    out->is_dir = false;
    out->size = 0;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return nhal_status_ok();
        return nhal_posix_error(errno);
    }
    out->exists = true;
    out->is_dir = S_ISDIR(st.st_mode);
    out->size = out->is_dir ? 0 : (uint64_t)st.st_size;
    return nhal_status_ok();
}

static nhal_status file_list_dir(const char* path, nhal_dir_fn fn, void* user) {
    if (!path || !fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "path oder fn ist NULL");
    DIR* d = opendir(path);
    if (!d) return nhal_posix_error(errno);
    const int dfd = dirfd(d);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        nhal_dir_entry entry = { e->d_name, false, 0 };
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, 0) == 0) {
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size = entry.is_dir ? 0 : (uint64_t)st.st_size;
        }
        if (!fn(user, &entry)) break;
    }
    closedir(d);
    return nhal_status_ok();
}

static nhal_status file_make_dir(const char* path) {
    if (!path || !*path) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist leer");
    if (mkdir(path, 0755) != 0) return nhal_posix_error(errno);
    return nhal_status_ok();
}

static nhal_status file_remove(const char* path) {
    if (!path || !*path) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist leer");
    if (remove(path) != 0) return nhal_posix_error(errno);
    return nhal_status_ok();
}

static nhal_status init(const nhal_allocator* allocator) {
    g_alloc = allocator;
    return nhal_status_ok();
}

static void shutdown(void) { g_alloc = NULL; }

static const nhal_fs_file_v1 table = {
    sizeof(nhal_fs_file_v1), file_open, file_close, file_read, file_write, file_seek,
    file_size, file_sync, file_stat, file_list_dir, file_make_dir, file_remove,
};

const nhal_feature_impl nhal_impl_fs_file = { 1u, init, shutdown, &table };
