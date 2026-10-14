/*
 * Quantum Tensor Sequence — Photon Sensor Fusion (Kalman Filter)
 *
 * Multi-sensor fusion engine that combines measurements from:
 *   - Ultrasonic sonar (range)
 *   - Acoustic TDOA (angle)
 *   - WiFi RSSI (coarse zone)
 *   - BLE/UWB (range/angle)
 *
 * Each tracked source gets its own Kalman filter with state:
 *   [x, y, z, vx, vy, vz]
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "qtsq_photon.h"

/* ═══════════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════════ */

#define DIM  PHOTON_KALMAN_STATE_DIM   /* 6: x,y,z,vx,vy,vz */
#define MAX_ASSOC_DIST  3.0f           /* Max distance for measurement association (meters) */
#define RANGE_GATE      1.0f           /* Range-only match tolerance: |‖track‖ − r| (meters) */
#define TRACK_LOST_THRESHOLD  30       /* Frames before a lost track is removed */
#define PROCESS_NOISE_POS     0.01f    /* Position process noise (m²) */
#define PROCESS_NOISE_VEL     0.1f     /* Velocity process noise ((m/s)²) */

static uint64_t fusion_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: 6x6 Matrix Helpers (row-major)
 * ═══════════════════════════════════════════════════════════════ */

static void mat_identity(float *M, int n) {
    memset(M, 0, n * n * sizeof(float));
    for (int i = 0; i < n; i++) M[i * n + i] = 1.0f;
}

static void mat_copy(float *dst, const float *src, int n) {
    memcpy(dst, src, n * n * sizeof(float));
}

/* C = A + B (element-wise, n×n) */
static void mat_add(float *C, const float *A, const float *B, int n) {
    for (int i = 0; i < n * n; i++) C[i] = A[i] + B[i];
}

/* C = A * B (n×n) */
static void mat_mul(float *C, const float *A, const float *B, int n) {
    float tmp[DIM * DIM];
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            float sum = 0.0f;
            for (int k = 0; k < n; k++) {
                sum += A[i * n + k] * B[k * n + j];
            }
            tmp[i * n + j] = sum;
        }
    }
    memcpy(C, tmp, n * n * sizeof(float));
}

/* B = transpose(A) (n×n) */
static void mat_transpose(float *B, const float *A, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            B[j * n + i] = A[i * n + j];
}

/* Invert a 3x3 submatrix (for the measurement update).
 * We use position-only measurements (3D), so the innovation covariance is 3x3. */
static int mat_invert_3x3(float *inv, const float *M) {
    float a = M[0], b = M[1], c = M[2];
    float d = M[3], e = M[4], f = M[5];
    float g = M[6], h = M[7], k = M[8];

    float det = a*(e*k - f*h) - b*(d*k - f*g) + c*(d*h - e*g);
    if (fabsf(det) < 1e-12f) return -1;

    float inv_det = 1.0f / det;
    inv[0] = (e*k - f*h) * inv_det;
    inv[1] = (c*h - b*k) * inv_det;
    inv[2] = (b*f - c*e) * inv_det;
    inv[3] = (f*g - d*k) * inv_det;
    inv[4] = (a*k - c*g) * inv_det;
    inv[5] = (c*d - a*f) * inv_det;
    inv[6] = (d*h - e*g) * inv_det;
    inv[7] = (b*g - a*h) * inv_det;
    inv[8] = (a*e - b*d) * inv_det;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Fusion Init / Free
 * ═══════════════════════════════════════════════════════════════ */

int photon_fusion_init(photon_fusion_state_t *state, const photon_config_t *cfg) {
    if (!state || !cfg) return -1;
    memset(state, 0, sizeof(*state));
    state->config = *cfg;
    state->next_id = 1;

    /* Default sensor weights */
    state->w_sonar = 0.90f;
    state->w_tdoa  = 0.60f;
    state->w_wifi  = 0.20f;
    state->w_ble   = 0.30f;
    state->w_uwb   = 0.85f;

    return 0;
}

void photon_fusion_free(photon_fusion_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Kalman Predict Step
 *
 *  State transition (constant velocity model):
 *    x_{k+1} = F * x_k
 *    P_{k+1} = F * P_k * F^T + Q
 *
 *  F = [I₃  dt*I₃]    Q = diag(σ²_pos, σ²_vel)
 *      [0   I₃   ]
 * ═══════════════════════════════════════════════════════════════ */

static void kalman_predict(photon_kalman_track_t *track, float dt) {
    /* State prediction: x += v * dt */
    track->state[0] += track->state[3] * dt;  /* x += vx * dt */
    track->state[1] += track->state[4] * dt;  /* y += vy * dt */
    track->state[2] += track->state[5] * dt;  /* z += vz * dt */

    /* Covariance prediction: P = F * P * F^T + Q */
    float F[DIM * DIM];
    mat_identity(F, DIM);
    F[0 * DIM + 3] = dt;  /* x depends on vx */
    F[1 * DIM + 4] = dt;  /* y depends on vy */
    F[2 * DIM + 5] = dt;  /* z depends on vz */

    float FT[DIM * DIM];
    mat_transpose(FT, F, DIM);

    float FP[DIM * DIM];
    mat_mul(FP, F, track->covariance, DIM);

    float FPFT[DIM * DIM];
    mat_mul(FPFT, FP, FT, DIM);

    /* Process noise Q */
    float Q[DIM * DIM];
    memset(Q, 0, sizeof(Q));
    Q[0 * DIM + 0] = PROCESS_NOISE_POS * dt * dt;
    Q[1 * DIM + 1] = PROCESS_NOISE_POS * dt * dt;
    Q[2 * DIM + 2] = PROCESS_NOISE_POS * dt * dt;
    Q[3 * DIM + 3] = PROCESS_NOISE_VEL * dt;
    Q[4 * DIM + 4] = PROCESS_NOISE_VEL * dt;
    Q[5 * DIM + 5] = PROCESS_NOISE_VEL * dt;

    mat_add(track->covariance, FPFT, Q, DIM);
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Kalman Update Step (position-only measurement)
 *
 *  Measurement model: z = H * x + noise
 *  H = [I₃ 0₃] (we measure position, not velocity)
 *
 *  Innovation:    y = z - H * x
 *  Innovation cov: S = H * P * H^T + R
 *  Kalman gain:    K = P * H^T * S^{-1}
 *  State update:   x = x + K * y
 *  Cov update:     P = (I - K * H) * P
 * ═══════════════════════════════════════════════════════════════ */

static void kalman_update(photon_kalman_track_t *track,
                           float mx, float my, float mz,
                           float meas_noise) {
    /* Innovation: y = measurement - predicted position */
    float y[3] = {
        mx - track->state[0],
        my - track->state[1],
        mz - track->state[2]
    };

    /* Innovation covariance: S = P[0:3,0:3] + R */
    float S[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            S[i * 3 + j] = track->covariance[i * DIM + j];
        }
        S[i * 3 + i] += meas_noise;
    }

    /* S_inv = inverse(S) */
    float S_inv[9];
    if (mat_invert_3x3(S_inv, S) != 0) return;  /* Singular, skip update */

    /* Kalman gain: K = P * H^T * S^{-1}
     * H^T is just the first 3 columns of identity extended to DIM rows
     * So P * H^T = P[:, 0:3] */
    float K[DIM * 3];
    for (int i = 0; i < DIM; i++) {
        for (int j = 0; j < 3; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 3; k++) {
                sum += track->covariance[i * DIM + k] * S_inv[k * 3 + j];
            }
            K[i * 3 + j] = sum;
        }
    }

    /* State update: x = x + K * y */
    for (int i = 0; i < DIM; i++) {
        float correction = 0.0f;
        for (int j = 0; j < 3; j++) {
            correction += K[i * 3 + j] * y[j];
        }
        track->state[i] += correction;
    }

    /* Covariance update: P = (I - K * H) * P */
    float KH[DIM * DIM];
    memset(KH, 0, sizeof(KH));
    for (int i = 0; i < DIM; i++) {
        for (int j = 0; j < 3; j++) {  /* H only has cols 0-2 */
            KH[i * DIM + j] = K[i * 3 + j];
        }
    }

    float I_KH[DIM * DIM];
    mat_identity(I_KH, DIM);
    for (int i = 0; i < DIM * DIM; i++) I_KH[i] -= KH[i];

    float new_P[DIM * DIM];
    mat_mul(new_P, I_KH, track->covariance, DIM);
    mat_copy(track->covariance, new_P, DIM);
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Kalman Update Step — Range-Only Measurement (EKF)
 *
 *  Sonar measures distance from the host, not a position: z = ‖p‖.
 *  The measurement is nonlinear, so linearize H at the predicted
 *  position: H = [px/r, py/r, pz/r, 0, 0, 0] where r = ‖p‖.
 *  The resulting scalar update keeps the track's TDOA-derived
 *  direction and corrects only its radius — this is what actually
 *  fuses sonar range with TDOA angle into one geometric position.
 * ═══════════════════════════════════════════════════════════════ */

static void kalman_update_range(photon_kalman_track_t *track,
                                float range_meas, float meas_noise) {
    float px = track->state[0], py = track->state[1], pz = track->state[2];
    float r = sqrtf(px * px + py * py + pz * pz);
    if (r < 1e-3f) return;  /* Direction undefined at the origin */

    float h[3] = { px / r, py / r, pz / r };

    /* Innovation covariance (scalar): S = H P Hᵀ + R */
    float S = meas_noise;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            S += h[i] * track->covariance[i * DIM + j] * h[j];
        }
    }
    if (S < 1e-12f) return;
    float inv_S = 1.0f / S;

    /* Kalman gain: K = P Hᵀ / S  (6×1) */
    float K[DIM];
    for (int i = 0; i < DIM; i++) {
        float ph = 0.0f;
        for (int j = 0; j < 3; j++) {
            ph += track->covariance[i * DIM + j] * h[j];
        }
        K[i] = ph * inv_S;
    }

    /* State update: x = x + K (z − ‖p‖) */
    float y = range_meas - r;
    for (int i = 0; i < DIM; i++) {
        track->state[i] += K[i] * y;
    }

    /* Covariance update: P = (I − K H) P */
    float KH[DIM * DIM];
    memset(KH, 0, sizeof(KH));
    for (int i = 0; i < DIM; i++) {
        for (int j = 0; j < 3; j++) {
            KH[i * DIM + j] = K[i] * h[j];
        }
    }
    float I_KH[DIM * DIM];
    mat_identity(I_KH, DIM);
    for (int i = 0; i < DIM * DIM; i++) I_KH[i] -= KH[i];

    float new_P[DIM * DIM];
    mat_mul(new_P, I_KH, track->covariance, DIM);
    mat_copy(track->covariance, new_P, DIM);
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Association for Range-Only Measurements
 *
 *  A range measurement matches a track when the track's radius is
 *  within RANGE_GATE of the measurement — direction is irrelevant.
 *  Among candidates, prefer the smallest range error. A deliberate
 *  gate miss (no candidate) creates a NEW track instead of yanking
 *  an unrelated track's radius.
 * ═══════════════════════════════════════════════════════════════ */

static int find_track_by_range(const photon_fusion_state_t *state,
                               float range_meas) {
    int best_idx = -1;
    float best_err = RANGE_GATE;

    for (uint32_t i = 0; i < state->num_tracks; i++) {
        if (!state->tracks[i].active) continue;

        float dx = state->tracks[i].state[0];
        float dy = state->tracks[i].state[1];
        float dz = state->tracks[i].state[2];
        float r = sqrtf(dx * dx + dy * dy + dz * dz);
        float err = fabsf(r - range_meas);

        if (err < best_err) {
            best_err = err;
            best_idx = (int)i;
        }
    }

    return best_idx;
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Associate measurement to closest track
 *
 *  Returns track index, or -1 if no match within MAX_ASSOC_DIST.
 * ═══════════════════════════════════════════════════════════════ */

static int find_nearest_track(const photon_fusion_state_t *state,
                               float mx, float my, float mz) {
    float best_dist = MAX_ASSOC_DIST;
    int best_idx = -1;

    for (uint32_t i = 0; i < state->num_tracks; i++) {
        if (!state->tracks[i].active) continue;

        float dx = state->tracks[i].state[0] - mx;
        float dy = state->tracks[i].state[1] - my;
        float dz = state->tracks[i].state[2] - mz;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = (int)i;
        }
    }

    return best_idx;
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Create new track
 * ═══════════════════════════════════════════════════════════════ */

static photon_kalman_track_t* create_track(photon_fusion_state_t *state,
                                             float mx, float my, float mz) {
    /* Find an empty slot */
    int slot = -1;
    for (uint32_t i = 0; i < PHOTON_MAX_SOURCES; i++) {
        if (!state->tracks[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) return NULL;  /* No room */

    photon_kalman_track_t *t = &state->tracks[slot];
    memset(t, 0, sizeof(*t));

    t->state[0] = mx;  t->state[1] = my;  t->state[2] = mz;
    t->state[3] = 0;   t->state[4] = 0;   t->state[5] = 0;

    /* Initial covariance: large uncertainty */
    mat_identity(t->covariance, DIM);
    for (int i = 0; i < DIM; i++) t->covariance[i * DIM + i] = 5.0f;

    t->source_id = state->next_id++;
    t->active = 1;
    t->frames_seen = 1;
    t->frames_lost = 0;
    t->confidence = 0.5f;

    if (state->num_tracks <= (uint32_t)slot) {
        state->num_tracks = (uint32_t)slot + 1;
    }

    return t;
}

/* ═══════════════════════════════════════════════════════════════
 *  Fusion Update
 *
 *  1. Predict all active tracks (constant velocity)
 *  2. Associate sonar measurements → update with high weight
 *  3. Associate TDOA measurements → update with medium weight
 *  4. WiFi zone activity → shift unmatched sources gently
 *  5. Prune lost tracks
 * ═══════════════════════════════════════════════════════════════ */

void photon_fusion_update(photon_fusion_state_t *state,
                           const photon_source_t *sonar_sources, uint32_t num_sonar,
                           const photon_source_t *tdoa_sources, uint32_t num_tdoa,
                           const float *wifi_zones, uint32_t num_zones,
                           const photon_rf_observation_t *ble_obs, uint32_t num_ble) {
    if (!state) return;

    float dt = 0.1f;  /* Assume ~10 Hz update rate */
    uint64_t now_us = fusion_now_us();

    /* ── Step 1: Predict all active tracks ── */
    for (uint32_t i = 0; i < state->num_tracks; i++) {
        if (state->tracks[i].active) {
            kalman_predict(&state->tracks[i], dt);
            state->tracks[i].frames_lost++;
        }
    }

    /* ── Step 2: Incorporate sonar measurements (range-only, highest weight) ──
     *
     * Sonar returns distance-to-reflector, not a position. Update each
     * matched track's radius via the EKF range update so the TDOA-derived
     * direction survives; a gate miss spawns a fresh track at (r, 0, 0). */
    float sonar_noise = 1.0f / (state->w_sonar * state->w_sonar + 1e-6f);  /* Lower noise = higher trust */
    for (uint32_t s = 0; s < num_sonar; s++) {
        float r_meas = sqrtf(sonar_sources[s].x * sonar_sources[s].x +
                             sonar_sources[s].y * sonar_sources[s].y +
                             sonar_sources[s].z * sonar_sources[s].z);
        int idx = find_track_by_range(state, r_meas);
        if (idx >= 0) {
            kalman_update_range(&state->tracks[idx], r_meas, sonar_noise);
            state->tracks[idx].frames_lost = 0;
            state->tracks[idx].frames_seen++;
            state->tracks[idx].last_update_us = now_us;
            /* Running confidence: weighted average */
            float α = 0.3f;
            state->tracks[idx].confidence = (1.0f - α) * state->tracks[idx].confidence + α * sonar_sources[s].confidence;
        } else {
            /* New track */
            photon_kalman_track_t *t = create_track(state, r_meas, 0.0f, 0.0f);
            if (t) {
                t->freq_lo = sonar_sources[s].freq_lo;
                t->freq_hi = sonar_sources[s].freq_hi;
                t->last_update_us = now_us;
            }
        }
    }

    /* ── Step 3: Incorporate TDOA measurements (angle @ assumed distance, medium weight) ── */
    float tdoa_noise = 1.0f / (state->w_tdoa * state->w_tdoa + 1e-6f);
    for (uint32_t s = 0; s < num_tdoa; s++) {
        int idx = find_nearest_track(state, tdoa_sources[s].x, tdoa_sources[s].y, tdoa_sources[s].z);
        if (idx >= 0) {
            kalman_update(&state->tracks[idx],
                          tdoa_sources[s].x, tdoa_sources[s].y, tdoa_sources[s].z,
                          tdoa_noise);
            state->tracks[idx].frames_lost = 0;
            state->tracks[idx].frames_seen++;
            state->tracks[idx].last_update_us = now_us;
            state->tracks[idx].freq_lo = tdoa_sources[s].freq_lo;
            state->tracks[idx].freq_hi = tdoa_sources[s].freq_hi;
        } else {
            /* New track from TDOA */
            photon_kalman_track_t *t = create_track(state, tdoa_sources[s].x, tdoa_sources[s].y, tdoa_sources[s].z);
            if (t) {
                t->freq_lo = tdoa_sources[s].freq_lo;
                t->freq_hi = tdoa_sources[s].freq_hi;
                t->confidence = 0.3f;  /* TDOA-only tracks start lower */
                t->last_update_us = now_us;
            }
        }
    }

    /* ── Step 4: BLE/UWB observations (if any) ── */
    if (ble_obs) {
        for (uint32_t i = 0; i < num_ble; i++) {
            if (ble_obs[i].type == PHOTON_RF_UWB && ble_obs[i].uwb_distance_m > 0.0f) {
                /* UWB gives direct distance + angle → position */
                float angle_rad = ble_obs[i].uwb_azimuth_deg * (float)M_PI / 180.0f;
                float mx = ble_obs[i].uwb_distance_m * sinf(angle_rad);
                float my = ble_obs[i].uwb_distance_m * cosf(angle_rad);

                float uwb_noise = 1.0f / (state->w_uwb * state->w_uwb + 1e-6f);
                int idx = find_nearest_track(state, mx, my, 0.0f);
                if (idx >= 0) {
                    kalman_update(&state->tracks[idx], mx, my, 0.0f, uwb_noise);
                    state->tracks[idx].frames_lost = 0;
                }
            }
        }
    }

    /* ── Step 5: WiFi zone validation (lowest weight) ──
     *
     * Zone activity = RSSI variance in a proximity band: high activity
     * means something is moving at that distance. Map each acoustic
     * track to its distance band; agreement boosts confidence slowly,
     * silence around a stale track decays it faster. */
    if (wifi_zones && num_zones > 0) {
        for (uint32_t i = 0; i < state->num_tracks; i++) {
            photon_kalman_track_t *tr = &state->tracks[i];
            if (!tr->active) continue;

            float d = sqrtf(tr->state[0] * tr->state[0] +
                            tr->state[1] * tr->state[1] +
                            tr->state[2] * tr->state[2]);
            uint32_t zone = 4;  /* far */
            if      (d < 1.0f) zone = 0;
            else if (d < 2.5f) zone = 1;
            else if (d < 4.0f) zone = 2;
            else if (d < 6.0f) zone = 3;

            if (zone >= num_zones) continue;
            float activity = wifi_zones[zone];

            if (activity > 0.5f && tr->frames_lost == 0) {
                /* RF motion agrees with a freshly-seen acoustic track */
                tr->confidence = fminf(1.0f, tr->confidence + 0.02f);
            } else if (activity < 0.05f && tr->frames_lost > 8) {
                /* Nothing moving in the band where this track claims to be */
                tr->confidence *= 0.9f;
            }
        }
    }

    /* ── Step 6: Prune lost tracks ── */
    for (uint32_t i = 0; i < state->num_tracks; i++) {
        if (state->tracks[i].active && state->tracks[i].frames_lost > TRACK_LOST_THRESHOLD) {
            state->tracks[i].active = 0;
        }
    }

    /* Update confidence based on time visibility */
    for (uint32_t i = 0; i < state->num_tracks; i++) {
        if (state->tracks[i].active) {
            /* Confidence decays when not seen */
            float decay = 0.95f;
            if (state->tracks[i].frames_lost > 0) {
                state->tracks[i].confidence *= decay;
            }
            if (state->tracks[i].confidence < 0.05f) {
                state->tracks[i].confidence = 0.05f;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Get Fused Sources
 * ═══════════════════════════════════════════════════════════════ */

uint32_t photon_fusion_get_sources(const photon_fusion_state_t *state,
                                    photon_source_t *out, uint32_t max_sources) {
    if (!state || !out) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < state->num_tracks && count < max_sources; i++) {
        if (!state->tracks[i].active) continue;

        out[count].x = state->tracks[i].state[0];
        out[count].y = state->tracks[i].state[1];
        out[count].z = state->tracks[i].state[2];
        out[count].confidence = state->tracks[i].confidence;
        out[count].freq_lo = state->tracks[i].freq_lo;
        out[count].freq_hi = state->tracks[i].freq_hi;
        out[count].source_id = state->tracks[i].source_id;
        out[count].last_seen_us = state->tracks[i].last_update_us;
        count++;
    }

    return count;
}
