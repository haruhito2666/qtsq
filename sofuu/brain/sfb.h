/*
 * sfb.h — Sofuu Brain v2: public C API
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/). Isolated namespace `sfb_`.
 * Builds on QTSQ's vendored crypto (AES-256-CTR + argon2) but does NOT modify,
 * include-from, or depend on QTSQ core headers beyond crypto primitives.
 *
 * Author: Haruhito
 */
#ifndef SFB_H
#define SFB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFB_VERSION_MAJOR 2
#define SFB_VERSION_MINOR 0
#define SFB_FORMAT_VERSION 2

/* ── error codes (exact negatives are part of the ABI) ─────────────────── */
typedef enum {
    SFB_OK              =    0,
    SFB_E_INVALID       =   -1,   /* bad argument / config                    */
    SFB_E_IO            =   -2,   /* generic I/O failure                      */
    SFB_E_NOMEM         =   -3,   /* allocation failure                       */
    SFB_E_NOTFOUND      =   -4,   /* record/generation not found              */
    SFB_E_STATE         =   -5,   /* call not valid in this state             */
    SFB_E_DIM           =  -1005, /* vector dimension mismatch                */
    SFB_E_TYPE          =  -1006, /* element type rejected (non-f32 input)    */
    SFB_E_NONFINITE     =  -1007, /* NaN / +Inf / -Inf in vector              */
    SFB_E_STALE         =  -1008, /* lock file names a dead holder            */
    SFB_E_LOCKED        =  -1002, /* another process owns the brain           */
    SFB_E_CORRUPT       =  -1003, /* manifest/segment failed integrity        */
    SFB_E_ENOSPC        =  -1004, /* short write / ENOSPC / EFBIG             */
    SFB_E_READONLY      =  -1012, /* mutation attempted on a reader           */
    SFB_E_CAPACITY      =  -1013  /* hard capacity reached, insert refused    */
} sfb_error_t;

const char *sfb_strerror(int code);

/* ── configuration ─────────────────────────────────────────────────────── */
typedef enum {
    SFB_MODE_COGNITIVE = 0,   /* decay, pins, episodic/semantic tiers        */
    SFB_MODE_VECTOR_DB = 1    /* zero decay, exact retention                 */
} sfb_mode_t;

typedef enum {
    SFB_DECAY_NONE        = 0,
    SFB_DECAY_EXPONENTIAL = 1,
    SFB_DECAY_TTL         = 2
} sfb_decay_t;

typedef enum {
    SFB_DEDUP_KEEP_BOTH = 0,
    SFB_DEDUP_MERGE     = 1,
    SFB_DEDUP_REJECT    = 2
} sfb_dedup_action_t;

typedef struct sfb_config {
    const char       *path;               /* brain path, required             */
    uint32_t          dim;                /* 64..4096, required               */
    sfb_mode_t        mode;               /* default COGNITIVE                */
    sfb_decay_t       decay;              /* default: mode-dependent          */
    double            half_life_secs;     /* default 86400                    */
    double            decay_floor;        /* default 0.05                     */
    int               pins_immune;        /* default 1                        */
    double            dedup_text_similarity; /* default 0.85 (Jaccard)        */
    sfb_dedup_action_t dedup_action;      /* default MERGE                    */
    int               quantize_int8;      /* default 0                        */
    int               read_only;          /* default 0 (writer)               */
    const char       *key;                /* optional passphrase (AES-256-GCM) */
    int               verify_full;        /* default 0 (lazy digests)         */
    uint64_t          capacity;           /* 0 = unlimited                    */
    uint32_t          ef_search;          /* 0 = default 64                   */
    int               no_lock;            /* test escape hatch                */
} sfb_config_t;

void sfb_config_default(sfb_config_t *cfg, const char *path, uint32_t dim);

typedef struct sfb_brain sfb_brain_t;

int sfb_open(const sfb_config_t *cfg, sfb_brain_t **out);
int sfb_close(sfb_brain_t *b);

/* ── records ───────────────────────────────────────────────────────────── */
typedef struct sfb_remember_result {
    uint32_t id;
    int      merged;      /* 1 if folded into an existing record          */
    uint32_t into;        /* valid when merged                            */
} sfb_remember_result_t;

int sfb_remember(sfb_brain_t *b, const float *vec, const char *text,
                 const char *role, sfb_remember_result_t *res);

/* Length-carrying variants (W1.3/W6.5). The plain entry points cannot see how
 * long the caller's buffer is, so a short buffer is undefined behaviour there.
 * These take vec_len and reject anything but exactly cfg.dim with SFB_E_DIM. */
int sfb_remember_ex(sfb_brain_t *b, const float *vec, size_t vec_len,
                    const char *text, const char *role,
                    sfb_remember_result_t *res);

typedef struct sfb_hit {
    uint32_t    id;
    float       distance;
    float       score;
    float       strength;
    uint32_t    tier;
    const char *text;      /* borrowed, valid until the next call          */
    const char *role;
} sfb_hit_t;

int sfb_recall(sfb_brain_t *b, const float *vec, uint32_t k,
               sfb_hit_t *hits, uint32_t *n);
int sfb_recall_ex(sfb_brain_t *b, const float *vec, size_t vec_len, uint32_t k,
                  sfb_hit_t *hits, uint32_t *n);
int sfb_recall_text(sfb_brain_t *b, const char *query, uint32_t k,
                    sfb_hit_t *hits, uint32_t *n);

int sfb_list(sfb_brain_t *b, uint64_t offset, uint32_t limit,
             sfb_hit_t *out, uint32_t *n);

int sfb_forget(sfb_brain_t *b, uint32_t id);
int sfb_pin(sfb_brain_t *b, uint32_t id, int pinned);
int sfb_decay_tick(sfb_brain_t *b, uint64_t seconds);
int sfb_retain(sfb_brain_t *b);       /* physically drop sub-floor rows     */
int sfb_consolidate(sfb_brain_t *b);  /* episodic -> semantic representatives */

/* ── durability ────────────────────────────────────────────────────────── */
typedef struct sfb_flush_result {
    int      ok;
    uint64_t records_written;
    uint64_t segments_sealed;
    uint64_t bytes_written;
    uint64_t duration_us;
} sfb_flush_result_t;

int sfb_flush(sfb_brain_t *b, sfb_flush_result_t *res);
int sfb_compact(sfb_brain_t *b, uint64_t *bytes_before, uint64_t *bytes_after);
int sfb_count(const sfb_brain_t *b, uint64_t *live);

/* ── observability ─────────────────────────────────────────────────────── */
typedef struct sfb_stats {
    uint64_t records;            /* rows in RAM including tombstones         */
    uint64_t live;               /* non-forgotten, non-tombstoned            */
    uint64_t tombstones;
    uint64_t segments;
    uint64_t generations;
    uint64_t bytes_records;      /* logical payload bytes                    */
    uint64_t bytes_on_disk;
    /* recovery counters — the honesty channel (W1.8) */
    uint64_t records_dropped;
    uint64_t segments_quarantined;
    uint64_t ids_renumbered;
    uint64_t index_rebuilt;
    /* performance */
    uint64_t flushes;
    uint64_t flush_last_us;
    uint64_t open_us;
    uint64_t events;
} sfb_stats_t;

int sfb_stats_get(const sfb_brain_t *b, sfb_stats_t *out);

typedef void (*sfb_event_cb)(const char *type, const char *json, void *ud);
int sfb_on_event(sfb_brain_t *b, sfb_event_cb cb, void *ud);

/* ── operations (W7) ───────────────────────────────────────────────────── */
/* verify: full digest + CRC scrub. repair=1 quarantines damaged pieces.
 * Writes a JSON report into json_out (may be NULL). */
int sfb_verify(const char *path, const char *key, int repair,
               char *json_out, size_t json_cap);
int sfb_backup(const char *path, const char *dest);
int sfb_restore(const char *backup_path, const char *dest);
int sfb_stats_json(const char *path, const char *key, char *out, size_t cap);
/* migrate: v1 single-file brain -> v2 segmented store */
int sfb_migrate(const char *src_path, const char *dst_path, const char *key);

#ifdef __cplusplus
}
#endif
#endif /* SFB_H */
