/*
 * QTSQ Phase D — GPU-First Vulkan Surfel Trainer
 *
 * Owns 4 Vulkan compute pipelines (forward, loss, backward, Adam),
 * all SSBOs, and the surfel model. Attaches to qtsq_capture_t to read
 * keyframes directly from the capture ring.
 *
 * Author: Haruhito
 */

#ifndef QTSQ_TRAINER_H
#define QTSQ_TRAINER_H

#include "qtsq_capture.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Surfel data layout (matches prototype/live_surfel/surfel.h) ── */
#define SURFEL_STRIDE_FLOATS  13   /* center[3], quat[4], log_su, log_sv, opacity_logit, color[3] */
#define GRAD_STRIDE_FLOATS    12   /* d_center[3], d_log_su, d_log_sv, d_opacity_logit, d_color[3], d_rot[3] */

/* ── Training stats returned to the UI ── */
typedef struct {
    int    step;
    int    surfel_count;
    float  loss;
    float  psnr;
    int    keyframe_count;
    float  coverage_pct;      /* % training pixels with alpha>0.5 (latest frame) */
    float  angle_spread_deg;  /* max pairwise keyframe view-angle spread */
    float  thermal_sleep_ms;  /* current adaptive inter-step sleep */
} trainer_stats_t;

/* ── Trainer context (opaque) ── */
typedef struct qtsq_trainer qtsq_trainer_t;

/*
 * Create a trainer attached to an existing capture session.
 * Uses the capture's W×H and kf_cap to size buffers.
 * Returns NULL on failure.
 */
qtsq_trainer_t *trainer_create(qtsq_capture_t *cap);

/*
 * Seed surfels from ARCore sparse point cloud.
 * Each point becomes one surfel: center=point, camera-facing normal,
 * small isotropic scale, opacity ~0.5, color from current keyframe.
 */
void trainer_seed_from_points(qtsq_trainer_t *t,
                              const float *points_xyz,   /* [n*3] world coords */
                              const float *points_rgb,   /* [n*3] color [0,1], or NULL */
                              int n);

/*
 * Seed with random surfels in a box around the camera (fallback).
 */
void trainer_seed_random(qtsq_trainer_t *t, int count,
                         const cap_camera_t *cam, float spread);

/*
 * Run one training iteration: pick a random keyframe, dispatch
 * forward -> loss -> backward -> adam on GPU.
 * Returns the loss for this iteration. Density control runs automatically
 * every DENSIFY_EVERY steps inside this function, and the GL-visible
 * surfel snapshot is refreshed on every call.
 */
float trainer_step(qtsq_trainer_t *t);

/*
 * CPU density control: prune transparent, clone high-gradient.
 * Runs every N steps on the train thread (also callable manually).
 */
void trainer_density_control(qtsq_trainer_t *t);

/*
 * Start the background train thread (one writer: it owns all trainer_step
 * calls). The GL thread reads snapshots via trainer_get_surfels / stats.
 * No-op if the loop is already running. Returns 0 on success.
 */
int trainer_start_loop(qtsq_trainer_t *t);

/*
 * Stop the background train thread and join it. Safe to call twice.
 */
void trainer_stop_loop(qtsq_trainer_t *t);

/*
 * On-device parity gate (Phase D validation §4): renders keyframe 0 with the
 * GPU forward pipeline and with the host shader-model reference, then reports
 * the max |difference| over all channels. Returns -1 if it cannot run;
 * otherwise fills *max_diff. GPU-vs-host must track ~1e-4.
 */
int trainer_parity_forward(qtsq_trainer_t *t, float *max_diff);

/*
 * Get current training stats for HUD display. Thread-safe.
 */
trainer_stats_t trainer_get_stats(const qtsq_trainer_t *t);

/*
 * Get the current surfel model as a float array for GL preview.
 * Returns a pointer to the thread-safe snapshot (valid until the next
 * snapshot update; do not retain across calls). out_count receives the
 * number of surfels. Thread-safe.
 */
const float *trainer_get_surfels(const qtsq_trainer_t *t, int *out_count);

/*
 * Snapshot the current surfels into a caller-owned buffer.
 * Returns the number of surfels copied (0 on error / empty).
 * Thread-safe — the train thread is never mid-write when this returns.
 */
int trainer_snapshot_surfels(const qtsq_trainer_t *t,
                             float *out_surfels, int max_count);

/*
 * Save the trained model to a .qtsq file via the fail-closed secure write.
 * Returns 0 on success, -1 on failure.
 */
int trainer_save(const qtsq_trainer_t *t, const char *path);

/*
 * Destroy the trainer and free all resources.
 */
void trainer_destroy(qtsq_trainer_t *t);

#ifdef __cplusplus
}
#endif

#endif /* QTSQ_TRAINER_H */
