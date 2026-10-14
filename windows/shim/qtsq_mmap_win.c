/* qtsq_mmap_win.c — QTSQ Turbo Layer 1: Memory-Mapped I/O, Windows backend.
 *
 * Mirrors the POSIX implementation in src/qtsq_turbo.c (QTSQ_MMAP_POSIX
 * block) exactly: same return codes (QTSQ_OK / QTSQ_ERR_NULL / QTSQ_ERR_IO /
 * QTSQ_ERR_FORMAT), same zero-then-fill contract on qtsq_mmap_t.
 *
 * Compile with QTSQ_MMAP_WIN defined so the `map_handle` field in
 * include/qtsq_turbo.h (qtsq_mmap_t) exists; it stores a small heap
 * qtsq_win_mmap_handles struct holding both the file and the mapping
 * HANDLEs (an `int fd` cannot safely hold a 64-bit HANDLE).
 *
 * Caller contract (src/qtsq_turbo.c qtsq_turbo_read): any nonzero return
 * falls back to the stdio qtsq_read() path, so a graceful failure here is
 * safe by construction.
 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "qtsq_turbo.h"

typedef struct {
    HANDLE file;    /* hFile */
    HANDLE mapping; /* hFileMappingObject */
} qtsq_win_mmap_handles;

int qtsq_mmap_open_read(qtsq_mmap_t *m, const char *filename)
{
    qtsq_win_mmap_handles *h;
    LARGE_INTEGER fsize;
    BOOL ok_size;

    if (!m || !filename)
        return QTSQ_ERR_NULL;
    memset(m, 0, sizeof(*m));
    m->fd = -1;

    h = (qtsq_win_mmap_handles *)calloc(1, sizeof(*h));
    if (!h)
        return QTSQ_ERR_IO;

    h->file = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h->file == INVALID_HANDLE_VALUE) {
        free(h);
        return QTSQ_ERR_IO;
    }
    fsize.QuadPart = 0;
    ok_size = GetFileSizeEx(h->file, &fsize);
    if (!ok_size || fsize.QuadPart <= 0) {
        CloseHandle(h->file);
        free(h);
        /* POSIX impl returns ERR_FORMAT for a zero-length file. */
        return ok_size ? QTSQ_ERR_FORMAT : QTSQ_ERR_IO;
    }
    m->size = (size_t)fsize.QuadPart;

    h->mapping = CreateFileMappingA(h->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!h->mapping) {
        CloseHandle(h->file);
        free(h);
        return QTSQ_ERR_IO;
    }
    m->data = MapViewOfFile(h->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!m->data) {
        CloseHandle(h->mapping);
        CloseHandle(h->file);
        free(h);
        return QTSQ_ERR_IO;
    }

    m->map_handle = h;
    m->is_write = 0;
    return QTSQ_OK;
}

int qtsq_mmap_open_write(qtsq_mmap_t *m, const char *filename, size_t size)
{
    qtsq_win_mmap_handles *h;
    LARGE_INTEGER seek;

    if (!m || !filename || size == 0)
        return QTSQ_ERR_NULL;
    memset(m, 0, sizeof(*m));
    m->fd = -1;

    h = (qtsq_win_mmap_handles *)calloc(1, sizeof(*h));
    if (!h)
        return QTSQ_ERR_IO;

    /* CREATE_ALWAYS == O_RDWR|O_CREAT|O_TRUNC in the POSIX impl. */
    h->file = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h->file == INVALID_HANDLE_VALUE) {
        free(h);
        return QTSQ_ERR_IO;
    }

    /* Pre-allocate the file to `size` (ftruncate equivalent). */
    seek.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(h->file, seek, NULL, FILE_BEGIN) ||
        !SetEndOfFile(h->file)) {
        CloseHandle(h->file);
        free(h);
        return QTSQ_ERR_IO;
    }

    h->mapping = CreateFileMappingA(h->file, NULL, PAGE_READWRITE,
                                    (DWORD)((ULONGLONG)size >> 32),
                                    (DWORD)((ULONGLONG)size & 0xFFFFFFFFULL),
                                    NULL);
    if (!h->mapping) {
        CloseHandle(h->file);
        free(h);
        return QTSQ_ERR_IO;
    }
    m->data = MapViewOfFile(h->mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!m->data) {
        CloseHandle(h->mapping);
        CloseHandle(h->file);
        free(h);
        return QTSQ_ERR_IO;
    }

    m->map_handle = h;
    m->size = size;
    m->is_write = 1;
    return QTSQ_OK;
}

void qtsq_mmap_close(qtsq_mmap_t *m)
{
    qtsq_win_mmap_handles *h;

    if (!m)
        return;
    h = (qtsq_win_mmap_handles *)m->map_handle;
    if (m->data) {
        if (m->is_write) {
            /* msync(MS_SYNC) equivalent: flush view, then file buffers. */
            FlushViewOfFile(m->data, m->size);
            if (h && h->file != INVALID_HANDLE_VALUE)
                FlushFileBuffers(h->file);
        }
        UnmapViewOfFile(m->data);
    }
    if (h) {
        if (h->mapping)
            CloseHandle(h->mapping);
        if (h->file && h->file != INVALID_HANDLE_VALUE)
            CloseHandle(h->file);
        free(h);
    }
    memset(m, 0, sizeof(*m));
    m->fd = -1;
}

#endif /* _WIN32 */
