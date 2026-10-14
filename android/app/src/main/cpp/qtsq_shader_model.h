/*
 * QTSQ Phase D — host shader-model reference (qtsq_shader_model.c)
 *
 * A 1:1 C mirror of the four GLSL compute shaders
 * (shaders/surfel_{forward,loss,backward,adam}.comp). It exists so the
 * Phase D validation gates can run without a GPU:
 *   - the host test suite validates this model against the Phase A prototype
 *     (finite differences + exact-match vs surfel_render/backward),
 *   - the on-device gate (trainer_parity_forward) renders one keyframe on
 *     the GPU and on this reference and compares.
 *
 * Buffer layouts match the shaders exactly:
 *   surfels: [n][ center[3], quat[4](w,x,y,z), log_su, log_sv,
 *              opacity_logit, color[3] ]            (13 floats / surfel)
 *   grads:   [n][ d_center[3], d_log_su, d_log_sv, d_opacity_logit,
 *              d_color[3], d_rot[3] ]               (12 floats / surfel)
 *   image:   W*H*3 float RGB (linear [0,1])
 *
 * Author: Haruhito
 */

#ifndef QTSQ_SHADER_MODEL_H
#define QTSQ_SHADER_MODEL_H

#include "qtsq_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SFM_SURFEL_STRIDE 13
#define SFM_GRAD_STRIDE   12
#define SFM_ALPHA_CLAMP   0.99f
#define SFM_T_EPS         1e-4f
#define SFM_G_EPS         1e-3f
#define SFM_LOWPASS_SIGMA 0.70710678f
#define SFM_MAX_HITS      256

/* Unbiased-depth parameters (Yang et al. 2025, Eq. 9): surface depth = first
 * hit where cumulative opacity O_i = Σ(α_j+ε)·G_j crosses O_THRESH. */
#define SFM_O_THRESH 0.6f
#define SFM_O_EPS    0.1f

/* Forward: sorted gather + front-to-back composite (mirror of
 * surfel_forward.comp). out_rgb must hold W*H*3 floats. */
int sfm_render(const float *surfels, int n, const cap_camera_t *cam,
               float *out_rgb);

/* Extended forward (Phase E): also writes the unbiased surface depth map
 * (W*H), the camera-facing accumulated normal map (W*H*3) and the opacity
 * map 1−T (W*H). Any output may be NULL to skip. Depth map semantics must
 * match the surface-drop rule re-derived in sfm_backward_full. */
int sfm_render_full(const float *surfels, int n, const cap_camera_t *cam,
                    float *out_rgb, float *out_depth, float *out_normal,
                    float *out_alpha);

/* Backward: sorted hits, back-to-front gradient accumulation (mirror of
 * surfel_backward.comp). grads must hold 12*n floats, pre-zeroed. */
int sfm_backward(const float *surfels, int n, const cap_camera_t *cam,
                 const float *dL_dpred, float *grads);

/* Extended backward (Phase E): photometric + normal-consistency +
 * depth-convergence gradients.
 *   dL_dpred    — ∂(L1)/∂pred (from sfm_loss_l1)
 *   depth_grad  — ∂Lnc/∂depth per pixel (from sfm_loss_nc), may be NULL
 *   normal_grad — ∂Lnc/∂normal per pixel (3 floats per pixel), may be NULL
 *   lam_n, lam_c — normal-consistency / depth-convergence weights (λn=0.05,
 *                  λc=7 per paper-2 defaults). Pass 0 to disable a term.
 *   out_dc      — optional: receives Σ_p depth-convergence value (unscaled)
 * Lists per hit: depth chain (surface hit → center/frame), DC chain over ALL
 * hits (d_k + G_k), and the NC normal suffix chain. */
int sfm_backward_full(const float *surfels, int n, const cap_camera_t *cam,
                      const float *dL_dpred, const float *depth_grad,
                      const float *normal_grad, float lam_n, float lam_c,
                      float *grads, float *out_dc);

/* Photometric L1 + dL/dpred (mirror of surfel_loss.comp). */
float sfm_loss_l1(const float *pred, const float *target, int W, int H,
                  float *dL_dpred);

/* Normal-consistency loss (2DGS Eq. 14, screen-space depth-gradient form):
 *   Lnc = mean over pixels of (α_p>0.5) · (1 − |n̂_p·N_p|)
 * with N_p = normalize(−dzx, −dzy, 1) from the depth map.
 * Fills depth_grad (W*H) and normal_grad (W*H*3) with ∂(mean)/∂depth resp.
 * ∂(mean)/∂normal_map, each normalized by the PIXEL count (uncovered pixels
 * contribute zero). Returns Lnc (before λ weighting). */
float sfm_loss_nc(const float *depth, const float *normal, const float *alpha,
                  int W, int H, float *depth_grad, float *normal_grad);

#ifdef __cplusplus
}
#endif

#endif /* QTSQ_SHADER_MODEL_H */
