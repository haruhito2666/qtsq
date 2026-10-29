/*
 * sfb_internal.h — Sofuu Brain v2: internal ABI between modules.
 *
 * Modules (each owns one .c file, no cross-includes beyond this header):
 *   sfb_util.c    crc32c, json escape, time helpers, error strings
 *   sfb_crypto.c  AES-256-GCM seal/open, argon2 KDF wrapper
 *   sfb_store.c   manifest + sealed segments + WAL + generations + reclamation
 *   sfb_hnsw.c    HNSW graph: diversity heuristic, bidirectional repair, persistence
 *   sfb_lock.c    flock writer/reader ownership + stale-PID reclaim
 *   sfb_quant.c   INT8 scalar quantisation with per-segment scale
 *   sfb_text.c    tokenise, Jaccard similarity, BM25 scoring
 *   sfb_brain.c   orchestrator implementing sfb.h (the memory model)
 *   sfb_ops.c     verify / repair / backup / restore / migrate
 *   sfb_cli.c     main() for the `sfb` binary
 */
#ifndef SFB_INTERNAL_H
#define SFB_INTERNAL_H

/* On macOS, _POSIX_C_SOURCE hides flock()/LOCK_EX and friends; the Darwin
 * extras macro must be defined first. Found by the W2 implementer. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "sfb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── format constants ──────────────────────────────────────────────────── */
#define SFB_MAGIC_MANIFEST   0x53464232u  /* 'SFB2' */
#define SFB_MAGIC_SEGMENT    0x53465332u  /* 'SFS2' */
#define SFB_MAGIC_GRAPH      0x53464732u  /* 'SFG2' */
#define SFB_MAGIC_WAL        0x53465732u  /* 'SFW2' */
#define SFB_SEGMENT_CAP      256u         /* records per sealed segment      */
#define SFB_MANIFEST_NAME    "brain.qtsq"
#define SFB_SEG_DIR          "brain.qtsq.d"
#define SFB_WAL_NAME         "brain.qtsq.wal.json"
#define SFB_LOCK_NAME        "brain.qtsq.lock"
#define SFB_MAX_SEGS         65536u
#define SFB_MIN_DIM          64u
#define SFB_MAX_DIM          4096u
#define SFB_DEFAULT_M         16u
#define SFB_DEFAULT_M0       32u
#define SFB_DEFAULT_EFC      200u
#define SFB_DEFAULT_EFS      64u
#define SFB_GRAPH_MAX_LEVEL  32u
#define SFB_TITLE_MAX        256u

/* ── segment header flags (offset 6 of the 48-byte segment header, LE) ────
 *   bit 0  SFB_SEG_FLAG_SEALED  payload is AES-256-GCM ciphertext||tag
 *   bit 1  SFB_SEG_FLAG_INT8    payload records hold int8 codes plus a
 *                               per-record f32 scale table instead of f32
 *                               vectors (W3.6)
 * Every other bit is a payload encoding this build does not know.  The header
 * decoder rejects the whole segment with SFB_E_CORRUPT, so an older build
 * meeting an int8 segment fails loudly instead of reading int8 bytes as f32.
 * The manifest format is deliberately untouched: bit 1 lives per segment, so a
 * mixed store (old f32 segments + new int8 ones) stays readable by both. */
#define SFB_SEG_FLAG_SEALED 0x0001u
#define SFB_SEG_FLAG_INT8   0x0002u

/* record flags */
#define SFB_F_LIVE      0x01u
#define SFB_F_PINNED    0x02u
#define SFB_F_TOMBSTONE 0x04u
#define SFB_F_DIRTY     0x08u

/* tier ids (cognitive mode) */
#define SFB_TIER_WORKING   0u
#define SFB_TIER_EPISODIC  1u
#define SFB_TIER_SEMANTIC  2u
#define SFB_TIER_FORGOTTEN 3u
#define SFB_TIER_ENTITY    4u

#define SFB_PIN_ROLE "user_pin"

/* ── utilities (sfb_util.c) ────────────────────────────────────────────── */
uint32_t sfb_crc32c(const void *data, size_t len);
uint32_t sfb_crc32c_update(uint32_t seed, const void *data, size_t len);
double   sfb_now_secs(void);
uint64_t sfb_now_us(void);
int      sfb_mkdir_p(const char *path, mode_t mode);
int      sfb_write_all(int fd, const void *buf, size_t len);   /* short write ⇒ SFB_E_ENOSPC */
int      sfb_pread_all(int fd, void *buf, size_t len, off_t off);
int      sfb_fsync_dir(const char *dir);
int      sfb_rename_atomic(const char *tmp, const char *dst);
/* json helpers */
size_t   sfb_json_escape(const char *in, char *out, size_t cap);
int      sfb_snprintf(char *out, size_t cap, const char *fmt, ...);
/* path helpers */
int      sfb_path_join(char *out, size_t cap, const char *a, const char *b);
int      sfb_path_mkdir_for(const char *file);

/* ── crypto (sfb_crypto.c) ─────────────────────────────────────────────── */
#define SFB_KEY_LEN   32u
#define SFB_IV_LEN    12u
#define SFB_TAG_LEN   16u
#define SFB_SALT_LEN  16u

/* AES-256-GCM; sealed = ciphertext||tag. Returns SFB_OK or SFB_E_CORRUPT. */
int sfb_gcm_seal(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len,
                 const void *pt, size_t pt_len, void *ct_out, uint8_t tag_out[SFB_TAG_LEN]);
int sfb_gcm_open(const uint8_t key[SFB_KEY_LEN], const uint8_t iv[SFB_IV_LEN],
                 const void *aad, size_t aad_len,
                 const void *ct, size_t ct_len, const uint8_t tag[SFB_TAG_LEN], void *pt_out);
int sfb_kdf(const char *passphrase, const uint8_t salt[SFB_SALT_LEN],
            uint8_t key_out[SFB_KEY_LEN]);
int sfb_random_bytes(void *out, size_t len);
/* self-test against NIST GCM vectors; returns SFB_OK on success */
int sfb_crypto_selftest(void);

/* ── records and vectors ───────────────────────────────────────────────── */
typedef struct sfb_record {
    uint32_t id;
    uint32_t flags;
    uint32_t tier;
    uint32_t text_off, text_len;
    uint32_t role_off, role_len;
    float    strength;
    float    distance_hint;
    double   created_at;
    double   last_hit;
    uint32_t crc;          /* per-record CRC32C, persisted (W3.5)            */
} sfb_record_t;

/* ── HNSW (sfb_hnsw.c) ─────────────────────────────────────────────────── */
typedef struct sfb_hnsw_hit { uint32_t id; float dist; } sfb_hnsw_hit_t;

typedef struct sfb_hnsw sfb_hnsw_t;

int  sfb_hnsw_init(sfb_hnsw_t **out, uint32_t dim, uint32_t m, uint32_t m0,
                   uint32_t ef_construction);
void sfb_hnsw_free(sfb_hnsw_t *h);
int  sfb_hnsw_add(sfb_hnsw_t *h, uint32_t id, const float *vec);
int  sfb_hnsw_search(const sfb_hnsw_t *h, const float *query, uint32_t k,
                     uint32_t ef, sfb_hnsw_hit_t *out, uint32_t *n_out);
int  sfb_hnsw_mark_deleted(sfb_hnsw_t *h, uint32_t id);
int  sfb_hnsw_undelete(sfb_hnsw_t *h, uint32_t id);
uint64_t sfb_hnsw_count(const sfb_hnsw_t *h);
/* persistence: graph-<gen>.bin, mmap on load (W3.1) */
int  sfb_hnsw_save(const sfb_hnsw_t *h, const char *path, uint64_t generation);
/* graph file is topology only; vectors are re-attached from segment replay */
int  sfb_hnsw_attach_vectors(sfb_hnsw_t *h, const float *vecs, uint32_t n);
/* zero-copy load: all graph arrays point into the mapping */
int  sfb_hnsw_map(sfb_hnsw_t **out, const char *path, uint32_t dim,
                  uint32_t m, uint32_t m0, uint32_t ef_construction);
int  sfb_hnsw_load(sfb_hnsw_t **out, const char *path, uint32_t dim,
                   uint32_t m, uint32_t m0, uint32_t ef_construction);

/* ── store (sfb_store.c) ───────────────────────────────────────────────── */
typedef struct sfb_seg_entry {
    uint32_t gen, seq;
    uint32_t records;
    uint64_t length;
    uint32_t digest;
    uint32_t min_id, max_id;
} sfb_seg_entry_t;

typedef struct sfb_manifest {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t dim;
    uint32_t count;
    uint64_t generation;
    uint64_t graph_gen;
    uint32_t graph_digest;
    uint32_t nseg;
    uint64_t total_bytes;
    sfb_seg_entry_t *segs;
    uint32_t crc;          /* CRC32C over the header + segment table (W1.4) */
} sfb_manifest_t;

typedef struct sfb_store {
    char     dir[4096];
    char     manifest_path[4096];
    char     seg_dir[4096];
    char     wal_path[4096];
    char     lock_path[4096];
    sfb_manifest_t man;
    uint8_t  key[SFB_KEY_LEN];
    int      encrypted;
    int      readonly;
    int      quantize_int8;   /* writer: seal int8 payloads (W3.6)             */
    uint32_t dim;
    uint64_t records_dropped, segments_quarantined, ids_renumbered, index_rebuilt;
    uint64_t bytes_written;
} sfb_store_t;

int  sfb_store_open(sfb_store_t *s, const sfb_config_t *cfg, int *fresh);
int  sfb_store_close(sfb_store_t *s);
int  sfb_store_manifest_save(sfb_store_t *s);
int  sfb_store_manifest_load(sfb_store_t *s);
/* append one sealed segment; data is the serialised record block */
int  sfb_store_segment_write(sfb_store_t *s, const void *data, size_t len,
                             uint32_t records, uint32_t min_id, uint32_t max_id,
                             uint32_t *out_seq);
int  sfb_store_segment_read(const sfb_store_t *s, const sfb_seg_entry_t *e,
                            void **out, size_t *len);
/* Same, but also reports the segment header flags (SFB_SEG_FLAG_*), which is
 * how the record-block reader learns whether the plaintext payload is f32 or
 * int8.  flags_out may be NULL. */
int  sfb_store_segment_read_ex(const sfb_store_t *s, const sfb_seg_entry_t *e,
                               void **out, size_t *len, uint16_t *flags_out);
int  sfb_store_segment_verify(const sfb_store_t *s, const sfb_seg_entry_t *e);
int  sfb_store_quarantine(sfb_store_t *s, const sfb_seg_entry_t *e);
/* reclamation: drop generations no longer referenced (W3.4) */
int  sfb_store_reclaim(sfb_store_t *s, uint64_t *files_removed);
uint64_t sfb_store_bytes_on_disk(const sfb_store_t *s);
int  sfb_store_wal_begin(sfb_store_t *s, const char *intent, size_t len, uint32_t digest);
int  sfb_store_wal_commit(sfb_store_t *s);
int  sfb_store_wal_rollback(sfb_store_t *s);

/* ── lock (sfb_lock.c) ─────────────────────────────────────────────────── */
typedef struct sfb_lock {
    int  fd;
    char path[4096];
    int  shared;
    pid_t pid;
} sfb_lock_t;

int sfb_lock_acquire(sfb_lock_t *l, const char *path, int shared, int no_lock);
int sfb_lock_release(sfb_lock_t *l);
int sfb_lock_holder(const char *path, pid_t *pid, char *started_at, size_t cap);

/* ── quantisation (sfb_quant.c) ────────────────────────────────────────── */
typedef struct sfb_quant_block {
    int8_t  *codes;      /* dim bytes per vector                           */
    float    scale;      /* per block                                      */
    uint32_t n, dim;
} sfb_quant_block_t;

int sfb_quant_encode(sfb_quant_block_t *b, const float *vecs, uint32_t n, uint32_t dim);
int sfb_quant_decode(const sfb_quant_block_t *b, uint32_t i, float *out);
int sfb_quant_distance(const sfb_quant_block_t *b, uint32_t i, const float *q, float *out);
void sfb_quant_free(sfb_quant_block_t *b);

/* Per-record form (W3.6), the one the segment payload uses: ONE f32 scale per
 * record, scale = max|v| / 127, code = clamp(lrint(v / scale), -127, 127).
 * Decoding is v' = code * scale in f32, so a round trip is exact and
 * deterministic: encode(decode(codes, scale)) reproduces scale and codes. */
int sfb_quant_encode_vec(const float *v, uint32_t dim, int8_t *codes,
                         float *scale_out);
int sfb_quant_decode_vec(const int8_t *codes, uint32_t dim, float scale,
                         float *out);

/* ── text (sfb_text.c) ─────────────────────────────────────────────────── */
double sfb_text_jaccard(const char *a, const char *b);
double sfb_text_bm25(const char *query, const char *doc, double avg_len, double k1, double b);
uint64_t sfb_text_hash(const char *s);

/* ── the brain (sfb_brain.c) ───────────────────────────────────────────── */
/* The vector store holds exactly one copy per row of whatever the brain is
 * configured to keep:
 *   quantize_int8 == 0: vecs[i*dim .. +dim) is f32  → 4*dim bytes/row
 *   quantize_int8 == 1: qcodes[i*dim .. +dim) + qscale[i] → dim+4 bytes/row,
 *                       and `vecs` stays NULL.  Rows are dequantised on demand
 *                       into a caller-supplied scratch buffer, so the corpus
 *                       never has a second f32 copy (W3.6). */
typedef struct sfb_vector_store {
    float   *vecs;           /* dim * n; NULL whenever quantized is set        */
    uint32_t n, cap, dim;
    int8_t  *qcodes;         /* dim * cap int8 codes, only when quantized      */
    float   *qscale;         /* cap per-row scales, only when quantized        */
    int      quantized;
} sfb_vector_store_t;

struct sfb_brain {
    sfb_config_t       cfg;
    sfb_store_t        store;
    sfb_lock_t         lock;
    sfb_hnsw_t        *index;
    sfb_vector_store_t vecs;

    sfb_record_t      *recs;
    uint32_t           nrec, crec;

    char              *arena;    /* text + role bytes                         */
    size_t             arena_len, arena_cap;

    uint32_t          *text_index;   /* hash -> id chain heads (open addressing) */
    uint32_t           text_cap;

    uint32_t           dirty_head, dirty_count;
    uint32_t          *dirty_ids;

    sfb_stats_t        stats;
    sfb_event_cb       event_cb;
    void              *event_ud;
    int                event_tail_text;
};

/* shared internals used by sfb_ops.c */
int  sfb_brain_load_all(sfb_brain_t *b);
/* seg_flags: the SFB_SEG_FLAG_* bits of the segment the payload came from.
 * SFB_SEG_FLAG_INT8 selects the int8 record-block layout. */
int  sfb_brain_replay_segment_ex(sfb_brain_t *b, const void *data, size_t len,
                                 uint32_t records, uint16_t seg_flags);
/* float32 payload form (flag 0), kept for existing callers */
int  sfb_brain_replay_segment(sfb_brain_t *b, const void *data, size_t len,
                              uint32_t records);
int  sfb_brain_serialize_dirty(sfb_brain_t *b, void **out, size_t *len,
                               uint32_t *n_records, uint32_t *min_id, uint32_t *max_id);

#endif /* SFB_INTERNAL_H */
