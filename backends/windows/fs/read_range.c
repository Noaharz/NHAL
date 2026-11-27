/*
 * fs.async_read – Lesen eines Bereichs, Windows (ReadFile mit Offset).
 */
#include "common/fs/read_range.h"
#include "windows/fs/path.h"

nhal_status nhal_platform_read_range(const nhal_allocator* alloc, const char* path, uint64_t offset,
                                     void* dst, size_t size, size_t* bytes) {
    *bytes = 0;
    size_t wbytes = 0;
    wchar_t* wpath = nhal_win_wide(alloc, path, &wbytes);
    if (!wpath) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Pfad ist kein gültiges UTF-8");
    HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    const DWORD open_err = GetLastError();
    nhal_win_free_wide(alloc, wpath, wbytes);
    if (h == INVALID_HANDLE_VALUE) return nhal_win_error(open_err);

    size_t total = 0;
    nhal_status status = nhal_status_ok();
    while (total < size) {
        const size_t left = size - total;
        const DWORD chunk = left > 0x40000000u ? 0x40000000u : (DWORD)left;
        const uint64_t at = offset + total;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.Offset = (DWORD)(at & 0xffffffffu);
        ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD got = 0;
        if (!ReadFile(h, (unsigned char*)dst + total, chunk, &got, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_HANDLE_EOF) status = nhal_win_error(err);
            break;
        }
        if (got == 0) break;
        total += got;
    }
    CloseHandle(h);
    *bytes = total;
    return status;
}
