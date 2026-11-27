/*
 * fs.file – Windows (Win32, UTF-16-Pfade).
 */
#include "nhal/features/fs/file.h"
#include "windows/fs/path.h"

struct nhal_file {
    HANDLE handle;
    bool append;
};

static const nhal_allocator* g_alloc = NULL;

#define INVALID_PATH nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist leer oder kein gültiges UTF-8")

static nhal_status file_open(const char* path, uint32_t flags, nhal_file** out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    *out = NULL;
    if (!path || !*path) return INVALID_PATH;
    const bool r = (flags & NHAL_FILE_READ) != 0;
    const bool w = (flags & (NHAL_FILE_WRITE | NHAL_FILE_APPEND)) != 0;
    if (!r && !w) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Weder READ noch WRITE angegeben");
    if ((flags & (NHAL_FILE_CREATE | NHAL_FILE_TRUNCATE)) && !w) {
        return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "CREATE/TRUNCATE nur zusammen mit WRITE");
    }
    DWORD access = (r ? GENERIC_READ : 0) | (w ? GENERIC_WRITE : 0);
    DWORD disposition;
    const bool create = (flags & NHAL_FILE_CREATE) != 0;
    const bool trunc = (flags & NHAL_FILE_TRUNCATE) != 0;
    if (create && (flags & NHAL_FILE_EXCLUSIVE)) disposition = CREATE_NEW;
    else if (create && trunc) disposition = CREATE_ALWAYS;
    else if (create) disposition = OPEN_ALWAYS;
    else if (trunc) disposition = TRUNCATE_EXISTING;
    else disposition = OPEN_EXISTING;

    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(g_alloc, path, &wbytes);
    if (!wpath) return INVALID_PATH;
    nhal_file* f = (nhal_file*)g_alloc->alloc(g_alloc->user, sizeof(nhal_file), _Alignof(nhal_file));
    if (!f) {
        nhal_win_free_wide(g_alloc, wpath, wbytes);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher für die Datei");
    }
    HANDLE h = CreateFileW(wpath, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    const DWORD err = GetLastError();
    nhal_win_free_wide(g_alloc, wpath, wbytes);
    if (h == INVALID_HANDLE_VALUE) {
        g_alloc->free(g_alloc->user, f, sizeof(nhal_file));
        return nhal_win_error(err);
    }
    f->handle = h;
    f->append = (flags & NHAL_FILE_APPEND) != 0;
    *out = f;
    return nhal_status_ok();
}

static void file_close(nhal_file* f) {
    if (!f) return;
    CloseHandle(f->handle);
    g_alloc->free(g_alloc->user, f, sizeof(nhal_file));
}

static nhal_status file_read(nhal_file* f, void* dst, size_t size, size_t* done) {
    size_t total = 0;
    while (total < size) {
        const size_t left = size - total;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : (DWORD)left;
        DWORD got = 0;
        if (!ReadFile(f->handle, (unsigned char*)dst + total, chunk, &got, NULL)) {
            if (done) *done = total;
            return nhal_win_error(GetLastError());
        }
        if (got == 0) break;
        total += got;
    }
    if (done) *done = total;
    return nhal_status_ok();
}

static nhal_status file_write(nhal_file* f, const void* src, size_t size) {
    if (f->append) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(f->handle, zero, NULL, FILE_END)) return nhal_win_error(GetLastError());
    }
    size_t total = 0;
    while (total < size) {
        const size_t left = size - total;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : (DWORD)left;
        DWORD put = 0;
        if (!WriteFile(f->handle, (const unsigned char*)src + total, chunk, &put, NULL)) {
            return nhal_win_error(GetLastError());
        }
        total += put;
    }
    return nhal_status_ok();
}

static nhal_status file_seek(nhal_file* f, int64_t offset, nhal_seek_origin origin, uint64_t* position) {
    if ((unsigned)origin > NHAL_SEEK_END) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Unbekannter Ursprung");
    const DWORD method = origin == NHAL_SEEK_SET ? FILE_BEGIN : origin == NHAL_SEEK_CUR ? FILE_CURRENT : FILE_END;
    LARGE_INTEGER dist, pos;
    dist.QuadPart = offset;
    if (!SetFilePointerEx(f->handle, dist, &pos, method)) return nhal_win_error(GetLastError());
    if (position) *position = (uint64_t)pos.QuadPart;
    return nhal_status_ok();
}

static nhal_status file_size(nhal_file* f, uint64_t* size) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(f->handle, &s)) return nhal_win_error(GetLastError());
    *size = (uint64_t)s.QuadPart;
    return nhal_status_ok();
}

static nhal_status file_sync(nhal_file* f) {
    if (!FlushFileBuffers(f->handle)) return nhal_win_error(GetLastError());
    return nhal_status_ok();
}

static nhal_status file_stat(const char* path, nhal_file_stat* out) {
    if (!path || !out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "path oder out ist NULL");
    out->exists = false;
    out->is_dir = false;
    out->size = 0;
    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(g_alloc, path, &wbytes);
    if (!wpath) return INVALID_PATH;
    WIN32_FILE_ATTRIBUTE_DATA data;
    const BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &data);
    const DWORD err = GetLastError();
    nhal_win_free_wide(g_alloc, wpath, wbytes);
    if (!ok) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return nhal_status_ok();
        return nhal_win_error(err);
    }
    out->exists = true;
    out->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = out->is_dir ? 0 : ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return nhal_status_ok();
}

static nhal_status file_list_dir(const char* path, nhal_dir_fn fn, void* user) {
    if (!path || !fn) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "path oder fn ist NULL");
    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(g_alloc, path, &wbytes);
    if (!wpath) return INVALID_PATH;
    /* "<pfad>\*" bauen */
    const size_t len = wbytes / sizeof(wchar_t) - 1;
    const size_t pbytes = (len + 3) * sizeof(wchar_t);
    wchar_t* pattern = (wchar_t*)g_alloc->alloc(g_alloc->user, pbytes, _Alignof(wchar_t));
    if (!pattern) {
        nhal_win_free_wide(g_alloc, wpath, wbytes);
        return nhal_status_error(NHAL_ERR_OUT_OF_MEMORY, "Kein Speicher");
    }
    size_t n = 0;
    for (; n < len; ++n) pattern[n] = wpath[n];
    if (n == 0 || pattern[n - 1] != L'\\') pattern[n++] = L'\\';
    pattern[n++] = L'*';
    pattern[n] = 0;
    nhal_win_free_wide(g_alloc, wpath, wbytes);

    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    const DWORD err = GetLastError();
    g_alloc->free(g_alloc->user, pattern, pbytes);
    if (h == INVALID_HANDLE_VALUE) {
        if (err == ERROR_FILE_NOT_FOUND) return nhal_status_ok(); /* leerer Ordner */
        return nhal_win_error(err);
    }
    char name[MAX_PATH * 4];
    do {
        if (data.cFileName[0] == L'.' &&
            (data.cFileName[1] == 0 || (data.cFileName[1] == L'.' && data.cFileName[2] == 0))) {
            continue;
        }
        if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, (int)sizeof(name), NULL, NULL) <= 0) {
            continue;
        }
        nhal_dir_entry entry;
        entry.name = name;
        entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = entry.is_dir ? 0 : ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        if (!fn(user, &entry)) break;
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return nhal_status_ok();
}

static nhal_status file_make_dir(const char* path) {
    if (!path || !*path) return INVALID_PATH;
    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(g_alloc, path, &wbytes);
    if (!wpath) return INVALID_PATH;
    const BOOL ok = CreateDirectoryW(wpath, NULL);
    const DWORD err = GetLastError();
    nhal_win_free_wide(g_alloc, wpath, wbytes);
    return ok ? nhal_status_ok() : nhal_win_error(err);
}

static nhal_status file_remove(const char* path) {
    if (!path || !*path) return INVALID_PATH;
    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(g_alloc, path, &wbytes);
    if (!wpath) return INVALID_PATH;
    const DWORD attr = GetFileAttributesW(wpath);
    BOOL ok;
    DWORD err;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        ok = FALSE;
        err = GetLastError();
    } else {
        ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(wpath) : DeleteFileW(wpath);
        err = GetLastError();
    }
    nhal_win_free_wide(g_alloc, wpath, wbytes);
    return ok ? nhal_status_ok() : nhal_win_error(err);
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
