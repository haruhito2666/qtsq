/*
 * qtc_mdct.c — Phase 5: the Modified Discrete Cosine Transform.
 *
 * This is the "Gravitational Lensing" layer from CONCEPT.md: a frequency
 * transform that concentrates a signal's energy into a few coefficients so
 * the rest can be quantized away cheaply.
 *
 * Convention (standard TDAC / MDCT):
 *   forward : 2N time samples  -> N frequency coefficients
 *   inverse : N coefficients   -> 2N time samples
 *
 *   forward:  X[k] = sum_{n=0}^{2N-1} w[n] x[n] cos(pi/N (n+1/2+N/2)(k+1/2))
 *   inverse:  t[n] = (2/N) sum_{k=0}^{N-1} X[k] cos(pi/N (n+1/2+N/2)(k+1/2))
 *   output :  y[n] = w[n] * t[n], overlap-added with the previous frame.
 *
 * With the sine window w[i] = sin(pi/(2N) * (i + 0.5)) — which satisfies the
 * Princen-Bradley condition w[i]^2 + w[i+N]^2 = 1 — and 50% overlap-add of
 * consecutive inverse frames, the transform is perfectly invertible.
 *
 * v0.9.0 (optimization #13): FFT-based O(N log N) implementation replacing
 * the direct O(N^2) kernel. Both directions reduce to ONE 2N-point complex
 * FFT of a pre-rotated sequence:
 *
 *   forward: X[k] = Re[ e^{i(pi/N)(k+1/2)(N/2+1/2)} * DFT_{2N}(f)[k] ]
 *            where f[n] = w[n] x[n] e^{i(pi/(2N))n}
 *   inverse: t[n] = (2/N) Re[ e^{i(pi/(2N))(n+N/2+1/2)} *
 *                             DFT_{2N}(s)[(n + N/2) mod 2N] ]
 *            where s[k] = X[k] e^{i(pi/(2N))k} for k < N, zero-padded.
 *
 * (The half-integer frequency (k+1/2) of the MDCT kernel becomes an
 * integer DFT bin after the e^{i(pi/(2N))n} pre-rotation.) The rotation
 * factors and the window are precomputed into the CALLER-OWNED context
 * (thread-safe: no shared state — audit M2).
 */
#include "qtc_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Iterative radix-2 complex FFT in place. sign = +1 gives the
 * e^{+i2π/n} kernel (the one both MDCT reductions need), -1 the usual
 * forward convention. n must be a power of two. */
static void fft_inplace(double *re, double *im, int n, int sign)
{
    /* Bit-reversal permutation. */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = (double)sign * 2.0 * M_PI / (double)len;
        double wre = cos(ang), wim = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cre = 1.0, cim = 0.0;
            int half = len >> 1;
            for (int k = 0; k < half; k++) {
                int a = i + k, b = i + k + half;
                double ure = re[a], uim = im[a];
                double vre = re[b] * cre - im[b] * cim;
                double vim = re[b] * cim + im[b] * cre;
                re[a] = ure + vre; im[a] = uim + vim;
                re[b] = ure - vre; im[b] = uim - vim;
                double ncre = cre * wre - cim * wim;
                cim = cre * wim + cim * wre;
                cre = ncre;
            }
        }
    }
}

int qtc_mdct_kernel_init(qtc_mdct_kernel_t *k, int N)
{
    if (!k || N < 16 || N > 4096 || (N & (N - 1)) != 0)
        return QTC_ERR_FORMAT;
    memset(k, 0, sizeof(*k));
    k->N = N;
    k->win = (double *)malloc((size_t)2 * N * sizeof(double));
    k->rot_re = (double *)malloc((size_t)2 * N * sizeof(double));
    k->rot_im = (double *)malloc((size_t)2 * N * sizeof(double));
    if (!k->win || !k->rot_re || !k->rot_im) {
        qtc_mdct_kernel_free(k);
        return QTC_ERR_ALLOC;
    }

    for (int n = 0; n < 2 * N; n++) {
        k->win[n] = sin(M_PI / (2.0 * N) * (n + 0.5));
        double a = M_PI / (2.0 * N) * n;      /* e^{i(pi/(2N))n} */
        k->rot_re[n] = cos(a);
        k->rot_im[n] = sin(a);
    }
    return QTC_OK;
}

void qtc_mdct_kernel_free(qtc_mdct_kernel_t *k)
{
    if (!k)
        return;
    free(k->win);
    free(k->rot_re);
    free(k->rot_im);
    k->win = NULL;
    k->rot_re = NULL;
    k->rot_im = NULL;
    k->N = 0;
}

void qtc_mdct_forward(const qtc_mdct_kernel_t *k, const double *in,
                      double *out)
{
    int N = k->N;
    int M = 2 * N;
    double *re = (double *)malloc((size_t)M * sizeof(double));
    double *im = (double *)malloc((size_t)M * sizeof(double));
    if (!re || !im) {
        free(re); free(im);
        /* The callers treat a zeroed output as silence; on allocation
         * failure there is nothing meaningful to return. */
        for (int kk = 0; kk < N; kk++)
            out[kk] = 0.0;
        return;
    }

    /* f[n] = w[n] x[n] e^{i(pi/(2N))n} */
    for (int n = 0; n < M; n++) {
        double f = k->win[n] * in[n];
        re[n] = f * k->rot_re[n];
        im[n] = f * k->rot_im[n];
    }
    fft_inplace(re, im, M, +1);

    /* X[k] = Re[ e^{i(pi/N)(k+1/2)(N/2+1/2)} * F[k] ] */
    for (int kk = 0; kk < N; kk++) {
        double ph = (M_PI / N) * (kk + 0.5) * (N / 2.0 + 0.5);
        out[kk] = cos(ph) * re[kk] - sin(ph) * im[kk];
    }
    free(re);
    free(im);
}

void qtc_mdct_inverse(const qtc_mdct_kernel_t *k, const double *in,
                      double *out)
{
    int N = k->N;
    int M = 2 * N;
    double *re = (double *)calloc((size_t)M, sizeof(double));
    double *im = (double *)calloc((size_t)M, sizeof(double));
    if (!re || !im) {
        free(re); free(im);
        for (int n = 0; n < M; n++)
            out[n] = 0.0;
        return;
    }

    /* s[k] = X[k] e^{i(pi/(2N))k}, zero-padded to 2N. */
    for (int kk = 0; kk < N; kk++) {
        re[kk] = in[kk] * k->rot_re[kk];
        im[kk] = in[kk] * k->rot_im[kk];
    }
    fft_inplace(re, im, M, +1);

    /* t[n] = (2/N) Re[ e^{i(pi/(2N))(n+N/2+1/2)} * S[(n+N/2) mod 2N] ],
     * then window. */
    double scale = 2.0 / N;
    for (int n = 0; n < M; n++) {
        int bin = n + N / 2;
        if (bin >= M)
            bin -= M;
        double ph = (M_PI / (2.0 * N)) * (n + N / 2.0 + 0.5);
        double t = cos(ph) * re[bin] - sin(ph) * im[bin];
        out[n] = scale * t * k->win[n];
    }
    free(re);
    free(im);
}
