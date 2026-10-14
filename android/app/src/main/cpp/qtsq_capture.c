/*
 * QTSQ Live Capture — Phase C : ARCore posed-frame keyframe buffer (impl)
 *
 * Ports the keyframe ring + admission test from
 * prototype/live_surfel/surfel_online.c into the Android native lib.
 *
 * PRIVACY GUARANTEE (enforced here):
 *   - A non-keyframe frame is never copied or stored — qtsq_capture_observe()
 *     returns 0 and the caller's buffer is untouched ("no video is ever stored").
 *   - Only a bounded window of kf_cap keyframes is held, in RAM only.
 *   - This module performs NO file I/O and NO network access.
 *
 * Author: Haruhito
 */

#include "qtsq_capture.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── local vec3 helpers (mirror surfel.h) ── */
static inline cap_vec3 cv3(float x, float y, float z){ cap_vec3 r={x,y,z}; return r; }
static inline cap_vec3 cv3sub(cap_vec3 a, cap_vec3 b){ return cv3(a.x-b.x,a.y-b.y,a.z-b.z); }
static inline float    cv3dot(cap_vec3 a, cap_vec3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline float    cv3len(cap_vec3 a){ return sqrtf(cv3dot(a,a)); }
static inline cap_vec3 cv3norm(cap_vec3 a){
    float l=cv3len(a); return (l>1e-20f)? cv3(a.x/l,a.y/l,a.z/l) : a;
}

qtsq_capture_t *qtsq_capture_create(int W, int H, int kf_cap,
                                    float t_thresh, float cos_thresh){
    if (W < 1 || H < 1) return NULL;
    if (kf_cap < 1) kf_cap = 1;
    qtsq_capture_t *c = (qtsq_capture_t*)calloc(1, sizeof(qtsq_capture_t));
    if (!c) return NULL;
    c->kf = (cap_keyframe_t*)calloc((size_t)kf_cap, sizeof(cap_keyframe_t));
    if (!c->kf){ free(c); return NULL; }
    c->cap = kf_cap; c->count = 0; c->head = 0;
    c->W = W; c->H = H;
    c->has_last = 0;
    c->t_thresh = t_thresh;
    c->cos_thresh = cos_thresh;
    c->frames_seen = 0;
    c->keyframes_total = 0;
    pthread_mutex_init(&c->c_lock, NULL);
    return c;
}

void qtsq_capture_destroy(qtsq_capture_t *c){
    if (!c) return;
    pthread_mutex_destroy(&c->c_lock);
    if (c->kf){
        for (int i=0;i<c->cap;i++) free(c->kf[i].image);
        free(c->kf);
    }
    free(c);
}

void qtsq_capture_camera_from_arpose(cap_camera_t *out,
                                     const float m[16],
                                     float fx, float fy, float cx, float cy,
                                     int W, int H){
    if (!out || !m) return;
    /* Column-major camera→world (OpenGL): col0=+X(right), col1=+Y(up),
     * col2=+Z(backward), col3=translation. View direction is −Z. Our camera
     * uses image-DOWN as 'up', which is −(world up) = −col1. */
    out->pos     = cv3(m[12], m[13], m[14]);
    out->right   = cv3norm(cv3( m[0],  m[1],  m[2]));
    out->forward = cv3norm(cv3(-m[8], -m[9], -m[10]));
    out->up      = cv3norm(cv3(-m[4], -m[5], -m[6]));
    out->fx = fx; out->fy = fy; out->cx = cx; out->cy = cy;
    out->width = W; out->height = H;
}

/* admit if window empty, or moved / rotated enough from the last keyframe */
static int is_keyframe(const qtsq_capture_t *c, const cap_camera_t *cam){
    if (!c->has_last) return 1;
    float dt = cv3len(cv3sub(cam->pos, c->last_pos));
    if (dt >= c->t_thresh) return 1;
    float cdot = cv3dot(cv3norm(cam->forward), c->last_fwd);
    if (cdot <= c->cos_thresh) return 1;
    return 0;
}

int qtsq_capture_observe(qtsq_capture_t *c, const cap_camera_t *cam,
                         const float *image){
    if (!c || !cam || !image) return 0;

    pthread_mutex_lock(&c->c_lock);
    c->frames_seen++;

    if (!is_keyframe(c, cam)){
        pthread_mutex_unlock(&c->c_lock);
        return 0;                     /* frame discarded — never stored */
    }

    int slot;
    if (c->count < c->cap){
        slot = (c->head + c->count) % c->cap;
        c->count++;
    } else {
        slot = c->head;               /* evict oldest */
        c->head = (c->head + 1) % c->cap;
    }
    size_t npx = (size_t)c->W * c->H * 3;
    if (!c->kf[slot].image)
        c->kf[slot].image = (float*)malloc(npx * sizeof(float));
    if (c->kf[slot].image)
        memcpy(c->kf[slot].image, image, npx * sizeof(float));
    c->kf[slot].cam = *cam;

    c->last_pos = cam->pos;
    c->last_fwd = cv3norm(cam->forward);
    c->has_last = 1;
    c->keyframes_total++;
    pthread_mutex_unlock(&c->c_lock);
    return 1;
}

int qtsq_capture_keyframe_count(const qtsq_capture_t *c){
    int n = 0;
    if (!c) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&c->c_lock);
    n = c->count;
    pthread_mutex_unlock((pthread_mutex_t*)&c->c_lock);
    return n;
}

int qtsq_capture_sample(const qtsq_capture_t *c, int r,
                        cap_camera_t *cam, float *out_img){
    if (!c || !cam || r < 0) return 0;

    pthread_mutex_lock((pthread_mutex_t*)&c->c_lock);
    if (r >= c->count){
        pthread_mutex_unlock((pthread_mutex_t*)&c->c_lock);
        return 0;
    }
    const cap_keyframe_t *kf = &c->kf[(c->head + r) % c->cap];
    if (!kf->image){
        pthread_mutex_unlock((pthread_mutex_t*)&c->c_lock);
        return 0;
    }
    *cam = kf->cam;
    if (out_img){
        size_t npx = (size_t)c->W * c->H * 3;
        memcpy(out_img, kf->image, npx * sizeof(float));
    }
    pthread_mutex_unlock((pthread_mutex_t*)&c->c_lock);
    return 1;
}
