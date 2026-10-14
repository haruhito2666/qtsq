/*
 * QTSQ 4D Gaussian Splatting Engine — Implementation
 * Full 3DGS/4DGS renderer: SH color, EWA rasterization, PLY import.
 *
 * Implements Kerbl et al. 2023 + Wu et al. 2024.
 * Author: Haruhito
 */

#include "qtsq_4dgs.h"
#include "qtsq.h"
#include "qtsq_keystore.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <zlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════
 *  SH Basis Functions — Associated Legendre + real SH
 * ══════════════════════════════════════════════════════════════ */

/* SH degree 0–3: 16 coefficients per channel, 48 total */
#define SH_DEGREE     3
#define SH_N_COEFFS  ((SH_DEGREE + 1) * (SH_DEGREE + 1))  /* 16 */

/* Precomputed normalization constants for real SH (degree 0-3) */
static const float SH_C0 =  0.28209479177387814f;  /* l=0,m=0 */
static const float SH_C1 =  0.48860251190291992f;  /* l=1,m=-1..1 */
static const float SH_C2_0 = 1.0925484305920792f;   /* l=2,m=0 */
static const float SH_C2_1 = 1.0925484305920792f;   /* l=2,m=-1,+1 */
static const float SH_C2_2 = 0.31539156525252005f;  /* l=2,m=-2,+2 */
static const float SH_C3_0 = 0.59004358992664352f;  /* l=3,m=0 */
static const float SH_C3_1 = 2.8906114426405541f;   /* l=3,m=-1,+1 */
static const float SH_C3_2 = 0.45704579946446577f;  /* l=3,m=-2,+2 */
static const float SH_C3_3 = 0.37317633259011540f;  /* l=3,m=-3,+3 */

/* Evaluate real SH up to degree 3 at a unit direction (x,y,z) */
static void sh_eval_basis(const float dir[3], float basis[SH_N_COEFFS]) {
    float x = dir[0], y = dir[1], z = dir[2];
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;

    /* Degree 0 */
    basis[0] = SH_C0;  /* Y_{0,0} */

    /* Degree 1 */
    basis[1] = -SH_C1 * y;   /* Y_{1,-1} */
    basis[2] =  SH_C1 * z;   /* Y_{1,0}  */
    basis[3] = -SH_C1 * x;   /* Y_{1,1}  */

    /* Degree 2 */
    basis[4] =  SH_C2_1 * xy;                     /* Y_{2,-2} */
    basis[5] = -SH_C2_1 * yz;                     /* Y_{2,-1} */
    basis[6] =  SH_C2_0 * (3.0f * zz - 1.0f);     /* Y_{2,0}  */
    basis[7] = -SH_C2_1 * xz;                     /* Y_{2,1}  */
    basis[8] =  SH_C2_2 * (xx - yy);              /* Y_{2,2}  */

    /* Degree 3 */
    basis[9]  = -SH_C3_3 * y * (3.0f * xx - yy);              /* Y_{3,-3} */
    basis[10] =  SH_C3_2 * xy * z;                             /* Y_{3,-2} */
    basis[11] = -SH_C3_1 * y * (5.0f * zz - 1.0f);            /* Y_{3,-1} */
    basis[12] =  SH_C3_0 * z * (5.0f * zz - 3.0f);            /* Y_{3,0}  */
    basis[13] = -SH_C3_1 * x * (5.0f * zz - 1.0f);            /* Y_{3,1}  */
    basis[14] =  SH_C3_2 * z * (xx - yy);                      /* Y_{3,2}  */
    basis[15] = -SH_C3_3 * x * (xx - 3.0f * yy);              /* Y_{3,3}  */
}

void gs_eval_sh(const gaussian_4d_t *g, const float view_dir[3],
                float *out_r, float *out_g, float *out_b) {
    float basis[SH_N_COEFFS];
    sh_eval_basis(view_dir, basis);

    /* Standard 3DGS: f_dc stored as (rgb - 0.5) / SH_C0
     * So DC color = 0.5 + SH_C0 * f_dc
     * basis[0] = SH_C0 (= 0.282...), so DC color = 0.5 + basis[0] * f_dc */
    float r = 0.5f + basis[0] * g->sh[0];
    float gr = 0.5f + basis[0] * g->sh[1];
    float b = 0.5f + basis[0] * g->sh[2];

    /* Higher-degree contributions: sum(rest_coeff * basis) */
    for (int i = 1; i < SH_N_COEFFS; i++) {
        r  += basis[i] * g->sh[3 + i - 1];         /* R channel: sh[3..17] */
        gr += basis[i] * g->sh[3 + 15 + i - 1];    /* G channel: sh[18..32] */
        b  += basis[i] * g->sh[3 + 30 + i - 1];    /* B channel: sh[33..47] */
    }

    /* Clamp to [0, 1] */
    *out_r = (r < 0.0f) ? 0.0f : ((r > 1.0f) ? 1.0f : r);
    *out_g = (gr < 0.0f) ? 0.0f : ((gr > 1.0f) ? 1.0f : gr);
    *out_b = (b < 0.0f) ? 0.0f : ((b > 1.0f) ? 1.0f : b);
}

/* ══════════════════════════════════════════════════════════════
 *  Quaternion → Rotation Matrix
 * ══════════════════════════════════════════════════════════════ */

static void quat_to_mat3(const float q[4], float R[9]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    R[0] = 1.0f - 2.0f * (yy + zz);
    R[1] = 2.0f * (xy + wz);
    R[2] = 2.0f * (xz - wy);

    R[3] = 2.0f * (xy - wz);
    R[4] = 1.0f - 2.0f * (xx + zz);
    R[5] = 2.0f * (yz + wx);

    R[6] = 2.0f * (xz + wy);
    R[7] = 2.0f * (yz - wx);
    R[8] = 1.0f - 2.0f * (xx + yy);
}

/* ══════════════════════════════════════════════════════════════
 *  Sigma Activation — exp for scale, sigmoid for opacity
 * ══════════════════════════════════════════════════════════════ */

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

/* ══════════════════════════════════════════════════════════════
 *  Camera
 * ══════════════════════════════════════════════════════════════ */

gs_camera_t gs_camera_default(float width, float height) {
    gs_camera_t cam;
    cam.width = width;  cam.height = height;
    cam.fx = width * 0.7f;  cam.fy = width * 0.7f;
    cam.cx = width * 0.5f;  cam.cy = height * 0.5f;
    cam.z_near = 0.01f;  cam.z_far = 1000.0f;

    /* Identity rotation */
    memset(cam.R, 0, sizeof(cam.R));
    cam.R[0] = 1.0f; cam.R[4] = 1.0f; cam.R[8] = 1.0f;
    memset(cam.t, 0, sizeof(cam.t));
    return cam;
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void normalize3(float v[3]) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-8f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

void gs_camera_lookat(gs_camera_t *cam,
                       float eye_x, float eye_y, float eye_z,
                       float target_x, float target_y, float target_z,
                       float up_x, float up_y, float up_z) {
    float fwd[3] = { target_x - eye_x, target_y - eye_y, target_z - eye_z };
    normalize3(fwd);
    float up[3] = { up_x, up_y, up_z };
    float right[3];
    /* Standard right-handed: right = fwd × up (was up × fwd, which gave
     * left-handed +Z forward and flipped/rotated rendered geometry). */
    cross3(fwd, up, right); normalize3(right);
    float real_up[3];
    cross3(right, fwd, real_up);

    /* R maps world -> camera: X=right, Y=real_up, Z=-fwd (right-handed -Z
     * forward, matching the standard 3DGS/PLY convention). */
    cam->R[0] = right[0];    cam->R[1] = right[1];    cam->R[2] = right[2];
    cam->R[3] = real_up[0];  cam->R[4] = real_up[1];  cam->R[5] = real_up[2];
    cam->R[6] = -fwd[0];     cam->R[7] = -fwd[1];     cam->R[8] = -fwd[2];

    /* t = -R * eye */
    cam->t[0] = -(right[0]*eye_x + right[1]*eye_y + right[2]*eye_z);
    cam->t[1] = -(real_up[0]*eye_x + real_up[1]*eye_y + real_up[2]*eye_z);
    cam->t[2] = -(-fwd[0]*eye_x + -fwd[1]*eye_y + -fwd[2]*eye_z);
}

/* ══════════════════════════════════════════════════════════════
 *  Gaussian Model
 * ══════════════════════════════════════════════════════════════ */

void gs_model_init(gs_model_t *model) {
    memset(model, 0, sizeof(*model));
    model->capacity = 1024;
    model->gaussians = (gaussian_4d_t *)malloc(model->capacity * sizeof(gaussian_4d_t));
    model->count = 0;
}

void gs_model_free(gs_model_t *model) {
    if (model->gaussians) free(model->gaussians);
    memset(model, 0, sizeof(*model));
}

int gs_model_add(gs_model_t *model, const gaussian_4d_t *g) {
    if (model->count >= model->capacity) {
        model->capacity *= 2;
        gaussian_4d_t *tmp = (gaussian_4d_t *)realloc(model->gaussians,
                                                       model->capacity * sizeof(gaussian_4d_t));
        if (!tmp) return -1;
        model->gaussians = tmp;
    }
    model->gaussians[model->count++] = *g;

    /* Update AABB */
    if (model->count == 1) {
        model->aabb_min[0] = g->x; model->aabb_min[1] = g->y; model->aabb_min[2] = g->z;
        model->aabb_max[0] = g->x; model->aabb_max[1] = g->y; model->aabb_max[2] = g->z;
    } else {
        if (g->x < model->aabb_min[0]) model->aabb_min[0] = g->x;
        if (g->y < model->aabb_min[1]) model->aabb_min[1] = g->y;
        if (g->z < model->aabb_min[2]) model->aabb_min[2] = g->z;
        if (g->x > model->aabb_max[0]) model->aabb_max[0] = g->x;
        if (g->y > model->aabb_max[1]) model->aabb_max[1] = g->y;
        if (g->z > model->aabb_max[2]) model->aabb_max[2] = g->z;
    }
    return 0;
}

int gs_model_append_array(gs_model_t *model, const gaussian_4d_t *arr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (gs_model_add(model, &arr[i]) != 0) return -1;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  PLY Import
 *
 *  Reads standard 3DGS .ply files with vertex properties:
 *    property float x, y, z
 *    property float f_dc_0, f_dc_1, f_dc_2
 *    property float f_rest_0 ... f_rest_44  (15 per channel × 3)
 *    property float opacity
 *    property float scale_0, scale_1, scale_2
 *    property float rot_0, rot_1, rot_2, rot_3
 *
 *  Supports binary little-endian (format binary_little_endian 1.0).
 * ══════════════════════════════════════════════════════════════ */

static int ply_read_line(char *buf, int max, const uint8_t **data, size_t *remaining) {
    int i = 0;
    while (*remaining > 0 && i < max - 1) {
        char c = (char)**data;
        (*data)++; (*remaining)--;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i;
}

static int ply_parse_int(const char *s, int *out) {
    char *end;
    *out = (int)strtol(s, &end, 10);
    return (end != s);
}

typedef struct {
    int offset_x, offset_y, offset_z;
    int offset_fdc[3];
    int offset_opacity;
    int offset_scale[3];
    int offset_rot[4];
    int offset_frest[45];
    int vertex_size;  /* bytes per vertex */
    int vertex_count;
} ply_header_info_t;

int gs_model_load_ply(gs_model_t *model, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return -1; }

    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) { free(data); fclose(f); return -1; }
    fclose(f);

    const uint8_t *ptr = data;
    size_t rem = (size_t)fsize;

    /* Parse header */
    ply_header_info_t h;
    memset(&h, 0, sizeof(h));
    int byte_offset = 0;

    while (rem > 0) {
        char line[4096];
        ply_read_line(line, sizeof(line), &ptr, &rem);
        if (strncmp(line, "end_header", 10) == 0) break;

        if (strncmp(line, "element vertex ", 15) == 0) {
            ply_parse_int(line + 15, &h.vertex_count);
        } else if (strncmp(line, "property float x", 16) == 0) {
            h.offset_x = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float y", 16) == 0) {
            h.offset_y = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float z", 16) == 0) {
            h.offset_z = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float f_dc_0", 21) == 0) {
            h.offset_fdc[0] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float f_dc_1", 21) == 0) {
            h.offset_fdc[1] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float f_dc_2", 21) == 0) {
            h.offset_fdc[2] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float opacity", 22) == 0) {
            h.offset_opacity = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float scale_0", 22) == 0) {
            h.offset_scale[0] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float scale_1", 22) == 0) {
            h.offset_scale[1] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float scale_2", 22) == 0) {
            h.offset_scale[2] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float rot_0", 20) == 0) {
            h.offset_rot[0] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float rot_1", 20) == 0) {
            h.offset_rot[1] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float rot_2", 20) == 0) {
            h.offset_rot[2] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float rot_3", 20) == 0) {
            h.offset_rot[3] = byte_offset; byte_offset += 4;
        } else if (strncmp(line, "property float f_rest_", 22) == 0) {
            int idx;
            if (ply_parse_int(line + 22, &idx) && idx >= 0 && idx < 45) {
                h.offset_frest[idx] = byte_offset; byte_offset += 4;
            }
        }
    }

    h.vertex_size = byte_offset;

    /* Read vertex data */
    for (int v = 0; v < h.vertex_count && rem >= (size_t)h.vertex_size; v++) {
        const uint8_t *vdata = ptr;
        ptr += h.vertex_size; rem -= h.vertex_size;

        gaussian_4d_t g;
        memset(&g, 0, sizeof(g));

        float fx, fy, fz, fdc[3], op, sc[3], rot[4];
        memcpy(&fx, vdata + h.offset_x, 4);
        memcpy(&fy, vdata + h.offset_y, 4);
        memcpy(&fz, vdata + h.offset_z, 4);
        memcpy(&fdc[0], vdata + h.offset_fdc[0], 4);
        memcpy(&fdc[1], vdata + h.offset_fdc[1], 4);
        memcpy(&fdc[2], vdata + h.offset_fdc[2], 4);
        memcpy(&op, vdata + h.offset_opacity, 4);
        memcpy(&sc[0], vdata + h.offset_scale[0], 4);
        memcpy(&sc[1], vdata + h.offset_scale[1], 4);
        memcpy(&sc[2], vdata + h.offset_scale[2], 4);
        memcpy(&rot[0], vdata + h.offset_rot[0], 4);
        memcpy(&rot[1], vdata + h.offset_rot[1], 4);
        memcpy(&rot[2], vdata + h.offset_rot[2], 4);
        memcpy(&rot[3], vdata + h.offset_rot[3], 4);

        g.x = fx; g.y = fy; g.z = fz;
        g.sx = sc[0]; g.sy = sc[1]; g.sz = sc[2];
        g.qw = rot[0]; g.qx = rot[1]; g.qy = rot[2]; g.qz = rot[3];
        g.opacity = op;
        g.sh[0] = fdc[0]; g.sh[1] = fdc[1]; g.sh[2] = fdc[2];

        /* Read f_rest coefficients */
        for (int i = 0; i < 45; i++) {
            float val = 0;
            memcpy(&val, vdata + h.offset_frest[i], 4);
            g.sh[3 + i] = val;
        }

        g.t_scale = 0.0f; /* Static by default */
        g.t_center = 0.0f;
        g.wx = g.wy = g.wz = 0.0f;

        if (gs_model_add(model, &g) != 0) {
            free(data);
            return -1;
        }
    }

    free(data);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Tile-Based EWA Rasterizer
 *
 *  Steps:
 *   1. Transform Gaussians to camera space, frustum cull
 *   2. Compute 3D covariance Σ = R S S^T R^T
 *   3. Project to screen: μ' = K(μ_cam), Σ' = J W Σ W^T J^T
 *   4. Depth-sort Gaussians (far to near for alpha blending)
 *   5. Tile-based rasterization: assign to tiles, blend per-tile
 * ══════════════════════════════════════════════════════════════ */

#define TILE_SIZE    16
#define MAX_TILES_X  256
#define MAX_TILES_Y  256

typedef struct {
    float    mu_x, mu_y;      /* screen-space projected center */
    float    depth;           /* camera-space Z for sorting */
    float    cov2d[4];        /* 2D covariance = [a b; c d] = [cov[0] cov[1]; cov[1] cov[2]] */
    float    color[3];        /* RGB from SH */
    float    alpha;           /* post-sigmoid opacity x Gaussian weight at center */
    float    radius;          /* axis-aligned bounding radius in pixels */
    float    world_x, world_y, world_z;  /* world-space position at time t (for SH eval) */
    uint32_t tile_min_x, tile_min_y, tile_max_x, tile_max_y;
} gs_splatted_t;

/* Assemble 3D covariance from scale + rotation */
static void compute_cov3d(const gaussian_4d_t *g, float cov3d[9]) {
    float R[9];
    float q[4] = { g->qw, g->qx, g->qy, g->qz };
    quat_to_mat3(q, R);

    float sx = expf(g->sx), sy = expf(g->sy), sz = expf(g->sz);

    /* M = R * S  (S is diagonal: R columns scaled by sx, sy, sz) */
    /* Sigma = M * M^T                                           */
    float M[9];
    M[0] = R[0] * sx; M[1] = R[1] * sy; M[2] = R[2] * sz;
    M[3] = R[3] * sx; M[4] = R[4] * sy; M[5] = R[5] * sz;
    M[6] = R[6] * sx; M[7] = R[7] * sy; M[8] = R[8] * sz;

    cov3d[0] = M[0]*M[0] + M[1]*M[1] + M[2]*M[2];
    cov3d[1] = M[0]*M[3] + M[1]*M[4] + M[2]*M[5];
    cov3d[2] = M[0]*M[6] + M[1]*M[7] + M[2]*M[8];
    cov3d[3] = cov3d[1];
    cov3d[4] = M[3]*M[3] + M[4]*M[4] + M[5]*M[5];
    cov3d[5] = M[3]*M[6] + M[4]*M[7] + M[5]*M[8];
    cov3d[6] = cov3d[2];
    cov3d[7] = cov3d[5];
    cov3d[8] = M[6]*M[6] + M[7]*M[7] + M[8]*M[8];
}

/* Project 3D covariance to 2D screen space using the EWA approximation.
 * Sigma' = J * W * Sigma * W^T * J^T
 * where W is the viewing transform (world->cam rotation) and
 * J is the projective Jacobian:
 *   J = [fx/z  0    -fx*x/z^2]
 *       [0    fy/z  -fy*y/z^2] */
static int project_to_screen(const gaussian_4d_t *g, const gs_camera_t *cam,
                              float t, gs_splatted_t *out) {
    /* Evaluate 4D conditional slice */
    float dt = t - g->t_center;
    float px = g->x + g->wx * dt;
    float py = g->y + g->wy * dt;
    float pz = g->z + g->wz * dt;
    out->world_x = px; out->world_y = py; out->world_z = pz;

    /* World → Camera */
    const float *R = cam->R;
    const float *T = cam->t;
    float cx = R[0]*px + R[1]*py + R[2]*pz + T[0];
    float cy = R[3]*px + R[4]*py + R[5]*pz + T[1];
    float cz = R[6]*px + R[7]*py + R[8]*pz + T[2];

    /* Frustum cull */
    if (cz <= cam->z_near || cz >= cam->z_far) return -1;

    /* Project: u = fx * cx/cz + cx, v = fy * cy/cz + cy */
    float inv_z = 1.0f / cz;
    float u = cam->fx * cx * inv_z + cam->cx;
    float v = cam->fy * cy * inv_z + cam->cy;

    /* Check if within image bounds (with margin) */
    float margin = 32.0f;
    if (u < -margin || u > cam->width + margin ||
        v < -margin || v > cam->height + margin) return -1;

    /* Compute 3D covariance */
    float cov3d[9];
    compute_cov3d(g, cov3d);

    /* W: camera rotation block */
    /* W * Sigma * W^T  (covariance in camera space) */
    float cov_cam[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float sum = 0;
            for (int k = 0; k < 3; k++) {
                for (int l = 0; l < 3; l++) {
                    sum += R[i*3 + k] * cov3d[k*3 + l] * R[j*3 + l];
                }
            }
            cov_cam[i*3 + j] = sum;
        }
    }

    /* Jacobian J for perspective projection:
     * J = [fx/z  0    -fx*x/z^2]
     *     [0    fy/z  -fy*y/z^2]
     */
    float j00 = cam->fx * inv_z;
    float j02 = -cam->fx * cx * inv_z * inv_z;
    float j11 = cam->fy * inv_z;
    float j12 = -cam->fy * cy * inv_z * inv_z;

    /* Sigma_2d = J * cov_cam_top2x3 * J^T
     * Only need top-left 2x2 of cov_cam for projection */
    float a = j00 * (j00 * cov_cam[0] + j02 * cov_cam[6]) +
              j02 * (j00 * cov_cam[6] + j02 * cov_cam[8]);
    float b = j00 * (j00 * cov_cam[1] + j02 * cov_cam[7]) +
              j02 * (j00 * cov_cam[3] + j02 * cov_cam[8]);
    float d = j11 * (j11 * cov_cam[4] + j12 * cov_cam[7]) +
              j12 * (j11 * cov_cam[7] + j12 * cov_cam[8]);

    /* Add a minimum blur to avoid degenerate splats */
    float min_std = 0.3f;
    if (a < min_std*min_std) a = min_std*min_std;
    if (d < min_std*min_std) d = min_std*min_std;

    out->cov2d[0] = a;
    out->cov2d[1] = b;
    out->cov2d[2] = d;
    out->mu_x = u;
    out->mu_y = v;
    out->depth = cz;

    /* Compute axis-aligned bounding box in pixels
     * radius = 3 * max(eigenvalue) */
    float trace = a + d;
    float det = a * d - b * b;
    float eig1 = (trace + sqrtf(trace*trace - 4.0f*det)) * 0.5f;
    float eig2 = (trace - sqrtf(trace*trace - 4.0f*det)) * 0.5f;
    float max_eig = (eig1 > eig2) ? eig1 : eig2;
    if (max_eig < 0.0f) max_eig = 0.0f;
    out->radius = 3.0f * sqrtf(max_eig);

    /* Temporal alpha falloff */
    float temporal_alpha = 1.0f;
    if (g->t_scale > 0.0f) {
        float zt = dt / g->t_scale;
        temporal_alpha = expf(-0.5f * zt * zt);
    }

    out->alpha = sigmoid(g->opacity) * temporal_alpha;
    out->color[0] = out->color[1] = out->color[2] = 0.0f;

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Depth-sort comparator (descending depth: far → near)
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t idx;
    float    depth;
} gs_sort_key_t;

static int sort_by_depth(const void *a, const void *b) {
    float da = ((const gs_sort_key_t *)a)->depth;
    float db = ((const gs_sort_key_t *)b)->depth;
    if (da > db) return -1;
    if (da < db) return 1;
    return 0;
}

int gs_render(const gs_model_t *model, const gs_camera_t *cam,
              float t, uint8_t *output) {
    if (!model || !cam || !output || model->count == 0) return -1;

    uint32_t W = (uint32_t)cam->width, H = (uint32_t)cam->height;
    uint32_t tiles_x = (W + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t tiles_y = (H + TILE_SIZE - 1) / TILE_SIZE;
    /* M12 fix: clamp tiles to MAX_TILES_X/Y to prevent uint32 overflow in
     * total_tiles = tiles_x * tiles_y for very large images (which would
     * undersize tile_counters[] → OOB writes). */
    if (tiles_x > MAX_TILES_X) tiles_x = MAX_TILES_X;
    if (tiles_y > MAX_TILES_Y) tiles_y = MAX_TILES_Y;

    /* Clear framebuffer to black/transparent */
    memset(output, 0, (size_t)W * H * 4);

    /* ── Step 1-3: Project all Gaussians and compute screen-space data ── */
    gs_splatted_t *splatted = (gs_splatted_t *)malloc(model->count * sizeof(gs_splatted_t));
    gs_sort_key_t *sort_keys = (gs_sort_key_t *)malloc(model->count * sizeof(gs_sort_key_t));
    if (!splatted || !sort_keys) { free(splatted); free(sort_keys); return -1; }

    /* Compute camera world position from R and t: C = -R^T * t */
    float cam_pos[3];
    cam_pos[0] = -(cam->R[0]*cam->t[0] + cam->R[3]*cam->t[1] + cam->R[6]*cam->t[2]);
    cam_pos[1] = -(cam->R[1]*cam->t[0] + cam->R[4]*cam->t[1] + cam->R[7]*cam->t[2]);
    cam_pos[2] = -(cam->R[2]*cam->t[0] + cam->R[5]*cam->t[1] + cam->R[8]*cam->t[2]);

    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < model->count; i++) {
        const gaussian_4d_t *g = &model->gaussians[i];
        gs_splatted_t *sp = &splatted[valid_count];

        if (project_to_screen(g, cam, t, sp) != 0) continue;

        /* Compute SH color using view direction from camera to Gaussian */
        float view_dir[3] = {
            sp->world_x - cam_pos[0],
            sp->world_y - cam_pos[1],
            sp->world_z - cam_pos[2]
        };
        float vlen = sqrtf(view_dir[0]*view_dir[0] + view_dir[1]*view_dir[1] + view_dir[2]*view_dir[2]);
        if (vlen > 1e-6f) { view_dir[0] /= vlen; view_dir[1] /= vlen; view_dir[2] /= vlen; }
        gs_eval_sh(g, view_dir, &sp->color[0], &sp->color[1], &sp->color[2]);

        /* Tile assignment */
        int tx0 = (int)((sp->mu_x - sp->radius) / TILE_SIZE);
        int ty0 = (int)((sp->mu_y - sp->radius) / TILE_SIZE);
        int tx1 = (int)((sp->mu_x + sp->radius) / TILE_SIZE);
        int ty1 = (int)((sp->mu_y + sp->radius) / TILE_SIZE);
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 >= (int)tiles_x) tx1 = (int)tiles_x - 1;
        if (ty1 >= (int)tiles_y) ty1 = (int)tiles_y - 1;
        sp->tile_min_x = (uint32_t)tx0;
        sp->tile_min_y = (uint32_t)ty0;
        sp->tile_max_x = (uint32_t)tx1;
        sp->tile_max_y = (uint32_t)ty1;

        sort_keys[valid_count].idx = valid_count;
        sort_keys[valid_count].depth = sp->depth;
        valid_count++;
    }

    if (valid_count == 0) { free(splatted); free(sort_keys); return 0; }

    /* ── Step 4: Sort by depth (descending: far to near) ── */
    qsort(sort_keys, valid_count, sizeof(gs_sort_key_t), sort_by_depth);

    /* Allocate tile arrays */
    uint32_t total_tiles = tiles_x * tiles_y;
    uint32_t *tile_offsets = (uint32_t *)calloc(total_tiles + 1, sizeof(uint32_t));
    uint32_t *tile_counters = (uint32_t *)calloc(total_tiles, sizeof(uint32_t));
    uint32_t *tile_list = NULL;
    if (!tile_offsets || !tile_counters) {
        free(splatted); free(sort_keys);
        free(tile_offsets); free(tile_counters);
        return -1;
    }

    /* First pass: count Gaussians per tile */
    for (uint32_t si = 0; si < valid_count; si++) {
        uint32_t idx = sort_keys[si].idx;
        for (uint32_t ty = splatted[idx].tile_min_y; ty <= splatted[idx].tile_max_y; ty++) {
            for (uint32_t tx = splatted[idx].tile_min_x; tx <= splatted[idx].tile_max_x; tx++) {
                tile_counters[ty * tiles_x + tx]++;
            }
        }
    }

    /* Build prefix sum for tile offsets */
    for (uint32_t i = 0; i < total_tiles; i++) {
        tile_offsets[i + 1] = tile_offsets[i] + tile_counters[i];
    }

    tile_list = (uint32_t *)malloc(tile_offsets[total_tiles] * sizeof(uint32_t));
    if (!tile_list) {
        free(splatted); free(sort_keys);
        free(tile_offsets); free(tile_counters);
        return -1;
    }

    /* Reset counters for second pass */
    memset(tile_counters, 0, total_tiles * sizeof(uint32_t));

    /* Second pass: fill tile_list */
    for (uint32_t si = 0; si < valid_count; si++) {
        uint32_t idx = sort_keys[si].idx;
        for (uint32_t ty = splatted[idx].tile_min_y; ty <= splatted[idx].tile_max_y; ty++) {
            for (uint32_t tx = splatted[idx].tile_min_x; tx <= splatted[idx].tile_max_x; tx++) {
                uint32_t tile_idx = ty * tiles_x + tx;
                uint32_t pos = tile_offsets[tile_idx] + tile_counters[tile_idx];
                tile_list[pos] = si;
                tile_counters[tile_idx]++;
            }
        }
    }
    free(tile_counters);

    /* ── Step 5: Tile-based rasterization with alpha blending ── */
    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t tile_idx = ty * tiles_x + tx;
            uint32_t start = tile_offsets[tile_idx];
            uint32_t end = tile_offsets[tile_idx + 1];

            if (start >= end) continue;

            uint32_t px0 = tx * TILE_SIZE, py0 = ty * TILE_SIZE;
            uint32_t px1 = (tx + 1) * TILE_SIZE; if (px1 > W) px1 = W;
            uint32_t py1 = (ty + 1) * TILE_SIZE; if (py1 > H) py1 = H;

            for (uint32_t py = py0; py < py1; py++) {
                for (uint32_t px = px0; px < px1; px++) {
                    float accum_r = 0, accum_g = 0, accum_b = 0;
                    float transmittance = 1.0f;

                    for (uint32_t g_idx = start; g_idx < end && transmittance > 0.001f; g_idx++) {
                        uint32_t si = tile_list[g_idx];
                        uint32_t idx = sort_keys[si].idx;
                        gs_splatted_t *sp = &splatted[idx];

                        float dx = (float)px - sp->mu_x;
                        float dy = (float)py - sp->mu_y;

                        /* Evaluate 2D Gaussian: exp(-0.5 * [x y] * inv_cov * [x y]^T) */
                        /* inv_cov = 1/det * [d -b; -b a] from cov = [a b; b d] */
                        float det = sp->cov2d[0] * sp->cov2d[2] - sp->cov2d[1] * sp->cov2d[1];
                        if (det <= 1e-10f) continue;
                        float inv_det = 1.0f / det;
                        float inv_a = sp->cov2d[2] * inv_det;
                        float inv_b = -sp->cov2d[1] * inv_det;
                        float inv_d = sp->cov2d[0] * inv_det;

                        float power = -0.5f * (dx * dx * inv_a + 2.0f * dx * dy * inv_b + dy * dy * inv_d);
                        if (power < -15.0f) continue;

                        float weight = sp->alpha * expf(power);
                        if (weight < 1.0f / 255.0f) continue;

                        float contrib_r = weight * sp->color[0] * transmittance;
                        float contrib_g = weight * sp->color[1] * transmittance;
                        float contrib_b = weight * sp->color[2] * transmittance;

                        accum_r += contrib_r;
                        accum_g += contrib_g;
                        accum_b += contrib_b;
                        transmittance *= (1.0f - weight);
                    }

                    /* Convert accumulated float → uint8 */
                    size_t pi = ((size_t)py * W + px) * 4;
                    float alpha = 1.0f - transmittance;
                    output[pi + 0] = (uint8_t)(accum_r * 255.0f + 0.5f);
                    output[pi + 1] = (uint8_t)(accum_g * 255.0f + 0.5f);
                    output[pi + 2] = (uint8_t)(accum_b * 255.0f + 0.5f);
                    output[pi + 3] = (uint8_t)(alpha * 255.0f + 0.5f);
                }
            }
        }
    }

    free(splatted);
    free(sort_keys);
    free(tile_list);
    free(tile_offsets);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Binary Pack / Unpack
 * ══════════════════════════════════════════════════════════════ */

int gs_4d_pack(const gaussian_4d_t *gaussians, uint32_t count,
               uint8_t **out_data, size_t *out_size) {
    if (!gaussians || !out_data || !out_size || count == 0) return -1;
    size_t total = 4 + (size_t)count * sizeof(gaussian_4d_t);
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;
    memcpy(buf, &count, 4);
    memcpy(buf + 4, gaussians, (size_t)count * sizeof(gaussian_4d_t));
    *out_data = buf;
    *out_size = total;
    return 0;
}

int gs_4d_unpack(const uint8_t *data, size_t size,
                 gaussian_4d_t **out_gaussians, uint32_t *out_count) {
    if (!data || !out_gaussians || !out_count || size < 4) return -1;
    uint32_t count;
    memcpy(&count, data, 4);
    size_t expected = 4 + (size_t)count * sizeof(gaussian_4d_t);
    if (size < expected) return -1;
    gaussian_4d_t *gs = (gaussian_4d_t *)malloc((size_t)count * sizeof(gaussian_4d_t));
    if (!gs) return -1;
    memcpy(gs, data + 4, (size_t)count * sizeof(gaussian_4d_t));
    *out_gaussians = gs;
    *out_count = count;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Zlib helpers
 * ══════════════════════════════════════════════════════════════ */

static int gs_zlib_compress(const uint8_t *input, size_t input_size,
                              uint8_t **out_c, size_t *out_cs) {
    uLongf bound = compressBound((uLong)input_size);
    uint8_t *buf = (uint8_t *)malloc(bound);
    if (!buf) return -1;
    if (compress2(buf, &bound, input, (uLong)input_size, Z_BEST_COMPRESSION) != Z_OK) {
        free(buf); return -1;
    }
    *out_c = buf; *out_cs = (size_t)bound;
    return 0;
}

static int gs_zlib_decompress(const uint8_t *input, size_t input_size,
                                uint8_t **out_d, size_t *out_ds, size_t orig_size) {
    uLongf dest_len = (uLongf)orig_size;
    uint8_t *buf = (uint8_t *)malloc(dest_len);
    if (!buf) return -1;
    if (uncompress(buf, &dest_len, input, (uLong)input_size) != Z_OK) {
        free(buf); return -1;
    }
    *out_d = buf; *out_ds = (size_t)dest_len;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Container I/O
 * ══════════════════════════════════════════════════════════════ */

int gs_container_write(const char *filename, const gs_model_t *model) {
    if (!filename || !model || model->count == 0) return -1;

    qtsq_context_t container;
    int result = qtsq_container_create(&container);
    if (result != QTSQ_OK) return result;

    /* Stream 0: metadata (JSON string) */
    char meta[512];
    snprintf(meta, sizeof(meta),
             "{\"count\":%u,\"aabb\":[%f,%f,%f,%f,%f,%f],\"sh_degree\":3}",
             model->count,
             model->aabb_min[0], model->aabb_min[1], model->aabb_min[2],
             model->aabb_max[0], model->aabb_max[1], model->aabb_max[2]);
    size_t meta_len = strlen(meta);

    qtsq_context_t meta_ctx;
    qtsq_init(&meta_ctx);
    meta_ctx.header.version = QTSQ_VERSION_3;
    meta_ctx.header.strategy = QTSQ_STRATEGY_NONE;
    meta_ctx.header.data_type = QTSQ_TYPE_JSON;
    meta_ctx.singularity = (uint8_t *)malloc(meta_len);
    if (!meta_ctx.singularity) { qtsq_free(&container); return -1; }
    memcpy(meta_ctx.singularity, meta, meta_len);
    meta_ctx.singularity_size = meta_len;
    meta_ctx.header.singularity_size = meta_len;
    meta_ctx.header.original_size = meta_len;
    result = qtsq_container_add_stream(&container, &meta_ctx, "metadata");
    qtsq_free(&meta_ctx);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    /* Stream 1: gaussians (zlib-compressed) */
    uint8_t *packed = NULL;
    size_t packed_size = 0;
    if (gs_4d_pack(model->gaussians, model->count, &packed, &packed_size) != 0) {
        qtsq_free(&container); return -1;
    }

    uint8_t *compressed = NULL;
    size_t compressed_size = 0;
    int zr = gs_zlib_compress(packed, packed_size, &compressed, &compressed_size);
    free(packed);
    if (zr != 0) { qtsq_free(&container); return -1; }

    qtsq_context_t gs_ctx;
    qtsq_init(&gs_ctx);
    gs_ctx.header.version = QTSQ_VERSION_3;
    gs_ctx.header.strategy = 0x0D;  /* QTSQ_STRATEGY_4DGS */
    gs_ctx.header.data_type = 0x81; /* QTSQ_TYPE_4DGS */
    gs_ctx.header.flags = QTSQ_FLAG_SPLAT_ONLY | QTSQ_FLAG_ZLIB;
    gs_ctx.singularity = compressed;
    gs_ctx.singularity_size = compressed_size;
    gs_ctx.header.singularity_size = compressed_size;
    gs_ctx.header.original_size = packed_size;
    result = qtsq_container_add_stream(&container, &gs_ctx, "gaussians");
    qtsq_free(&gs_ctx);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    /* Pack + write */
    result = qtsq_container_pack(&container);
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    container.header.flags |= QTSQ_FLAG_TSUBU_ATOMS;
    memset(&container.schema, 0, sizeof(container.schema));
    strncpy(container.schema.name, "4DGS", 63);
    container.schema.data_type = QTSQ_TYPE_CONTAINER;
    container.schema.dimensions[0] = model->count;
    container.schema.num_dims = 1;
    snprintf(container.schema.encoding, sizeof(container.schema.encoding),
             "4dgs-v1;codec=zlib;sh_degree=3");
    container.header.horizon_size = sizeof(qtsq_schema_t);

    /* Phase 1: fail-closed — encrypt before writing. qtsq_container_pack
     * encrypts stream names (Phase 5); the vault preserves the salt. */
    container.is_sanitized = 1;
    container.sanitize_report |= QTSQ_SANITIZE_NAMES_HIDE | QTSQ_SANITIZE_ENCRYPTED;
    if (qtsq_keystore_is_initialized()) {
        result = qtsq_keystore_encrypt(&container);
    } else {
        result = QTSQ_ERR_NOT_ENCRYPTED;
    }
    if (result != QTSQ_OK) { qtsq_free(&container); return result; }

    result = qtsq_write(&container, filename);
    qtsq_free(&container);
    return result;
}

int gs_container_read(const char *filename, gs_model_t *model) {
    if (!filename || !model) return -1;

    qtsq_context_t ctx;
    qtsq_init(&ctx);
    int result = qtsq_read(&ctx, filename);
    if (result != QTSQ_OK) { qtsq_free(&ctx); return result; }

    /* Fail-closed round-trip: gs_container_write keystore-encrypts the
     * container, so decrypt in place before the stream parser sees the
     * singularity. Without this, an encrypted .qtsq unpacks ciphertext as
     * model data → corruption (the "3DGS broken" symptom). */
    if (ctx.is_encrypted) {
        if (!qtsq_keystore_is_initialized()) { qtsq_free(&ctx); return QTSQ_ERR_ENCRYPTED; }
        result = qtsq_keystore_decrypt(&ctx);
        if (result != QTSQ_OK) { qtsq_free(&ctx); return result; }
    }

    if (ctx.header.data_type != QTSQ_TYPE_CONTAINER) {
        qtsq_free(&ctx); return -1;
    }

    uint32_t num_streams = 0;
    qtsq_container_get_count(&ctx, &num_streams);

    gs_model_init(model);

    for (uint32_t i = 0; i < num_streams; i++) {
        char name[256];
        qtsq_context_t stream;
        result = qtsq_container_get_stream(&ctx, i, &stream, name, sizeof(name));
        if (result != QTSQ_OK) continue;

        if (strcmp(name, "gaussians") == 0) {
            uint8_t *raw = NULL;
            size_t raw_size = 0;

            if (stream.header.flags & QTSQ_FLAG_ZLIB) {
                gs_zlib_decompress(stream.singularity, stream.singularity_size,
                                   &raw, &raw_size, (size_t)stream.header.original_size);
            } else {
                raw_size = stream.singularity_size;
                raw = (uint8_t *)malloc(raw_size);
                if (raw) memcpy(raw, stream.singularity, raw_size);
            }

            if (raw) {
                gaussian_4d_t *gs = NULL;
                uint32_t gs_count = 0;
                gs_4d_unpack(raw, raw_size, &gs, &gs_count);
                if (gs) {
                    gs_model_append_array(model, gs, gs_count);
                    free(gs);
                }
                free(raw);
            }
        }
        qtsq_free(&stream);
    }

    qtsq_free(&ctx);
    return (model->count > 0) ? 0 : -1;
}
