/*
 * Quantum Tensor Sequence — Spaghettification Engine
 * Author: Haruhito
 */

#include "qtsq.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

extern double qtsq_entropy(const uint8_t *data, size_t len);

static qtsq_data_type_t detect_by_magic(const uint8_t *data, size_t size) {
    if (size < 4) return QTSQ_TYPE_RAW;

    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return QTSQ_TYPE_IMAGE_RGB;
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return QTSQ_TYPE_IMAGE_RGB;
    if (data[0] == 'B' && data[1] == 'M')
        return QTSQ_TYPE_IMAGE_RGB;
    if (size >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E')
        return QTSQ_TYPE_AUDIO_STEREO;
    if (data[0] == '{' || data[0] == '[')
        return QTSQ_TYPE_JSON;

    return QTSQ_TYPE_RAW;
}

static int is_ascii_printable(uint8_t c) {
    return (c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t';
}

static qtsq_data_type_t detect_text(const uint8_t *data, size_t size) {
    size_t check = size > 4096 ? 4096 : size;
    size_t printable = 0, utf8_multi = 0, comma_count = 0;
    size_t newline_count = 0, brace_count = 0;

    for (size_t i = 0; i < check; i++) {
        if (is_ascii_printable(data[i])) printable++;
        if (data[i] > 127) utf8_multi++;
        if (data[i] == ',') comma_count++;
        if (data[i] == '\n') newline_count++;
        if (data[i] == '{' || data[i] == '}') brace_count++;
    }

    double text_ratio = (double)printable / (double)check;
    if (text_ratio < 0.80) return QTSQ_TYPE_RAW;

    if (newline_count > 2 && comma_count > newline_count)
        return QTSQ_TYPE_CSV;
    if (brace_count >= 2 && comma_count > 0)
        return QTSQ_TYPE_JSON;
    if (utf8_multi > check / 20)
        return QTSQ_TYPE_TEXT_UTF8;

    return QTSQ_TYPE_TEXT_ASCII;
}

static int detect_signal_pattern(const uint8_t *data, size_t size) {
    if (size < 64) return 0;
    size_t smooth_count = 0;
    size_t limit = size > 1024 ? 1024 : size;
    for (size_t i = 1; i < limit; i++) {
        int delta = abs((int)data[i] - (int)data[i-1]);
        if (delta < 16) smooth_count++;
    }
    return (smooth_count > (limit - 1) * 7 / 10);
}

qtsq_data_type_t qtsq_detect_type(const uint8_t *data, size_t size) {
    if (!data || size == 0) return QTSQ_TYPE_RAW;

    qtsq_data_type_t magic = detect_by_magic(data, size);
    if (magic != QTSQ_TYPE_RAW) return magic;

    qtsq_data_type_t text = detect_text(data, size);
    if (text != QTSQ_TYPE_RAW) return text;

    double ent = qtsq_entropy(data, size > 8192 ? 8192 : size);

    if (ent < 3.0) {
        if (detect_signal_pattern(data, size)) return QTSQ_TYPE_SIGNAL_1D;
        return QTSQ_TYPE_TABLE_INT;
    }

    if (ent < 6.0 && detect_signal_pattern(data, size))
        return QTSQ_TYPE_IMAGE_RGB;

    return QTSQ_TYPE_RAW;
}

qtsq_strategy_t qtsq_optimal_strategy(qtsq_data_type_t type) {
    switch (type) {
        case QTSQ_TYPE_IMAGE_GRAY:
        case QTSQ_TYPE_IMAGE_RGB:
        case QTSQ_TYPE_IMAGE_RGBA:    return QTSQ_STRATEGY_FRACTAL;
        case QTSQ_TYPE_AUDIO_MONO:
        case QTSQ_TYPE_AUDIO_STEREO:
        case QTSQ_TYPE_AUDIO_MULTI:
        case QTSQ_TYPE_SIGNAL_1D:
        case QTSQ_TYPE_SIGNAL_2D:     return QTSQ_STRATEGY_FOURIER;
        case QTSQ_TYPE_VIDEO_FRAMES:  return QTSQ_STRATEGY_TEMPORAL;
        case QTSQ_TYPE_TEXT_ASCII:
        case QTSQ_TYPE_TEXT_UTF8:
        case QTSQ_TYPE_JSON:
        case QTSQ_TYPE_CSV:           return QTSQ_STRATEGY_DICTIONARY;
        case QTSQ_TYPE_TABLE_INT:
        case QTSQ_TYPE_TABLE_FLOAT:   return QTSQ_STRATEGY_DELTA_SEED;
        case QTSQ_TYPE_TENSOR:        return QTSQ_STRATEGY_TENSOR;
        case QTSQ_TYPE_CONTAINER:     return QTSQ_STRATEGY_LAYERED;
        default:                     return QTSQ_STRATEGY_DELTA_SEED;
    }
}

const char* qtsq_type_name(qtsq_data_type_t type) {
    switch (type) {
        case QTSQ_TYPE_RAW:          return "Raw Binary";
        case QTSQ_TYPE_IMAGE_GRAY:   return "Image (Grayscale)";
        case QTSQ_TYPE_IMAGE_RGB:    return "Image (RGB)";
        case QTSQ_TYPE_IMAGE_RGBA:   return "Image (RGBA)";
        case QTSQ_TYPE_AUDIO_MONO:   return "Audio (Mono)";
        case QTSQ_TYPE_AUDIO_STEREO: return "Audio (Stereo)";
        case QTSQ_TYPE_AUDIO_MULTI:  return "Audio (Multi-ch)";
        case QTSQ_TYPE_VIDEO_FRAMES: return "Video (Frames)";
        case QTSQ_TYPE_TEXT_ASCII:    return "Text (ASCII)";
        case QTSQ_TYPE_TEXT_UTF8:     return "Text (UTF-8)";
        case QTSQ_TYPE_TABLE_INT:    return "Table (Integer)";
        case QTSQ_TYPE_TABLE_FLOAT:  return "Table (Float)";
        case QTSQ_TYPE_JSON:         return "JSON";
        case QTSQ_TYPE_CSV:          return "CSV";
        case QTSQ_TYPE_SIGNAL_1D:    return "Signal (1D)";
        case QTSQ_TYPE_SIGNAL_2D:    return "Signal (2D)";
        case QTSQ_TYPE_TENSOR:       return "Tensor (Neural)";
        case QTSQ_TYPE_ATOM:         return "Atom (4D Tsubu)";
        case QTSQ_TYPE_4DGS:         return "Gaussian (4DGS)";
        case QTSQ_TYPE_CONTAINER:    return "Container";
        default:                    return "Unknown";
    }
}

const char* qtsq_strategy_name(qtsq_strategy_t strategy) {
    switch (strategy) {
        case QTSQ_STRATEGY_NONE:       return "None (White Dwarf)";
        case QTSQ_STRATEGY_PROCEDURAL: return "Procedural (Singularity)";
        case QTSQ_STRATEGY_FRACTAL:    return "Fractal (Accretion)";
        case QTSQ_STRATEGY_FOURIER:    return "Fourier (Lensing)";
        case QTSQ_STRATEGY_DELTA_SEED: return "Delta-Seed (Hybrid)";
        case QTSQ_STRATEGY_DICTIONARY: return "Dictionary (Horizon)";
        case QTSQ_STRATEGY_TEMPORAL:   return "Temporal (Frame Delta)";
        case QTSQ_STRATEGY_LAYERED:    return "Layered (Multi-Strategy)";
        case QTSQ_STRATEGY_TRIANGLE:   return "Triangle (RGB Mesh)";
        case QTSQ_STRATEGY_PHOTON:     return "Photon Audio HQ";
        case QTSQ_STRATEGY_SPLAT:      return "Pure Gaussian Splat";
        case QTSQ_STRATEGY_TENSOR:     return "Tensor Delta";
        case QTSQ_STRATEGY_PIXEL:      return "Pixel";
        case QTSQ_STRATEGY_TSUBU:      return "Tsubu (4D Atom)";
        case QTSQ_STRATEGY_4DGS:       return "4DGS (Gaussian Splatting)";
        default:                      return "Unknown";
    }
}

int qtsq_type_is_image(qtsq_data_type_t type) {
    return (type >= 0x10 && type <= 0x12);
}

int qtsq_type_is_audio(qtsq_data_type_t type) {
    return (type >= 0x20 && type <= 0x22);
}

int qtsq_type_is_text(qtsq_data_type_t type) {
    return (type >= 0x40 && type <= 0x41) ||
           type == QTSQ_TYPE_JSON || type == QTSQ_TYPE_CSV;
}
