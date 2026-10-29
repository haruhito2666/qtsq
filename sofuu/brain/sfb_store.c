/*
 * sfb_store.c — Sofuu Brain v2: manifest, sealed segments, WAL, generations.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/).
 *
 * ── On-disk layout ──────────────────────────────────────────────────────
 *   <dir>/brain.qtsq                    manifest (atomic publish)
 *   <dir>/brain.qtsq.d/seg-<gen6>-<seq6>.qtsq   sealed, immutable segments
 *   <dir>/brain.qtsq.d/graph-<gen>.bin          HNSW snapshot (owned by hnsw)
 *   <dir>/brain.qtsq.wal.json           intent record for an in-flight publish
 *   <dir>/brain.qtsq.lock               flock ownership (owned by sfb_lock.c)
 *
 * All integers are little-endian regardless of host.  Every structure is
 * length-prefixed and CRC-terminated so that a truncated or bit-flipped file
 * is detected instead of being misread as "empty".
 *
 * MANIFEST  (CRC32C over every byte before the trailing crc)
 *   off  size
 *    0    4  magic   = SFB_MAGIC_MANIFEST
 *    4    2  version = SFB_FORMAT_VERSION (2)
 *    6    2  flags   bit0 = payloads are encrypted
 *    8    4  dim
 *   12    4  count
 *   16    8  generation
 *   24    8  graph_gen
 *   32    4  graph_digest
 *   36    4  nseg
 *   40    8  total_bytes
 *   48   32*nseg  { gen u32, seq u32, records u32, length u64,
 *                   digest u32, min_id u32, max_id u32 }
 *   ..    4  crc32c of [0, ..)
 *
 * SEGMENT   (immutable once published)
 *    0    4  magic   = SFB_MAGIC_SEGMENT
 *    4    2  version = 2
 *    6    2  flags   bit0 = payload is sealed (ciphertext||tag)
 *                    bit1 = payload records are INT8 (SFB_SEG_FLAG_INT8):
 *                           int8 codes + a per-record f32 scale table, so the
 *                           vector part of the payload is 4x smaller.  The
 *                           payload is otherwise opaque to this file; sfb_brain.c
 *                           owns the record-block layout, and a reader that
 *                           does not know bit1 rejects the segment with
 *                           SFB_E_CORRUPT instead of misparsing it.
 *                    any other bit = unknown format ⇒ SFB_E_CORRUPT
 *    8    4  dim
 *   12    4  records
 *   16    4  min_id
 *   20    4  max_id
 *   24    8  payload_len   (bytes on disk: ciphertext+tag when sealed)
 *   32    4  crc_of_payload (over the bytes as stored, i.e. ciphertext)
 *   36   12  iv  (zero when unsealed)
 *   48    payload_len  payload
 *
 *   Encryption commutes with bit1: the seal covers the whole plaintext payload
 *   (scale table included), so a sealed int8 segment is just
 *   flags = bit0|bit1 and the same open path.
 *
 *   manifest.digest = CRC32C of the whole file, header included.
 *
 * WAL
 *    0    4  magic = SFB_MAGIC_WAL
 *    4    2  version
 *    6    2  flags
 *    8    8  target -- the generation the publish intends to reach
 *   16    4  intent_len
 *   20    4  digest -- caller's digest of the bytes being published
 *   24    intent_len  intent (opaque JSON/text)
 *   ..    4  crc32c over [0, ..)
 *
 * ── rules this file guarantees ──────────────────────────────────────────
 *  R1. Append-only.  A segment file is written to <name>.tmp, fsynced,
 *      renamed into place and only then named by the manifest.  Published
 *      segments are never reopened for writing and a name is never reused.
 *  R2. Publish or perish.  The manifest is the single consistency point: it
 *      is written tmp -> fsync -> rename -> fsync(dir) and carries a CRC over
 *      everything before it, so a torn write reads as a complete old manifest
 *      or as SFB_E_CORRUPT -- never as an empty brain.
 *  R3. Generations only move forward.  A fresh store sits at generation 0
 *      (nothing published); sfb_store_segment_write() opens the next
 *      generation, so the segments and the HNSW snapshot of one flush share
 *      it (sfb_brain.c relies on the bump).  Reclamation deletes only files
 *      that no manifest entry references and that are not younger than the
 *      manifest's generation.
 *  R4. Encryption is authenticated, not assumed.  Each segment carries its
 *      own random IV, the AAD is the segment header, and open authenticates
 *      the newest segment so a wrong passphrase is reported as damage-free
 *      corruption instead of being handed to a repair path that quarantines.
 *
 * Archive Author: Haruhito
 */
#include "sfb_internal.h"

#include <dirent.h>
#include <fcntl.h>

/* ── format geometry ─────────────────────────────────────────────────────  */
#define SFB_MAN_HDR_LEN   48u
#define SFB_MAN_ENT_LEN   32u
#define SFB_SEG_HDR_LEN   48u
#define SFB_WAL_HDR_LEN   24u

#define SFB_MAN_F_ENCRYPTED 0x0001u
/* Segment flag names live in sfb_internal.h (W3.6): SFB_SEG_FLAG_SEALED and
 * SFB_SEG_FLAG_INT8.  The mask below is the complete set this build can read;
 * anything else is a format it does not understand. */
#define SFB_SEG_FLAGS_KNOWN (SFB_SEG_FLAG_SEALED | SFB_SEG_FLAG_INT8)

/* KDF salt.  The frozen manifest and sfb_store_t have no salt field, so the
 * salt is a fixed, path-independent domain separator.  It is deliberately NOT
 * derived from the directory path: a brain must stay readable after
 * backup/restore or a `mv`.  A future format bump should persist a random
 * per-brain salt in the manifest. */
static const uint8_t sfb_kdf_salt[SFB_SALT_LEN] = {
    's', 'f', 'b', '2', '-', 'k', 'd', 'f', '-',
    's', 'a', 'l', 't', '-', 'v', '2'
};

/* ── little-endian codecs ────────────────────────────────────────────────  */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xFFFFFFFFull));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static int st_errno(int e)
{
    switch (e) {
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
    case EFBIG:
        return SFB_E_ENOSPC;
    case ENOENT:
        return SFB_E_NOTFOUND;
    case ENOMEM:
        return SFB_E_NOMEM;
    case EACCES:
    case EPERM:
    case EROFS:
        return SFB_E_IO;
    default:
        return SFB_E_IO;
    }
}

/* ── paths ───────────────────────────────────────────────────────────────  */
static int store_build_paths(sfb_store_t *s, const char *dir)
{
    size_t n = strlen(dir);

    while (n > 1 && dir[n - 1] == '/')
        n--;
    if (n == 0 || n >= sizeof s->dir)
        return SFB_E_INVALID;
    memcpy(s->dir, dir, n);
    s->dir[n] = '\0';

    if (sfb_path_join(s->manifest_path, sizeof s->manifest_path,
                      s->dir, SFB_MANIFEST_NAME) != SFB_OK)
        return SFB_E_INVALID;
    if (sfb_path_join(s->seg_dir, sizeof s->seg_dir,
                      s->dir, SFB_SEG_DIR) != SFB_OK)
        return SFB_E_INVALID;
    if (sfb_path_join(s->wal_path, sizeof s->wal_path,
                      s->dir, SFB_WAL_NAME) != SFB_OK)
        return SFB_E_INVALID;
    if (sfb_path_join(s->lock_path, sizeof s->lock_path,
                      s->dir, SFB_LOCK_NAME) != SFB_OK)
        return SFB_E_INVALID;
    return SFB_OK;
}

static int seg_path(const sfb_store_t *s, uint32_t gen, uint32_t seq,
                    char *out, size_t cap, const char *suffix)
{
    char name[96];
    int rc = sfb_snprintf(name, sizeof name, "seg-%06u-%06u.qtsq%s",
                          (unsigned)gen, (unsigned)seq,
                          suffix != NULL ? suffix : "");

    if (rc != SFB_OK)
        return SFB_E_INVALID;
    return sfb_path_join(out, cap, s->seg_dir, name);
}

/* ── segment header ──────────────────────────────────────────────────────  */
typedef struct seg_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t dim;
    uint32_t records;
    uint32_t min_id;
    uint32_t max_id;
    uint64_t payload_len;
    uint32_t payload_crc;
    uint8_t  iv[SFB_IV_LEN];
} seg_hdr_t;

static void seg_hdr_encode(uint8_t out[SFB_SEG_HDR_LEN], const seg_hdr_t *h)
{
    memset(out, 0, SFB_SEG_HDR_LEN);
    put_u32(out + 0, h->magic);
    put_u16(out + 4, h->version);
    put_u16(out + 6, h->flags);
    put_u32(out + 8, h->dim);
    put_u32(out + 12, h->records);
    put_u32(out + 16, h->min_id);
    put_u32(out + 20, h->max_id);
    put_u64(out + 24, h->payload_len);
    put_u32(out + 32, h->payload_crc);
    memcpy(out + 36, h->iv, SFB_IV_LEN);
}

static int seg_hdr_decode(const uint8_t in[SFB_SEG_HDR_LEN], seg_hdr_t *h)
{
    memset(h, 0, sizeof *h);
    h->magic = get_u32(in + 0);
    h->version = get_u16(in + 4);
    h->flags = get_u16(in + 6);
    h->dim = get_u32(in + 8);
    h->records = get_u32(in + 12);
    h->min_id = get_u32(in + 16);
    h->max_id = get_u32(in + 20);
    h->payload_len = get_u64(in + 24);
    h->payload_crc = get_u32(in + 32);
    memcpy(h->iv, in + 36, SFB_IV_LEN);

    if (h->magic != SFB_MAGIC_SEGMENT)
        return SFB_E_CORRUPT;
    if (h->version != SFB_FORMAT_VERSION)
        return SFB_E_CORRUPT;
    /* Reject unknown payload encodings.  Bit 1 is the int8 record block; a
     * build that predates it has this same test with bit 1 absent from the
     * mask, so an int8 segment reads as SFB_E_CORRUPT there — a loud format
     * error, never f32 vectors decoded out of int8 bytes. */
    if (h->flags & ~SFB_SEG_FLAGS_KNOWN)
        return SFB_E_CORRUPT;
    return SFB_OK;
}

/* AAD for GCM: the on-disk header with the crc slot zeroed.
 *
 * The header carries crc_of_payload, which is only known once the ciphertext
 * exists, so the seal/open AAD is the header as written with that one field
 * cleared.  Everything that describes the payload — magic, version, flags,
 * dim, records, id range, length, IV — is authenticated; the crc slot itself
 * is covered by the manifest's whole-file digest and is checked against the
 * stored bytes before decryption. */
#ifndef SFB_NO_CRYPTO
static void seg_hdr_aad(uint8_t out[SFB_SEG_HDR_LEN], const uint8_t raw[SFB_SEG_HDR_LEN])
{
    memcpy(out, raw, SFB_SEG_HDR_LEN);
    put_u32(out + 32, 0u);
}
#endif

/* ── manifest ────────────────────────────────────────────────────────────  */
int sfb_store_manifest_save(sfb_store_t *s)
{
    uint8_t *buf = NULL;
    char tmp[4096];
    size_t total, off;
    uint32_t i;
    int fd = -1, rc;

    if (s == NULL)
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;
    if (s->man.nseg > SFB_MAX_SEGS)
        return SFB_E_CAPACITY;
    if (s->man.dim == 0 || s->man.dim > SFB_MAX_DIM)
        return SFB_E_INVALID;            /* never publish an unopenable brain */

    total = (size_t)SFB_MAN_HDR_LEN + (size_t)s->man.nseg * SFB_MAN_ENT_LEN + 4u;
    buf = (uint8_t *)calloc(1, total);
    if (buf == NULL)
        return SFB_E_NOMEM;

    put_u32(buf + 0, SFB_MAGIC_MANIFEST);
    put_u16(buf + 4, (uint16_t)SFB_FORMAT_VERSION);
    put_u16(buf + 6, s->man.flags);
    put_u32(buf + 8, s->man.dim);
    put_u32(buf + 12, s->man.count);
    put_u64(buf + 16, s->man.generation);
    put_u64(buf + 24, s->man.graph_gen);
    put_u32(buf + 32, s->man.graph_digest);
    put_u32(buf + 36, s->man.nseg);
    put_u64(buf + 40, s->man.total_bytes);

    off = SFB_MAN_HDR_LEN;
    for (i = 0; i < s->man.nseg; i++) {
        const sfb_seg_entry_t *e = &s->man.segs[i];
        uint8_t *q = buf + off;

        put_u32(q + 0, e->gen);
        put_u32(q + 4, e->seq);
        put_u32(q + 8, e->records);
        put_u64(q + 12, e->length);
        put_u32(q + 20, e->digest);
        put_u32(q + 24, e->min_id);
        put_u32(q + 28, e->max_id);
        off += SFB_MAN_ENT_LEN;
    }
    put_u32(buf + off, sfb_crc32c(buf, off));
    off += 4;
    /* off == total by construction */
    if (off != total) {
        free(buf);
        return SFB_E_STATE;
    }

    rc = sfb_snprintf(tmp, sizeof tmp, "%s.tmp.%ld", s->manifest_path,
                      (long)getpid());
    if (rc != SFB_OK) {
        free(buf);
        return SFB_E_INVALID;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
              , 0644);
    if (fd < 0) {
        rc = st_errno(errno);
        free(buf);
        return rc;
    }

    rc = sfb_write_all(fd, buf, total);
    if (rc == SFB_OK && fsync(fd) != 0)
        rc = st_errno(errno);
    if (close(fd) != 0 && rc == SFB_OK)
        rc = st_errno(errno);

    if (rc == SFB_OK)
        rc = sfb_rename_atomic(tmp, s->manifest_path);
    if (rc == SFB_OK)
        rc = sfb_fsync_dir(s->dir);
    if (rc != SFB_OK)
        (void)unlink(tmp);
    else
        s->bytes_written += (uint64_t)total;

    free(buf);
    return rc;
}

/* Loads and fully validates the manifest.  On ANY failure s->man is left
 * untouched: a damaged manifest must never look like an empty brain. */
int sfb_store_manifest_load(sfb_store_t *s)
{
    struct stat st;
    sfb_manifest_t m;
    sfb_seg_entry_t *segs = NULL;
    uint8_t *buf = NULL;
    uint32_t nseg = 0, i;
    size_t size, expect;
    int fd, rc;

    if (s == NULL)
        return SFB_E_INVALID;

    if (stat(s->manifest_path, &st) != 0)
        return (errno == ENOENT) ? SFB_E_NOTFOUND : st_errno(errno);
    if (!S_ISREG(st.st_mode))
        return SFB_E_CORRUPT;
    if (st.st_size < (off_t)(SFB_MAN_HDR_LEN + 4u))
        return SFB_E_CORRUPT;            /* truncated / zero-length          */
    if ((uint64_t)st.st_size
        > (uint64_t)SFB_MAN_HDR_LEN + (uint64_t)SFB_MAX_SEGS * SFB_MAN_ENT_LEN + 4u)
        return SFB_E_CORRUPT;

    size = (size_t)st.st_size;
    buf = (uint8_t *)malloc(size);
    if (buf == NULL)
        return SFB_E_NOMEM;

    fd = open(s->manifest_path, O_RDONLY
#ifdef O_CLOEXEC
                                 | O_CLOEXEC
#endif
              );
    if (fd < 0) {
        rc = st_errno(errno);
        free(buf);
        return rc;
    }
    rc = sfb_pread_all(fd, buf, size, 0);
    (void)close(fd);
    if (rc != SFB_OK) {
        free(buf);
        return (rc == SFB_E_CORRUPT) ? SFB_E_CORRUPT : rc;
    }

    if (get_u32(buf + 0) != SFB_MAGIC_MANIFEST
        || get_u16(buf + 4) != (uint16_t)SFB_FORMAT_VERSION) {
        free(buf);
        return SFB_E_CORRUPT;
    }
    if (get_u16(buf + 6) & ~SFB_MAN_F_ENCRYPTED) {
        free(buf);
        return SFB_E_CORRUPT;
    }

    nseg = get_u32(buf + 36);
    if (nseg > SFB_MAX_SEGS) {
        free(buf);
        return SFB_E_CORRUPT;
    }
    expect = (size_t)SFB_MAN_HDR_LEN + (size_t)nseg * SFB_MAN_ENT_LEN + 4u;
    if (size != expect) {
        free(buf);
        return SFB_E_CORRUPT;
    }
    if (sfb_crc32c(buf, size - 4u) != get_u32(buf + size - 4u)) {
        free(buf);
        return SFB_E_CORRUPT;
    }

    memset(&m, 0, sizeof m);
    m.magic = get_u32(buf + 0);
    m.version = get_u16(buf + 4);
    m.flags = get_u16(buf + 6);
    m.dim = get_u32(buf + 8);
    m.count = get_u32(buf + 12);
    m.generation = get_u64(buf + 16);
    m.graph_gen = get_u64(buf + 24);
    m.graph_digest = get_u32(buf + 32);
    m.nseg = nseg;
    m.total_bytes = get_u64(buf + 40);

    /* structural sanity AFTER the CRC passes, so these checks describe a file
     * that is internally consistent but semantically impossible. */
    if (m.dim == 0 || m.dim > SFB_MAX_DIM) {
        free(buf);
        return SFB_E_CORRUPT;
    }

    if (nseg > 0) {
        segs = (sfb_seg_entry_t *)calloc(nseg, sizeof *segs);
        if (segs == NULL) {
            free(buf);
            return SFB_E_NOMEM;
        }
        for (i = 0; i < nseg; i++) {
            const uint8_t *q = buf + SFB_MAN_HDR_LEN + (size_t)i * SFB_MAN_ENT_LEN;

            segs[i].gen = get_u32(q + 0);
            segs[i].seq = get_u32(q + 4);
            segs[i].records = get_u32(q + 8);
            segs[i].length = get_u64(q + 12);
            segs[i].digest = get_u32(q + 20);
            segs[i].min_id = get_u32(q + 24);
            segs[i].max_id = get_u32(q + 28);
            if (segs[i].seq == 0 || segs[i].length < SFB_SEG_HDR_LEN) {
                free(segs);
                free(buf);
                return SFB_E_CORRUPT;
            }
        }
    }
    m.segs = segs;
    m.crc = get_u32(buf + size - 4u);   /* the CRC that was verified        */
    free(buf);

    free(s->man.segs);
    s->man = m;
    s->dim = m.dim;
    return SFB_OK;
}

/* ── open / close ────────────────────────────────────────────────────────  */
static int count_segment_files(const char *seg_dir, uint64_t *out)
{
    DIR *d;
    struct dirent *de;
    uint64_t n = 0;

    d = opendir(seg_dir);
    if (d == NULL)
        return (errno == ENOENT) ? SFB_OK : st_errno(errno);
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "seg-", 4) == 0
            && strstr(de->d_name, ".qtsq") != NULL)
            n++;
    }
    (void)closedir(d);
    *out = n;
    return SFB_OK;
}

int sfb_store_open(sfb_store_t *s, const sfb_config_t *cfg, int *fresh)
{
    int rc, is_fresh = 0, want_enc;
    struct stat wst;

    if (s == NULL || cfg == NULL || cfg->path == NULL || *cfg->path == '\0')
        return SFB_E_INVALID;
    if (cfg->dim > SFB_MAX_DIM)
        return SFB_E_DIM;
    if (cfg->dim != 0 && cfg->dim < SFB_MIN_DIM)
        return SFB_E_DIM;

    memset(s, 0, sizeof *s);
    s->dim = cfg->dim;
    s->readonly = cfg->read_only ? 1 : 0;
    /* W3.6: what this writer puts in a segment payload.  A store opened over
     * existing f32 segments with quantize_int8 set simply starts writing int8
     * ones from the next flush; readers dispatch per segment on the header
     * flag, so mixed stores stay readable. */
    s->quantize_int8 = cfg->quantize_int8 ? 1 : 0;

    rc = store_build_paths(s, cfg->path);
    if (rc != SFB_OK)
        return rc;

    if (!s->readonly) {
        rc = sfb_mkdir_p(s->dir, 0755);
        if (rc != SFB_OK)
            return rc;
        rc = sfb_mkdir_p(s->seg_dir, 0755);
        if (rc != SFB_OK)
            return rc;
    } else if (stat(s->dir, &wst) != 0 || !S_ISDIR(wst.st_mode)) {
        return SFB_E_NOTFOUND;
    }

    rc = sfb_store_manifest_load(s);
    if (rc == SFB_E_NOTFOUND) {
        uint64_t orphans = 0;

        if (s->readonly)
            return SFB_E_NOTFOUND;
        if (cfg->dim == 0)
            return SFB_E_DIM;            /* a new brain needs its dimension  */

        /* Sealed segments without a manifest is data loss wearing a clean
         * shirt: refuse to present it as an empty brain. */
        rc = count_segment_files(s->seg_dir, &orphans);
        if (rc != SFB_OK)
            return rc;
        if (orphans > 0)
            return SFB_E_CORRUPT;

        memset(&s->man, 0, sizeof s->man);
        s->man.magic = SFB_MAGIC_MANIFEST;
        s->man.version = (uint16_t)SFB_FORMAT_VERSION;
        s->man.dim = cfg->dim;
        s->man.generation = 0;           /* nothing published yet            */
        is_fresh = 1;
    } else if (rc != SFB_OK) {
        return rc;                       /* CORRUPT stays CORRUPT            */
    } else if (cfg->dim != 0 && s->man.dim != cfg->dim) {
        return SFB_E_DIM;
    } else {
        s->dim = s->man.dim;             /* dim 0 asked for: adopt it        */
    }

    /* Encryption must match the store, in both directions: silently ignoring
     * a provided key would tell the caller their data is encrypted when it is
     * not, and reading an encrypted brain without the key cannot work. */
    want_enc = (cfg->key != NULL && *cfg->key != '\0') ? 1 : 0;
    if (!is_fresh) {
        int man_enc = (s->man.flags & SFB_MAN_F_ENCRYPTED) != 0;

        if (man_enc != want_enc)
            return SFB_E_INVALID;
    }
#ifdef SFB_NO_CRYPTO
    if (want_enc)
        return SFB_E_INVALID;            /* encryption compiled out          */
#endif
    s->encrypted = want_enc;
    if (want_enc) {
        if (sfb_kdf(cfg->key, sfb_kdf_salt, s->key) != SFB_OK) {
            memset(s->key, 0, sizeof s->key);
            s->encrypted = 0;
            return SFB_E_INVALID;
        }
        s->man.flags = (uint16_t)(s->man.flags | SFB_MAN_F_ENCRYPTED);
    } else {
        s->man.flags = (uint16_t)(s->man.flags & ~SFB_MAN_F_ENCRYPTED);
    }

    /* Interrupted publish: rename() is atomic and the manifest we just loaded
     * is authoritative, so the WAL carries no work that still needs replay.
     * Segments it referenced are either in the manifest or orphans that
     * reclaim collects.  A manifest that failed its CRC already returned
     * above — the WAL never papers over damage. */
    if (!s->readonly && stat(s->wal_path, &wst) == 0) {
        (void)unlink(s->wal_path);
        (void)sfb_fsync_dir(s->dir);
    }

    if (is_fresh) {
        rc = sfb_store_manifest_save(s);
        if (rc != SFB_OK)
            return rc;
    }

    /* A manifest cannot prove the passphrase right, and every segment failing
     * authentication would look exactly like a damaged brain — which callers
     * are entitled to repair by quarantining, i.e. by throwing the data away.
     * So authenticate the newest segment here: bytes that pass their CRC but
     * refuse to decrypt mean the key is wrong, not that the data is broken.
     * Damaged segments are left alone for the caller's recovery policy. */
    if (s->encrypted && s->man.nseg > 0) {
        const sfb_seg_entry_t *e = &s->man.segs[s->man.nseg - 1];
        void *probe = NULL;
        size_t probe_len = 0;

        if (sfb_store_segment_read(s, e, &probe, &probe_len) == SFB_OK) {
            free(probe);
        } else if (sfb_store_segment_verify(s, e) == SFB_OK) {
            memset(s->key, 0, sizeof s->key);
            s->encrypted = 0;
            return SFB_E_CORRUPT;        /* intact bytes, wrong passphrase   */
        }
    }
    if (fresh != NULL)
        *fresh = is_fresh;
    return SFB_OK;
}

int sfb_store_close(sfb_store_t *s)
{
    if (s == NULL)
        return SFB_E_INVALID;
    free(s->man.segs);
    memset(s, 0, sizeof *s);
    return SFB_OK;
}

/* ── segments ────────────────────────────────────────────────────────────  */
static uint32_t next_seq(const sfb_store_t *s, uint32_t gen)
{
    uint32_t i, max = 0;
    char path[4096];
    struct stat st;

    for (i = 0; i < s->man.nseg; i++) {
        if (s->man.segs[i].gen == gen && s->man.segs[i].seq > max)
            max = s->man.segs[i].seq;
    }
    if (max == 0xFFFFFFFFu)
        return 0;

    /* Never reuse a name that already exists on disk: a crashed flush may
     * have left an orphan, and overwriting it would corrupt the old bytes
     * before the manifest ever referenced the new ones. */
    for (i = 0; i < 1000000u; i++) {
        uint32_t cand = max + 1u + i;

        if (cand == 0)
            return 0;
        if (seg_path(s, gen, cand, path, sizeof path, NULL) != SFB_OK)
            return 0;
        if (stat(path, &st) != 0)
            return cand;
    }
    return 0;
}

int sfb_store_segment_write(sfb_store_t *s, const void *data, size_t len,
                            uint32_t records, uint32_t min_id, uint32_t max_id,
                            uint32_t *out_seq)
{
    seg_hdr_t h;
    uint8_t hb[SFB_SEG_HDR_LEN];
    uint8_t *payload = NULL;
    const uint8_t *pay;
    size_t pay_len, file_len;
    char final[4096], tmp[4096];
    uint32_t gen, seq, fcrc;
    sfb_seg_entry_t *ns;
    int fd = -1, rc;

    if (s == NULL || (data == NULL && len != 0))
        return SFB_E_INVALID;
    if (len == 0 || records == 0 || min_id > max_id)
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;
    if (s->man.nseg >= SFB_MAX_SEGS)
        return SFB_E_CAPACITY;
    if (s->man.count > 0xFFFFFFFFu - records)
        return SFB_E_CAPACITY;
    if (s->man.generation >= 0xFFFFFFFFull)
        return SFB_E_CAPACITY;           /* generation counter exhausted     */
    if (len > SIZE_MAX - SFB_SEG_HDR_LEN - SFB_TAG_LEN)
        return SFB_E_INVALID;

    /* A flush publishes: it opens a new generation and every segment and the
     * graph it writes carry that generation.  Callers rely on the bump
     * (sfb_brain.c A2); generations only ever move forward, so a segment name
     * is never reused after a failed open (W1.5). */
    gen = (uint32_t)(++s->man.generation);
    seq = next_seq(s, gen);
    if (seq == 0) {
        s->man.generation--;
        return SFB_E_CAPACITY;
    }

    memset(&h, 0, sizeof h);
    h.magic = SFB_MAGIC_SEGMENT;
    h.version = (uint16_t)SFB_FORMAT_VERSION;
    h.dim = s->dim;
    h.records = records;
    h.min_id = min_id;
    h.max_id = max_id;
    /* W3.6: the writer says what it actually put in the payload.  s->quantize_int8
     * is the brain's vector encoding, so an int8 brain only ever writes int8
     * segments; a legacy f32 store keeps writing flag-0 segments byte for byte. */
    if (s->quantize_int8)
        h.flags = (uint16_t)SFB_SEG_FLAG_INT8;

    if (s->encrypted) {
#ifdef SFB_NO_CRYPTO
        return SFB_E_INVALID;
#else
        pay_len = len + SFB_TAG_LEN;
        payload = (uint8_t *)malloc(pay_len);
        if (payload == NULL)
            return SFB_E_NOMEM;
        if (sfb_random_bytes(h.iv, SFB_IV_LEN) != SFB_OK) {
            free(payload);
            return SFB_E_IO;
        }
        /* bit1 stays as set above: sealing is orthogonal to the encoding and
         * the AAD authenticates the flag word as part of the header. */
        h.flags |= (uint16_t)SFB_SEG_FLAG_SEALED;
        h.payload_len = (uint64_t)pay_len;
        h.payload_crc = 0;
        seg_hdr_encode(hb, &h);
        {
            uint8_t aad[SFB_SEG_HDR_LEN];

            seg_hdr_aad(aad, hb);
            rc = sfb_gcm_seal(s->key, h.iv, aad, sizeof aad, data, len,
                              payload, payload + len);
            if (rc != SFB_OK) {
                free(payload);
                return (rc == SFB_E_NOMEM) ? SFB_E_NOMEM : SFB_E_IO;
            }
        }
        h.payload_crc = sfb_crc32c(payload, pay_len);
        seg_hdr_encode(hb, &h);          /* final header, crc filled in      */
        pay = payload;
#endif
    } else {
        pay_len = len;
        pay = (const uint8_t *)data;
        h.payload_len = (uint64_t)pay_len;
        h.payload_crc = sfb_crc32c(data, len);
        seg_hdr_encode(hb, &h);
    }

    file_len = (size_t)SFB_SEG_HDR_LEN + pay_len;
    fcrc = sfb_crc32c_update(sfb_crc32c(hb, SFB_SEG_HDR_LEN), pay, pay_len);

    rc = seg_path(s, gen, seq, final, sizeof final, NULL);
    if (rc != SFB_OK)
        goto out;
    rc = seg_path(s, gen, seq, tmp, sizeof tmp, ".tmp");
    if (rc != SFB_OK)
        goto out;

    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
              , 0644);
    if (fd < 0) {
        rc = st_errno(errno);
        goto out;
    }

    rc = sfb_write_all(fd, hb, SFB_SEG_HDR_LEN);
    if (rc == SFB_OK)
        rc = sfb_write_all(fd, pay, pay_len);
    if (rc == SFB_OK && fsync(fd) != 0)
        rc = st_errno(errno);
    if (close(fd) != 0 && rc == SFB_OK)
        rc = st_errno(errno);
    fd = -1;

    if (rc == SFB_OK)
        rc = sfb_rename_atomic(tmp, final);
    if (rc == SFB_OK)
        rc = sfb_fsync_dir(s->seg_dir);
    if (rc != SFB_OK) {
        (void)unlink(tmp);               /* never leave a half segment       */
        goto out;
    }

    ns = (sfb_seg_entry_t *)realloc(s->man.segs,
                                    ((size_t)s->man.nseg + 1) * sizeof *ns);
    if (ns == NULL) {
        rc = SFB_E_NOMEM;                /* sealed file stays as an orphan   */
        goto out;
    }
    s->man.segs = ns;
    memset(&ns[s->man.nseg], 0, sizeof ns[0]);
    ns[s->man.nseg].gen = gen;
    ns[s->man.nseg].seq = seq;
    ns[s->man.nseg].records = records;
    ns[s->man.nseg].length = (uint64_t)file_len;
    ns[s->man.nseg].digest = fcrc;
    ns[s->man.nseg].min_id = min_id;
    ns[s->man.nseg].max_id = max_id;
    s->man.nseg++;
    s->man.count += records;
    s->man.total_bytes += (uint64_t)file_len;

    rc = sfb_store_manifest_save(s);
    if (rc != SFB_OK) {
        /* Keep memory truthful about what is durable: the segment is not
         * referenced, so drop the entry and let reclaim collect the file. */
        s->man.nseg--;
        s->man.count -= records;
        s->man.total_bytes -= (uint64_t)file_len;
        goto out;
    }
    s->bytes_written += (uint64_t)file_len;
    if (out_seq != NULL)
        *out_seq = seq;

out:
    if (fd >= 0)
        (void)close(fd);
    free(payload);
    return rc;
}

int sfb_store_segment_read(const sfb_store_t *s, const sfb_seg_entry_t *e,
                           void **out, size_t *len)
{
    return sfb_store_segment_read_ex(s, e, out, len, NULL);
}

int sfb_store_segment_read_ex(const sfb_store_t *s, const sfb_seg_entry_t *e,
                              void **out, size_t *len, uint16_t *flags_out)
{
    uint8_t hb[SFB_SEG_HDR_LEN];
    seg_hdr_t h;
    char path[4096];
    struct stat st;
    uint8_t *pay = NULL;
    uint32_t fcrc;
    int fd = -1, rc;
#ifndef SFB_NO_CRYPTO
    uint8_t *pt = NULL;
#endif

    if (s == NULL || e == NULL || out == NULL || len == NULL)
        return SFB_E_INVALID;
    *out = NULL;
    *len = 0;
    if (flags_out != NULL)
        *flags_out = 0;

    if (e->length < SFB_SEG_HDR_LEN)
        return SFB_E_CORRUPT;

    rc = seg_path(s, e->gen, e->seq, path, sizeof path, NULL);
    if (rc != SFB_OK)
        return rc;

    fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                    | O_CLOEXEC
#endif
              );
    if (fd < 0)
        return (errno == ENOENT) ? SFB_E_NOTFOUND : st_errno(errno);

    if (fstat(fd, &st) != 0) {
        rc = st_errno(errno);
        goto out;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != e->length) {
        rc = SFB_E_CORRUPT;              /* length disagrees with manifest   */
        goto out;
    }

    rc = sfb_pread_all(fd, hb, SFB_SEG_HDR_LEN, 0);
    if (rc != SFB_OK) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    if (seg_hdr_decode(hb, &h) != SFB_OK) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    if (h.dim != s->dim || h.records != e->records
        || h.payload_len != e->length - SFB_SEG_HDR_LEN) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    if (h.payload_len > (uint64_t)SIZE_MAX - 1u) {
        rc = SFB_E_CORRUPT;
        goto out;
    }

    pay = (uint8_t *)malloc((size_t)h.payload_len + 1u);
    if (pay == NULL) {
        rc = SFB_E_NOMEM;
        goto out;
    }
    rc = (h.payload_len == 0)
             ? SFB_OK
             : sfb_pread_all(fd, pay, (size_t)h.payload_len, SFB_SEG_HDR_LEN);
    (void)close(fd);
    fd = -1;
    if (rc != SFB_OK) {
        rc = SFB_E_CORRUPT;
        goto out;
    }

    if (sfb_crc32c(pay, (size_t)h.payload_len) != h.payload_crc) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    fcrc = sfb_crc32c_update(sfb_crc32c(hb, SFB_SEG_HDR_LEN),
                             pay, (size_t)h.payload_len);
    if (fcrc != e->digest) {
        rc = SFB_E_CORRUPT;              /* manifest digest mismatch         */
        goto out;
    }

    if ((h.flags & SFB_SEG_FLAG_SEALED) != 0) {
#ifdef SFB_NO_CRYPTO
        rc = SFB_E_CORRUPT;
        goto out;
#else
        size_t ct_len;
        uint8_t aad[SFB_SEG_HDR_LEN];

        if (!s->encrypted || h.payload_len < SFB_TAG_LEN) {
            rc = SFB_E_CORRUPT;          /* sealed segment, unsealed store   */
            goto out;
        }
        ct_len = (size_t)h.payload_len - SFB_TAG_LEN;
        pt = (uint8_t *)malloc(ct_len + 1u);
        if (pt == NULL) {
            rc = SFB_E_NOMEM;
            goto out;
        }
        seg_hdr_aad(aad, hb);
        rc = sfb_gcm_open(s->key, h.iv, aad, sizeof aad, pay, ct_len,
                          pay + ct_len, pt);
        if (rc != SFB_OK) {
            free(pt);
            pt = NULL;
            rc = SFB_E_CORRUPT;          /* authentication failure           */
            goto out;
        }
        free(pay);
        *out = pt;
        *len = ct_len;
        if (flags_out != NULL)
            *flags_out = h.flags;    /* caller learns SEALED/INT8 of the plaintext */
        return SFB_OK;
#endif
    }

    if (s->encrypted && (h.flags & SFB_SEG_FLAG_SEALED) == 0) {
        rc = SFB_E_CORRUPT;              /* downgraded segment               */
        goto out;
    }

    *out = pay;
    *len = (size_t)h.payload_len;
    if (flags_out != NULL)
        *flags_out = h.flags;
    return SFB_OK;

out:
    if (fd >= 0)
        (void)close(fd);
    free(pay);
    return rc;
}

/* Structural + CRC verification with no allocation at all: safe to run over
 * every segment during a scrub.  GCM tag authentication lives in
 * sfb_store_segment_read(), which needs a plaintext buffer anyway. */
int sfb_store_segment_verify(const sfb_store_t *s, const sfb_seg_entry_t *e)
{
    uint8_t hb[SFB_SEG_HDR_LEN];
    uint8_t buf[65536];
    seg_hdr_t h;
    char path[4096];
    struct stat st;
    uint64_t done;
    uint32_t fcrc, pcrc;
    int fd = -1, rc;

    if (s == NULL || e == NULL)
        return SFB_E_INVALID;
    if (e->length < SFB_SEG_HDR_LEN)
        return SFB_E_CORRUPT;

    rc = seg_path(s, e->gen, e->seq, path, sizeof path, NULL);
    if (rc != SFB_OK)
        return rc;

    fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                    | O_CLOEXEC
#endif
              );
    if (fd < 0)
        return (errno == ENOENT) ? SFB_E_NOTFOUND : st_errno(errno);

    if (fstat(fd, &st) != 0) {
        rc = st_errno(errno);
        goto out;
    }
    if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size != e->length) {
        rc = SFB_E_CORRUPT;
        goto out;
    }

    rc = sfb_pread_all(fd, hb, SFB_SEG_HDR_LEN, 0);
    if (rc != SFB_OK) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    if (seg_hdr_decode(hb, &h) != SFB_OK) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    if (h.dim != s->dim || h.records != e->records
        || h.payload_len != e->length - SFB_SEG_HDR_LEN
        || ((h.flags & SFB_SEG_FLAG_SEALED) != 0) != (s->encrypted != 0)) {
        rc = SFB_E_CORRUPT;
        goto out;
    }

    fcrc = sfb_crc32c(hb, SFB_SEG_HDR_LEN);
    pcrc = 0;
    done = SFB_SEG_HDR_LEN;
    while (done < e->length) {
        uint64_t remaining = e->length - done;
        size_t n = (remaining > (uint64_t)sizeof buf) ? sizeof buf
                                                     : (size_t)remaining;

        rc = sfb_pread_all(fd, buf, n, (off_t)done);
        if (rc != SFB_OK) {
            rc = SFB_E_CORRUPT;          /* shrank under us                  */
            goto out;
        }
        fcrc = sfb_crc32c_update(fcrc, buf, n);
        pcrc = sfb_crc32c_update(pcrc, buf, n);
        done += (uint64_t)n;
    }

    if (pcrc != h.payload_crc || fcrc != e->digest) {
        rc = SFB_E_CORRUPT;
        goto out;
    }
    rc = SFB_OK;

out:
    if (fd >= 0)
        (void)close(fd);
    return rc;
}

int sfb_store_quarantine(sfb_store_t *s, const sfb_seg_entry_t *e)
{
    char path[4096], dest[4096];
    uint32_t i, idx = 0xFFFFFFFFu;
    int rc;

    if (s == NULL || e == NULL)
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;

    for (i = 0; i < s->man.nseg; i++) {
        if (s->man.segs[i].gen == e->gen && s->man.segs[i].seq == e->seq) {
            idx = i;
            break;
        }
    }
    if (idx == 0xFFFFFFFFu)
        return SFB_E_NOTFOUND;

    rc = seg_path(s, e->gen, e->seq, path, sizeof path, NULL);
    if (rc != SFB_OK)
        return rc;
    rc = sfb_snprintf(dest, sizeof dest, "%s.quarantine", path);
    if (rc != SFB_OK)
        return SFB_E_INVALID;

    rc = sfb_rename_atomic(path, dest);
    if (rc != SFB_OK && access(path, F_OK) == 0)
        return rc;                       /* cannot preserve the bytes        */

    s->man.count -= (s->man.count >= s->man.segs[idx].records)
                       ? s->man.segs[idx].records : s->man.count;
    s->man.total_bytes -= (s->man.total_bytes >= s->man.segs[idx].length)
                             ? s->man.segs[idx].length : s->man.total_bytes;
    memmove(&s->man.segs[idx], &s->man.segs[idx + 1],
            (size_t)(s->man.nseg - idx - 1) * sizeof s->man.segs[0]);
    s->man.nseg--;

    rc = sfb_store_manifest_save(s);
    if (rc != SFB_OK)
        return rc;

    s->segments_quarantined++;
    s->records_dropped += e->records;
    return SFB_OK;
}

/* ── reclamation (W3.4) ──────────────────────────────────────────────────
 * Delete only files that are provably unreferenced:
 *   segment: no manifest entry has (gen,seq), AND gen <= man.generation
 *            (a younger gen may belong to a flush in progress).
 *   graph:   gen != man.graph_gen, AND gen < man.generation (the newest
 *            snapshot is kept even before graph_gen has been updated).
 * ----------------------------------------------------------------────── */
static int parse_seg_name(const char *name, uint32_t *gen, uint32_t *seq)
{
    unsigned g = 0, q = 0;
    int consumed = -1;

    if (sscanf(name, "seg-%u-%u.qtsq%n", &g, &q, &consumed) != 2)
        return 0;
    if (consumed < 0 || (size_t)consumed != strlen(name))
        return 0;
    *gen = (uint32_t)g;
    *seq = (uint32_t)q;
    return 1;
}

static int parse_graph_name(const char *name, uint64_t *gen)
{
    unsigned long long g = 0;
    int consumed = -1;

    if (sscanf(name, "graph-%llu.bin%n", &g, &consumed) != 1)
        return 0;
    if (consumed < 0 || (size_t)consumed != strlen(name))
        return 0;
    *gen = (uint64_t)g;
    return 1;
}

static int is_referenced(const sfb_store_t *s, uint32_t gen, uint32_t seq)
{
    uint32_t i;

    for (i = 0; i < s->man.nseg; i++) {
        if (s->man.segs[i].gen == gen && s->man.segs[i].seq == seq)
            return 1;
    }
    return 0;
}

static int reclaim_dir(sfb_store_t *s, const char *dir, int graphs,
                       uint64_t *removed)
{
    DIR *d;
    struct dirent *de;
    char path[4096];

    d = opendir(dir);
    if (d == NULL)
        return (errno == ENOENT) ? SFB_OK : st_errno(errno);

    while ((de = readdir(d)) != NULL) {
        uint32_t g32 = 0, q32 = 0;
        uint64_t g64 = 0;
        struct stat st;

        if (!graphs) {
            if (!parse_seg_name(de->d_name, &g32, &q32))
                continue;
            if ((uint64_t)g32 > s->man.generation)
                continue;                /* younger than the manifest: keep */
            if (is_referenced(s, g32, q32))
                continue;                /* live                             */
        } else {
            if (!parse_graph_name(de->d_name, &g64))
                continue;
            if (g64 >= s->man.generation)
                continue;                /* newest snapshot: keep           */
            if (g64 == s->man.graph_gen)
                continue;                /* referenced by the manifest      */
        }

        if (sfb_path_join(path, sizeof path, dir, de->d_name) != SFB_OK)
            continue;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;                    /* never unlink a directory        */
        if (unlink(path) == 0)
            (*removed)++;
    }
    (void)closedir(d);
    return SFB_OK;
}

int sfb_store_reclaim(sfb_store_t *s, uint64_t *files_removed)
{
    uint64_t removed = 0;
    int rc;

    if (s == NULL)
        return SFB_E_INVALID;
    if (files_removed != NULL)
        *files_removed = 0;
    if (s->readonly)
        return SFB_E_READONLY;

    rc = reclaim_dir(s, s->seg_dir, 0, &removed);
    if (rc != SFB_OK)
        return rc;
    rc = reclaim_dir(s, s->seg_dir, 1, &removed);
    if (rc != SFB_OK)
        return rc;
    /* hnsw may place graph-<gen>.bin next to the manifest instead */
    if (strcmp(s->seg_dir, s->dir) != 0) {
        rc = reclaim_dir(s, s->dir, 1, &removed);
        if (rc != SFB_OK)
            return rc;
    }

    if (files_removed != NULL)
        *files_removed = removed;
    return SFB_OK;
}

uint64_t sfb_store_bytes_on_disk(const sfb_store_t *s)
{
    struct stat st;
    uint64_t total = 0;
    uint32_t i;
    char path[4096];

    if (s == NULL)
        return 0;

    if (stat(s->manifest_path, &st) == 0 && S_ISREG(st.st_mode)
        && st.st_size > 0)
        total += (uint64_t)st.st_size;

    for (i = 0; i < s->man.nseg; i++) {
        if (seg_path(s, s->man.segs[i].gen, s->man.segs[i].seq,
                     path, sizeof path, NULL) != SFB_OK)
            continue;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            total += (uint64_t)st.st_size;
    }
    return total;
}

/* ── WAL ─────────────────────────────────────────────────────────────────  */
int sfb_store_wal_begin(sfb_store_t *s, const char *intent, size_t len,
                        uint32_t digest)
{
    uint8_t *buf = NULL;
    char tmp[4096];
    size_t total;
    int fd = -1, rc;

    if (s == NULL || (intent == NULL && len != 0))
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;
    if (len > 0xFFFFFFFFu || len > SIZE_MAX - SFB_WAL_HDR_LEN - 4u)
        return SFB_E_CAPACITY;

    total = (size_t)SFB_WAL_HDR_LEN + len + 4u;
    buf = (uint8_t *)calloc(1, total);
    if (buf == NULL)
        return SFB_E_NOMEM;

    put_u32(buf + 0, SFB_MAGIC_WAL);
    put_u16(buf + 4, (uint16_t)SFB_FORMAT_VERSION);
    put_u16(buf + 6, 0u);
    put_u64(buf + 8, s->man.generation); /* target generation                */
    put_u32(buf + 16, (uint32_t)len);
    put_u32(buf + 20, digest);
    if (len > 0)
        memcpy(buf + SFB_WAL_HDR_LEN, intent, len);
    put_u32(buf + SFB_WAL_HDR_LEN + len,
            sfb_crc32c(buf, SFB_WAL_HDR_LEN + len));

    rc = sfb_snprintf(tmp, sizeof tmp, "%s.tmp", s->wal_path);
    if (rc != SFB_OK) {
        free(buf);
        return SFB_E_INVALID;
    }

    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
              , 0644);
    if (fd < 0) {
        rc = st_errno(errno);
        free(buf);
        return rc;
    }

    rc = sfb_write_all(fd, buf, total);
    if (rc == SFB_OK && fsync(fd) != 0)
        rc = st_errno(errno);            /* intent must reach the platter    */
    if (close(fd) != 0 && rc == SFB_OK)
        rc = st_errno(errno);
    free(buf);

    if (rc == SFB_OK)
        rc = sfb_rename_atomic(tmp, s->wal_path);
    if (rc == SFB_OK)
        rc = sfb_fsync_dir(s->dir);
    if (rc != SFB_OK)
        (void)unlink(tmp);
    else
        s->bytes_written += (uint64_t)total;
    return rc;
}

int sfb_store_wal_commit(sfb_store_t *s)
{
    if (s == NULL)
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;
    if (unlink(s->wal_path) != 0 && errno != ENOENT)
        return st_errno(errno);
    (void)sfb_fsync_dir(s->dir);
    return SFB_OK;
}

/* Restores nothing: rename() already made the manifest either the old or the
 * new version, and both are complete.  The caller re-reads the last good
 * manifest (sfb_store_manifest_load) to resync its in-memory state. */
int sfb_store_wal_rollback(sfb_store_t *s)
{
    if (s == NULL)
        return SFB_E_INVALID;
    if (s->readonly)
        return SFB_E_READONLY;
    if (unlink(s->wal_path) != 0 && errno != ENOENT)
        return st_errno(errno);
    (void)sfb_fsync_dir(s->dir);
    return SFB_OK;
}
