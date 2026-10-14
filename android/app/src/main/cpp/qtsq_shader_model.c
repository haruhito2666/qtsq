/*
 * QTSQ Phase D — host shader-model reference (implementation)
 *
 * C mirror of the GLSL compute shaders. See qtsq_shader_model.h for the
 * contract. Every formula and every ordering decision below matches
 * shaders/surfel_forward.comp, surfel_loss.comp and surfel_backward.comp
 * VERBATIM (including the insertion sort, the clamp tests, and the
 * low-pass floor), so a GPU-vs-host comparison is a genuine gate, not a
 * re-derivation.
 *
 * Author: Haruhito
 */

#include "qtsq_shader_model.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ── local vec3 (matches cap_vec3) ── */
typedef cap_vec3 s3;
static inline s3 s3v(float x, float y, float z){ s3 r={x,y,z}; return r; }
static inline s3 s3add(s3 a, s3 b){ return s3v(a.x+b.x,a.y+b.y,a.z+b.z); }
static inline s3 s3sub(s3 a, s3 b){ return s3v(a.x-b.x,a.y-b.y,a.z-b.z); }
static inline s3 s3scale(s3 a, float s){ return s3v(a.x*s,a.y*s,a.z*s); }
static inline float s3dot(s3 a, s3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline s3 s3cross(s3 a, s3 b){
    return s3v(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
static inline float s3len(s3 a){ return sqrtf(s3dot(a,a)); }
static inline s3 s3norm(s3 a){ float l=s3len(a); return (l>1e-20f)? s3scale(a,1.0f/l) : a; }

/* Per-pixel intersection record — same fields as the shader Hit. */
typedef struct {
    int   idx;
    float depth;
    float alpha;
    float G;
    float u, v;
    int   lowpass;
} sf_hit;

/* quat [w,x,y,z] → frame columns (tu, tv, n) — same expansion as the shader. */
static void sf_frame(const float *q, s3 *tu, s3 *tv, s3 *n){
    float qn = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (qn < 1e-20f) qn = 1.0f;
    float w=q[0]/qn, x=q[1]/qn, y=q[2]/qn, z=q[3]/qn;
    float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
    *tu = s3v(1-2*(yy+zz), 2*(xy+wz),   2*(xz-wy));
    *tv = s3v(2*(xy-wz),   1-2*(xx+zz), 2*(yz+wx));
    *n  = s3v(2*(xz+wy),   2*(yz-wx),   1-2*(xx+yy));
}

static s3 sf_ray_dir(const cap_camera_t *c, int px, int py){
    float ndx = ((float)px + 0.5f - c->cx) / c->fx;
    float ndy = ((float)py + 0.5f - c->cy) / c->fy;
    return s3norm(s3add(c->forward, s3add(s3scale(c->right, ndx), s3scale(c->up, ndy))));
}

/* Gather — acceptance criteria identical to both GPU shaders. */
static int sf_gather(const float *surfels, int n, const cap_camera_t *cam,
                     int px, int py, sf_hit *hits){
    s3 o = cam->pos;
    s3 d = sf_ray_dir(cam, px, py);
    int m = 0;

    for (int i = 0; i < n && m < SFM_MAX_HITS; i++){
        const float *s = surfels + (size_t)i * SFM_SURFEL_STRIDE;
        s3 center = s3v(s[0], s[1], s[2]);
        s3 tu, tv, nrm;
        sf_frame(s + 3, &tu, &tv, &nrm);

        float denom = s3dot(d, nrm);
        if (fabsf(denom) < 1e-6f) continue;
        float t = s3dot(s3sub(center, o), nrm) / denom;
        if (t <= 1e-4f) continue;

        s3 hitp = s3add(o, s3scale(d, t));
        s3 dp = s3sub(hitp, center);
        float su = expf(s[7]);
        float sv = expf(s[8]);
        float u_val = s3dot(dp, tu) / su;
        float v_val = s3dot(dp, tv) / sv;
        float G = expf(-0.5f * (u_val*u_val + v_val*v_val));
        float depth = s3dot(s3sub(hitp, o), cam->forward);
        if (depth <= 0.0f) continue;

        int lowpass = 0;
        s3 rel = s3sub(center, o);
        float zc = s3dot(rel, cam->forward);
        if (zc > 1e-6f){
            float sx = cam->cx + cam->fx * s3dot(rel, cam->right) / zc;
            float sy = cam->cy + cam->fy * s3dot(rel, cam->up) / zc;
            float ddx = ((float)px + 0.5f) - sx;
            float ddy = ((float)py + 0.5f) - sy;
            float pd2 = ddx*ddx + ddy*ddy;
            float Glp = expf(-0.5f * pd2 / (SFM_LOWPASS_SIGMA*SFM_LOWPASS_SIGMA));
            if (Glp > G){ G = Glp; lowpass = 1; }
        }

        if (G < SFM_G_EPS) continue;

        float sig = 1.0f / (1.0f + expf(-s[9]));
        float alpha = sig * G;
        if (alpha > SFM_ALPHA_CLAMP) alpha = SFM_ALPHA_CLAMP;

        hits[m].idx = i;
        hits[m].depth = depth;
        hits[m].alpha = alpha;
        hits[m].G = G;
        hits[m].u = u_val;
        hits[m].v = v_val;
        hits[m].lowpass = lowpass;
        m++;
    }

    /* Insertion sort by ascending depth — same as the shader sortHits(). */
    for (int i = 1; i < m; i++){
        sf_hit key = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].depth > key.depth){ hits[j+1] = hits[j]; j--; }
        hits[j+1] = key;
    }
    return m;
}

/* ── forward (shader surfel_forward.comp) ── */

static void sfm_composite_pixel(const float *surfels, const sf_hit *hits, int m,
                                const cap_camera_t *cam,
                                float out_rgb[3], float *out_depth,
                                float out_nrm[3], float *out_alpha){
    float T = 1.0f;
    float acc[3] = {0,0,0};
    float nrm_acc[3] = {0,0,0};
    float O = 0.0f;
    float surf_depth = 0.0f;
    int   depth_set = 0;

    for (int k = 0; k < m; k++){
        const sf_hit *h = &hits[k];
        float w = h->alpha * T;
        const float *s = surfels + (size_t)h->idx * SFM_SURFEL_STRIDE;

        acc[0] += w * s[10];
        acc[1] += w * s[11];
        acc[2] += w * s[12];

        /* camera-facing normal: flip the frame column when it points away */
        float q[4] = {s[3], s[4], s[5], s[6]};
        s3 tu, tv, nrm;
        sf_frame(q, &tu, &tv, &nrm);
        float sfx = 1.0f;
        if (s3dot(nrm, cam->forward) > 0.0f) sfx = -1.0f;
        nrm_acc[0] += w * (nrm.x * sfx);
        nrm_acc[1] += w * (nrm.y * sfx);
        nrm_acc[2] += w * (nrm.z * sfx);

        /* Unbiased depth (Yang et al. 2025 Eq. 9) */
        O += (h->alpha + SFM_O_EPS) * h->G;
        if (!depth_set && O >= SFM_O_THRESH){
            surf_depth = h->depth;
            depth_set = 1;
        }

        T *= (1.0f - h->alpha);
        if (T < SFM_T_EPS) break;
    }
    if (!depth_set && m > 0) surf_depth = hits[m-1].depth;

    out_rgb[0]=acc[0]; out_rgb[1]=acc[1]; out_rgb[2]=acc[2];
    if (out_alpha) *out_alpha = 1.0f - T;
    if (out_depth) *out_depth = surf_depth;
    if (out_nrm){
        float nl = sqrtf(nrm_acc[0]*nrm_acc[0]+nrm_acc[1]*nrm_acc[1]+nrm_acc[2]*nrm_acc[2]);
        if (nl > 1e-12f){
            out_nrm[0]=nrm_acc[0]/nl; out_nrm[1]=nrm_acc[1]/nl; out_nrm[2]=nrm_acc[2]/nl;
        } else out_nrm[0]=out_nrm[1]=out_nrm[2]=0.0f;
    }
}

int sfm_render(const float *surfels, int n, const cap_camera_t *cam,
               float *out_rgb){
    if (!surfels || !cam || !out_rgb || n <= 0) return -1;
    int W = cam->width, H = cam->height;
    sf_hit *hits = (sf_hit*)malloc(sizeof(sf_hit)*SFM_MAX_HITS);
    if (!hits) return -1;

    for (int py = 0; py < H; py++){
        for (int px = 0; px < W; px++){
            size_t pix = (size_t)py*W + px;
            int m = sf_gather(surfels, n, cam, px, py, hits);
            float rgb[3] = {0,0,0};
            if (m > 0) sfm_composite_pixel(surfels, hits, m, cam, rgb, NULL, NULL, NULL);
            out_rgb[pix*3+0]=rgb[0]; out_rgb[pix*3+1]=rgb[1]; out_rgb[pix*3+2]=rgb[2];
        }
    }
    free(hits);
    return 0;
}

int sfm_render_full(const float *surfels, int n, const cap_camera_t *cam,
                    float *out_rgb, float *out_depth, float *out_normal,
                    float *out_alpha){
    if (!surfels || !cam || n <= 0) return -1;
    if (!out_rgb && !out_depth && !out_normal && !out_alpha) return -1;
    int W = cam->width, H = cam->height;
    sf_hit *hits = (sf_hit*)malloc(sizeof(sf_hit)*SFM_MAX_HITS);
    if (!hits) return -1;

    for (int py = 0; py < H; py++){
        for (int px = 0; px < W; px++){
            size_t pix = (size_t)py*W + px;
            int m = sf_gather(surfels, n, cam, px, py, hits);
            float rgb[3] = {0,0,0}, nrm[3] = {0,0,0}, dep = 0.0f, al = 0.0f;
            if (m > 0) sfm_composite_pixel(surfels, hits, m, cam, rgb, &dep, nrm, &al);
            if (out_rgb){ out_rgb[pix*3+0]=rgb[0]; out_rgb[pix*3+1]=rgb[1]; out_rgb[pix*3+2]=rgb[2]; }
            if (out_depth) out_depth[pix] = dep;
            if (out_alpha)  out_alpha[pix] = al;
            if (out_normal){ out_normal[pix*3+0]=nrm[0]; out_normal[pix*3+1]=nrm[1]; out_normal[pix*3+2]=nrm[2]; }
        }
    }
    free(hits);
    return 0;
}

/* ── loss (shader surfel_loss.comp) ── */

float sfm_loss_l1(const float *pred, const float *target, int W, int H,
                  float *dL_dpred){
    size_t N = (size_t)W * H;
    float invN = 1.0f / (float)(N * 3);
    double sum = 0.0;
    for (size_t i = 0; i < N*3; i++){
        float diff = pred[i] - target[i];
        sum += fabs((double)diff);
        if (dL_dpred) dL_dpred[i] = (diff > 0.0f) ? invN : (diff < 0.0f ? -invN : 0.0f);
    }
    return (float)(sum / (double)(N*3));
}

/* ── Phase E: normal-consistency loss (2DGS Eq. 14, screen form) ──
 * Per covered pixel (α > 0.5):
 *   N(x,y) = normalize(−dzx, −dzy, 1),  dzx = d[x+1]−d[x−1], dzy = d[y+1]−d[y−1]
 *   Lnc_p = 1 − |n̂·N|                       (n̂ = accumulated normal map)
 *   Mean over ALL pixels (uncovered contribute 0), so the gradient is
 *   normalized by the pixel count N — matching the plan's λn on means.
 * Gradients (chain):
 *   ∂/∂n̂:  −sign(n̂·N)·N
 *   ∂/∂N:  −sign(n̂·N)·n̂, and dN/dg = (I − NNᵀ)/|g| with g = (−dzx,−dzy,1)
 *   ∂g/∂d[x±1] = (∓1, 0, 0), ∂g/∂d[y±1] = (0, ∓1, 0)   → scattered to the
 *   four neighbours of pixel p directly (each d[p] receives from the two
 *   pixels to its left/right/up/down via their own stencils), so
 *   depth_grad[p] is a plain write, no atomics.
 * Fills depth_grad (W*H) and normal_grad (W*H*3). Returns Lnc. */

float sfm_loss_nc(const float *depth, const float *normal, const float *alpha,
                  int W, int H, float *depth_grad, float *normal_grad){
    if (!depth || !normal || !alpha || !depth_grad || !normal_grad) return 0.0f;
    int N = W * H;
    double sum = 0.0;
    for (int i = 0; i < N; i++){ depth_grad[i] = 0.0f; }
    for (int i = 0; i < N*3; i++){ normal_grad[i] = 0.0f; }

    for (int y = 1; y < H-1; y++){
        for (int x = 1; x < W-1; x++){
            size_t p = (size_t)y*W + x;
            if (alpha[p] < 0.5f) continue;          /* uncovered: no term */

            float d0 = depth[p];
            float dzx = depth[(size_t)y*W + (x+1)] - depth[(size_t)y*W + (x-1)];
            float dzy = depth[(size_t)(y+1)*W + x] - depth[(size_t)(y-1)*W + x];

            float g[3] = {-dzx, -dzy, 1.0f};
            float gl = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
            if (gl < 1e-12f) continue;
            float Nx[3] = {g[0]/gl, g[1]/gl, g[2]/gl};

            const float *ns = normal + p*3;
            float nd = ns[0]*Nx[0] + ns[1]*Nx[1] + ns[2]*Nx[2];
            float sgn = (nd > 0.0f) ? 1.0f : (nd < 0.0f ? -1.0f : 0.0f);
            if (sgn == 0.0f) continue;

            float val = 1.0f - fabsf(nd);
            sum += val;

            /* ∂L/∂n̂ = −sgn·N */
            normal_grad[p*3+0] += -sgn * Nx[0];
            normal_grad[p*3+1] += -sgn * Nx[1];
            normal_grad[p*3+2] += -sgn * Nx[2];

            /* ∂L/∂N = −sgn·n̂; ∂N/∂g = (I − NNᵀ)/|g| */
            float dl_dg[3];
            float nNg = 0.0f;
            for (int a = 0; a < 3; a++) nNg += Nx[a]*(-sgn*ns[a]);
            for (int a = 0; a < 3; a++){
                dl_dg[a] = (-sgn*ns[a] - nNg*Nx[a]) / gl;
            }
            /* ∂g/∂d[x±1] = (∓1,0,0) → scatter into the four neighbours */
            depth_grad[(size_t)y*W + (x+1)] += -dl_dg[0];   /* d[x+1] contributes +1 to dzx */
            depth_grad[(size_t)y*W + (x-1)] +=  dl_dg[0];   /* d[x−1] contributes −1 to dzx */
            depth_grad[(size_t)(y+1)*W + x] += -dl_dg[1];
            depth_grad[(size_t)(y-1)*W + x] +=  dl_dg[1];
            (void)d0;
        }
    }
    /* mean normalization over pixels */
    float invN = 1.0f / (float)N;
    for (int i = 0; i < N; i++) depth_grad[i] *= invN;
    for (int i = 0; i < N*3; i++) normal_grad[i] *= invN;
    return (float)(sum / (double)N);
}

/* ── backward (shader surfel_backward.comp) ── */

int sfm_backward(const float *surfels, int n, const cap_camera_t *cam,
                 const float *dL_dpred, float *grads){
    if (!surfels || !cam || !dL_dpred || !grads || n <= 0) return -1;
    int W = cam->width, H = cam->height;
    sf_hit *hits = (sf_hit*)malloc(sizeof(sf_hit)*SFM_MAX_HITS);
    if (!hits) return -1;
    float *Tarr = (float*)malloc(sizeof(float)*SFM_MAX_HITS);
    if (!Tarr){ free(hits); return -1; }

    for (int py = 0; py < H; py++){
        for (int px = 0; px < W; px++){
            int m = sf_gather(surfels, n, cam, px, py, hits);
            if (m == 0) continue;
            size_t pix = (size_t)py*W + px;
            s3 dCp = s3v(dL_dpred[pix*3+0], dL_dpred[pix*3+1], dL_dpred[pix*3+2]);
            if (dCp.x == 0.0f && dCp.y == 0.0f && dCp.z == 0.0f) continue;

            int use = m;
            float T = 1.0f;
            for (int k = 0; k < m; k++){
                Tarr[k] = T;
                T *= (1.0f - hits[k].alpha);
                if (T < SFM_T_EPS){ use = k+1; break; }
            }

            s3 o = cam->pos;
            s3 d = sf_ray_dir(cam, px, py);
            s3 A = s3v(0,0,0);
            for (int k = use-1; k >= 0; k--){
                int si = hits[k].idx;
                const float *s = surfels + (size_t)si * SFM_SURFEL_STRIDE;
                float *g = grads + (size_t)si * SFM_GRAD_STRIDE;
                float ak = hits[k].alpha, Tk = Tarr[k];
                s3 ck = s3v(s[10], s[11], s[12]);

                float wk = ak * Tk;
                g[6] += dCp.x * wk;
                g[7] += dCp.y * wk;
                g[8] += dCp.z * wk;

                float dL_da = s3dot(dCp, ck)*Tk - s3dot(dCp, A)/(1.0f - ak + 1e-8f);
                A = s3add(A, s3scale(ck, wk));

                float sig = 1.0f / (1.0f + expf(-s[9]));
                float Graw = hits[k].G;
                if (sig * Graw >= SFM_ALPHA_CLAMP) continue;

                float dL_dsig = dL_da * Graw;
                g[5] += dL_dsig * sig * (1.0f - sig);

                if (hits[k].lowpass) continue;

                float dL_dG = dL_da * sig;
                float dL_du = dL_dG * (-hits[k].u * Graw);
                float dL_dv = dL_dG * (-hits[k].v * Graw);

                s3 tu, tv, nrm;
                sf_frame(s + 3, &tu, &tv, &nrm);
                s3 center = s3v(s[0], s[1], s[2]);
                float su = expf(s[7]);
                float sv = expf(s[8]);

                float denom = s3dot(d, nrm);
                if (fabsf(denom) < 1e-6f) continue;
                s3 cmo = s3sub(center, o);
                float num = s3dot(cmo, nrm);
                float tt = num / denom;
                s3 P = s3add(o, s3scale(d, tt));
                s3 dp = s3sub(P, center);
                float dtu_d = s3dot(d, tu);
                float dtv_d = s3dot(d, tv);

                g[3] += dL_du * (-hits[k].u);
                g[4] += dL_dv * (-hits[k].v);

                s3 du_dc = s3scale(s3add(s3scale(tu, -1.0f), s3scale(nrm, dtu_d/denom)), 1.0f/su);
                s3 dv_dc = s3scale(s3add(s3scale(tv, -1.0f), s3scale(nrm, dtv_d/denom)), 1.0f/sv);
                s3 dc = s3add(s3scale(du_dc, dL_du), s3scale(dv_dc, dL_dv));
                g[0] += dc.x;
                g[1] += dc.y;
                g[2] += dc.z;

                s3 dL_dtu = s3scale(dp, dL_du/su);
                s3 dL_dtv = s3scale(dp, dL_dv/sv);
                s3 dt_dn  = s3scale(s3sub(s3scale(cmo, denom), s3scale(d, num)), 1.0f/(denom*denom));
                float ncoef = dL_du*dtu_d/su + dL_dv*dtv_d/sv;
                s3 dL_dn = s3scale(dt_dn, ncoef);
                s3 tq = s3add(s3add(s3cross(tu, dL_dtu), s3cross(tv, dL_dtv)),
                              s3cross(nrm, dL_dn));
                g[9] += tq.x;
                g[10] += tq.y;
                g[11] += tq.z;
            }
        }
    }

    free(hits);
    free(Tarr);
    return 0;
}

/* ── Phase E: full backward (photometric + NC + DC) ──
 * See the header for the contract. Per pixel:
 *   1. Depth-convergence term (Σ min(G)Δd²) — accumulates per-hit depth and
 *      G gradients; depth chains into center + frame(n); G chains through
 *      the uv-Gaussian (skipped when the low-pass dominated).
 *   2. NC depth chain — the surface hit (first O ≥ O_THRESH in the same
 *      sorted order the renderer uses) receives ∂Lnc/∂depth · ∂depth/∂params.
 *   3. Photometric + NC normal-suffix chain, merged back-to-front pass:
 *      the accumulated normal n̂ = Σ w_k·n_k/|…|, so α-chain terms mirror
 *      the colour suffix A with "colours" = facing normals, and each hit
 *      contributes a frame-column gradient dL/dn = w_k·(I−n̂n̂ᵀ)/|acc|·Ng. */

int sfm_backward_full(const float *surfels, int n, const cap_camera_t *cam,
                      const float *dL_dpred, const float *depth_grad,
                      const float *normal_grad, float lam_n, float lam_c,
                      float *grads, float *out_dc){
    if (!surfels || !cam || !grads || n <= 0) return -1;
    if (!dL_dpred && !depth_grad && !normal_grad && lam_c == 0.0f) return -1;
    int W = cam->width, H = cam->height;
    size_t N = (size_t)W * H;
    float invN = 1.0f / (float)N;
    double dc_sum = 0.0;

    sf_hit *hits = (sf_hit*)malloc(sizeof(sf_hit)*SFM_MAX_HITS);
    if (!hits) return -1;
    float *Tarr = (float*)malloc(sizeof(float)*SFM_MAX_HITS);
    float *warr = (float*)malloc(sizeof(float)*SFM_MAX_HITS);
    float *nxf  = (float*)malloc(sizeof(float)*SFM_MAX_HITS*3);
    float *dgrad_k = (float*)calloc(SFM_MAX_HITS, sizeof(float));
    float *ggrad_k  = (float*)calloc(SFM_MAX_HITS, sizeof(float));
    if (!Tarr || !warr || !nxf || !dgrad_k || !ggrad_k){
        free(hits); free(Tarr); free(warr); free(nxf);
        free(dgrad_k); free(ggrad_k);
        return -1;
    }

    for (int py = 0; py < H; py++){
        for (int px = 0; px < W; px++){
            int m = sf_gather(surfels, n, cam, px, py, hits);
            if (m == 0) continue;
            size_t pix = (size_t)py*W + px;

            s3 dCp = s3v(0,0,0);
            if (dL_dpred)
                dCp = s3v(dL_dpred[pix*3+0], dL_dpred[pix*3+1], dL_dpred[pix*3+2]);
            if (dCp.x == 0.0f && dCp.y == 0.0f && dCp.z == 0.0f &&
                !depth_grad && !normal_grad && lam_c == 0.0f) continue;

            s3 o = cam->pos;
            s3 d = sf_ray_dir(cam, px, py);
            float ddotf = s3dot(d, cam->forward);

            /* ── 1. depth-convergence (per-pair over sorted hits) ── */
            float dc_p = 0.0f;
            if (lam_c != 0.0f && m > 1){
                for (int k = 0; k < m; k++){ dgrad_k[k] = 0.0f; ggrad_k[k] = 0.0f; }
                for (int k = 1; k < m; k++){
                    float gmin = hits[k].G < hits[k-1].G ? hits[k].G : hits[k-1].G;
                    float ddk  = hits[k].depth - hits[k-1].depth;
                    dc_p += gmin * ddk * ddk;
                    float d_dd = 2.0f * gmin * ddk;
                    dgrad_k[k]   += d_dd;
                    dgrad_k[k-1] -= d_dd;
                    if (hits[k].G <= hits[k-1].G)     ggrad_k[k]   += ddk*ddk;
                    if (hits[k-1].G <= hits[k].G)     ggrad_k[k-1] += ddk*ddk;
                }
                dc_sum += dc_p;

                float lc_invN = lam_c * invN;
                for (int k = 0; k < m; k++){
                    if (dgrad_k[k] == 0.0f && ggrad_k[k] == 0.0f) continue;
                    int si = hits[k].idx;
                    const float *s = surfels + (size_t)si * SFM_SURFEL_STRIDE;
                    float *g = grads + (size_t)si * SFM_GRAD_STRIDE;
                    s3 tu, tv, nrm;
                    sf_frame(s + 3, &tu, &tv, &nrm);
                    s3 center = s3v(s[0], s[1], s[2]);
                    float su = expf(s[7]);
                    float sv = expf(s[8]);
                    float denom = s3dot(d, nrm);
                    if (fabsf(denom) < 1e-6f) continue;
                    s3 cmo = s3sub(center, o);
                    float num = s3dot(cmo, nrm);
                    float tt = num / denom;
                    s3 P = s3add(o, s3scale(d, tt));
                    s3 dp = s3sub(P, center);
                    float dtu_d = s3dot(d, tu);
                    float dtv_d = s3dot(d, tv);

                    if (dgrad_k[k] != 0.0f){
                        /* depth = t·(d·fwd); ∂t/∂c = n/denom; ∂t/∂n = dt_dn */
                        s3 dt_dn = s3scale(s3sub(s3scale(cmo, denom), s3scale(d, num)),
                                           1.0f/(denom*denom));
                        float kc = lc_invN * dgrad_k[k] * ddotf;
                        g[0] += kc * nrm.x / denom;
                        g[1] += kc * nrm.y / denom;
                        g[2] += kc * nrm.z / denom;
                        s3 dL_dn = s3scale(dt_dn, kc);
                        s3 tq = s3cross(nrm, dL_dn);
                        g[9] += tq.x; g[10] += tq.y; g[11] += tq.z;
                    }
                    if (ggrad_k[k] != 0.0f && !hits[k].lowpass){
                        /* G = exp(−½(u²+v²)) → dL_du = dQ·G·(−u) */
                        float dQ = lc_invN * ggrad_k[k];
                        float Gval = hits[k].G;
                        float dL_du = dQ * (-hits[k].u * Gval);
                        float dL_dv = dQ * (-hits[k].v * Gval);
                        g[3] += dL_du * (-hits[k].u);
                        g[4] += dL_dv * (-hits[k].v);
                        s3 du_dc = s3scale(s3add(s3scale(tu, -1.0f),
                                                  s3scale(nrm, dtu_d/denom)), 1.0f/su);
                        s3 dv_dc = s3scale(s3add(s3scale(tv, -1.0f),
                                                  s3scale(nrm, dtv_d/denom)), 1.0f/sv);
                        s3 dc = s3add(s3scale(du_dc, dL_du), s3scale(dv_dc, dL_dv));
                        g[0] += dc.x; g[1] += dc.y; g[2] += dc.z;
                        s3 dL_dtu = s3scale(dp, dL_du/su);
                        s3 dL_dtv = s3scale(dp, dL_dv/sv);
                        s3 dt_dn2 = s3scale(s3sub(s3scale(cmo, denom), s3scale(d, num)),
                                            1.0f/(denom*denom));
                        float ncoef = dL_du*dtu_d/su + dL_dv*dtv_d/sv;
                        s3 dL_dn2v = s3scale(dt_dn2, ncoef);
                        s3 tq = s3add(s3add(s3cross(tu, dL_dtu), s3cross(tv, dL_dtv)),
                                      s3cross(nrm, dL_dn2v));
                        g[9] += tq.x; g[10] += tq.y; g[11] += tq.z;
                    }
                }
            }

            /* ── 2. NC depth chain (surface hit) ── */
            if (depth_grad && lam_n != 0.0f){
                float dg = lam_n * depth_grad[pix];
                if (dg != 0.0f){
                    int surf_k = m - 1;
                    float O = 0.0f;
                    for (int k = 0; k < m; k++){
                        O += (hits[k].alpha + SFM_O_EPS) * hits[k].G;
                        if (O >= SFM_O_THRESH){ surf_k = k; break; }
                    }
                    int si = hits[surf_k].idx;
                    const float *s = surfels + (size_t)si * SFM_SURFEL_STRIDE;
                    float *g = grads + (size_t)si * SFM_GRAD_STRIDE;
                    s3 tu, tv, nrm;
                    sf_frame(s + 3, &tu, &tv, &nrm);
                    s3 center = s3v(s[0], s[1], s[2]);
                    float denom = s3dot(d, nrm);
                    if (fabsf(denom) > 1e-6f){
                        s3 cmo = s3sub(center, o);
                        float num = s3dot(cmo, nrm);
                        float kc = dg * ddotf;
                        g[0] += kc * nrm.x / denom;
                        g[1] += kc * nrm.y / denom;
                        g[2] += kc * nrm.z / denom;
                        s3 dt_dn = s3scale(s3sub(s3scale(cmo, denom), s3scale(d, num)),
                                           1.0f/(denom*denom));
                        s3 dL_dn = s3scale(dt_dn, kc);
                        s3 tq = s3cross(nrm, dL_dn);
                        g[9] += tq.x; g[10] += tq.y; g[11] += tq.z;
                    }
                }
            }

            /* ── 3. photometric + NC normal suffix chain ── */
            int use = m;
            float T = 1.0f;
            float rawacc[3] = {0,0,0};
            for (int k = 0; k < m; k++){
                Tarr[k] = T;
                float w = hits[k].alpha * T;
                warr[k] = w;
                int si = hits[k].idx;
                const float *s = surfels + (size_t)si * SFM_SURFEL_STRIDE;
                s3 tu, tv, nrm;
                sf_frame(s + 3, &tu, &tv, &nrm);
                float sfx = (s3dot(nrm, cam->forward) > 0.0f) ? -1.0f : 1.0f;
                nxf[k*3+0] = nrm.x*sfx; nxf[k*3+1] = nrm.y*sfx; nxf[k*3+2] = nrm.z*sfx;
                s3 nf = s3v(nxf[k*3], nxf[k*3+1], nxf[k*3+2]);
                rawacc[0] += w*nf.x; rawacc[1] += w*nf.y; rawacc[2] += w*nf.z;
                T *= (1.0f - hits[k].alpha);
                if (T < SFM_T_EPS){ use = k+1; break; }
            }
            /* (keep unused trailing entries from mattering: use caps all walks) */

            s3 dL_drawacc = s3v(0,0,0);
            if (normal_grad && lam_n != 0.0f){
                float Ngx = lam_n * normal_grad[pix*3+0];
                float Ngy = lam_n * normal_grad[pix*3+1];
                float Ngz = lam_n * normal_grad[pix*3+2];
                if (Ngx || Ngy || Ngz){
                    float al = sqrtf(rawacc[0]*rawacc[0]+rawacc[1]*rawacc[1]+rawacc[2]*rawacc[2]);
                    if (al > 1e-12f){
                        float nh[3] = {rawacc[0]/al, rawacc[1]/al, rawacc[2]/al};
                        /* (I − n̂n̂ᵀ)/|acc| · Ng */
                        float nNg = nh[0]*Ngx + nh[1]*Ngy + nh[2]*Ngz;
                        dL_drawacc.x = (Ngx - nNg*nh[0]) / al;
                        dL_drawacc.y = (Ngy - nNg*nh[1]) / al;
                        dL_drawacc.z = (Ngz - nNg*nh[2]) / al;
                    }
                }
            }

            s3 A = s3v(0,0,0);
            s3 An = s3v(0,0,0);
            for (int k = use-1; k >= 0; k--){
                int si = hits[k].idx;
                const float *s = surfels + (size_t)si * SFM_SURFEL_STRIDE;
                float *g = grads + (size_t)si * SFM_GRAD_STRIDE;
                float ak = hits[k].alpha, Tk = Tarr[k], wk = warr[k];
                s3 ck = s3v(s[10], s[11], s[12]);
                s3 nf = s3v(nxf[k*3], nxf[k*3+1], nxf[k*3+2]);

                /* colour gradient (photometric) */
                g[6] += dCp.x * wk;
                g[7] += dCp.y * wk;
                g[8] += dCp.z * wk;

                /* alpha gradient: photometric + NC-suffix */
                float dL_da = s3dot(dCp, ck)*Tk - s3dot(dCp, A)/(1.0f - ak + 1e-8f);
                if (dL_drawacc.x || dL_drawacc.y || dL_drawacc.z){
                    dL_da += s3dot(dL_drawacc, nf)*Tk
                           - s3dot(dL_drawacc, An)/(1.0f - ak + 1e-8f);
                    /* frame-column gradient for the normal map (clamp-independent) */
                    s3 dL_dn_col = s3scale(dL_drawacc, wk);
                    s3 tq = s3cross(nf, dL_dn_col);
                    g[9] += tq.x; g[10] += tq.y; g[11] += tq.z;
                }
                A = s3add(A, s3scale(ck, wk));
                An = s3add(An, s3scale(nf, wk));

                float sig = 1.0f / (1.0f + expf(-s[9]));
                float Graw = hits[k].G;
                if (sig * Graw >= SFM_ALPHA_CLAMP) continue;

                float dL_dsig = dL_da * Graw;
                g[5] += dL_dsig * sig * (1.0f - sig);

                if (hits[k].lowpass) continue;

                float dL_dG = dL_da * sig;
                float dL_du = dL_dG * (-hits[k].u * Graw);
                float dL_dv = dL_dG * (-hits[k].v * Graw);

                s3 tu, tv, nrm;
                sf_frame(s + 3, &tu, &tv, &nrm);
                s3 center = s3v(s[0], s[1], s[2]);
                float su = expf(s[7]);
                float sv = expf(s[8]);

                float denom = s3dot(d, nrm);
                if (fabsf(denom) < 1e-6f) continue;
                s3 cmo = s3sub(center, o);
                float num = s3dot(cmo, nrm);
                float tt = num / denom;
                s3 P = s3add(o, s3scale(d, tt));
                s3 dp = s3sub(P, center);
                float dtu_d = s3dot(d, tu);
                float dtv_d = s3dot(d, tv);

                g[3] += dL_du * (-hits[k].u);
                g[4] += dL_dv * (-hits[k].v);

                s3 du_dc = s3scale(s3add(s3scale(tu, -1.0f), s3scale(nrm, dtu_d/denom)), 1.0f/su);
                s3 dv_dc = s3scale(s3add(s3scale(tv, -1.0f), s3scale(nrm, dtv_d/denom)), 1.0f/sv);
                s3 dc = s3add(s3scale(du_dc, dL_du), s3scale(dv_dc, dL_dv));
                g[0] += dc.x;
                g[1] += dc.y;
                g[2] += dc.z;

                s3 dL_dtu = s3scale(dp, dL_du/su);
                s3 dL_dtv = s3scale(dp, dL_dv/sv);
                s3 dt_dn  = s3scale(s3sub(s3scale(cmo, denom), s3scale(d, num)), 1.0f/(denom*denom));
                float ncoef = dL_du*dtu_d/su + dL_dv*dtv_d/sv;
                s3 dL_dn = s3scale(dt_dn, ncoef);
                s3 tq = s3add(s3add(s3cross(tu, dL_dtu), s3cross(tv, dL_dtv)),
                              s3cross(nrm, dL_dn));
                g[9] += tq.x; g[10] += tq.y; g[11] += tq.z;
            }
        }
    }

    if (out_dc) *out_dc = (float)(dc_sum * invN);

    free(hits); free(Tarr); free(warr); free(nxf);
    free(dgrad_k); free(ggrad_k);
    return 0;
}
