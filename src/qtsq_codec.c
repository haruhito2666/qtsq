/*
 * Quantum Tensor Sequence — Codec Shim
 * Thin bridge to libqtc (compressor/). See include/qtsq_codec.h.
 */

#include "qtsq_codec.h"
#include "qtsq.h"
#include "qtsq_format.h"
#include "qtc.h"
#include <stdlib.h>
#include <string.h>

int qtsq_codec_is_qtc(const uint8_t *buf, size_t len)
{
    if (!buf || len < 4) return 0;
    return buf[0] == QTSQ_QTC_MAGIC_0 && buf[1] == QTSQ_QTC_MAGIC_1 &&
           buf[2] == QTSQ_QTC_MAGIC_2 && buf[3] == QTSQ_QTC_MAGIC_3;
}

uint64_t qtsq_codec_declared_size(const uint8_t *buf, size_t len)
{
    if (!qtsq_codec_is_qtc(buf, len) || len < QTSQ_QTC_HEADER_SIZE)
        return 0;
    uint64_t orig = 0;
    memcpy(&orig, buf + 8, 8);   /* little-endian u64 at offset 8 */
    return orig;
}

/* Map qtc error codes onto QTSQ error codes. */
static int qtc_to_qtsq_error(int qtc_err)
{
    switch (qtc_err) {
        case QTC_OK:          return QTSQ_OK;
        case QTC_ERR_ALLOC:   return QTSQ_ERR_ALLOC;
        case QTC_ERR_SIZE:    return QTSQ_ERR_TOO_LARGE;
        default:              return QTSQ_ERR_FORMAT;
    }
}

int qtsq_codec_compress(const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return QTSQ_ERR_NULL;
    *out = NULL;
    *out_len = 0;
    if (!in && in_len > 0) return QTSQ_ERR_NULL;

    int rc = qtc_compress(in, in_len, out, out_len, NULL);
    if (rc != QTC_OK) {
        *out = NULL;
        *out_len = 0;
        return qtc_to_qtsq_error(rc);
    }
    return QTSQ_OK;
}

int qtsq_codec_decompress(const uint8_t *in, size_t in_len,
                          uint64_t max_out,
                          uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return QTSQ_ERR_NULL;
    *out = NULL;
    *out_len = 0;
    if (!in || in_len == 0) return QTSQ_ERR_FORMAT;

    int rc = qtc_decompress_bounded(in, in_len, out, out_len, max_out);
    if (rc != QTC_OK) {
        *out = NULL;
        *out_len = 0;
        return qtc_to_qtsq_error(rc);
    }
    return QTSQ_OK;
}

/* ── Payload sniffing → explicit codec id (Phase 1) ──
 *
 * The container TOC now records which codec produced each stream so a
 * reader can report kind/codec/ratio from the directory alone. The id
 * is DERIVED by sniffing — deliberately the same sniffing the decode
 * path has always done — so the recorded id can never disagree with
 * what an old reader infers. Two independent files written from the
 * same input must agree byte-for-byte, so this function must stay a
 * pure function of the payload bytes.
 *
 * Anything unrecognized returns QTSQ_CODECID_LEGACY, which is exactly
 * the value pre-2.2 files carry and the value readers already treat as
 * "sniff it". Never returns an error: an unknown payload is normal.
 */
uint32_t qtsq_codec_identify(const uint8_t *buf, size_t len, uint8_t data_type)
{
    if (!buf || len == 0) return QTSQ_CODECID_STORE;

    /* BLB1 typed stream: its own header says which inner codec. */
    if (len >= sizeof(qtsq_blob_header_t) &&
        buf[0] == QTSQ_BLOB_MAGIC_0 && buf[1] == QTSQ_BLOB_MAGIC_1 &&
        buf[2] == QTSQ_BLOB_MAGIC_2 && buf[3] == QTSQ_BLOB_MAGIC_3) {
        return QTSQ_CODECID_BLOB(buf[4]);
    }

    /* TNS2 tensor envelope: mode byte at offset 4. */
    if (len >= 5 && buf[0] == 'T' && buf[1] == 'N' && buf[2] == 'S' &&
        buf[3] == '2') {
        return QTSQ_CODECID_TNS2(buf[4]);
    }

    /* PIX1 image payload. */
    if (len >= 4 && buf[0] == 'P' && buf[1] == 'I' && buf[2] == 'X' &&
        buf[3] == '1') {
        return QTSQ_CODECID_PIXEL(len >= 18 ? buf[17] : 0);
    }

    /* OPS1 audio payload. */
    if (len >= 4 && buf[0] == 'O' && buf[1] == 'P' && buf[2] == 'S' &&
        buf[3] == '1') {
        return QTSQ_CODECID_AUDIOC(0);
    }

    /* qtc stream: the lens id is at byte 4 of the 16-byte header. */
    if (qtsq_codec_is_qtc(buf, len))
        return QTSQ_CODECID_QTC(len >= 5 ? buf[4] : 0);

    /* Horizon raw-text winner: "SQTR" + a qtc stream (the embedded lens
     * id sits at byte 8, not byte 4). This framing has no family in the
     * registry, and claiming BLB1 was actively wrong: a reader that
     * trusted the id would try to read a 64-byte blob header out of
     * "SQTR" and fail. Family 0 means "sniff", which is both truthful
     * and what every decode path already does. */
    /* Only the 4-byte magic is needed here: the container hands this a
     * short preview of the payload, not the whole thing, so a tighter
     * length test would silently fall through to the data_type guess. */
    if (len >= 4 && buf[0] == 'S' && buf[1] == 'Q' && buf[2] == 'T' &&
        buf[3] == 'R') {
        return QTSQ_CODEC_MAKE(0x00, 0);
    }

    /* zlib streams always lead with 0x78. Checked last: it is the
     * weakest signal, one byte, so anything else must win first. */
    if (buf[0] == 0x78) return QTSQ_CODECID_ZLIB;

    /* No recognised framing.
     *
     * It is tempting to fall back on data_type and record "this is a
     * BLB1 <kind> stream". That inference is unsound: the BLB1 check
     * above is what establishes that a payload *is* a typed blob, and
     * a stream that reaches here has already failed it. The legacy
     * Horizon dictionary stream ([u64 raw_size][compressed], which is
     * how a sharded model manifest is encoded) has no BLB1 header at
     * all, yet data_type says JSON — so the guess labelled it BLB1
     * text, and a reader trusting codec_id would go looking for a
     * 64-byte blob header that was never written.
     *
     * So: only positive framing evidence produces a specific id.
     * Everything else is family 0, which means "sniff" — the contract
     * every decode path already honours. */
    return QTSQ_CODEC_MAKE(0x00, 0);
}
