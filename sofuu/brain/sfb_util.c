/*
 * sfb_util.c — Sofuu Brain v2: CRC32C, POSIX I/O helpers, paths, JSON, errors.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/). Implements exactly the
 * utility block of sfb_internal.h; no other module may grow this file's job.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>

/* ── CRC32C ─────────────────────────────────────────────────────────────
 * Castagnoli polynomial, reflected form 0x82F63B78 (a.k.a. CRC-32/ISCSI).
 * Table is a compile-time constant: no lazy init, no data race, no branch on
 * a "have we built the table yet" flag.  check("123456789") == 0xE3069283.
 * ---------------------------------------------------------------------- */
static const uint32_t sfb_crc32c_tab[256] = {
    0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u,
    0xc79a971fu, 0x35f1141cu, 0x26a1e7e8u, 0xd4ca64ebu,
    0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu,
    0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u,
    0x105ec76fu, 0xe235446cu, 0xf165b798u, 0x030e349bu,
    0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
    0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u,
    0x5d1d08bfu, 0xaf768bbcu, 0xbc267848u, 0x4e4dfb4bu,
    0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au,
    0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u,
    0xaa64d611u, 0x580f5512u, 0x4b5fa6e6u, 0xb93425e5u,
    0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
    0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u,
    0xf779deaeu, 0x05125dadu, 0x1642ae59u, 0xe4292d5au,
    0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au,
    0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u,
    0x417b1dbcu, 0xb3109ebfu, 0xa0406d4bu, 0x522bee48u,
    0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
    0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u,
    0x0c38d26cu, 0xfe53516fu, 0xed03a29bu, 0x1f682198u,
    0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u,
    0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u,
    0xdbfc821cu, 0x2997011fu, 0x3ac7f2ebu, 0xc8ac71e8u,
    0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
    0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u,
    0xa65c047du, 0x5437877eu, 0x4767748au, 0xb50cf789u,
    0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u,
    0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u,
    0x7198540du, 0x83f3d70eu, 0x90a324fau, 0x62c8a7f9u,
    0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
    0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u,
    0x3cdb9bddu, 0xceb018deu, 0xdde0eb2au, 0x2f8b6829u,
    0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu,
    0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u,
    0x082f63b7u, 0xfa44e0b4u, 0xe9141340u, 0x1b7f9043u,
    0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
    0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u,
    0x55326b08u, 0xa759e80bu, 0xb4091bffu, 0x466298fcu,
    0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu,
    0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u,
    0xa24bb5a6u, 0x502036a5u, 0x4370c551u, 0xb11b4652u,
    0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
    0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du,
    0xef087a76u, 0x1d63f975u, 0x0e330a81u, 0xfc588982u,
    0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du,
    0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u,
    0x38cc2a06u, 0xcaa7a905u, 0xd9f75af1u, 0x2b9cd9f2u,
    0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
    0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u,
    0x0417b1dbu, 0xf67c32d8u, 0xe52cc12cu, 0x1747422fu,
    0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu,
    0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u,
    0xd3d3e1abu, 0x21b862a8u, 0x32e8915cu, 0xc083125fu,
    0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
    0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u,
    0x9e902e7bu, 0x6cfbad78u, 0x7fab5e8cu, 0x8dc0dd8fu,
    0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu,
    0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u,
    0x69e9f0d5u, 0x9b8273d6u, 0x88d28022u, 0x7ab90321u,
    0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
    0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u,
    0x34f4f86au, 0xc69f7b69u, 0xd5cf889du, 0x27a40b9eu,
    0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu,
    0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u
};

uint32_t sfb_crc32c_update(uint32_t seed, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t c = seed ^ 0xFFFFFFFFu;

    if (p == NULL)
        return (len == 0) ? (c ^ 0xFFFFFFFFu) : 0u;

    while (len-- > 0)
        c = sfb_crc32c_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);

    return c ^ 0xFFFFFFFFu;
}

uint32_t sfb_crc32c(const void *data, size_t len)
{
    if (data == NULL && len != 0)
        return 0u;
    return sfb_crc32c_update(0u, data, len);
}

/* ── time ───────────────────────────────────────────────────────────────
 * Wall-clock, not monotonic: decay and record ages are wall-clock concepts
 * and must survive a reboot the way a file's mtime does.
 * ---------------------------------------------------------------------- */
double sfb_now_secs(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

uint64_t sfb_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ── errno mapping ──────────────────────────────────────────────────────── */
static int sfb_errno_code(int e, int eof_code)
{
    switch (e) {
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
    case EFBIG:
        return SFB_E_ENOSPC;
    case ENOENT:
        return eof_code ? eof_code : SFB_E_NOTFOUND;
    case ENOMEM:
        return SFB_E_NOMEM;
    case EACCES:
    case EPERM:
    case EROFS:
    case EIO:
    case ENXIO:
    case EISDIR:
    case ENOTDIR:
    case ENAMETOOLONG:
    case ELOOP:
    default:
        return SFB_E_IO;
    }
}

/* ── directories ─────────────────────────────────────────────────────────
 * mkdir -p.  Idempotent; an existing non-directory in the way is an error.
 * ---------------------------------------------------------------------- */
int sfb_mkdir_p(const char *path, mode_t mode)
{
    char buf[4096];
    struct stat st;
    size_t n;
    char *p;
    int rc;

    if (path == NULL || *path == '\0')
        return SFB_E_INVALID;

    n = strlen(path);
    if (n >= sizeof buf)
        return SFB_E_INVALID;           /* path longer than any brain dir     */
    memcpy(buf, path, n + 1);

    /* strip trailing separators, keep a bare "/" intact */
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0';

    if (stat(buf, &st) == 0)
        return S_ISDIR(st.st_mode) ? SFB_OK : SFB_E_IO;

    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST) {
            rc = sfb_errno_code(errno, 0);
            *p = '/';
            return rc;
        }
        *p = '/';
    }

    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return sfb_errno_code(errno, 0);

    if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode))
        return SFB_E_IO;
    return SFB_OK;
}

/* ── writes ──────────────────────────────────────────────────────────────
 * sfb_write_all: a *short write means the destination ran out of room*.
 * POSIX allows write() to return less than asked at any time; when the
 * remainder then fails with ENOSPC/EFBIG/EDQUOT — or with EAGAIN on a
 * non-blocking descriptor, i.e. the sink accepted a prefix and cannot take
 * the rest — the caller must hear SFB_E_ENOSPC, never a silent success.
 * ---------------------------------------------------------------------- */
int sfb_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;

    if (fd < 0)
        return SFB_E_INVALID;
    if (buf == NULL && len != 0)
        return SFB_E_INVALID;
    if (len == 0)
        return SFB_OK;

    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ENOSPC || errno == EFBIG
#ifdef EDQUOT
                || errno == EDQUOT
#endif
            )
                return SFB_E_ENOSPC;
            /* accepted a prefix, then stopped: that is a short write */
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && done > 0)
                return SFB_E_ENOSPC;
            return SFB_E_IO;
        }
        if (w == 0)
            return SFB_E_ENOSPC;        /* no progress at all: treat as full  */
        done += (size_t)w;
    }
    return SFB_OK;
}

/* sfb_pread_all: a read that ends before len bytes means the file is shorter
 * than the caller's contract — that is corruption of a length-prefixed
 * format, not a transient I/O error.  Never returns a half-filled buffer. */
int sfb_pread_all(int fd, void *buf, size_t len, off_t off)
{
    char *p = (char *)buf;
    size_t done = 0;

    if (fd < 0)
        return SFB_E_INVALID;
    if (buf == NULL && len != 0)
        return SFB_E_INVALID;
    if (len == 0)
        return SFB_OK;

    while (done < len) {
        ssize_t r = pread(fd, p + done, len - done, off + (off_t)done);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return sfb_errno_code(errno, 0);
        }
        if (r == 0)
            return SFB_E_CORRUPT;       /* premature EOF                     */
        done += (size_t)r;
    }
    return SFB_OK;
}

int sfb_fsync_dir(const char *dir)
{
    int flags = O_RDONLY;
    int fd, rc = SFB_OK;

    if (dir == NULL || *dir == '\0')
        return SFB_E_INVALID;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(dir, flags);
    if (fd < 0)
        return sfb_errno_code(errno, 0);

    if (fsync(fd) != 0) {
        /* Some filesystems (and some FUSE/virtual mounts) cannot flush a
         * directory handle; EINVAL/ENOTSUP there means "nothing to do",
         * not "the rename is lost".  Any other failure is real. */
        int e = errno;
        if (e != EINVAL && e != ENOTSUP && e != EBADF)
            rc = sfb_errno_code(e, 0);
    }
    if (close(fd) != 0 && rc == SFB_OK)
        rc = sfb_errno_code(errno, 0);
    return rc;
}

/* POSIX rename(2) is atomic within a filesystem; durability additionally
 * needs sfb_fsync_dir() on the destination directory. */
int sfb_rename_atomic(const char *tmp, const char *dst)
{
    if (tmp == NULL || *tmp == '\0' || dst == NULL || *dst == '\0')
        return SFB_E_INVALID;
    if (rename(tmp, dst) != 0)
        return sfb_errno_code(errno, 0);
    return SFB_OK;
}

/* ── JSON ────────────────────────────────────────────────────────────────
 * Escapes ", \\, and every control byte below 0x20 (short escapes where
 * JSON defines them, \u00XX otherwise).  Bytes >= 0x20 — including UTF-8 and
 * DEL — pass through untouched; JSON only requires escaping U+0000..U+001F.
 * ---------------------------------------------------------------------- */
size_t sfb_json_escape(const char *in, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t w = 0;

    if (out == NULL || cap == 0)
        return 0;
    out[0] = '\0';
    if (p == NULL)
        return 0;

    while (*p != '\0') {
        char ubuf[8];
        const char *rep = NULL;
        size_t rl;

        switch (*p) {
        case '"':  rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n";  break;
        case '\r': rep = "\\r";  break;
        case '\t': rep = "\\t";  break;
        case '\b': rep = "\\b";  break;
        case '\f': rep = "\\f";  break;
        default:
            if (*p < 0x20) {
                (void)snprintf(ubuf, sizeof ubuf, "\\u%04x", (unsigned)*p);
                rep = ubuf;
            }
            break;
        }

        if (rep != NULL) {
            rl = strlen(rep);
            if (w + rl + 1 > cap)
                break;                  /* never emit a partial escape      */
            memcpy(out + w, rep, rl);
            w += rl;
        } else {
            if (w + 1 + 1 > cap)
                break;
            out[w++] = (char)*p;
        }
        p++;
    }
    out[w] = '\0';
    return w;
}

/* vsnprintf with typed results: SFB_OK when the whole string fit,
 * SFB_E_CAPACITY when it was truncated (output still NUL-terminated). */
int sfb_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (out == NULL || cap == 0 || fmt == NULL)
        return SFB_E_INVALID;

    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);

    if (n < 0) {
        out[0] = '\0';
        return SFB_E_INVALID;
    }
    if ((size_t)n >= cap)
        return SFB_E_CAPACITY;
    return SFB_OK;
}

/* ── paths ───────────────────────────────────────────────────────────────
 * sfb_path_join("/a/b/", "/c") == "/a/b/c" — one separator, always.
 * ---------------------------------------------------------------------- */
int sfb_path_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t al, bl, w;

    if (out == NULL || cap == 0 || a == NULL)
        return SFB_E_INVALID;

    al = strlen(a);
    while (al > 1 && a[al - 1] == '/')
        al--;                            /* "/" stays "/"                    */

    bl = b != NULL ? strlen(b) : 0;
    while (bl > 0 && b[0] == '/') {
        b++;
        bl--;
    }

    if (bl == 0) {
        if (al + 1 > cap)
            return SFB_E_CAPACITY;
        memcpy(out, a, al);
        out[al] = '\0';
        return SFB_OK;
    }

    if (al + 1 + bl + 1 > cap)
        return SFB_E_CAPACITY;

    memcpy(out, a, al);
    w = al;
    if (w == 0 || out[w - 1] != '/')
        out[w++] = '/';
    memcpy(out + w, b, bl);
    w += bl;
    out[w] = '\0';
    return SFB_OK;
}

/* Create the parent directory of `file` (mkdir -p semantics). */
int sfb_path_mkdir_for(const char *file)
{
    char stack[512];
    char *dir = stack;
    const char *slash;
    size_t n;
    int rc;

    if (file == NULL || *file == '\0')
        return SFB_E_INVALID;

    slash = strrchr(file, '/');
    if (slash == NULL || slash == file)
        return SFB_OK;                   /* no directory component           */

    n = (size_t)(slash - file);
    if (n + 1 > sizeof stack) {
        dir = (char *)malloc(n + 1);
        if (dir == NULL)
            return SFB_E_NOMEM;
    }
    memcpy(dir, file, n);
    dir[n] = '\0';
    rc = sfb_mkdir_p(dir, 0755);
    if (dir != stack)
        free(dir);
    return rc;
}

/* ── error strings ───────────────────────────────────────────────────────  */
const char *sfb_strerror(int code)
{
    switch (code) {
    case SFB_OK:           return "ok";
    case SFB_E_INVALID:    return "invalid argument or configuration";
    case SFB_E_IO:         return "I/O failure";
    case SFB_E_NOMEM:      return "out of memory";
    case SFB_E_NOTFOUND:   return "not found";
    case SFB_E_STATE:      return "invalid state for this call";
    case SFB_E_DIM:        return "vector dimension mismatch";
    case SFB_E_TYPE:       return "element type rejected (f32 only)";
    case SFB_E_NONFINITE:  return "non-finite value in vector (NaN or Inf)";
    case SFB_E_STALE:      return "lock file names a dead holder";
    case SFB_E_LOCKED:     return "another process owns the brain";
    case SFB_E_CORRUPT:    return "manifest or segment failed integrity check";
    case SFB_E_ENOSPC:     return "out of space (short write, ENOSPC, EFBIG)";
    case SFB_E_READONLY:   return "mutation attempted on a read-only brain";
    case SFB_E_CAPACITY:   return "hard capacity reached, insert refused";
    default:               return "unknown sfb error";
    }
}
