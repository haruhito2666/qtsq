/*
 * Quantum Tensor Sequence — C2PA / Content Credentials (Phase 10)
 *
 * Implements a compact, self-contained C2PA-style claim store:
 *   JSON-LD assertions → CBOR claim (RFC 8949) → JUMBF superbox → ML-DSA-65
 *   signature.  See include/qtsq_c2pa.h for the API and the interoperability
 *   note.
 *
 * Author: Haruhito
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "qtsq.h"
#include "qtsq_c2pa.h"
#include "../crypto/common/fips202.h"
#include "../crypto/ml-dsa-65/api.h"

#define C2PA_MAGIC_B0 'C'
#define C2PA_MAGIC_B1 '2'
#define C2PA_MAGIC_B2 'P'
#define C2PA_MAGIC_B3 'A'

/* ═══════════════════ Dynamic byte buffer ═══════════════════ */
typedef struct { uint8_t *b; size_t n, cap; } cbuf_t;
static int cbuf_reserve(cbuf_t *c, size_t extra) {
    if (c->n + extra <= c->cap) return 0;
    size_t nc = c->cap ? c->cap : 64;
    while (nc < c->n + extra) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(c->b, nc);
    if (!nb) return QTSQ_ERR_ALLOC;
    c->b = nb; c->cap = nc;
    return QTSQ_OK;
}
static int cb_put(cbuf_t *c, const void *data, size_t len) {
    int r = cbuf_reserve(c, len);
    if (r) return r;
    memcpy(c->b + c->n, data, len); c->n += len;
    return QTSQ_OK;
}
static int cb_u32(cbuf_t *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v>>24), (uint8_t)(v>>16), (uint8_t)(v>>8), (uint8_t)v };
    return cb_put(c, b, 4);
}

/* ═══════════════════ CBOR (RFC 8949) encoder ═══════════════════ */
static int cbor_head(cbuf_t *c, uint8_t major, uint64_t n) {
    uint8_t h[9]; int hlen;
    if (n < 24) { h[0] = (uint8_t)((major<<5) | n); hlen = 1; }
    else if (n <= 0xFF) { h[0] = (uint8_t)((major<<5) | 24); h[1] = (uint8_t)n; hlen = 2; }
    else if (n <= 0xFFFF) { h[0]=(uint8_t)((major<<5)|25); h[1]=(uint8_t)(n>>8); h[2]=(uint8_t)n; hlen=3; }
    else { h[0]=(uint8_t)((major<<5)|26); h[1]=(uint8_t)(n>>24); h[2]=(uint8_t)(n>>16);
           h[3]=(uint8_t)(n>>8); h[4]=(uint8_t)n; hlen=5; }
    return cb_put(c, h, (size_t)hlen);
}
static int cbor_uint(cbuf_t *c, uint64_t n) { return cbor_head(c, 0, n); }
static int cbor_text(cbuf_t *c, const char *s) {
    size_t l = strlen(s);
    int r = cbor_head(c, 3, l);
    if (r) return r;
    return cb_put(c, s, l);
}
static int cbor_bytes(cbuf_t *c, const void *p, size_t l) {
    int r = cbor_head(c, 2, l);
    if (r) return r;
    return cb_put(c, p, l);
}

/* ═══════════════════ JUMBF box writer ═══════════════════ */
static int jumb_box(cbuf_t *out, const char type[4], const uint8_t *payload, size_t plen) {
    uint32_t size = (uint32_t)(8 + plen);
    cbuf_t body = {0,0,0};
    int r = cb_u32(&body, size);
    if (!r) r = cb_put(&body, type, 4);
    if (!r) r = cb_put(&body, payload, plen);
    if (!r) r = cb_put(out, body.b, body.n);
    free(body.b);
    return r;
}

/* ═══════════════════ Manifest construction ═══════════════════ */
static void c2pa_now(char iso[64], uint64_t *unix_t) {
    time_t t = time(NULL);
    if (unix_t) *unix_t = (uint64_t)t;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(iso, 64, "%Y-%m-%dT%H:%M:%SZ", &g);
}

int qtsq_c2pa_build_manifest(const qtsq_c2pa_metadata_t *meta,
                             const uint8_t sign_sk[4032],
                             const uint8_t sign_pk[1952],
                             uint8_t **out_manifest, size_t *out_size) {
    if (!meta || !sign_sk || !sign_pk || !out_manifest || !out_size)
        return QTSQ_ERR_NULL;
    *out_manifest = NULL; *out_size = 0;

    /* ISO-8601 creation timestamp (fill if the caller left it empty). */
    char iso_local[64];
    uint64_t unix_t = meta->created_at;
    if (meta->created_at == 0 || meta->created_iso[0] == '\0') {
        c2pa_now(iso_local, &unix_t);
    } else {
        strncpy(iso_local, meta->created_iso, 63); iso_local[63] = 0;
    }

    int rc = QTSQ_OK;
    cbuf_t claim = {0,0,0};    /* CBOR claim */
    cbuf_t jumd = {0,0,0};     /* description box payload */
    cbuf_t children = {0,0,0}; /* child boxes of the JUMBF superbox */
    cbuf_t man = {0,0,0};      /* assembled manifest */

    /* ── 1. CBOR claim (map of 11 pairs) ── */
    rc = cbor_head(&claim, 5, 11);
    if (!rc) rc = cbor_text(&claim, "alg");
    if (!rc) rc = cbor_text(&claim, "ML-DSA-65");
    if (!rc) rc = cbor_text(&claim, "vendor");
    if (!rc) rc = cbor_text(&claim, "QTSQ");
    if (!rc) rc = cbor_text(&claim, "claim_generator");
    if (!rc) rc = cbor_text(&claim, meta->claim_generator);
    if (!rc) rc = cbor_text(&claim, "action");
    if (!rc) rc = cbor_text(&claim, "c2pa.created");
    if (!rc) rc = cbor_text(&claim, "c2pa.created");
    if (!rc) rc = cbor_text(&claim, iso_local);
    if (!rc) rc = cbor_text(&claim, "author_id");
    if (!rc) rc = cbor_text(&claim, meta->author_id);
    if (!rc) rc = cbor_text(&claim, "license");
    if (!rc) rc = cbor_text(&claim, meta->license);
    if (!rc) rc = cbor_text(&claim, "created_at");
    if (!rc) rc = cbor_uint(&claim, unix_t);
    if (!rc) rc = cbor_text(&claim, "intent");
    if (!rc) rc = cbor_uint(&claim, meta->intent);
    if (!rc) rc = cbor_text(&claim, "consent_flags");
    if (!rc) rc = cbor_uint(&claim, meta->consent_flags);
    if (!rc) rc = cbor_text(&claim, "work_hash");
    if (!rc) rc = cbor_bytes(&claim, meta->work_hash, 32);
    if (rc) goto done;

    /* ── 2. JUMBF superbox: jumd + cbor + qtsi + qtpk as child boxes ── */
    /* jumd: version=1, flags=0, toggle=0, label "c2pa\0" */
    if (!rc) rc = cb_u32(&jumd, 1);
    if (!rc) rc = cb_u32(&jumd, 0);
    if (!rc) { uint8_t togg = 0; rc = cb_put(&jumd, &togg, 1); }
    if (!rc) rc = cb_put(&jumd, QTSQ_C2PA_LABEL, 5);
    { cbuf_t b = {0,0,0}; cb_put(&b, jumd.b, jumd.n);
      rc = jumb_box(&children, "jumd", b.b, b.n); free(b.b); }
    if (rc) goto done;
    { cbuf_t b = {0,0,0}; cb_put(&b, claim.b, claim.n);
      rc = jumb_box(&children, "cbor", b.b, b.n); free(b.b); }
    if (rc) goto done;

    /* Sign the claim digest with ML-DSA-65. */
    {
        uint8_t digest[32];
        sha3_256(digest, claim.b, claim.n);
        uint8_t sig[PQCLEAN_MLDSA65_CLEAN_CRYPTO_BYTES];
        size_t siglen = 0;
        if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_signature(
                sig, &siglen, digest, 32, sign_sk) != 0) { rc = QTSQ_ERR_ALLOC; goto done; }
        cbuf_t b = {0,0,0};
        cb_u32(&b, (uint32_t)siglen); cb_put(&b, sig, siglen);
        rc = jumb_box(&children, "qtsi", b.b, b.n); free(b.b);
    }
    if (rc) goto done;
    { cbuf_t b = {0,0,0}; cb_put(&b, sign_pk, 1952);
      rc = jumb_box(&children, "qtpk", b.b, b.n); free(b.b); }
    if (rc) goto done;

    /* ── 3. Wrap the children in a "jumb" superbox; assemble manifest ── */
    { cbuf_t b = {0,0,0}; cb_put(&b, children.b, children.n);
      rc = jumb_box(&man, "jumb", b.b, b.n); free(b.b); }
    if (rc) goto done;

    /* Prepend magic + version. */
    { uint8_t hdr[5] = {C2PA_MAGIC_B0, C2PA_MAGIC_B1, C2PA_MAGIC_B2, C2PA_MAGIC_B3,
                        QTSQ_C2PA_VERSION};
      cbuf_t full = {0,0,0};
      cb_put(&full, hdr, 5); cb_put(&full, man.b, man.n);
      free(man.b); man = full;
    }
    if (rc) goto done;

    *out_manifest = man.b; man.b = NULL;
    *out_size = man.n;

done:
    free(claim.b); free(jumd.b); free(children.b); free(man.b);
    return rc;
}

/* ═══════════════════ Manifest parsing ═══════════════════ */
/* Box header reader: returns payload pointer/size and advances. */
typedef struct { const uint8_t *p; size_t n; } rd_t;
static int rd_box(rd_t *r, char type[4], const uint8_t **payload, size_t *plen) {
    if (r->n < 8) return QTSQ_ERR_FORMAT;
    uint32_t size = ((uint32_t)r->p[0]<<24)|((uint32_t)r->p[1]<<16)|((uint32_t)r->p[2]<<8)|r->p[3];
    memcpy(type, r->p + 4, 4);
    if (size < 8 || size > r->n) return QTSQ_ERR_FORMAT;
    *payload = r->p + 8;
    *plen = size - 8;
    r->p += size; r->n -= size;
    return QTSQ_OK;
}

/* Minimal CBOR reader: parse claim map, extract keys we know. */
typedef struct { const uint8_t *p; size_t n; } cbr_t;
static int cbr_advance(cbr_t *r, uint8_t *major, uint64_t *val, size_t *payload_off) {
    if (r->n < 1) return QTSQ_ERR_FORMAT;
    uint8_t ini = r->p[0];
    *major = ini >> 5;
    uint8_t ai = ini & 0x1F;
    size_t off = 1;
    uint64_t v = 0;
    if (ai < 24) v = ai;
    else if (ai == 24) { if (r->n < 2) return QTSQ_ERR_FORMAT; v = r->p[1]; off = 2; }
    else if (ai == 25) { if (r->n < 3) return QTSQ_ERR_FORMAT; v=((uint64_t)r->p[1]<<8)|r->p[2]; off=3; }
    else if (ai == 26) { if (r->n < 5) return QTSQ_ERR_FORMAT;
        v=((uint64_t)r->p[1]<<24)|((uint64_t)r->p[2]<<16)|((uint64_t)r->p[3]<<8)|r->p[4]; off=5; }
    else return QTSQ_ERR_FORMAT;
    *val = v; *payload_off = off;
    return QTSQ_OK;
}

int qtsq_c2pa_parse_manifest(const uint8_t *manifest, size_t size,
                             qtsq_c2pa_metadata_t *meta_out,
                             char **claim_json_out) {
    if (!manifest || !meta_out) return QTSQ_ERR_NULL;
    memset(meta_out, 0, sizeof(*meta_out));
    if (size < 6) return QTSQ_ERR_FORMAT;
    if (manifest[0] != C2PA_MAGIC_B0 || manifest[1] != C2PA_MAGIC_B1 ||
        manifest[2] != C2PA_MAGIC_B2 || manifest[3] != C2PA_MAGIC_B3)
        return QTSQ_ERR_FORMAT;
    if (manifest[4] != QTSQ_C2PA_VERSION) return QTSQ_ERR_FORMAT;

    rd_t rd = { manifest + 5, size - 5 };
    char type[4]; const uint8_t *pload; size_t plen;
    if (rd_box(&rd, type, &pload, &plen) != QTSQ_OK) return QTSQ_ERR_FORMAT;
    if (memcmp(type, "jumb", 4) != 0) return QTSQ_ERR_FORMAT;

    /* Walk children of the superbox. */
    const uint8_t *claim = NULL; size_t claim_len = 0;
    rd_t sub = { pload, plen };
    while (sub.n > 0) {
        char t2[4]; const uint8_t *p2; size_t l2;
        if (rd_box(&sub, t2, &p2, &l2) != QTSQ_OK) return QTSQ_ERR_FORMAT;
        if (memcmp(t2, "cbor", 4) == 0) { claim = p2; claim_len = l2; }
    }
    if (!claim) return QTSQ_ERR_FORMAT;

    /* Parse CBOR claim map. */
    cbr_t cr = { claim, claim_len };
    uint8_t major; uint64_t npairs; size_t off;
    if (cbr_advance(&cr, &major, &npairs, &off) != QTSQ_OK) return QTSQ_ERR_FORMAT;
    if (major != 5) return QTSQ_ERR_FORMAT;
    cr.p += off; cr.n -= off;

    for (uint64_t pair = 0; pair < npairs; pair++) {
        uint64_t keylen; size_t koff;
        if (cbr_advance(&cr, &major, &keylen, &koff) != QTSQ_OK) return QTSQ_ERR_FORMAT;
        if (major != 3 || keylen >= 64) return QTSQ_ERR_FORMAT;
        char key[64]; memcpy(key, cr.p + koff, (size_t)keylen); key[keylen] = 0;
        cr.p += koff + (size_t)keylen; cr.n -= koff + (size_t)keylen;

        /* value */
        uint8_t vm; uint64_t vv; size_t voff;
        if (cbr_advance(&cr, &vm, &vv, &voff) != QTSQ_OK) return QTSQ_ERR_FORMAT;
        if (vm == 3 && vv < 128) { /* text value */
            char val[160]; memcpy(val, cr.p + voff, (size_t)vv); val[vv] = 0;
            if (!strcmp(key,"claim_generator")) strncpy(meta_out->claim_generator, val, 63);
            else if (!strcmp(key,"author_id")) strncpy(meta_out->author_id, val, 127);
            else if (!strcmp(key,"license")) strncpy(meta_out->license, val, 63);
            else if (!strcmp(key,"c2pa.created")) strncpy(meta_out->created_iso, val, 63);
            cr.p += voff + (size_t)vv; cr.n -= voff + (size_t)vv;
        } else if (vm == 0) { /* uint */
            if (!strcmp(key,"created_at")) meta_out->created_at = vv;
            else if (!strcmp(key,"intent")) meta_out->intent = (uint32_t)vv;
            else if (!strcmp(key,"consent_flags")) meta_out->consent_flags = (uint32_t)vv;
            cr.p += voff; cr.n -= voff;
        } else if (vm == 2 && vv == 32) { /* byte string work_hash */
            if (cr.n < voff + 32) return QTSQ_ERR_FORMAT;
            memcpy(meta_out->work_hash, cr.p + voff, 32);
            cr.p += voff + 32; cr.n -= voff + 32;
        } else {
            cr.p += voff; cr.n -= voff;
        }
    }

    if (claim_json_out && *claim_json_out == NULL) {
        /* Build a JSON-LD representation of the claim assertions. */
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"@context\":\"http://schema.org\",\"claim_generator\":\"%s\","
                 "\"action\":\"c2pa.created\",\"c2pa.created\":\"%s\","
                 "\"author_id\":\"%s\",\"intent\":%u,\"consent_flags\":%u}",
                 meta_out->claim_generator, meta_out->created_iso,
                 meta_out->author_id, meta_out->intent, meta_out->consent_flags);
        *claim_json_out = (char *)malloc(strlen(buf) + 1);
        if (!*claim_json_out) return QTSQ_ERR_ALLOC;
        strcpy(*claim_json_out, buf);
    }
    return QTSQ_OK;
}

int qtsq_c2pa_verify_manifest(const uint8_t *manifest, size_t size,
                              const uint8_t sign_pk[1952]) {
    if (!manifest || !sign_pk) return QTSQ_ERR_NULL;
    if (size < 6) return QTSQ_ERR_FORMAT;
    if (manifest[0] != C2PA_MAGIC_B0 || manifest[1] != C2PA_MAGIC_B1 ||
        manifest[2] != C2PA_MAGIC_B2 || manifest[3] != C2PA_MAGIC_B3)
        return QTSQ_ERR_FORMAT;

    rd_t rd = { manifest + 5, size - 5 };
    char type[4]; const uint8_t *pload; size_t plen;
    if (rd_box(&rd, type, &pload, &plen) != QTSQ_OK) return QTSQ_ERR_FORMAT;
    if (memcmp(type, "jumb", 4) != 0) return QTSQ_ERR_FORMAT;

    const uint8_t *claim = NULL; size_t claim_len = 0;
    const uint8_t *sig = NULL; size_t sig_len = 0;
    rd_t sub = { pload, plen };
    while (sub.n > 0) {
        char t2[4]; const uint8_t *p2; size_t l2;
        if (rd_box(&sub, t2, &p2, &l2) != QTSQ_OK) return QTSQ_ERR_FORMAT;
        if (memcmp(t2, "cbor", 4) == 0) { claim = p2; claim_len = l2; }
        else if (memcmp(t2, "qtsi", 4) == 0) {
            if (l2 < 4) return QTSQ_ERR_FORMAT;
            uint32_t sl = ((uint32_t)p2[0]<<24)|((uint32_t)p2[1]<<16)|((uint32_t)p2[2]<<8)|p2[3];
            if (4 + sl <= l2) { sig = p2 + 4; sig_len = sl; }
        }
    }
    if (!claim || !sig) return QTSQ_ERR_FORMAT;

    uint8_t digest[32];
    sha3_256(digest, claim, claim_len);
    if (PQCLEAN_MLDSA65_CLEAN_crypto_sign_verify((uint8_t *)sig, sig_len,
                                                 digest, 32, sign_pk) != 0)
        return QTSQ_ERR_CHECKSUM;
    return QTSQ_OK;
}

int qtsq_c2pa_cross_verify(const uint8_t *manifest, size_t size,
                           const uint8_t work_hash[32],
                           const char *author_id,
                           uint64_t created_at) {
    qtsq_c2pa_metadata_t meta;
    if (qtsq_c2pa_parse_manifest(manifest, size, &meta, NULL) != QTSQ_OK)
        return QTSQ_ERR_FORMAT;
    if (memcmp(meta.work_hash, work_hash, 32) != 0) return QTSQ_ERR_CHECKSUM;
    if (author_id && strcmp(meta.author_id, author_id) != 0) return QTSQ_ERR_CHECKSUM;
    if (created_at != 0 && meta.created_at != created_at) return QTSQ_ERR_CHECKSUM;
    return QTSQ_OK;
}

/* ═══════════════════ File + container integration ═══════════════════ */
int qtsq_c2pa_write_manifest(const uint8_t *manifest, size_t size,
                             const char *filename) {
    if (!manifest || !filename) return QTSQ_ERR_NULL;
    FILE *f = fopen(filename, "wb");
    if (!f) return QTSQ_ERR_IO;
    size_t w = fwrite(manifest, 1, size, f);
    fclose(f);
    return (w == size) ? QTSQ_OK : QTSQ_ERR_IO;
}

int qtsq_c2pa_load_manifest(const char *filename,
                            uint8_t **out_manifest, size_t *out_size) {
    if (!filename || !out_manifest || !out_size) return QTSQ_ERR_NULL;
    *out_manifest = NULL; *out_size = 0;
    FILE *f = fopen(filename, "rb");
    if (!f) return QTSQ_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return QTSQ_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return QTSQ_ERR_IO; }
    rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return QTSQ_ERR_ALLOC; }
    size_t r = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(b); return QTSQ_ERR_IO; }
    *out_manifest = b; *out_size = (size_t)sz;
    return QTSQ_OK;
}

int qtsq_c2pa_add_manifest_stream(qtsq_context_t *container,
                                  const uint8_t *manifest, size_t size,
                                  const char *stream_name) {
    if (!container || !manifest || !stream_name) return QTSQ_ERR_NULL;
    /* Wrap the manifest as a lossless raw stream. */
    qtsq_context_t sub;
    if (qtsq_init(&sub) != QTSQ_OK) return QTSQ_ERR_ALLOC;
    int rc = qtsq_compress_raw(&sub, manifest, size, 0);
    if (rc != QTSQ_OK) { qtsq_free(&sub); return rc; }
    rc = qtsq_container_add_stream(container, &sub, stream_name);
    qtsq_free(&sub);
    return rc;
}

int qtsq_c2pa_get_manifest_stream(qtsq_context_t *container,
                                  const char *stream_name,
                                  uint8_t **out_manifest, size_t *out_size) {
    if (!container || !stream_name || !out_manifest || !out_size)
        return QTSQ_ERR_NULL;
    *out_manifest = NULL; *out_size = 0;
    uint32_t count = 0;
    if (qtsq_container_get_count(container, &count) != QTSQ_OK) return QTSQ_ERR_FORMAT;
    for (uint32_t i = 0; i < count; i++) {
        qtsq_context_t sub;
        char name[256];
        if (qtsq_container_get_stream(container, i, &sub, name, sizeof(name)) != QTSQ_OK)
            continue;
        int match = (strcmp(name, stream_name) == 0);
        if (!match) { qtsq_free(&sub); continue; }
        uint8_t *data = NULL; size_t dsz = 0;
        int rc = qtsq_decompress(&sub, &data, &dsz);
        if (rc != QTSQ_OK) { qtsq_free(&sub); return rc; }
        qtsq_free(&sub);
        *out_manifest = data; *out_size = dsz;
        return QTSQ_OK;
    }
    return QTSQ_ERR_FORMAT;
}
