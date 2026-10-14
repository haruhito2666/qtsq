/*
 * Quantum Tensor Sequence — Photon WiFi/BLE RSSI Engine
 *
 * Tracks WiFi access points and BLE devices via RSSI fingerprinting.
 * Detects movement zones based on RSSI variance over time.
 *
 * This is a passive sensor: it receives observations from the platform
 * layer (CoreWLAN on macOS, WifiManager on Android) and computes
 * coarse spatial zones.
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "qtsq_photon.h"

/* ═══════════════════════════════════════════════════════════════
 *  WiFi Init / Free
 * ═══════════════════════════════════════════════════════════════ */

int photon_wifi_init(photon_wifi_state_t *state) {
    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    state->num_zones = PHOTON_MAX_ZONES;
    return 0;
}

void photon_wifi_free(photon_wifi_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

/* ═══════════════════════════════════════════════════════════════
 *  Internal: Find or Create AP Entry
 * ═══════════════════════════════════════════════════════════════ */

static photon_wifi_ap_t* find_or_create_ap(photon_wifi_state_t *state, const char *id) {
    /* Search existing */
    for (uint32_t i = 0; i < state->num_aps; i++) {
        if (strncmp(state->aps[i].id, id, 63) == 0) {
            return &state->aps[i];
        }
    }

    /* Create new if space available */
    if (state->num_aps < PHOTON_WIFI_MAX_APS) {
        photon_wifi_ap_t *ap = &state->aps[state->num_aps];
        memset(ap, 0, sizeof(*ap));
        strncpy(ap->id, id, 63);
        ap->id[63] = '\0';
        state->num_aps++;
        return ap;
    }

    return NULL;  /* Table full */
}

/* ═══════════════════════════════════════════════════════════════
 *  Update: Feed new RF observations
 *
 *  For each observation:
 *    1. Find/create the AP entry
 *    2. Push RSSI into circular history
 *    3. Recompute rolling average and variance
 * ═══════════════════════════════════════════════════════════════ */

void photon_wifi_update(photon_wifi_state_t *state,
                         const photon_rf_observation_t *obs, uint32_t num_obs) {
    if (!state || !obs) return;

    for (uint32_t i = 0; i < num_obs; i++) {
        photon_wifi_ap_t *ap = find_or_create_ap(state, obs[i].identifier);
        if (!ap) continue;

        /* Push RSSI into circular history */
        uint32_t slot = ap->history_count % PHOTON_WIFI_HISTORY;
        ap->rssi_history[slot] = obs[i].rssi_dbm;
        if (ap->history_count < PHOTON_WIFI_HISTORY) {
            ap->history_count++;
        } else {
            ap->history_count++;  /* Keep incrementing for modular indexing */
        }
        ap->last_seen_us = obs[i].timestamp_us;

        /* Recompute rolling average and variance */
        uint32_t n = ap->history_count < PHOTON_WIFI_HISTORY ? ap->history_count : PHOTON_WIFI_HISTORY;
        float sum = 0.0f;
        for (uint32_t j = 0; j < n; j++) {
            sum += ap->rssi_history[j];
        }
        ap->rssi_avg = sum / (float)n;

        float var_sum = 0.0f;
        for (uint32_t j = 0; j < n; j++) {
            float diff = ap->rssi_history[j] - ap->rssi_avg;
            var_sum += diff * diff;
        }
        ap->rssi_variance = var_sum / (float)n;
    }

    /* ── Recompute zone activity ──
     *
     * Zone model: divide APs into groups by RSSI strength.
     * High variance in a zone = movement detected there.
     *
     * Zone mapping (by average RSSI):
     *   Zone 0: very strong  (> -40 dBm)  = very close
     *   Zone 1: strong       (-40 to -55) = nearby
     *   Zone 2: medium       (-55 to -65) = mid-range
     *   Zone 3: weak         (-65 to -75) = far
     *   Zone 4: very weak    (-75 to -85) = distant
     *   Zone 5-7: reserved
     */
    for (uint32_t z = 0; z < state->num_zones; z++) {
        state->zone_activity[z] = 0.0f;
    }

    for (uint32_t i = 0; i < state->num_aps; i++) {
        photon_wifi_ap_t *ap = &state->aps[i];
        if (ap->history_count < 2) continue;

        /* Determine zone by RSSI */
        uint32_t zone = 4;  /* Default: distant */
        if      (ap->rssi_avg > -40.0f) zone = 0;
        else if (ap->rssi_avg > -55.0f) zone = 1;
        else if (ap->rssi_avg > -65.0f) zone = 2;
        else if (ap->rssi_avg > -75.0f) zone = 3;
        else if (ap->rssi_avg > -85.0f) zone = 4;

        if (zone < state->num_zones) {
            /* Activity = sum of variances in the zone, normalized */
            state->zone_activity[zone] += ap->rssi_variance;
        }
    }

    /* Normalize zone activities to 0.0 – 1.0 */
    float max_activity = 0.0f;
    for (uint32_t z = 0; z < state->num_zones; z++) {
        if (state->zone_activity[z] > max_activity) {
            max_activity = state->zone_activity[z];
        }
    }
    if (max_activity > 0.01f) {
        for (uint32_t z = 0; z < state->num_zones; z++) {
            state->zone_activity[z] /= max_activity;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Get Zone Activity
 * ═══════════════════════════════════════════════════════════════ */

void photon_wifi_get_zones(const photon_wifi_state_t *state,
                            float *zone_activity, uint32_t *num_zones) {
    if (!state || !zone_activity || !num_zones) return;

    uint32_t n = state->num_zones;
    if (n > *num_zones) n = *num_zones;

    for (uint32_t i = 0; i < n; i++) {
        zone_activity[i] = state->zone_activity[i];
    }
    *num_zones = n;
}
