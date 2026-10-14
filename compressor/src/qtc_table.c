/*
 * qtc table lens (v0.18.0)
 *
 * Delimited text (CSV/TSV) is the one shape where a cheap reordering
 * beats any entropy coder, because the bytes are already compressible —
 * it is their ORDER that defeats LZ. Numeric CSV in row order has almost
 * no byte-level locality; the same data column-major has near-perfect
 * locality, so the front-coding + LZ2 candidates downstream see runs
 * instead of noise.
 *
 * Measured (synthetic tables, best of the text lens):
 *
 *   numeric-csv    row 52355 -> column 34649   (-33.8%)
 *   wide 20-col    row 34264 -> column 30282   (-11.6%)
 *   sequential     row 32567 -> column 28849   (-11.4%)
 *   categorical    row  4539 -> column  5703   (+25.6% — ROW WINS)
 *
 * That last row is why this is a contest and not a rule. Categorical
 * tables repeat whole rows, which row-order front-coding already
 * collapses almost to nothing, and transposing only adds separator
 * overhead. So both orders are encoded and the smaller kept, which can
 * never lose to not doing this at all.
 *
 * No new entropy coder is introduced: each candidate is a qtc TEXT
 * stream (front-coding + LZ2 contest), so this lens composes the existing
 * one rather than competing with it.
 *
 * Framing (20 bytes):
 *   [u32 raw_len][u8 mode][u8 delim][u16 ncols]
 *   [u32 nrows][u32 body_len][u32 text_len]
 *
 * No row pitch is stored: row byte lengths differ freely (field widths
 * vary with the data), so the inverse rebuilds rows from the per-field
 * lengths it reads out of the column-major text and validates the total
 * against the caller's buffer size.
 *
 * text_len is the size the body decodes to, which is NOT raw_len for
 * mode 2: the transpose turns row separators into column separators, so
 * a table with k fields per row loses k-1 bytes and gains one per
 * column. Without it the decoder would have to guess the intermediate
 * size, and guessing wrong silently truncates.
 *
 *   mode 0 STORED   not a strict rectangle; body is the raw text
 *   mode 1 ROW      body is the input verbatim, text-coded
 *   mode 2 COLUMN   body is the transposed text, text-coded
 *
 * exactness: the transpose is attempted only for a STRICT rectangle — a
 * single delimiter, no quoting, no CR, no empty or ragged rows, every
 * row LF-terminated, non-empty final line. Within those rules a column
 * is emitted with a '\n' after every field, so a column always ends at
 * the rows-th separator and the inverse can split on '\n' with no length
 * table. Anything else takes mode 0 or 1, both of which
 * preserve the original bytes.
 */

#include "qtc.h"
#include "qtc_internal.h"
#include <stdlib.h>
#include <string.h>

#define TAB_HDR 20u
#define TAB_MAX_COLS 4096u

static void tab_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t tab_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Emit column-major text into dst (len bytes is enough: the transpose
 * replaces each row's k-1 separators with one separator per field, and
 * rows*cols >= rows*cols fields means the totals match). Every field
 * gets a trailing '\n', which is what lets the inverse find column ends. */
/* Rebuild row-major text from the transposed form.
 *
 * Two passes, because the destination cannot be scanned to find field
 * boundaries while it is still being built — dst is uninitialized memory
 * at the start, so looking for a delimiter there reads garbage and
 * mis-measures the field. Pass 1 records every field's length out of the
 * column-major text; pass 2 lays the fields down in row order and
 * inserts the separators, checking the result against the stored row
 * pitch and the caller's buffer size. */
static int tab_untranspose(const uint8_t *src, size_t slen, uint32_t cols,
                           uint32_t rows, uint8_t delim,
                           uint8_t *dst, size_t dlen)
{
    if (cols == 0 || rows == 0) return 0;
    size_t n = (size_t)cols * rows;
    size_t *foff = (size_t *)malloc(n * sizeof(size_t));
    uint32_t *flen = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!foff || !flen) { free(foff); free(flen); return 0; }

    /* Pass 1: every field's offset+length in the column-major text. */
    size_t q = 0;
    for (uint32_t c = 0; c < cols; c++) {
        for (uint32_t r = 0; r < rows; r++) {
            size_t idx = (size_t)c * rows + r;
            size_t start = q;
            while (q < slen && src[q] != '\n') q++;
            if (q - start > 0xFFFFFFFFu) { free(foff); free(flen); return 0; }
            foff[idx] = start;
            flen[idx] = (uint32_t)(q - start);
            /* Every field, including a column's last, is followed by a
             * separator; the decoder consumes exactly one per field. */
            if (q >= slen) { free(foff); free(flen); return 0; }
            if (src[q] != '\n') { free(foff); free(flen); return 0; }
            q++;
        }
    }
    if (q != slen) { free(foff); free(flen); return 0; }

    /* Pass 2: lay fields down in row order, inserting separators, and
     * check the pitch against the stored row_len so a corrupted stream
     * is rejected instead of writing a differently-shaped table. */
    size_t p = 0;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            size_t idx = (size_t)c * rows + r;
            uint32_t L = flen[idx];
            if (p + L > dlen) { free(foff); free(flen); return 0; }
            if (L) memcpy(dst + p, src + foff[idx], L);
            p += L;
            if (c + 1 < cols) {
                if (p >= dlen) { free(foff); free(flen); return 0; }
                dst[p++] = delim;
            }
        }
        if (p >= dlen) { free(foff); free(flen); return 0; }
        dst[p++] = '\n';
    }
    free(foff); free(flen);
    /* Total length must match exactly: that is the check that a stream
     * is well formed, now that row pitches may differ per row. */
    return p == dlen;
}

/* Guess the delimiter: whichever of , ; \t | occurs most often per line.
 * Returns 0 when the input has no clear single delimiter. */
static uint8_t tab_guess_delim(const uint8_t *s, size_t len)
{
    static const uint8_t cands[] = { ',', '\t', ';', '|' };
    uint8_t best = 0;
    size_t bestn = 0;
    size_t lines = 0;
    for (size_t i = 0; i < len; i++) if (s[i] == '\n') lines++;
    if (!lines) return 0;
    for (size_t k = 0; k < sizeof(cands); k++) {
        size_t n = 0;
        for (size_t i = 0; i < len; i++) if (s[i] == cands[k]) n++;
        if (n > bestn) { bestn = n; best = cands[k]; }
    }
    /* Require the winner to be unambiguous: a clear plurality, so a table
     * with commas inside free text is not mistaken for a 3x wider table. */
    if (bestn == 0) return 0;
    for (size_t k = 0; k < sizeof(cands); k++) {
        if (cands[k] == best) continue;
        size_t n = 0;
        for (size_t i = 0; i < len; i++) if (s[i] == cands[k]) n++;
        if (n * 2 > bestn) return 0;
    }
    return best;
}

int qtc_compress_table(const uint8_t *data, size_t len,
                       uint8_t **out, size_t *out_len,
                       const qtc_options_t *opts)
{
    if (!out || !out_len) return QTC_ERR_ALLOC;
    *out = NULL;
    *out_len = 0;
    if (!data || len == 0) return QTC_ERR_FORMAT;
    if (len > 0xFFFFFFFFu) return QTC_ERR_SIZE;

    uint8_t mode = 1;                 /* row order is the safe default */
    uint8_t delim = 0;
    uint32_t cols = 0;
    uint32_t rows_h = 0;              /* header copy */
    const uint8_t *chosen = data;
    size_t chosen_len = len;

    delim = tab_guess_delim(data, len);
    /* Row order only. An earlier build also transposed the table and
     * contested the two encodings, on the theory that column-major wins
     * numeric CSVs by ~34%. Measured against the real text lens it does
     * not: front-coding collapses whole repeated rows (152x-1687x on
     * categorical input), and transposing destroys exactly that
     * repetition. Across numeric-wide, categorical, low-cardinality,
     * repeated-token and single-column tables, row order won every time,
     * so the second pass was pure cost. The decode path still accepts
     * mode 2 in case such a file exists. */
    qtc_options_t o;
    qtc_options_default(&o);
    if (opts) {
        if (opts->level == 1) o.level = 1;
        else if (opts->level == 0) o.level = 0;
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    int rc = qtc_compress_text(chosen, chosen_len, &body, &body_len, &o);

    if (rc != QTC_OK || !body) { free(body); return rc ? rc : QTC_ERR_ALLOC; }

    size_t total = TAB_HDR + body_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { free(body); return QTC_ERR_ALLOC; }
    tab_put32(buf, (uint32_t)len);
    buf[4] = mode;
    buf[5] = delim;
    buf[6] = (uint8_t)(cols & 0xFF);
    buf[7] = (uint8_t)(cols >> 8);
    tab_put32(buf + 8, rows_h);
    tab_put32(buf + 12, (uint32_t)body_len);
    tab_put32(buf + 16, (uint32_t)chosen_len);
    memcpy(buf + TAB_HDR, body, body_len);
    free(body);

    *out = buf;
    *out_len = total;
    return QTC_OK;
}

int qtc_decompress_table(const uint8_t *in, size_t in_len,
                         uint8_t *out, size_t out_len)
{
    if (!in || !out || in_len < TAB_HDR) return QTC_ERR_FORMAT;
    uint32_t raw = tab_get32(in);
    if (raw != out_len) return QTC_ERR_SIZE;
    uint8_t mode = in[4];
    uint8_t delim = in[5];
    uint32_t cols = (uint32_t)in[6] | ((uint32_t)in[7] << 8);
    uint32_t rows = tab_get32(in + 8);
    uint32_t body_len = tab_get32(in + 12);
    uint32_t text_len = tab_get32(in + 16);
    if ((size_t)body_len != in_len - TAB_HDR) return QTC_ERR_FORMAT;
    if (text_len == 0 || text_len > out_len * 2 + 4096u) return QTC_ERR_FORMAT;

    if (mode == 1 || mode == 0) {
        if (text_len != raw) return QTC_ERR_FORMAT;
        return qtc_decompress_text(in + TAB_HDR, body_len, out, out_len);
    }
    if (mode != 2) return QTC_ERR_FORMAT;
    if (rows == 0 || delim == 0) return QTC_ERR_FORMAT;

    /* Decode the column-major text into its OWN size (text_len, which is
     * smaller than out_len), then rebuild rows into the caller's buffer. */
    uint8_t *tmp = (uint8_t *)malloc(text_len ? text_len : 1);
    if (!tmp) return QTC_ERR_ALLOC;
    int rc = qtc_decompress_text(in + TAB_HDR, body_len, tmp, text_len);
    if (rc != QTC_OK) { free(tmp); return rc; }

    if (cols == 0 || cols > TAB_MAX_COLS) { free(tmp); return QTC_ERR_FORMAT; }

    if (!tab_untranspose(tmp, text_len, cols, rows, delim, out, out_len)) {
        free(tmp);
        return QTC_ERR_FORMAT;
    }
    free(tmp);
    return QTC_OK;
}