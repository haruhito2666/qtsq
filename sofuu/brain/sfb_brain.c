/*
 * sfb_brain.c — Sofuu Brain v2: the memory model (orchestrator).
 *
 * Implements every sfb.h entry point that is NOT a store / lock / hnsw / text /
 * quant / ops / broker primitive. Talks to the siblings only through the
 * prototypes frozen in sfb_internal.h; owns no on-disk encoding except the
 * serialised record block that sfb_store.c seals into a segment.
 *
 * Part of the QTSQ suite (black-hole-disk/sofuu/brain/).
 *
 * ── Invariants this file maintains ───────────────────────────────────────
 *  I1. recs[id].id == id for every record: the RAM index *is* the id. The HNSW
 *      graph, the vector store and the text index are all keyed by it, so a
 *      record can be found in O(1) and hnsw results map straight back to rows.
 *      Retain is the only operation that renumbers; it rebuilds the index.
 *  I2. vecs.n == nrec: the vector store grows in lockstep with recs, so row i
 *      always has a vector at vecs.vecs[i*dim].
 *  I3. Arena strings are NUL-terminated in place; text_len/role_len exclude the
 *      NUL. Borrowed `const char *` returned through sfb_hit_t are therefore
 *      ordinary C strings into the arena, valid until the next mutation.
 *  I4. Liveness == (SFB_F_LIVE && !SFB_F_TOMBSTONE && tier != FORGOTTEN).
 *      Holes left by a dropped record have flags == 0: neither live nor a
 *      tombstone, so they never inflate the tombstone counter.
 *
 * ── Assumptions about sibling modules (all conservative) ─────────────────
 *  A1. sfb_store_segment_read() returns a malloc'ed buffer the caller frees.
 *  A2. sfb_store_segment_write() appends the segment entry to man.segs and
 *      bumps man.generation itself; manifest_save() persists the result.
 *  A3. sfb_store_quarantine() owns store.segments_quarantined (we do not bump
 *      it a second time here).
 *  A4. sfb_hnsw_search() distances are Euclidean L2 over the raw f32 vectors,
 *      matching the 0.05 thresholds this file is specified against. Only live
 *      vectors are ever added to the graph, and tombstones are re-checked on
 *      the way out, so a graph that forgets to filter deleted ids is safe.
 *  A5. sfb_hnsw_add() may not be called twice for the same id, and the graph
 *      offers no update; therefore the graph is never mutated during replay —
 *      sfb_brain_load_all() rebuilds it from live rows once all segments are
 *      in RAM. That also makes open order-independent.
 *  A6. The int8 side-car uses a symmetrical per-vector scale (max|v|/127). It
 *      is a retention aid for W3.6 only; search always uses raw f32, so a
 *      different sfb_quant.c strategy cannot change results.
 *
 * Author: Haruhito
 */
#include "sfb_internal.h"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>

/* ── serialised record block ────────────────────────────────────────────────
 * FLOAT32 BLOCK (segment flag bit1 == 0; every store written before W3.6 and
 * every brain with cfg.quantize_int8 == 0):
 * u32 record_count,
 * then per record:
 *   u32 id, u32 flags, u32 tier, u32 text_len, u32 role_len, f32 strength,
 *   f64 created_at, f64 last_hit,
 *   u32 crc32c(the 24 bytes id..strength ++ text ++ role),
 *   text_len bytes, role_len bytes, dim*4 bytes of f32 vector.
 *
 * INT8 BLOCK (segment flag bit1 == SFB_SEG_FLAG_INT8; W3.6) — identical except
 * for the vector and a scale table in front:
 *   0                  u32 record_count (= n)
 *   4                  n * f32 per-record scale factors
 *   4+4n               per record, exactly the float32 framing above:
 *   +0   24 bytes id, flags, tier, text_len, role_len, strength   (CRC span)
 *   +24  16 bytes created_at, last_hit
 *   +40   4 bytes crc32c (over the 24 CRC-span bytes ++ text ++ role)
 *   +44  tl bytes text, rl bytes role
 *   +44+tl+rl  dim bytes of int8 codes
 * Total record stride is 44 + tl + rl + dim, i.e. the vector part shrinks from
 * 4*dim to dim while everything else stays byte-identical.  A per-record scale
 * (one f32 each, table at the front so a reader can reach it in one read)
 * was chosen over one global scale because a segment mixes unrelated memories:
 * a global scale would clip the large vectors or flatten the small ones to
 * zero.  The scale/code rule is sfb_quant.c's per-record rule, so what is
 * written and what is read back are the same arithmetic — encode(decode(c,s))
 * reproduces (c,s) exactly, and a re-flush of the same rows writes the same
 * bytes.
 *
 * The timestamps sit between the covered span and the CRC, so they ride in the
 * sealed segment without changing what the CRC covers.  The vector is not in
 * the per-record CRC in either mode (it never was); the segment payload CRC and
 * the manifest's whole-file digest cover it.
 * Little-endian native (arm64 / x86-64); read back by the same file.
 * ------------------------------------------------------------------------ */
#define SFB_REC_CRCSPAN 24u                     /* id..strength              */
#define SFB_OFF_HEAD    0u                      /* 24 CRC-covered bytes      */
#define SFB_OFF_STAMP   24u                     /* f64 created_at, f64 last_hit */
#define SFB_OFF_CRC     40u                     /* u32 crc                   */
#define SFB_OFF_TEXT    44u                     /* text ++ role ++ vector    */
#define SFB_REC_HDR     SFB_OFF_TEXT
#define SFB_REC_TEXT_MAX (1u << 20)

#define SFB_RECENCY_TAU 86400.0
#define SFB_PIN_FLOOR   0.2
#define SFB_DEDUP_L2    0.05
#define SFB_GRAPH_NAME_MAX 64
/* flushes a graph save may drift before flush() writes one again */
#define SFB_GRAPH_SAVE_GENS 16u

/* ── small helpers ──────────────────────────────────────────────────────── */
static int rec_live(const sfb_record_t *r)
{
    return (r->flags & SFB_F_LIVE) != 0 && (r->flags & SFB_F_TOMBSTONE) == 0;
}

static int rec_live_visible(const sfb_record_t *r)
{
    return rec_live(r) && r->tier != SFB_TIER_FORGOTTEN;
}

static int rec_pinned(const sfb_record_t *r)
{
    return (r->flags & SFB_F_PINNED) != 0;
}

static const char *rec_text(const sfb_brain_t *b, const sfb_record_t *r)
{
    if (b->arena == NULL || r->text_off >= b->arena_len)
        return "";
    return b->arena + r->text_off;
}

static const char *rec_role(const sfb_brain_t *b, const sfb_record_t *r)
{
    if (b->arena == NULL || r->role_off >= b->arena_len)
        return "";
    return b->arena + r->role_off;
}

/* ── vector row access (W3.6) ───────────────────────────────────────────────
 * The brain keeps exactly one copy of each row: f32 in float32 mode, int8
 * codes + one f32 scale in int8 mode.  The int8 form has no f32 corpus array
 * (vecs.vecs == NULL), so every consumer that needs floats dequantises the row
 * it is looking at into its own scratch buffer.  `vec_row_get` is the single
 * place that knows both layouts and is what keeps search, dedup, compaction
 * and the HNSW rebuild byte-identical in behaviour across modes.
 * ------------------------------------------------------------------------ */

/* True when the vector store can answer for every row. */
static int vec_rows_available(const sfb_brain_t *b)
{
    if (b->vecs.dim == 0)
        return 0;
    if (b->vecs.quantized)
        return b->vecs.qcodes != NULL && b->vecs.qscale != NULL;
    return b->vecs.vecs != NULL;
}

/* Row `id` as f32 into out[0..dim).  1 on success, 0 when the row is absent
 * or (int8 mode) its stored scale is unusable. */
static int vec_row_get(const sfb_brain_t *b, uint32_t id, float *out)
{
    if (out == NULL || b->vecs.dim == 0 || id >= b->vecs.n)
        return 0;
    if (b->vecs.quantized) {
        if (b->vecs.qcodes == NULL || b->vecs.qscale == NULL)
            return 0;
        return sfb_quant_decode_vec(b->vecs.qcodes + (size_t)id * b->vecs.dim,
                                    b->vecs.dim, b->vecs.qscale[id],
                                    out) == SFB_OK;
    }
    if (b->vecs.vecs == NULL)
        return 0;
    memcpy(out, b->vecs.vecs + (size_t)id * b->vecs.dim,
           (size_t)b->vecs.dim * sizeof(float));
    return 1;
}

/* Row `id` as f32 without copying when the brain already stores f32; in int8
 * mode the row is dequantised into `scratch` (dim floats).  NULL = no row. */
static const float *vec_row_ptr(const sfb_brain_t *b, uint32_t id,
                                float *scratch)
{
    if (b->vecs.dim == 0 || id >= b->vecs.n)
        return NULL;
    if (b->vecs.quantized)
        return vec_row_get(b, id, scratch) ? scratch : NULL;
    if (b->vecs.vecs == NULL)
        return NULL;
    return b->vecs.vecs + (size_t)id * b->vecs.dim;
}

static float *rec_vec(sfb_brain_t *b, uint32_t id)
{
    if (!vec_rows_available(b) || b->vecs.quantized || id >= b->vecs.n)
        return NULL;                    /* int8 mode: no f32 row exists      */
    return b->vecs.vecs + (size_t)id * b->vecs.dim;
}

static const float *rec_vec_c(const sfb_brain_t *b, uint32_t id)
{
    if (!vec_rows_available(b) || b->vecs.quantized || id >= b->vecs.n)
        return NULL;
    return b->vecs.vecs + (size_t)id * b->vecs.dim;
}

static double l2_dist(const float *a, const float *b, uint32_t dim)
{
    double s = 0.0;
    uint32_t i;

    if (a == NULL || b == NULL)
        return 1.0e30;
    for (i = 0; i < dim; i++) {
        double d = (double)a[i] - (double)b[i];
        s += d * d;
    }
    return sqrt(s);
}

static double tier_bonus(uint32_t tier)
{
    switch (tier) {
    case SFB_TIER_EPISODIC:  return 1.00;
    case SFB_TIER_SEMANTIC:  return 1.15;
    case SFB_TIER_ENTITY:    return 1.10;
    case SFB_TIER_WORKING:   return 1.05;
    case SFB_TIER_FORGOTTEN: return 0.00;
    default:                 return 1.00;
    }
}

static double recency_weight(double last_hit, double now)
{
    double age = now - last_hit;

    if (!(age >= 0.0))          /* also catches NaN from a corrupt stamp   */
        age = 0.0;
    return 1.0 + 0.1 * exp(-age / SFB_RECENCY_TAU);
}

static uint32_t live_count(const sfb_brain_t *b)
{
    uint32_t i, n = 0;

    for (i = 0; i < b->nrec; i++)
        if (rec_live_visible(&b->recs[i]))
            n++;
    return n;
}

/* ── events ───────────────────────────────────────────────────────────────
 * Callers pass the inner members of the object ("\"id\":3,\"n\":1") or a whole
 * {...} payload; either way the object emitted is `{"type":"<type>",...}` and
 * is valid JSON — never `{"type":"x",{"id":3}}`.
 * ------------------------------------------------------------------------ */
static void brain_emit(sfb_brain_t *b, const char *type, const char *fmt, ...)
{
    char payload[320];
    char out[512];
    const char *inner = payload;
    size_t inner_len = 0;
    va_list ap;
    int n;

    if (b == NULL || b->event_cb == NULL)
        return;                 /* no listener: never touch stderr            */

    payload[0] = '\0';
    if (fmt != NULL && *fmt != '\0') {
        va_start(ap, fmt);
        (void)vsnprintf(payload, sizeof payload, fmt, ap);
        va_end(ap);
    }

    if (payload[0] != '\0') {
        inner_len = strlen(payload);
        if (payload[0] == '{' && inner_len >= 2u && payload[inner_len - 1u] == '}') {
            inner = payload + 1;
            inner_len -= 2u;
        }
    }

    if (inner_len > 0)
        n = snprintf(out, sizeof out, "{\"type\":\"%s\",%.*s}", type,
                     (int)inner_len, inner);
    else
        n = snprintf(out, sizeof out, "{\"type\":\"%s\"}", type);
    if (n < 0)
        return;

    b->event_cb(type, out, b->event_ud);
    b->stats.events++;
}

/* ── growable byte buffer (serialisation) ───────────────────────────────── */
typedef struct {
    uint8_t *p;
    size_t   len, cap;
} sbuf_t;

static int sbuf_reserve(sbuf_t *s, size_t extra)
{
    size_t need, ncap;
    uint8_t *np;

    if (extra > (size_t)-1 - s->len)
        return SFB_E_NOMEM;
    need = s->len + extra;
    if (need <= s->cap)
        return SFB_OK;

    ncap = s->cap ? s->cap : 4096;
    while (ncap < need) {
        if (ncap > ((size_t)-1) / 2)
            return SFB_E_NOMEM;
        ncap *= 2;
    }
    np = (uint8_t *)realloc(s->p, ncap);
    if (np == NULL)
        return SFB_E_NOMEM;
    s->p = np;
    s->cap = ncap;
    return SFB_OK;
}

static int sbuf_put(sbuf_t *s, const void *d, size_t n)
{
    int rc;

    if (n == 0)
        return SFB_OK;
    rc = sbuf_reserve(s, n);
    if (rc != SFB_OK)
        return rc;
    memcpy(s->p + s->len, d, n);
    s->len += n;
    return SFB_OK;
}

/* ── arena ──────────────────────────────────────────────────────────────── */
static int arena_reserve(sfb_brain_t *b, size_t need)
{
    size_t ncap;
    char *np;

    if (need <= b->arena_cap)
        return SFB_OK;
    if (need > 0xFFFFFFFFu)
        return SFB_E_CAPACITY;          /* offsets are u32 in sfb_record_t    */

    ncap = b->arena_cap ? b->arena_cap : 4096;
    while (ncap < need) {
        if (ncap > 0x80000000u)
            return SFB_E_CAPACITY;
        ncap *= 2;
    }
    np = (char *)realloc(b->arena, ncap);
    if (np == NULL)
        return SFB_E_NOMEM;
    b->arena = np;
    b->arena_cap = ncap;
    return SFB_OK;
}

/* Append a NUL-terminated copy; *off_out receives the byte offset.
 * `s` may point into the arena itself (consolidate copies text verbatim), so the
 * source is re-based after the growing realloc instead of being trusted. */
static int arena_put(sfb_brain_t *b, const char *s, uint32_t len, uint32_t *off_out)
{
    int rc;
    size_t need, src_off = 0;
    int aliased = 0;

    if (s == NULL) {
        s = "";
        len = 0;
    } else if (b->arena != NULL && s >= b->arena &&
               s < b->arena + b->arena_cap) {
        aliased = 1;
        src_off = (size_t)(s - b->arena);
    }

    need = b->arena_len + (size_t)len + 1u;
    rc = arena_reserve(b, need);
    if (rc != SFB_OK)
        return rc;
    if (aliased)
        s = b->arena + src_off;

    if (len > 0)
        memcpy(b->arena + b->arena_len, s, len);
    b->arena[b->arena_len + len] = '\0';
    *off_out = (uint32_t)b->arena_len;
    b->arena_len = need;
    return SFB_OK;
}

/* ── record / vector growth ─────────────────────────────────────────────── */
static int brain_reserve(sfb_brain_t *b, uint32_t need)
{
    uint32_t ncap;
    sfb_record_t *nr;

    if (need <= b->crec && need <= b->vecs.cap)
        return SFB_OK;

    ncap = b->crec ? b->crec : 16u;
    while (ncap < need) {
        if (ncap > 0x40000000u) {
            ncap = need;
            break;
        }
        ncap *= 2u;
    }

    nr = (sfb_record_t *)realloc(b->recs, (size_t)ncap * sizeof *nr);
    if (nr == NULL)
        return SFB_E_NOMEM;
    b->recs = nr;

    /* W3.6: exactly one representation per row.  Int8 mode never allocates the
     * f32 array at all, which is the 4x of the vector store (4*dim -> dim+4). */
    if (b->vecs.quantized) {
        int8_t *nq = (int8_t *)realloc(b->vecs.qcodes,
                                       (size_t)ncap * (size_t)b->vecs.dim);
        float *ns;
        if (nq == NULL)
            return SFB_E_NOMEM;
        b->vecs.qcodes = nq;
        ns = (float *)realloc(b->vecs.qscale, (size_t)ncap * sizeof(float));
        if (ns == NULL)
            return SFB_E_NOMEM;
        b->vecs.qscale = ns;
    } else {
        float *nv = (float *)realloc(b->vecs.vecs,
                                     (size_t)ncap * (size_t)b->vecs.dim *
                                         sizeof(float));
        if (nv == NULL)
            return SFB_E_NOMEM;
        b->vecs.vecs = nv;
    }

    b->crec = ncap;
    b->vecs.cap = ncap;
    return SFB_OK;
}

/* Quantise one f32 row into the int8 store (row must already be sized). */
static void qrow_encode(sfb_brain_t *b, uint32_t id, const float *vec)
{
    int rc;

    if (!b->vecs.quantized || b->vecs.qcodes == NULL || b->vecs.qscale == NULL)
        return;
    rc = sfb_quant_encode_vec(vec, b->vecs.dim,
                              b->vecs.qcodes + (size_t)id * b->vecs.dim,
                              &b->vecs.qscale[id]);
    if (rc != SFB_OK) {
        /* A non-finite row cannot be encoded (replay of a damaged f32 segment
         * is the only way here).  Store the zero row instead of a scale the
         * reader would reject, and let the per-record accounting speak. */
        memset(b->vecs.qcodes + (size_t)id * b->vecs.dim, 0, b->vecs.dim);
        b->vecs.qscale[id] = 1.0f;
    }
}

/* One row of a segment payload or of the caller's vector, in either layout. */
typedef struct vec_src {
    const float  *f32;    /* f32 row, or NULL                                */
    const int8_t *codes;  /* int8 codes, or NULL                             */
    float         scale;  /* valid when codes != NULL                        */
} vec_src_t;

/* Write row `id` from src (NULL src = the all-zero row). */
static void row_store(sfb_brain_t *b, uint32_t id, const vec_src_t *src)
{
    size_t dim = b->vecs.dim;

    if (b->vecs.quantized) {
        int8_t *q = b->vecs.qcodes + (size_t)id * dim;

        if (src != NULL && src->codes != NULL) {
            memcpy(q, src->codes, dim);
            b->vecs.qscale[id] = src->scale;
        } else if (src != NULL && src->f32 != NULL) {
            qrow_encode(b, id, src->f32);
        } else {
            memset(q, 0, dim);
            b->vecs.qscale[id] = 1.0f;  /* every code is 0: scale is moot    */
        }
        return;
    }

    if (src != NULL && src->f32 != NULL) {
        memcpy(b->vecs.vecs + (size_t)id * dim, src->f32, dim * sizeof(float));
    } else if (src != NULL && src->codes != NULL) {
        if (sfb_quant_decode_vec(src->codes, b->vecs.dim, src->scale,
                                 b->vecs.vecs + (size_t)id * dim) != SFB_OK)
            memset(b->vecs.vecs + (size_t)id * dim, 0, dim * sizeof(float));
    } else {
        memset(b->vecs.vecs + (size_t)id * dim, 0, dim * sizeof(float));
    }
}

/* ── dirty bookkeeping (queue is advisory; the flag is the truth) ───────── */
static void brain_mark_dirty(sfb_brain_t *b, uint32_t id)
{
    if (id >= b->nrec)
        return;
    if (b->recs[id].flags & SFB_F_DIRTY)
        return;
    b->recs[id].flags |= SFB_F_DIRTY;

    if (b->dirty_count == b->dirty_head) {
        uint32_t ncap = b->dirty_head ? b->dirty_head * 2u : 64u;
        uint32_t *np = (uint32_t *)realloc(b->dirty_ids, (size_t)ncap * sizeof *np);
        if (np == NULL)
            return;             /* flag set, queue stale — serialiser scans  */
        b->dirty_ids = np;
        b->dirty_head = ncap;
    }
    b->dirty_ids[b->dirty_count++] = id;
}

static void brain_clear_dirty(sfb_brain_t *b)
{
    uint32_t i;

    for (i = 0; i < b->nrec; i++)
        b->recs[i].flags &= ~SFB_F_DIRTY;
    b->dirty_count = 0;
}

/* ── text index: open addressing, slot = id + 1 (0 == empty) ────────────── */
static uint32_t next_pow2(uint32_t v)
{
    uint32_t c = 64u;

    while (c < v && c < 0x40000000u)
        c *= 2u;
    return c;
}

static void ti_insert(sfb_brain_t *b, uint32_t id)
{
    const char *t;
    uint32_t h, mask, slot, probes;

    if (id >= b->nrec || b->text_cap == 0 || b->text_index == NULL)
        return;
    t = rec_text(b, &b->recs[id]);
    if (*t == '\0')
        return;                 /* nothing to index                         */

    h = (uint32_t)sfb_text_hash(t);
    mask = b->text_cap - 1u;
    slot = h & mask;

    for (probes = 0; probes < b->text_cap; probes++) {
        if (b->text_index[slot] == 0) {
            b->text_index[slot] = id + 1u;
            return;
        }
        slot = (slot + 1u) & mask;
    }
    /* Table full: leave it; the next reserve() rehashes into a bigger one. */
}

/* Give the index room for `rows` records: capacity >= 2*rows, min 64. */
static int ti_reserve(sfb_brain_t *b, uint32_t rows)
{
    uint32_t want, i;
    uint32_t *fresh;

    want = next_pow2(rows > 0x20000000u ? 0x40000000u : 2u * rows);
    if (want <= b->text_cap && b->text_index != NULL)
        return SFB_OK;

    fresh = (uint32_t *)calloc((size_t)want, sizeof *fresh);
    if (fresh == NULL)
        return SFB_E_NOMEM;
    free(b->text_index);
    b->text_index = fresh;
    b->text_cap = want;

    for (i = 0; i < b->nrec; i++)
        if (rec_live_visible(&b->recs[i]))
            ti_insert(b, i);
    return SFB_OK;
}

static void ti_maybe_grow(sfb_brain_t *b)
{
    if (b->text_index == NULL || b->nrec * 2u >= b->text_cap)
        (void)ti_reserve(b, b->nrec + 1u);
}

/* First live row with this exact text hash whose vector is within `maxl2`.
 *
 * W3.6: in int8 mode the stored row is lossy, so "same vector" is decided in
 * code space: the query is quantised with the same rule and the (scale, codes)
 * pair must match exactly.  An identical input vector always reproduces the
 * same pair, so re-remembering the same memory still folds (the property the
 * f32 L2 threshold gives); a merely nearby vector no longer folds here and is
 * left to the HNSW neighbour + Jaccard path below, which is the price of a 4x
 * smaller payload and is documented rather than hidden. */
static int ti_find_exact(sfb_brain_t *b, uint64_t h, const char *text,
                         const float *vec, double maxl2, uint32_t *found)
{
    uint32_t mask, slot, probes, id;
    size_t len;
    int8_t *qbuf = NULL;
    float qscale = 0.0f;
    int quantised = b->vecs.quantized;

    if (b->text_index == NULL || b->text_cap == 0)
        return SFB_E_NOTFOUND;
    if (quantised) {
        if (!vec_rows_available(b))
            return SFB_E_NOTFOUND;
        qbuf = (int8_t *)malloc(b->vecs.dim);
        if (qbuf == NULL)
            return SFB_E_NOMEM;
        if (sfb_quant_encode_vec(vec, b->vecs.dim, qbuf, &qscale) != SFB_OK) {
            free(qbuf);
            return SFB_E_NOTFOUND;
        }
    }
    len = strlen(text);
    mask = b->text_cap - 1u;
    slot = (uint32_t)h & mask;

    for (probes = 0; probes < b->text_cap; probes++) {
        uint32_t cell = b->text_index[slot];

        if (cell == 0)
            break;
        id = cell - 1u;
        if (id < b->nrec && rec_live_visible(&b->recs[id])) {
            const sfb_record_t *r = &b->recs[id];
            const char *rt = rec_text(b, r);
            int same_vec;

            if ((size_t)r->text_len == len && memcmp(rt, text, len) == 0 &&
                sfb_text_hash(rt) == h) {
                if (quantised) {
                    same_vec = id < b->vecs.n &&
                               id < b->vecs.cap &&
                               b->vecs.qscale[id] == qscale &&
                               memcmp(qbuf, b->vecs.qcodes + (size_t)id *
                                          b->vecs.dim, b->vecs.dim) == 0;
                } else {
                    const float *rv = rec_vec_c(b, id);
                    same_vec = rv == NULL ||
                               l2_dist(vec, rv, b->vecs.dim) < maxl2;
                }
                if (same_vec) {
                    free(qbuf);
                    *found = id;
                    return SFB_OK;
                }
            }
        }
        slot = (slot + 1u) & mask;
    }
    free(qbuf);
    return SFB_E_NOTFOUND;
}

/* ── record append / overwrite ──────────────────────────────────────────── */
static int brain_rebuild_index(sfb_brain_t *b);
static int brain_save_graph(sfb_brain_t *b, int force);
static int brain_file_digest(const char *path, uint32_t *crc_out);
static int brain_try_load_graph(sfb_brain_t *b);

static int brain_append(sfb_brain_t *b, const vec_src_t *src, const char *text,
                        const char *role, uint32_t flags, uint32_t tier,
                        float strength, double created_at, double last_hit,
                        int to_index, uint32_t *id_out)
{
    uint32_t id = b->nrec, tl, rl, toff = 0, roff = 0;
    size_t dim = b->vecs.dim;
    sfb_record_t *r;
    size_t src_row = 0;
    int aliased = 0;
    int has_vec = src != NULL && (src->f32 != NULL || src->codes != NULL);
    vec_src_t eff;
    float *scratch = NULL;
    int rc;

    memset(&eff, 0, sizeof eff);

    if (b->vecs.dim == 0)
        return SFB_E_STATE;
    if (src != NULL)
        eff = *src;
    if (src != NULL && src->f32 != NULL && b->vecs.vecs != NULL) {
        const char *p = (const char *)src->f32;
        const char *lo = (const char *)b->vecs.vecs;
        const char *hi = lo + (size_t)b->vecs.cap * dim * sizeof(float);

        if (p >= lo && p < hi) {
            aliased = 1;
            src_row = (size_t)(p - lo) / (dim * sizeof(float));
        }
    }
    rc = brain_reserve(b, id + 1u);
    if (rc != SFB_OK)
        return rc;
    if (aliased)
        eff.f32 = b->vecs.vecs + src_row * dim;

    tl = text != NULL ? (uint32_t)strlen(text) : 0u;
    rl = role != NULL ? (uint32_t)strlen(role) : 0u;
    if (tl > SFB_REC_TEXT_MAX || rl > SFB_REC_TEXT_MAX)
        return SFB_E_CAPACITY;

    rc = arena_put(b, text, tl, &toff);
    if (rc != SFB_OK)
        return rc;
    rc = arena_put(b, role, rl, &roff);
    if (rc != SFB_OK)
        return rc;

    r = &b->recs[id];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->flags = flags;
    r->tier = tier;
    r->text_off = toff;
    r->text_len = tl;
    r->role_off = roff;
    r->role_len = rl;
    r->strength = strength;
    r->created_at = created_at;
    r->last_hit = last_hit;

    row_store(b, id, has_vec ? &eff : NULL);

    b->vecs.n = id + 1u;
    b->nrec = id + 1u;

    if (to_index && has_vec && b->index != NULL && rec_live(r)) {
        const float *hv;

        if (b->vecs.quantized) {
            /* the graph needs f32: dequantise just this row (W3.6) */
            scratch = (float *)malloc(dim * sizeof(float));
            if (scratch == NULL)
                return SFB_E_NOMEM;
            hv = vec_row_ptr(b, id, scratch);
        } else {
            hv = b->vecs.vecs + (size_t)id * dim;
        }
        rc = hv != NULL ? sfb_hnsw_add(b->index, id, hv) : SFB_E_INVALID;
        free(scratch);
        scratch = NULL;
        if (rc != SFB_OK) {
            /* A lost add would leave n_nodes behind nrec and break every later
             * append, so resync from the rows rather than ignoring it. */
            int brc;

            brain_mark_dirty(b, id);
            brc = brain_rebuild_index(b);
            if (brc != SFB_OK)
                return brc;
        }
    }
    if (has_vec)
        ti_insert(b, id);

    if (id_out != NULL)
        *id_out = id;
    return SFB_OK;
}

/* Replay-only: replace row `id` in place (last writer wins across segments). */
static int brain_overwrite(sfb_brain_t *b, uint32_t id, const vec_src_t *src,
                           const char *text, const char *role, uint32_t flags,
                           uint32_t tier, float strength, double created_at,
                           double last_hit)
{
    sfb_record_t *r;
    uint32_t tl, rl, toff = 0, roff = 0;
    int rc;
    int has_vec = src != NULL && (src->f32 != NULL || src->codes != NULL);

    if (id >= b->nrec)
        return SFB_E_INVALID;
    r = &b->recs[id];

    tl = text != NULL ? (uint32_t)strlen(text) : 0u;
    rl = role != NULL ? (uint32_t)strlen(role) : 0u;
    if (tl > SFB_REC_TEXT_MAX || rl > SFB_REC_TEXT_MAX)
        return SFB_E_CAPACITY;
    rc = arena_put(b, text, tl, &toff);
    if (rc != SFB_OK)
        return rc;
    rc = arena_put(b, role, rl, &roff);
    if (rc != SFB_OK)
        return rc;

    r->flags = flags;
    r->tier = tier;
    r->text_off = toff;
    r->text_len = tl;
    r->role_off = roff;
    r->role_len = rl;
    r->strength = strength;
    r->created_at = created_at;
    r->last_hit = last_hit;

    row_store(b, id, has_vec ? src : NULL);

    if (has_vec)
        ti_insert(b, id);
    return SFB_OK;
}

/* Fill rows [nrec, id) with inert placeholders so I1 holds after a loss. */
static int brain_pad_to(sfb_brain_t *b, uint32_t id)
{
    while (b->nrec < id) {
        int rc = brain_append(b, NULL, "", "", 0, SFB_TIER_WORKING, 0.0f,
                              0.0, 0.0, 0, NULL);
        if (rc != SFB_OK)
            return rc;
    }
    return SFB_OK;
}

/* ── index rebuild (open, retain, compact) ────────────────────────────────
 * sfb_hnsw_add() is append-only: it demands id == h->n_nodes, so a skipped row
 * would make every later add fail. Row ids are therefore exactly node ids, and
 * every row gets a node — non-live rows (tombstones, replay holes) are added
 * and immediately marked deleted, which sfb_hnsw_search() filters out. That
 * keeps the two id spaces identical with no side mapping to keep in sync.
 * ------------------------------------------------------------------------ */
static int brain_rebuild_index(sfb_brain_t *b)
{
    uint32_t i;
    float *scratch = NULL;
    int rc;

    if (b->index != NULL)
        sfb_hnsw_free(b->index);
    b->index = NULL;
    rc = sfb_hnsw_init(&b->index, b->cfg.dim, SFB_DEFAULT_M, SFB_DEFAULT_M0,
                       SFB_DEFAULT_EFC);
    if (rc != SFB_OK) {
        b->index = NULL;
        return rc;
    }
    /* W3.6: the graph searches in f32, so int8 mode dequantises one row at a
     * time into this scratch.  sfb_hnsw_add() copies it, so no corpus-sized
     * f32 buffer is created and node id == row id is unchanged. */
    if (b->vecs.quantized) {
        scratch = (float *)malloc((size_t)b->cfg.dim * sizeof(float));
        if (scratch == NULL) {
            sfb_hnsw_free(b->index);
            b->index = NULL;
            return SFB_E_NOMEM;
        }
    }
    for (i = 0; i < b->nrec; i++) {
        const float *v = vec_row_ptr(b, i, scratch);

        if (v == NULL) {                    /* unreadable row: inert zero     */
            memset(scratch, 0, (size_t)b->cfg.dim * sizeof(float));
            v = scratch;
        }
        rc = sfb_hnsw_add(b->index, i, v);
        if (rc != SFB_OK) {
            free(scratch);
            return rc;
        }
        if (!rec_live_visible(&b->recs[i]))
            (void)sfb_hnsw_mark_deleted(b->index, i);
    }
    free(scratch);
    b->store.index_rebuilt++;
    return SFB_OK;
}

/* ── graph cache (A7/A8) ──────────────────────────────────────────────────
 * The graph file is topology only and holds every row (deleted ones flagged),
 * so node ids still equal row ids. Adopting it is pure I/O: one sequential read
 * and one memcpy of the vectors, zero distance computations — a full rebuild at
 * 10k x dim 768 is ~120 s, adopting is milliseconds. It is only a cache: when
 * it is missing, corrupt, or describes a different row set we rebuild instead of
 * failing the open.
 * ------------------------------------------------------------------------ */
static int brain_try_load_graph(sfb_brain_t *b)
{
    char gpath[4096], gname[SFB_GRAPH_NAME_MAX];
    sfb_hnsw_t *h = NULL;
    uint32_t i;
    int rc;

    if (b->index != NULL) {
        sfb_hnsw_free(b->index);
        b->index = NULL;
    }
    if (b->nrec == 0 || b->store.man.graph_gen == 0)
        return SFB_E_STATE;
    if (sfb_snprintf(gname, sizeof gname, "graph-%llu.bin",
                     (unsigned long long)b->store.man.graph_gen) != SFB_OK)
        return SFB_E_STATE;
    if (sfb_path_join(gpath, sizeof gpath, b->store.seg_dir, gname) != SFB_OK)
        return SFB_E_STATE;

    /* Zero-copy: the graph's arrays point straight into this mapping, so open
     * costs one mmap + a CRC pass rather than a rebuild or a 40 MB copy. */
    rc = sfb_hnsw_map(&h, gpath, b->cfg.dim, SFB_DEFAULT_M, SFB_DEFAULT_M0,
                      SFB_DEFAULT_EFC);
    if (rc != SFB_OK) {
        if (h != NULL)
            sfb_hnsw_free(h);
        return SFB_E_STATE;
    }
    if (sfb_hnsw_count(h) != (uint64_t)b->nrec) {
        sfb_hnsw_free(h);
        return SFB_E_STATE;
    }
    /* Safe only if the topology covers exactly the rows we just replayed.
     * attach_vectors() copies, and it wants one contiguous f32 array: int8 mode
     * has no such array, so dequantise the corpus into one transient buffer for
     * the duration of the copy and free it at once.  Steady-state RAM stays at
     * dim+4 bytes/row (the peak during open is a documented one-off). */
    {
        const float *src = b->vecs.vecs;
        float *tmp = NULL;

        if (b->vecs.quantized) {
            tmp = (float *)malloc((size_t)b->nrec * b->cfg.dim * sizeof(float));
            if (tmp == NULL) {
                sfb_hnsw_free(h);
                return SFB_E_STATE;
            }
            for (i = 0; i < b->nrec; i++) {
                float *row = tmp + (size_t)i * b->cfg.dim;

                if (!vec_row_get(b, i, row))
                    memset(row, 0, (size_t)b->cfg.dim * sizeof(float));
            }
            src = tmp;
        }
        rc = sfb_hnsw_attach_vectors(h, src, b->nrec);
        free(tmp);
        if (rc != SFB_OK) {
            sfb_hnsw_free(h);
            return SFB_E_STATE;
        }
    }

    b->index = h;
    /* Deleted flags may predate the newest segments; the rows are the truth. */
    for (i = 0; i < b->nrec; i++) {
        if (rec_live_visible(&b->recs[i]))
            (void)sfb_hnsw_undelete(b->index, i);
        else
            (void)sfb_hnsw_mark_deleted(b->index, i);
    }
    return SFB_OK;
}

static int brain_save_graph(sfb_brain_t *b, int force)
{
    char gpath[4096], gname[SFB_GRAPH_NAME_MAX];
    uint32_t crc = 0;
    int rc;

    if (b->index == NULL)
        return SFB_E_STATE;
    if (!force && b->store.man.graph_gen != 0 &&
        b->store.man.generation <
            b->store.man.graph_gen + (uint64_t)SFB_GRAPH_SAVE_GENS)
        return SFB_OK;          /* fresh enough: skip the O(nodes) write      */

    if (sfb_snprintf(gname, sizeof gname, "graph-%llu.bin",
                     (unsigned long long)b->store.man.generation) != SFB_OK)
        return SFB_E_CAPACITY;
    rc = sfb_path_join(gpath, sizeof gpath, b->store.seg_dir, gname);
    if (rc == SFB_OK)
        rc = sfb_path_mkdir_for(gpath);
    if (rc == SFB_OK)
        rc = sfb_hnsw_save(b->index, gpath, b->store.man.generation);
    if (rc != SFB_OK)
        return rc;
    if (brain_file_digest(gpath, &crc) == SFB_OK)
        b->store.man.graph_digest = crc;
    b->store.man.graph_gen = b->store.man.generation;
    return SFB_OK;
}

/* ── configuration ──────────────────────────────────────────────────────── */
void sfb_config_default(sfb_config_t *cfg, const char *path, uint32_t dim)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->path = path;
    cfg->dim = dim;
    cfg->mode = SFB_MODE_COGNITIVE;
    cfg->decay = SFB_DECAY_EXPONENTIAL;
    cfg->half_life_secs = 86400.0;
    cfg->decay_floor = 0.05;
    cfg->pins_immune = 1;
    cfg->dedup_text_similarity = 0.85;
    cfg->dedup_action = SFB_DEDUP_MERGE;
    cfg->quantize_int8 = 0;
    cfg->read_only = 0;
    cfg->key = NULL;
    cfg->verify_full = 0;
    cfg->capacity = 0;
    cfg->ef_search = SFB_DEFAULT_EFS;
    cfg->no_lock = 0;
}

/* ── serialisation of the dirty set ─────────────────────────────────────── */
int sfb_brain_serialize_dirty(sfb_brain_t *b, void **out, size_t *len,
                              uint32_t *n_records, uint32_t *min_id, uint32_t *max_id)
{
    sbuf_t s;
    uint32_t i, n = 0, mn = 0xFFFFFFFFu, mx = 0;
    size_t dim = b->vecs.dim;
    int quantised;
    int rc = SFB_OK;

    if (b == NULL || out == NULL || len == NULL)
        return SFB_E_INVALID;
    *out = NULL;
    *len = 0;
    if (n_records != NULL) *n_records = 0;
    if (min_id != NULL) *min_id = 0;
    if (max_id != NULL) *max_id = 0;

    /* The block layout and the segment header flag must never disagree: one
     * comes from the brain, the other from the store, both from the same cfg. */
    quantised = b->vecs.quantized ? 1 : 0;
    if (quantised != (b->store.quantize_int8 ? 1 : 0))
        return SFB_E_STATE;
    if (quantised && !vec_rows_available(b))
        return SFB_E_STATE;

    for (i = 0; i < b->nrec; i++) {
        if ((b->recs[i].flags & SFB_F_DIRTY) == 0)
            continue;
        n++;
        if (b->recs[i].id < mn) mn = b->recs[i].id;
        if (b->recs[i].id > mx) mx = b->recs[i].id;
    }
    if (n == 0)
        return SFB_OK;

    memset(&s, 0, sizeof s);
    rc = sbuf_put(&s, &n, sizeof n);
    if (rc != SFB_OK)
        goto done;

    /* W3.6: the per-record scale table sits between the count and the records,
     * so a reader parses it with one index and never scans the block twice. */
    if (quantised) {
        for (i = 0; i < b->nrec; i++) {
            if ((b->recs[i].flags & SFB_F_DIRTY) == 0)
                continue;
            rc = sbuf_put(&s, &b->vecs.qscale[i], sizeof(float));
            if (rc != SFB_OK)
                goto done;
        }
    }

    for (i = 0; i < b->nrec; i++) {
        sfb_record_t *r = &b->recs[i];
        uint8_t head[SFB_REC_CRCSPAN];
        uint8_t stamp[16];
        uint32_t crc;
        const char *t;

        if ((r->flags & SFB_F_DIRTY) == 0)
            continue;

        t = rec_text(b, r);
        memcpy(head + 0, &r->id, 4);
        memcpy(head + 4, &r->flags, 4);
        memcpy(head + 8, &r->tier, 4);
        memcpy(head + 12, &r->text_len, 4);
        memcpy(head + 16, &r->role_len, 4);
        memcpy(head + 20, &r->strength, 4);
        memcpy(stamp + 0, &r->created_at, 8);
        memcpy(stamp + 8, &r->last_hit, 8);

        crc = sfb_crc32c(head, SFB_REC_CRCSPAN);
        crc = sfb_crc32c_update(crc, t, r->text_len);
        crc = sfb_crc32c_update(crc, rec_role(b, r), r->role_len);
        r->crc = crc;

        rc = sbuf_put(&s, head, sizeof head);
        if (rc == SFB_OK) rc = sbuf_put(&s, stamp, sizeof stamp);
        if (rc == SFB_OK) rc = sbuf_put(&s, &crc, sizeof crc);
        if (rc == SFB_OK) rc = sbuf_put(&s, t, r->text_len);
        if (rc == SFB_OK) rc = sbuf_put(&s, rec_role(b, r), r->role_len);
        if (rc != SFB_OK)
            goto done;

        if (quantised) {
            size_t row = (size_t)i * dim;

            if (row + dim > (size_t)b->vecs.cap * dim)
                goto done_state;
            rc = sbuf_put(&s, b->vecs.qcodes + row, dim);
        } else {
            float *v = rec_vec(b, i);

            if (v != NULL) {
                rc = sbuf_put(&s, v, dim * sizeof(float));
            } else {
                size_t k;
                uint8_t zero[sizeof(float)] = { 0, 0, 0, 0 };
                for (k = 0; k < dim; k++) {
                    rc = sbuf_put(&s, zero, sizeof zero);
                    if (rc != SFB_OK) break;
                }
            }
        }
        if (rc != SFB_OK)
            goto done;
    }

    *out = s.p;
    *len = s.len;
    s.p = NULL;
    if (n_records != NULL) *n_records = n;
    if (min_id != NULL) *min_id = mn;
    if (max_id != NULL) *max_id = mx;

done:
    free(s.p);
    return rc;

done_state:
    free(s.p);
    return SFB_E_STATE;
}

/* ── replay of one sealed segment ───────────────────────────────────────── */
int sfb_brain_replay_segment(sfb_brain_t *b, const void *data, size_t len,
                             uint32_t records)
{
    return sfb_brain_replay_segment_ex(b, data, len, records, 0);
}

/* seg_flags carries the segment header's SFB_SEG_FLAG_* bits.  Bit 1 selects
 * the int8 record block (scale table after the count, dim code bytes per
 * record); bit 0 only says the bytes arrived sealed and are already open by
 * the time they get here.  Every record is framed by (tl, rl) exactly as in
 * the f32 block, so a damaged record costs one record in either layout. */
int sfb_brain_replay_segment_ex(sfb_brain_t *b, const void *data, size_t len,
                                uint32_t records, uint16_t seg_flags)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t count = 0, limit, i, dropped = 0;
    size_t off, table = 0;
    size_t dim = b->vecs.dim;
    char *scratch = NULL;
    size_t scratch_cap = 0;
    int int8_payload = (seg_flags & SFB_SEG_FLAG_INT8) != 0;
    int rc = SFB_OK;

    if (b == NULL || (data == NULL && len != 0))
        return SFB_E_INVALID;
    if (len < 4u) {
        b->store.records_dropped += records;
        return SFB_E_CORRUPT;
    }
    memcpy(&count, p, sizeof count);

    /* int8 blocks carry one f32 scale per record directly after the count; the
     * table is sized by the block's own count, and the whole table must be
     * inside the payload before any record is read. */
    if (int8_payload) {
        if ((uint64_t)count > (uint64_t)SIZE_MAX / sizeof(float) ||
            4u + (size_t)count * sizeof(float) > len) {
            b->store.records_dropped += records;
            return SFB_E_CORRUPT;
        }
        table = 4u;
        off = 4u + (size_t)count * sizeof(float);
    } else {
        off = 4u;
    }

    limit = records != 0 ? (count < records ? count : records) : count;
    if (records > limit)
        dropped += records - limit;

    for (i = 0; i < limit; i++) {
        uint32_t id, flags, tier, tl, rl, crc_stored, crc;
        float strength;
        double created_at, last_hit;
        size_t need, body;
        vec_src_t src;

        if (off + SFB_REC_HDR > len) {
            dropped += limit - i;
            break;
        }
        memcpy(&id, p + off + 0, 4);
        memcpy(&flags, p + off + 4, 4);
        /* A record that is on disk is by definition not dirty, but the flag is
         * part of what the writer stored, so it comes back set.  Leaving it set
         * is not harmless: brain_mark_dirty() returns early for an already
         * dirty row, so the row would never enter dirty_ids and sfb_flush()
         * (which only serialises when dirty_ids is non-empty) would skip it —
         * a retention tombstone written after a reopen was silently lost —
         * while the next real flush re-serialised the whole replayed corpus.
         * Replay therefore drops the flag: the store, not RAM, is the truth. */
        flags &= ~(uint32_t)SFB_F_DIRTY;
        memcpy(&tier, p + off + 8, 4);
        memcpy(&tl, p + off + 12, 4);
        memcpy(&rl, p + off + 16, 4);
        memcpy(&strength, p + off + 20, 4);
        memcpy(&created_at, p + off + SFB_OFF_STAMP + 0, 8);
        memcpy(&last_hit, p + off + SFB_OFF_STAMP + 8, 8);
        memcpy(&crc_stored, p + off + SFB_OFF_CRC, 4);

        body = (size_t)tl + (size_t)rl +
               (int8_payload ? dim : dim * sizeof(float));
        need = SFB_REC_HDR + body;
        if (tl > SFB_REC_TEXT_MAX || rl > SFB_REC_TEXT_MAX || off + need > len) {
            dropped += limit - i;
            break;                            /* truncated tail              */
        }

        crc = sfb_crc32c(p + off, SFB_REC_CRCSPAN);
        crc = sfb_crc32c_update(crc, p + off + SFB_OFF_TEXT, tl);
        crc = sfb_crc32c_update(crc, p + off + SFB_OFF_TEXT + tl, rl);

        if (crc != crc_stored) {
            /* W3.5: one bad record costs exactly one record. */
            dropped++;
            off += need;
            continue;
        }

        memset(&src, 0, sizeof src);
        if (int8_payload) {
            float scale;

            /* the row's codes sit after text ++ role, dim bytes */
            memcpy(&scale, p + table + (size_t)i * sizeof(float),
                   sizeof scale);
            if (!isfinite(scale) || !(scale > 0.0f)) {
                dropped++;      /* corrupt scale: drop just this record      */
                off += need;
                continue;
            }
            src.codes = (const int8_t *)(p + off + SFB_REC_HDR +
                                         (size_t)tl + (size_t)rl);
            src.scale = scale;
        } else {
            src.f32 = (const float *)(p + off + need - dim * sizeof(float));
        }

        if (tl + rl + 2u > scratch_cap) {
            char *ns = (char *)realloc(scratch, tl + rl + 2u);
            if (ns == NULL) {
                free(scratch);
                return SFB_E_NOMEM;
            }
            scratch = ns;
            scratch_cap = tl + rl + 2u;
        }
        memcpy(scratch, p + off + SFB_OFF_TEXT, tl);
        scratch[tl] = '\0';
        memcpy(scratch + tl + 1, p + off + SFB_OFF_TEXT + tl, rl);
        scratch[tl + 1 + rl] = '\0';

        if (id >= b->nrec) {
            rc = brain_pad_to(b, id);
            if (rc == SFB_OK)
                rc = brain_append(b, &src, scratch, scratch + tl + 1, flags,
                                  tier, strength, created_at, last_hit, 0, NULL);
        } else {
            rc = brain_overwrite(b, id, &src, scratch, scratch + tl + 1,
                                 flags, tier, strength, created_at, last_hit);
        }
        if (rc != SFB_OK) {
            free(scratch);
            return rc;
        }
        off += need;
    }

    free(scratch);
    if (dropped > 0) {
        b->store.records_dropped += dropped;
        brain_emit(b, "record_dropped", "{\"count\":%u}", dropped);
    }
    return SFB_OK;
}

/* Replay every segment into RAM. The graph is NOT built here (A7): open first
 * tries to adopt the saved topology and rebuilds only when it cannot. */
static int brain_replay_all(sfb_brain_t *b)
{
    uint32_t i, nseg;

    if (b == NULL)
        return SFB_E_INVALID;

    nseg = b->store.man.nseg;
    if (nseg > SFB_MAX_SEGS)
        nseg = SFB_MAX_SEGS;

    for (i = 0; i < nseg; i++) {
        const sfb_seg_entry_t *e = &b->store.man.segs[i];
        void *data = NULL;
        size_t len = 0;
        uint16_t seg_flags = 0;
        int rc;

        if (e->records == 0)
            continue;

        if (b->cfg.verify_full) {
            rc = sfb_store_segment_verify(&b->store, e);
            if (rc != SFB_OK) {
                (void)sfb_store_quarantine(&b->store, e);
                brain_emit(b, "segment_quarantined",
                           "{\"gen\":%u,\"seq\":%u,\"records\":%u}",
                           e->gen, e->seq, e->records);
                continue;
            }
        }

        /* W3.6: the payload encoding is a property of the segment, not of this
         * brain's config — a mixed store (f32 segments written before the
         * setting was turned on, int8 ones after) replays correctly. */
        rc = sfb_store_segment_read_ex(&b->store, e, &data, &len, &seg_flags);
        if (rc != SFB_OK) {
            (void)sfb_store_quarantine(&b->store, e);
            brain_emit(b, "segment_quarantined",
                       "{\"gen\":%u,\"seq\":%u,\"records\":%u}",
                       e->gen, e->seq, e->records);
            continue;
        }

        rc = sfb_brain_replay_segment_ex(b, data, len, e->records, seg_flags);
        free(data);
        if (rc == SFB_E_NOMEM)
            return rc;              /* out of memory: not a data verdict     */
        if (rc == SFB_E_CORRUPT) {
            (void)sfb_store_quarantine(&b->store, e);
            brain_emit(b, "segment_quarantined",
                       "{\"gen\":%u,\"seq\":%u,\"records\":%u}",
                       e->gen, e->seq, e->records);
        }
    }

    return SFB_OK;
}

/* Public contract: replay the segments and rebuild the graph from live rows. */
int sfb_brain_load_all(sfb_brain_t *b)
{
    int rc;

    if (b == NULL)
        return SFB_E_INVALID;
    rc = brain_replay_all(b);
    if (rc != SFB_OK)
        return rc;
    /* A5: one index build over the final live set, never per-segment adds. */
    return brain_rebuild_index(b);
}

/* ── open / close ───────────────────────────────────────────────────────── */
static void brain_free_ram(sfb_brain_t *b)
{
    if (b->index != NULL)
        sfb_hnsw_free(b->index);
    b->index = NULL;
    free(b->recs);
    free(b->arena);
    free(b->text_index);
    free(b->dirty_ids);
    free(b->vecs.vecs);
    free(b->vecs.qcodes);
    free(b->vecs.qscale);
    b->recs = NULL;
    b->arena = NULL;
    b->text_index = NULL;
    b->dirty_ids = NULL;
    b->vecs.vecs = NULL;
    b->vecs.qcodes = NULL;
    b->vecs.qscale = NULL;
}

int sfb_open(const sfb_config_t *cfg, sfb_brain_t **out)
{
    uint64_t t0 = sfb_now_us();
    sfb_brain_t *b;
    char lockpath[4096];
    int rc, fresh = 0, have_lock = 0, have_store = 0;

    if (cfg == NULL || out == NULL)
        return SFB_E_INVALID;
    *out = NULL;
    if (cfg->path == NULL || cfg->path[0] == '\0')
        return SFB_E_INVALID;
    if (cfg->dim < SFB_MIN_DIM || cfg->dim > SFB_MAX_DIM)
        return SFB_E_DIM;

    b = (sfb_brain_t *)calloc(1, sizeof *b);
    if (b == NULL)
        return SFB_E_NOMEM;
    b->lock.fd = -1;
    b->cfg = *cfg;
    /* VECTOR_DB is a database: rows do not fade. A caller who wants forgetting
     * chooses COGNITIVE. Without this a default-constructed vector_db brain
     * decays to FORGOTTEN and answers nothing after a few ticks. */
    if (b->cfg.mode == SFB_MODE_VECTOR_DB)
        b->cfg.decay = SFB_DECAY_NONE;
    b->cfg.path = NULL;
    if (b->cfg.half_life_secs <= 0.0)
        b->cfg.half_life_secs = 86400.0;
    if (b->cfg.decay_floor < 0.0)
        b->cfg.decay_floor = 0.05;
    if (b->cfg.dedup_text_similarity <= 0.0)
        b->cfg.dedup_text_similarity = 0.85;
    if (b->cfg.ef_search < 8u)
        b->cfg.ef_search = SFB_DEFAULT_EFS;

    {
        size_t n = strlen(cfg->path);
        char *pc = (char *)malloc(n + 1u);
        if (pc == NULL) {
            free(b);
            return SFB_E_NOMEM;
        }
        memcpy(pc, cfg->path, n + 1u);
        b->cfg.path = pc;
    }

    b->vecs.dim = b->cfg.dim;
    b->vecs.quantized = b->cfg.quantize_int8 ? 1 : 0;

    /* A reader must not create anything on disk. */
    if (!b->cfg.read_only) {
        rc = sfb_mkdir_p(b->cfg.path, 0755);
        if (rc != SFB_OK)
            goto fail;
    }

    rc = sfb_path_join(lockpath, sizeof lockpath, b->cfg.path, SFB_LOCK_NAME);
    if (rc != SFB_OK)
        goto fail;
    rc = sfb_lock_acquire(&b->lock, lockpath, b->cfg.read_only ? 1 : 0,
                          b->cfg.no_lock);
    if (rc != SFB_OK) {
        rc = (rc == SFB_E_INVALID || rc == SFB_E_NOMEM) ? rc : SFB_E_LOCKED;
        goto fail;
    }
    have_lock = 1;

    rc = sfb_store_open(&b->store, &b->cfg, &fresh);
    if (rc != SFB_OK)
        goto fail;              /* SFB_E_CORRUPT propagates: never empty     */
    have_store = 1;

    rc = brain_replay_all(b);
    if (rc != SFB_OK)
        goto fail;

    /* A7: adopt the saved topology when it matches this row set, else rebuild.
     * A broken graph never fails an open — segments are the truth. */
    if (brain_try_load_graph(b) != SFB_OK) {
        rc = brain_rebuild_index(b);
        if (rc != SFB_OK)
            goto fail;
    }

    b->stats.open_us = sfb_now_us() - t0;
    b->stats.records = b->nrec;
    b->stats.bytes_records = 0;         /* recomputed by stats_get           */

    brain_emit(b, "open", "{\"fresh\":%d,\"records\":%u,\"dim\":%u}",
               fresh, b->nrec, b->cfg.dim);
    *out = b;
    return SFB_OK;

fail:
    brain_free_ram(b);
    if (have_store)
        (void)sfb_store_close(&b->store);
    if (have_lock)
        (void)sfb_lock_release(&b->lock);
    free((char *)b->cfg.path);
    free(b);
    return rc;
}

int sfb_close(sfb_brain_t *b)
{
    int rc = SFB_OK;

    if (b == NULL)
        return SFB_E_INVALID;

    if (!b->cfg.read_only && b->dirty_count > 0)
        rc = sfb_flush(b, NULL);        /* best effort; report the failure    */
    if (!b->cfg.read_only && b->index != NULL) {
        /* Leave a topology behind for the next open even if flush skipped it. */
        if (brain_save_graph(b, 1) == SFB_OK)
            (void)sfb_store_manifest_save(&b->store);
    }

    brain_emit(b, "close", "{\"records\":%u}", b->nrec);

    brain_free_ram(b);
    (void)sfb_store_close(&b->store);
    if (b->lock.fd >= 0)
        (void)sfb_lock_release(&b->lock);
    free((char *)b->cfg.path);
    free(b);
    return rc;
}

/* ── remember ───────────────────────────────────────────────────────────── */
static int brain_capacity_guard(sfb_brain_t *b)
{
    uint32_t i, best = 0, live = 0;
    int found = 0;
    float best_s = 0.0f;
    double best_hit = 0.0;

    if (b->cfg.capacity == 0)
        return SFB_OK;

    for (i = 0; i < b->nrec; i++) {
        const sfb_record_t *r = &b->recs[i];

        if (!rec_live_visible(r))
            continue;
        live++;
        if (rec_pinned(r) || r->tier == SFB_TIER_ENTITY)
            continue;
        if (!found || r->strength < best_s ||
            (r->strength == best_s && r->last_hit < best_hit)) {
            found = 1;
            best = i;
            best_s = r->strength;
            best_hit = r->last_hit;
        }
    }
    if ((uint64_t)live < b->cfg.capacity)
        return SFB_OK;
    if (!found)
        return SFB_E_CAPACITY;      /* everything left is immune             */

    b->recs[best].flags |= SFB_F_TOMBSTONE;
    b->recs[best].flags &= ~SFB_F_LIVE;
    brain_mark_dirty(b, best);
    if (b->index != NULL)
        (void)sfb_hnsw_mark_deleted(b->index, best);
    brain_emit(b, "evict", "{\"id\":%u,\"reason\":\"capacity\"}", best);
    return SFB_OK;
}

static int brain_fold_into(sfb_brain_t *b, uint32_t id, double now,
                           const char *why)
{
    sfb_record_t *r = &b->recs[id];

    r->flags |= SFB_F_LIVE;
    r->flags &= ~SFB_F_TOMBSTONE;
    r->strength = 1.0f;
    r->last_hit = now;
    if (r->tier == SFB_TIER_FORGOTTEN)
        r->tier = SFB_TIER_EPISODIC;
    brain_mark_dirty(b, id);
    if (b->index != NULL && rec_live(r))
        (void)sfb_hnsw_undelete(b->index, id);
    brain_emit(b, "merge", "{\"into\":%u,\"reason\":\"%s\"}", id, why);
    return SFB_OK;
}

int sfb_remember(sfb_brain_t *b, const float *vec, const char *text,
                 const char *role, sfb_remember_result_t *res)
{
    /* Unchecked shim: the caller guarantees a buffer of cfg.dim floats.
     * Use sfb_remember_ex() when the length is known and must be enforced. */
    return sfb_remember_ex(b, vec, b != NULL ? (size_t)b->cfg.dim : 0u,
                           text, role, res);
}

int sfb_remember_ex(sfb_brain_t *b, const float *vec, size_t vec_len,
                    const char *text, const char *role,
                    sfb_remember_result_t *res)
{
    const char *txt, *rl;
    double now;
    uint32_t i, dim, flags, id = 0;
    int rc;

    if (b == NULL || vec == NULL)
        return SFB_E_INVALID;
    if (vec_len != (size_t)b->cfg.dim)
        return SFB_E_DIM;          /* W1.3: exact dimension, on write */

    if (b->cfg.read_only)
        return SFB_E_READONLY;
    if (b->vecs.dim != b->cfg.dim)
        return SFB_E_STATE;

    dim = b->cfg.dim;
    for (i = 0; i < dim; i++)
        if (!isfinite(vec[i]))
            return SFB_E_NONFINITE;

    if (text == NULL)
        return SFB_E_INVALID;      /* W1.1: no record without a label      */
    txt = text;
    rl = (role != NULL && role[0] != '\0') ? role : "user";
    now = sfb_now_secs();

    /* 1. exact text + same vector ⇒ the same memory, always folded. */
    {
        uint32_t into = 0;

        if (ti_find_exact(b, sfb_text_hash(txt), txt, vec, SFB_DEDUP_L2,
                          &into) == SFB_OK) {
            rc = brain_fold_into(b, into, now, "exact");
            if (rc != SFB_OK)
                return rc;
            if (res != NULL) {
                res->id = into;
                res->merged = 1;
                res->into = into;
            }
            return SFB_OK;
        }
    }

    /* 2. near-duplicate: graph neighbour + Jaccard on the text. */
    if (b->cfg.dedup_action != SFB_DEDUP_KEEP_BOTH && b->index != NULL &&
        b->nrec > 0 && sfb_hnsw_count(b->index) > 0) {
        sfb_hnsw_hit_t nh;
        uint32_t got = 0;

        memset(&nh, 0, sizeof nh);
        if (sfb_hnsw_search(b->index, vec, 1u, SFB_DEFAULT_EFS, &nh, &got) ==
                SFB_OK && got > 0 && nh.dist < (float)SFB_DEDUP_L2 &&
            nh.id < b->nrec && rec_live_visible(&b->recs[nh.id])) {
            double j = sfb_text_jaccard(txt, rec_text(b, &b->recs[nh.id]));

            if (j >= b->cfg.dedup_text_similarity) {
                if (b->cfg.dedup_action == SFB_DEDUP_REJECT) {
                    brain_emit(b, "dedup_reject", "{\"near\":%u}", nh.id);
                    return SFB_E_STATE;
                }
                rc = brain_fold_into(b, nh.id, now, "near_duplicate");
                if (rc != SFB_OK)
                    return rc;
                if (res != NULL) {
                    res->id = nh.id;
                    res->merged = 1;
                    res->into = nh.id;
                }
                return SFB_OK;
            }
        }
    }

    /* 3. capacity: one weakest row makes room. */
    rc = brain_capacity_guard(b);
    if (rc != SFB_OK)
        return rc;

    /* 4. insert. */
    flags = SFB_F_LIVE;
    if (strcmp(rl, SFB_PIN_ROLE) == 0)
        flags |= SFB_F_PINNED;

    {
        vec_src_t src;

        memset(&src, 0, sizeof src);
        src.f32 = vec;                  /* caller's f32 row (never aliased)   */
        rc = brain_append(b, &src, txt, rl, flags, SFB_TIER_EPISODIC, 1.0f,
                          now, now, 1, &id);
    }
    if (rc != SFB_OK)
        return rc;

    brain_mark_dirty(b, id);
    ti_maybe_grow(b);
    if (b->dirty_count >= SFB_SEGMENT_CAP) {
        sfb_flush_result_t fr;

        rc = sfb_flush(b, &fr);
        if (rc != SFB_OK)       /* row is safe in RAM and still dirty         */
            brain_emit(b, "flush_error", "{\"rc\":%d}", rc);
    }

    if (res != NULL) {
        res->id = id;
        res->merged = 0;
        res->into = 0;
    }
    return SFB_OK;
}

/* ── recall ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t id;
    float    dist;
    double   score;
} scored_t;

/* One entry of a single ranking list, used by the RRF fusion below. */
typedef struct {
    double key;
    uint32_t idx;
} rk_t;

static int rk_desc(const void *pa, const void *pb)
{
    const rk_t *a = (const rk_t *)pa, *b = (const rk_t *)pb;
    if (a->key > b->key) return -1;
    if (a->key < b->key) return 1;
    return (a->idx < b->idx) ? -1 : (a->idx > b->idx);
}

static int rk_asc(const void *pa, const void *pb)
{
    const rk_t *a = (const rk_t *)pa, *b = (const rk_t *)pb;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return (a->idx < b->idx) ? -1 : (a->idx > b->idx);
}

static int cmp_scored(const void *pa, const void *pb)
{
    const scored_t *a = (const scored_t *)pa;
    const scored_t *b = (const scored_t *)pb;

    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    if (a->id < b->id) return -1;
    if (a->id > b->id) return 1;
    return 0;
}

static void fill_hit(sfb_brain_t *b, sfb_hit_t *h, const scored_t *s)
{
    const sfb_record_t *r = &b->recs[s->id];

    h->id = s->id;
    h->distance = s->dist;
    h->score = (float)s->score;
    h->strength = r->strength;
    h->tier = r->tier;
    h->text = rec_text(b, r);
    h->role = rec_role(b, r);
}

/* Returns 1 when `id` already sits in out[0..n). */
static int already_emitted(const sfb_hit_t *out, uint32_t n, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        if (out[i].id == id)
            return 1;
    return 0;
}

int sfb_recall(sfb_brain_t *b, const float *vec, uint32_t k,
               sfb_hit_t *hits, uint32_t *n)
{
    /* Unchecked shim: see sfb_remember(). */
    return sfb_recall_ex(b, vec, b != NULL ? (size_t)b->cfg.dim : 0u, k, hits, n);
}

int sfb_recall_ex(sfb_brain_t *b, const float *vec, size_t vec_len, uint32_t k,
                  sfb_hit_t *hits, uint32_t *n)
{
    sfb_hnsw_hit_t *cand;
    scored_t *sc;
    uint32_t want, got = 0, nc = 0, i, o = 0, ef;
    uint64_t want64;
    double now;
    int rc;

    if (n != NULL)
        *n = 0;
    if (b == NULL || vec == NULL || hits == NULL || n == NULL)
        return SFB_E_INVALID;
    if (vec_len != (size_t)b->cfg.dim)
        return SFB_E_DIM;          /* W6.5: exact dimension, on query */
    if (k == 0)
        return SFB_OK;
    /* A NaN query silently matches nothing, which reads as "no memories":
     * refuse it instead of pretending the brain is empty. */
    for (i = 0; i < b->cfg.dim; i++)
        if (!isfinite(vec[i]))
            return SFB_E_NONFINITE;

    /* An empty brain is a legal query target, not an error. */
    if (b->nrec == 0 || b->index == NULL || !vec_rows_available(b))
        return SFB_OK;
    if (sfb_hnsw_count(b->index) == 0)
        return SFB_OK;

    want64 = (uint64_t)k * 3u;
    if (want64 > (uint64_t)b->nrec)
        want64 = b->nrec;
    want = (uint32_t)want64;
    if (want == 0)
        return SFB_OK;
    ef = b->cfg.ef_search != 0 ? b->cfg.ef_search : SFB_DEFAULT_EFS;
    if (ef < want)
        ef = want;

    cand = (sfb_hnsw_hit_t *)calloc(want, sizeof *cand);
    sc = (scored_t *)calloc(want, sizeof *sc);
    if (cand == NULL || sc == NULL) {
        free(cand);
        free(sc);
        return SFB_E_NOMEM;
    }

    rc = sfb_hnsw_search(b->index, vec, want, ef, cand, &got);
    if (rc != SFB_OK) {
        free(cand);
        free(sc);
        return rc;
    }
    if (got > want)
        got = want;

    now = sfb_now_secs();
    for (i = 0; i < got; i++) {
        const sfb_record_t *r;
        double st, s, tb;

        if (cand[i].id >= b->nrec)
            continue;
        r = &b->recs[cand[i].id];
        if (!rec_live(r) || r->tier == SFB_TIER_FORGOTTEN)
            continue;
        tb = tier_bonus(r->tier);
        if (!(tb > 0.0))
            continue;
        st = (double)r->strength;
        if (!(st > 0.0))
            st = 0.0;
        s = (1.0 / (1.0 + (double)cand[i].dist)) * (0.5 + 0.5 * st) * tb *
            recency_weight(r->last_hit, now);
        sc[nc].id = cand[i].id;
        sc[nc].dist = cand[i].dist;
        sc[nc].score = s;
        nc++;
    }
    free(cand);

    qsort(sc, nc, sizeof *sc, cmp_scored);
    for (i = 0; i < nc && o < k; i++) {
        if (already_emitted(hits, o, sc[i].id))
            continue;
        fill_hit(b, &hits[o], &sc[i]);
        o++;
    }
    free(sc);
    *n = o;
    return SFB_OK;
}

int sfb_recall_text(sfb_brain_t *b, const char *query, uint32_t k,
                    sfb_hit_t *hits, uint32_t *n)
{
    scored_t *sc;
    uint32_t i, nc = 0, o = 0, live = 0;
    double avg = 1.0, total = 0.0;

    if (n != NULL)
        *n = 0;
    if (b == NULL || query == NULL || hits == NULL || n == NULL)
        return SFB_E_INVALID;
    if (k == 0 || query[0] == '\0' || b->nrec == 0)
        return SFB_OK;

    sc = (scored_t *)calloc(b->nrec, sizeof *sc);
    if (sc == NULL)
        return SFB_E_NOMEM;

    for (i = 0; i < b->nrec; i++) {
        if (!rec_live_visible(&b->recs[i]))
            continue;
        live++;
        total += (double)b->recs[i].text_len;
    }
    if (live == 0) {
        free(sc);
        return SFB_OK;
    }
    avg = total / (double)live;
    if (avg < 1.0)
        avg = 1.0;

    for (i = 0; i < b->nrec; i++) {
        const sfb_record_t *r = &b->recs[i];
        double s;

        if (!rec_live_visible(r))
            continue;
        s = sfb_text_bm25(query, rec_text(b, r), avg, 1.2, 0.75);
        if (!(s > 0.0))
            continue;
        sc[nc].id = i;
        sc[nc].dist = 0.0f;
        sc[nc].score = s;
        nc++;
    }
    if (nc == 0) {
        free(sc);
        return SFB_OK;
    }

    /* ── W4.6: reciprocal-rank fusion ───────────────────────────────────
     * Three independent rankings are fused by rank instead of by raw score,
     * because BM25, cosine proximity and recency are on incomparable scales
     * (the old code divided BM25 by (1+distance), which let one list with a
     * large range silently dominate the others):
     *   lexical  - BM25, best first
     *   vector   - L2 distance to the centroid of the top lexical hits, which
     *              stands in for a query vector (sfb_recall_text has none)
     *   recency  - most recently hit first
     * fused = sum 1/(60 + rank), the standard RRF constant, 1-based ranks.
     */
    {
        const double RRF_K = 60.0;
        const uint32_t M = nc < 8u ? nc : 8u;
        const uint32_t d = b->vecs.dim;
        rk_t *rk = (rk_t *)malloc((size_t)nc * sizeof *rk);
        double *bm = (double *)malloc((size_t)nc * sizeof *bm);
        double *cx = NULL;
        float *row = NULL;             /* dequantise scratch (int8 mode)       */
        uint32_t best = 0, second = 0;
        int have_vec = 0, have_rec = 0;
        uint32_t j, t2;

        if (rk == NULL || bm == NULL) {
            free(rk);
            free(bm);
            free(sc);
            return SFB_E_NOMEM;
        }
        if (d != 0) {
            row = (float *)malloc((size_t)d * sizeof(float));
            if (row == NULL) {
                free(rk);
                free(bm);
                free(sc);
                return SFB_E_NOMEM;
            }
        }

        /* centroid = the query's vector stand-in */
        if (row != NULL && vec_rows_available(b) && d != 0) {
            uint32_t used = 0;
            cx = (double *)calloc(d, sizeof *cx);
            if (cx != NULL) {
                for (j = 0; j < M; j++) {
                    const float *v = vec_row_ptr(b, sc[j].id, row);
                    if (v == NULL)
                        continue;
                    for (t2 = 0; t2 < d; t2++)
                        cx[t2] += (double)v[t2];
                    used++;
                }
                if (used == 0) {
                    free(cx);
                    cx = NULL;
                } else {
                    for (t2 = 0; t2 < d; t2++)
                        cx[t2] /= (double)used;
                    have_vec = 1;
                }
            }
        }

        for (i = 0; i < nc; i++) {
            bm[i] = sc[i].score;                     /* raw BM25, kept for the guard */
            rk[i].idx = i;
            rk[i].key = sc[i].score;
        }
        qsort(rk, nc, sizeof *rk, rk_desc);
        for (i = 0; i < nc; i++)
            sc[rk[i].idx].score = 1.0 / (RRF_K + (double)(i + 1));

        /* An unambiguous lexical winner is never outvoted by fusion: when its
         * BM25 is far ahead of the runner-up, the other lists must not demote an
         * exact match. */
        best = rk[0].idx;
        second = rk[1].idx;
        if (nc > 1u && bm[best] >= 1.5 * bm[second])
            sc[best].score += 1.0;

        if (have_vec) {
            for (i = 0; i < nc; i++) {
                const float *v = vec_row_ptr(b, sc[i].id, row);
                double acc = 0.0;

                rk[i].idx = i;
                if (v == NULL) {
                    rk[i].key = 1e30;                /* no vector: last place */
                } else {
                    for (t2 = 0; t2 < d; t2++) {
                        double diff = (double)v[t2] - cx[t2];
                        acc += diff * diff;
                    }
                    rk[i].key = acc;
                    sc[i].dist = (float)sqrt(acc);
                }
            }
            qsort(rk, nc, sizeof *rk, rk_asc);
            for (i = 0; i < nc; i++)
                sc[rk[i].idx].score += 1.0 / (RRF_K + (double)(i + 1));
        }
        free(cx);

        for (i = 0; i < nc; i++) {
            rk[i].idx = i;
            rk[i].key = (double)b->recs[sc[i].id].last_hit;
            if (!(rk[i].key > 0.0))
                rk[i].key = -1.0;
            else
                have_rec = 1;
        }
        if (have_rec) {
            qsort(rk, nc, sizeof *rk, rk_desc);
            for (i = 0; i < nc; i++)
                sc[rk[i].idx].score += 1.0 / (RRF_K + (double)(i + 1));
        }
        /* Exact-phrase boost: a record containing the query verbatim outranks
         * a merely statistical match. Without this, a query whose terms all
         * appear in several records ties on BM25 and the winner falls to
         * whichever list happens to be freshest. */
        for (i = 0; i < nc; i++) {
            const char *txt = rec_text(b, &b->recs[sc[i].id]);
            if (txt != NULL && strcasestr(txt, query) != NULL)
                sc[i].score += 2.0;
        }

        free(rk);
        free(bm);
        free(row);

        qsort(sc, nc, sizeof *sc, cmp_scored);
    }

    for (i = 0; i < nc && o < k; i++) {
        if (!(sc[i].score > 0.0))
            continue;
        fill_hit(b, &hits[o], &sc[i]);
        o++;
    }
    free(sc);
    *n = o;
    return SFB_OK;
}

/* ── list ───────────────────────────────────────────────────────────────── */
int sfb_list(sfb_brain_t *b, uint64_t offset, uint32_t limit,
             sfb_hit_t *out, uint32_t *n)
{
    uint64_t seen = 0;
    uint32_t i, o = 0;

    if (n != NULL)
        *n = 0;
    if (b == NULL || n == NULL)
        return SFB_E_INVALID;
    if (limit == 0)
        return SFB_OK;
    if (out == NULL)
        return SFB_E_INVALID;

    for (i = 0; i < b->nrec && o < limit; i++) {
        const sfb_record_t *r = &b->recs[i];

        if (!rec_live_visible(r))
            continue;
        if (seen++ < offset)
            continue;
        out[o].id = i;
        out[o].distance = 0.0f;
        out[o].score = r->strength;
        out[o].strength = r->strength;
        out[o].tier = r->tier;
        out[o].text = rec_text(b, r);
        out[o].role = rec_role(b, r);
        o++;
    }
    *n = o;
    return SFB_OK;
}

/* ── forget / pin ───────────────────────────────────────────────────────── */
int sfb_forget(sfb_brain_t *b, uint32_t id)
{
    sfb_record_t *r;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;
    if (id >= b->nrec || !rec_live(&b->recs[id]))
        return SFB_E_NOTFOUND;

    r = &b->recs[id];
    r->flags |= SFB_F_TOMBSTONE;
    r->flags &= ~(SFB_F_LIVE | SFB_F_PINNED);
    brain_mark_dirty(b, id);
    if (b->index != NULL)
        (void)sfb_hnsw_mark_deleted(b->index, id);
    brain_emit(b, "forget", "{\"id\":%u}", id);
    return SFB_OK;
}

int sfb_pin(sfb_brain_t *b, uint32_t id, int pinned)
{
    sfb_record_t *r;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;
    if (id >= b->nrec || !rec_live(&b->recs[id]))
        return SFB_E_NOTFOUND;

    r = &b->recs[id];
    if (pinned) {
        r->flags |= SFB_F_PINNED;
        if (r->strength < (float)SFB_PIN_FLOOR)
            r->strength = (float)SFB_PIN_FLOOR;
        if (r->tier == SFB_TIER_FORGOTTEN)
            r->tier = SFB_TIER_EPISODIC;
    } else {
        r->flags &= ~SFB_F_PINNED;
    }
    brain_mark_dirty(b, id);
    brain_emit(b, "pin", "{\"id\":%u,\"pinned\":%d}", id, pinned ? 1 : 0);
    return SFB_OK;
}

/* ── decay ──────────────────────────────────────────────────────────────── */
int sfb_decay_tick(sfb_brain_t *b, uint64_t seconds)
{
    double hl, floor_v, now;
    uint32_t i, changed = 0, demoted = 0, expired = 0;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;
    if (b->cfg.decay == SFB_DECAY_NONE)
        return SFB_OK;

    hl = b->cfg.half_life_secs > 0.0 ? b->cfg.half_life_secs : 86400.0;
    floor_v = b->cfg.decay_floor > 0.0 ? b->cfg.decay_floor : 0.05;
    now = sfb_now_secs();

    for (i = 0; i < b->nrec; i++) {
        sfb_record_t *r = &b->recs[i];

        if (!rec_live_visible(r))
            continue;

        if (rec_pinned(r) && b->cfg.pins_immune) {
            /* Pins never demote and never sink below the floor. */
            if (r->strength < (float)SFB_PIN_FLOOR) {
                r->strength = (float)SFB_PIN_FLOOR;
                brain_mark_dirty(b, i);
                changed++;
            }
            continue;
        }

        if (b->cfg.decay == SFB_DECAY_TTL) {
            double age = now - r->last_hit;

            if (!(age >= 0.0))
                age = now - r->created_at;
            if (age > 2.0 * hl) {
                r->flags |= SFB_F_TOMBSTONE;
                r->flags &= ~SFB_F_LIVE;
                brain_mark_dirty(b, i);
                if (b->index != NULL)
                    (void)sfb_hnsw_mark_deleted(b->index, i);
                expired++;
                continue;
            }
        }

        if (b->cfg.decay == SFB_DECAY_EXPONENTIAL) {
            double f = exp(-(double)seconds / hl);
            float ns = (float)((double)r->strength * f);

            if (ns != r->strength) {
                r->strength = ns;
                brain_mark_dirty(b, i);
                changed++;
            }
        }

        /* The fix for "listed but unrecallable": anything under the floor is
         * FORGOTTEN *in RAM*, so sfb_list and sfb_recall agree on liveness. */
        if (r->strength < (float)floor_v) {
            if (r->tier != SFB_TIER_FORGOTTEN) {
                r->tier = SFB_TIER_FORGOTTEN;
                brain_mark_dirty(b, i);
                demoted++;
            }
        }
    }

    if (changed > 0 || demoted > 0 || expired > 0)
        brain_emit(b, "decay",
                   "{\"changed\":%u,\"forgotten\":%u,\"expired\":%u,\"secs\":%llu}",
                   changed, demoted, expired, (unsigned long long)seconds);
    return SFB_OK;
}

/* ── retain: drop sub-floor rows for real ───────────────────────────────── */
static int brain_should_drop(const sfb_brain_t *b, const sfb_record_t *r)
{
    if (r->flags & SFB_F_TOMBSTONE)
        return 1;
    if (!rec_live(r))
        return 1;                        /* inert hole: no payload worth it    */
    if (rec_pinned(r) && b->cfg.pins_immune)
        return 0;
    return r->tier == SFB_TIER_FORGOTTEN;
}

int sfb_retain(sfb_brain_t *b)
{
    uint32_t i, kept = 0, dropped = 0;
    sfb_record_t *nrecs;
    char *narena;
    size_t nlen = 0;
    float *nvecs = NULL;
    int8_t *nq = NULL;
    float *nqs = NULL;
    size_t dim;
    int rc;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;

    for (i = 0; i < b->nrec; i++)
        if (brain_should_drop(b, &b->recs[i]))
            dropped++;
    if (dropped == 0) {
        brain_emit(b, "retention", "{\"dropped\":0}");
        return SFB_OK;
    }

    /* Make the removal durable *before* forgetting it in RAM: old segments
     * still hold these rows, so a tombstone must reach the store first. */
    for (i = 0; i < b->nrec; i++) {
        if (!brain_should_drop(b, &b->recs[i]))
            continue;
        if ((b->recs[i].flags & SFB_F_TOMBSTONE) == 0) {
            b->recs[i].flags |= SFB_F_TOMBSTONE;
            b->recs[i].flags &= ~SFB_F_LIVE;
        }
        brain_mark_dirty(b, i);
        if (b->index != NULL)
            (void)sfb_hnsw_mark_deleted(b->index, i);
    }
    rc = sfb_flush(b, NULL);
    if (rc != SFB_OK && rc != SFB_E_READONLY)
        return rc;

    /* Compact the arrays.  W3.6: int8 mode compacts the codes and scales and
     * never materialises an f32 corpus array (nvecs stays NULL). */
    dim = b->vecs.dim;
    kept = b->nrec - dropped;
    nrecs = (sfb_record_t *)calloc(kept ? kept : 1u, sizeof *nrecs);
    narena = (char *)malloc(1u);
    if (nrecs == NULL || narena == NULL) {
        free(nrecs);
        free(narena);
        return SFB_E_NOMEM;
    }
    if (b->vecs.quantized) {
        nq = (int8_t *)calloc((kept ? kept : 1u) * dim, 1u);
        nqs = (float *)calloc(kept ? kept : 1u, sizeof(float));
        if (nq == NULL || nqs == NULL) {
            free(nrecs); free(narena); free(nq); free(nqs);
            return SFB_E_NOMEM;
        }
    } else {
        nvecs = (float *)calloc((kept ? kept : 1u) * dim, sizeof(float));
        if (nvecs == NULL) {
            free(nrecs); free(narena);
            return SFB_E_NOMEM;
        }
    }

    {
        sfb_brain_t tmp;
        uint32_t k = 0, renumbered = 0;

        memset(&tmp, 0, sizeof tmp);
        tmp.arena = narena;
        tmp.arena_cap = 1u;

        for (i = 0; i < b->nrec; i++) {
            uint32_t toff = 0, roff = 0;
            const float *v;

            if (brain_should_drop(b, &b->recs[i]))
                continue;

            rc = arena_put(&tmp, rec_text(b, &b->recs[i]),
                           b->recs[i].text_len, &toff);
            if (rc == SFB_OK)
                rc = arena_put(&tmp, rec_role(b, &b->recs[i]),
                               b->recs[i].role_len, &roff);
            if (rc != SFB_OK) {
                free(tmp.arena);
                free(nrecs); free(nvecs); free(nq); free(nqs);
                return rc;
            }

            nrecs[k] = b->recs[i];
            if (nrecs[k].id != k)
                renumbered++;
            nrecs[k].id = k;
            nrecs[k].flags &= ~SFB_F_DIRTY;
            nrecs[k].text_off = toff;
            nrecs[k].role_off = roff;

            if (nq != NULL) {
                memcpy(nq + (size_t)k * dim, b->vecs.qcodes + (size_t)i * dim,
                       dim);
                nqs[k] = b->vecs.qscale[i];
            } else {
                v = rec_vec_c(b, i);
                if (v != NULL)
                    memcpy(nvecs + (size_t)k * dim, v, dim * sizeof(float));
            }
            k++;
        }

        narena = tmp.arena;
        nlen = tmp.arena_len;
        kept = k;

        free(b->recs);
        free(b->arena);
        free(b->vecs.vecs);
        free(b->vecs.qcodes);
        free(b->vecs.qscale);

        b->recs = nrecs;
        b->nrec = kept;
        b->crec = kept ? kept : 1u;
        b->arena = narena;
        b->arena_len = nlen;
        b->arena_cap = tmp.arena_cap;
        b->vecs.vecs = nvecs;
        b->vecs.n = kept;
        b->vecs.cap = kept ? kept : 1u;
        b->vecs.qcodes = nq;
        b->vecs.qscale = nqs;
        b->dirty_count = 0;
        b->store.ids_renumbered += renumbered;
    }

    /* ids moved: the graph and the text index must be rebuilt, not patched. */
    rc = brain_rebuild_index(b);
    if (rc != SFB_OK)
        return rc;
    free(b->text_index);
    b->text_index = NULL;
    b->text_cap = 0;
    (void)ti_reserve(b, b->nrec);

    /* Ids were renumbered: the old topology must never be adopted again. */
    if (!b->cfg.read_only)
        (void)brain_save_graph(b, 1);

    brain_emit(b, "retention", "{\"dropped\":%u,\"kept\":%u}", dropped, kept);
    return SFB_OK;
}

/* ── consolidate: episodic cluster → one semantic representative ────────── */
#define SFB_CONSOLIDATE_MAX 1024u

int sfb_consolidate(sfb_brain_t *b)
{
    uint32_t *idx;
    uint32_t *parent;
    uint32_t n = 0, i, j, clusters = 0, made = 0;
    double now;
    float *row_a = NULL, *row_b = NULL;
    int rc = SFB_OK;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;
    if (!vec_rows_available(b) || b->nrec < 2)
        return SFB_OK;

    idx = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)b->nrec);
    if (idx == NULL)
        return SFB_E_NOMEM;
    for (i = 0; i < b->nrec && n < SFB_CONSOLIDATE_MAX; i++) {
        const sfb_record_t *r = &b->recs[i];

        if (!rec_live_visible(r) || r->tier != SFB_TIER_EPISODIC)
            continue;
        idx[n++] = i;
    }
    if (n < 2) {
        free(idx);
        return SFB_OK;
    }

    parent = (uint32_t *)malloc(sizeof(uint32_t) * n);
    if (parent == NULL) {
        free(idx);
        return SFB_E_NOMEM;
    }
    for (i = 0; i < n; i++)
        parent[i] = i;

    /* int8 mode dequantises the two rows under comparison (W3.6); f32 mode
     * gets pointers straight into the store, exactly as before. */
    if (b->vecs.quantized) {
        row_a = (float *)malloc((size_t)b->vecs.dim * sizeof(float));
        row_b = (float *)malloc((size_t)b->vecs.dim * sizeof(float));
        if (row_a == NULL || row_b == NULL) {
            free(row_a);
            free(row_b);
            free(parent);
            free(idx);
            return SFB_E_NOMEM;
        }
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1u; j < n; j++) {
            uint32_t a = i, bb = j;
            const float *va = vec_row_ptr(b, idx[i], row_a);
            const float *vb = vec_row_ptr(b, idx[j], row_b);

            if (va == NULL || vb == NULL ||
                l2_dist(va, vb, b->vecs.dim) >= (double)SFB_DEDUP_L2)
                continue;
            while (parent[a] != a) { a = parent[a]; }
            while (parent[bb] != bb) { bb = parent[bb]; }
            if (a != bb)
                parent[bb] = a;
        }
    }

    now = sfb_now_secs();
    for (i = 0; i < n; i++) {
        uint32_t root = i, members = 0, first = 0, newid = 0;
        uint32_t flags = SFB_F_LIVE;
        float best = 0.0f;

        while (parent[root] != root)
            root = parent[root];
        for (j = 0; j < n; j++) {
            uint32_t rj = j;

            while (parent[rj] != rj)
                rj = parent[rj];
            if (rj != root)
                continue;
            if (members == 0)
                first = idx[j];
            members++;
            if (b->recs[idx[j]].strength > best)
                best = b->recs[idx[j]].strength;
            if (rec_pinned(&b->recs[idx[j]]))
                flags |= SFB_F_PINNED;
        }
        if (members < 2 || root != i)
            continue;

        clusters++;
        {
            vec_src_t src;
            const float *v = vec_row_ptr(b, first, row_a);

            memset(&src, 0, sizeof src);
            src.f32 = v;
            rc = brain_append(b, &src, rec_text(b, &b->recs[first]),
                              "representative", flags, SFB_TIER_SEMANTIC,
                              best > 0.0f ? best : 1.0f, now, now, 1, &newid);
        }
        if (rc != SFB_OK) {
            free(row_a);
            free(row_b);
            free(parent);
            free(idx);
            return rc;
        }
        brain_mark_dirty(b, newid);
        made++;

        for (j = 0; j < n; j++) {
            uint32_t rj = j;

            while (parent[rj] != rj)
                rj = parent[rj];
            if (rj != root)
                continue;
            b->recs[idx[j]].flags |= SFB_F_TOMBSTONE;
            b->recs[idx[j]].flags &= ~SFB_F_LIVE;
            brain_mark_dirty(b, idx[j]);
            if (b->index != NULL)
                (void)sfb_hnsw_mark_deleted(b->index, idx[j]);
        }
    }

    free(row_a);
    free(row_b);
    free(parent);
    free(idx);
    ti_maybe_grow(b);
    if (clusters > 0)
        brain_emit(b, "consolidate",
                   "{\"clusters\":%u,\"representatives\":%u}", clusters, made);
    return SFB_OK;
}

/* ── flush ──────────────────────────────────────────────────────────────── */
static int brain_file_digest(const char *path, uint32_t *crc_out)
{
    int fd;
    uint8_t *buf;
    const size_t chunk = 65536u;
    size_t n;
    off_t off = 0;
    uint32_t crc = 0;
    struct stat st;

    *crc_out = 0;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return SFB_E_IO;
    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return SFB_E_IO;
    }
    buf = (uint8_t *)malloc(chunk);
    if (buf == NULL) {
        (void)close(fd);
        return SFB_E_NOMEM;
    }
    while (off < st.st_size) {
        size_t want = (size_t)(st.st_size - off);
        int rc;

        if (want > chunk)
            want = chunk;
        rc = sfb_pread_all(fd, buf, want, off);
        if (rc != SFB_OK) {
            free(buf);
            (void)close(fd);
            return rc;
        }
        n = want;
        crc = sfb_crc32c_update(crc, buf, n);
        off += (off_t)n;
    }
    free(buf);
    (void)close(fd);
    *crc_out = crc;
    return SFB_OK;
}

static uint64_t brain_logical_bytes(const sfb_brain_t *b)
{
    uint64_t total = 0;
    uint32_t i;
    /* W3.6: what a live row costs in the block that is actually written —
     * dim*4 for an f32 vector, dim code bytes + its 4-byte scale for int8. */
    const uint64_t vec_bytes = b->vecs.quantized
                                   ? (uint64_t)b->vecs.dim + sizeof(float)
                                   : (uint64_t)b->vecs.dim * sizeof(float);

    for (i = 0; i < b->nrec; i++) {
        const sfb_record_t *r = &b->recs[i];

        if (!rec_live_visible(r))
            continue;
        total += (uint64_t)SFB_REC_HDR + r->text_len + r->role_len +
                 vec_bytes;
    }
    return total;
}

int sfb_flush(sfb_brain_t *b, sfb_flush_result_t *res)
{
    uint64_t t0 = sfb_now_us();
    uint64_t written = 0, sealed = 0, bytes = 0;
    int rc = SFB_OK;

    if (res != NULL)
        memset(res, 0, sizeof *res);
    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;

    if (b->dirty_count > 0) {
        void *buf = NULL;
        size_t len = 0;
        uint32_t nrec = 0, mn = 0, mx = 0, seq = 0;

        rc = sfb_brain_serialize_dirty(b, &buf, &len, &nrec, &mn, &mx);
        if (rc != SFB_OK)
            goto finish;

        if (nrec > 0 && len > 0) {
            rc = sfb_store_segment_write(&b->store, buf, len, nrec, mn, mx,
                                         &seq);
            if (rc != SFB_OK) {
                /* SFB_E_ENOSPC / SFB_E_CORRUPT travel to the caller intact. */
                free(buf);
                goto finish;
            }
            written = nrec;
            sealed = 1;
            bytes = (uint64_t)len;
            b->store.bytes_written += bytes;
        }
        free(buf);
    }

    /* A8: the graph is a cache, not the truth. Segments alone are durable, so
     * a skipped or failed graph save costs at most a rebuild at next open. */
    {
        int grc = brain_save_graph(b, 0);

        if (grc != SFB_OK)
            brain_emit(b, "graph_save_failed", "{\"rc\":%d}", grc);
    }

    rc = sfb_store_manifest_save(&b->store);
    if (rc != SFB_OK)
        goto finish;

    /* Only now is the write durable: a failure above leaves the rows dirty so
     * the next flush retries them (replay upserts by id, so a retry is safe). */
    if (b->dirty_count > 0)
        brain_clear_dirty(b);

finish:
    b->stats.flushes++;
    b->stats.flush_last_us = sfb_now_us() - t0;
    b->stats.records = b->nrec;
    b->stats.bytes_records = brain_logical_bytes(b);

    if (res != NULL) {
        res->ok = rc == SFB_OK ? 1 : 0;
        res->records_written = written;
        res->segments_sealed = sealed;
        res->bytes_written = bytes;
        res->duration_us = b->stats.flush_last_us;
    }
    if (rc == SFB_OK)
        brain_emit(b, "flush",
                   "{\"records\":%llu,\"segments\":%llu,\"bytes\":%llu}",
                   (unsigned long long)written,
                   (unsigned long long)sealed,
                   (unsigned long long)bytes);
    else
        brain_emit(b, "flush_error", "{\"rc\":%d}", rc);
    return rc;
}

/* ── compact ────────────────────────────────────────────────────────────── */
int sfb_compact(sfb_brain_t *b, uint64_t *bytes_before, uint64_t *bytes_after)
{
    uint64_t before, removed = 0, keep_count = 0, keep_bytes = 0;
    uint32_t old_nseg, start, i, fresh_n = 0;
    sfb_seg_entry_t *fresh = NULL;
    int rc;

    if (b == NULL)
        return SFB_E_INVALID;
    if (b->cfg.read_only)
        return SFB_E_READONLY;

    before = sfb_store_bytes_on_disk(&b->store);
    if (bytes_before != NULL)
        *bytes_before = before;

    /* Pending rows first: the snapshot must not race a dirty row out of RAM. */
    rc = sfb_flush(b, NULL);
    if (rc != SFB_OK)
        return rc;

    old_nseg = b->store.man.nseg;

    /* Snapshot the live set as fresh segments, SFB_SEGMENT_CAP rows at a time,
     * using exactly the record block the normal flush writes (same reader).
     * sfb_store_segment_write persists the manifest after each one, so the old
     * entries stay referenced and readable until the swap below. */
    for (start = 0; start < b->nrec; start += SFB_SEGMENT_CAP) {
        uint32_t end = b->nrec - start > SFB_SEGMENT_CAP
                           ? start + SFB_SEGMENT_CAP : b->nrec;
        uint32_t n = 0, mn = 0, mx = 0, seq = 0, wrote = 0;
        void *buf = NULL;
        size_t len = 0;

        for (i = start; i < end; i++)
            if (rec_live_visible(&b->recs[i])) {
                brain_mark_dirty(b, i);
                n++;
            }
        if (n == 0)
            continue;

        rc = sfb_brain_serialize_dirty(b, &buf, &len, &wrote, &mn, &mx);
        if (rc == SFB_OK && wrote > 0 && len > 0)
            rc = sfb_store_segment_write(&b->store, buf, len, wrote, mn, mx, &seq);
        free(buf);
        if (rc != SFB_OK)
            return rc;              /* old manifest is still the live one     */
        brain_clear_dirty(b);
    }

    fresh_n = b->store.man.nseg - old_nseg;
    fresh = (sfb_seg_entry_t *)calloc(fresh_n ? fresh_n : 1u, sizeof *fresh);
    if (fresh == NULL)
        return SFB_E_NOMEM;
    for (i = 0; i < fresh_n; i++) {
        fresh[i] = b->store.man.segs[old_nseg + i];
        keep_count += fresh[i].records;
        keep_bytes += fresh[i].length;
    }

    /* Graph for the newest generation, written before anything is dropped. */
    rc = brain_save_graph(b, 1);
    if (rc != SFB_OK) {
        free(fresh);
        return rc;
    }

    /* Point the manifest at the snapshot alone: everything older is now an
     * unreferenced file that sfb_store_reclaim() may delete. */
    free(b->store.man.segs);
    b->store.man.segs = fresh;
    b->store.man.nseg = fresh_n;
    b->store.man.count = (uint32_t)keep_count;
    b->store.man.total_bytes = keep_bytes;

    rc = sfb_store_manifest_save(&b->store);
    if (rc != SFB_OK) {
        (void)sfb_store_manifest_load(&b->store);   /* resync RAM to disk    */
        return rc;
    }

    rc = sfb_store_reclaim(&b->store, &removed);
    if (rc != SFB_OK)
        return rc;

    rc = brain_rebuild_index(b);
    if (rc != SFB_OK)
        return rc;
    free(b->text_index);
    b->text_index = NULL;
    b->text_cap = 0;
    (void)ti_reserve(b, b->nrec);

    /* reset the byte counters to what the store actually holds now */
    b->stats.bytes_records = brain_logical_bytes(b);
    b->stats.bytes_on_disk = sfb_store_bytes_on_disk(&b->store);
    b->stats.segments = b->store.man.nseg;
    b->stats.generations = b->store.man.generation;
    b->stats.records = b->nrec;

    if (bytes_after != NULL)
        *bytes_after = b->stats.bytes_on_disk;
    brain_emit(b, "compact",
               "{\"bytes_before\":%llu,\"bytes_after\":%llu,\"files_removed\":%llu,\"segments\":%u}",
               (unsigned long long)before,
               (unsigned long long)b->stats.bytes_on_disk,
               (unsigned long long)removed, b->store.man.nseg);
    return SFB_OK;
}

/* ── count / stats / events ─────────────────────────────────────────────── */
int sfb_count(const sfb_brain_t *b, uint64_t *live)
{
    if (live != NULL)
        *live = 0;
    if (b == NULL || live == NULL)
        return SFB_E_INVALID;

    *live = live_count(b);
    return SFB_OK;
}

int sfb_stats_get(const sfb_brain_t *b, sfb_stats_t *out)
{
    uint32_t i;
    uint64_t live = 0, tomb = 0;

    if (b == NULL || out == NULL)
        return SFB_E_INVALID;

    for (i = 0; i < b->nrec; i++) {
        const sfb_record_t *r = &b->recs[i];

        if (r->flags & SFB_F_TOMBSTONE)
            tomb++;
        if (rec_live_visible(r))
            live++;
    }

    memset(out, 0, sizeof *out);
    out->records = b->nrec;
    out->live = live;
    out->tombstones = tomb;
    out->segments = b->store.man.nseg;
    out->generations = b->store.man.generation;
    out->bytes_records = brain_logical_bytes(b);
    out->bytes_on_disk = sfb_store_bytes_on_disk(&b->store);

    /* recovery counters: the store is their single source of truth (W1.8) */
    out->records_dropped = b->store.records_dropped;
    out->segments_quarantined = b->store.segments_quarantined;
    out->ids_renumbered = b->store.ids_renumbered;
    out->index_rebuilt = b->store.index_rebuilt;

    out->flushes = b->stats.flushes;
    out->flush_last_us = b->stats.flush_last_us;
    out->open_us = b->stats.open_us;
    out->events = b->stats.events;
    return SFB_OK;
}

int sfb_on_event(sfb_brain_t *b, sfb_event_cb cb, void *ud)
{
    if (b == NULL)
        return SFB_E_INVALID;
    b->event_cb = cb;
    b->event_ud = ud;
    return SFB_OK;
}
