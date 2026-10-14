/*
 * QTSQ Live Capture — Phase C : ARCore posed-frame keyframe buffer
 *
 * On-device stand-in host for the live 2DGS capture loop. ARCore streams posed
 * camera frames in via qtsq_capture_observe(); only a bounded sliding window of
 * KEYFRAMES retains its image. Every non-keyframe is dropped the instant it is
 * scored — this is the "no video is ever stored" privacy guarantee. No file I/O,
 * no network: the window lives in RAM only.
 *
 * This module is intentionally self-contained (a minimal vec3 + camera live
 * here, laid out to match prototype/live_surfel/surfel.h camera_t) so it links
 * into libqtsq.so without pulling the host render/backward/optim units. Phase D
 * attaches the Vulkan trainer + a qtsq_surfel_model_t to this same struct.
 *
 * Author: Haruhito
 */

#ifndef QTSQ_CAPTURE_H
#define QTSQ_CAPTURE_H

#include <stddef.h>
#include <pthread.h>

/* ── minimal world-space vector (matches surfel.h vec3 layout) ── */
typedef struct { float x, y, z; } cap_vec3;

/* ── pinhole camera: +forward view dir, +right, +up = image-DOWN, world space.
 *    Field order matches surfel.h camera_t so Phase D can reuse it directly. ── */
typedef struct {
    cap_vec3 pos;                 /* camera centre in world                      */
    cap_vec3 right, up, forward;  /* orthonormal world basis (image axes)        */
    float    fx, fy, cx, cy;      /* intrinsics (pixels, in W×H image space)     */
    int      width, height;
} cap_camera_t;

/* ── one retained keyframe: pose + owned downscaled RGB-float image (W*H*3) ── */
typedef struct {
    cap_camera_t cam;
    float       *image;           /* owned copy, linear RGB in [0,1]             */
} cap_keyframe_t;

/* ── bounded keyframe ring + admission state ── */
typedef struct {
    cap_keyframe_t *kf;           /* ring buffer of keyframes                    */
    int   cap, count, head;       /* head = oldest slot                          */
    int   W, H;                   /* training resolution                         */
    int   has_last;               /* a keyframe has been admitted                */
    cap_vec3 last_pos, last_fwd;  /* pose of the most recent keyframe            */
    float t_thresh;               /* translation (world units) to trigger a KF   */
    float cos_thresh;             /* forward-axis dot below this triggers a KF   */
    long  frames_seen;            /* total frames offered (kept + discarded)     */
    long  keyframes_total;        /* total ever admitted (incl. evicted)         */

    /* Guard for the ring: the GL thread admits keyframes (captureObserve) while
     * the Phase D train thread samples them (trainer_step). All ring access
     * goes through this lock. */
    pthread_mutex_t c_lock;
} qtsq_capture_t;

/* Create/destroy. kf_cap = window size, W×H = training resolution. */
qtsq_capture_t *qtsq_capture_create(int W, int H, int kf_cap,
                                    float t_thresh, float cos_thresh);
void            qtsq_capture_destroy(qtsq_capture_t *c);

/* Build a cap_camera_t from an ARCore pose. 'pose_colmajor' is the 4×4
 * camera→world matrix as returned by Pose.toMatrix() (column-major, OpenGL
 * convention: +X right, +Y up, −Z view direction). fx,fy,cx,cy must already be
 * scaled into the W×H image space this capture uses. */
void qtsq_capture_camera_from_arpose(cap_camera_t *out,
                                     const float pose_colmajor[16],
                                     float fx, float fy, float cx, float cy,
                                     int W, int H);

/* Offer one posed frame. 'image' is W*H*3 linear RGB, caller-owned; it is copied
 * only if admitted. Returns 1 if admitted as a keyframe, 0 if discarded. */
int qtsq_capture_observe(qtsq_capture_t *c, const cap_camera_t *cam,
                         const float *image);

/* Current live keyframe count (window occupancy). Thread-safe. */
int qtsq_capture_keyframe_count(const qtsq_capture_t *c);

/* Sample the keyframe at ring offset r (0 ≤ r < live count) under the ring
 * lock. Copies pose into *cam and, if out_img is non-NULL, the W*H*3 float
 * image. The slot is read inside the lock so a concurrent eviction can never
 * tear the pose/image. Returns 1 on success, 0 if r is out of range. */
int qtsq_capture_sample(const qtsq_capture_t *c, int r,
                        cap_camera_t *cam, float *out_img);

#endif /* QTSQ_CAPTURE_H */
