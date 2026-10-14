#include <metal_stdlib>
using namespace metal;

struct TriVertex {
    float x;
    float y;
    uchar r;
    uchar g;
    uchar b;
};

struct TriFace {
    uint v0;
    uint v1;
    uint v2;
};

/* Must exactly match C splat2d_t (12 bytes packed) */
struct Splat2D {
    ushort x;      /* center X (pixels) */
    ushort y;      /* center Y (pixels) */
    uchar  sx;     /* sigma X spread */
    uchar  sy;     /* sigma Y spread */
    uchar  theta;  /* rotation */
    uchar  r;      /* color R */
    uchar  g;      /* color G */
    uchar  b;      /* color B */
    uchar  alpha;  /* opacity */
    uchar  z;      /* depth + shape bits */
};

/* ══════════════════════════════════════════════════════════════
 *  Kernel 1: Barycentric Triangle Rasterization
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void rasterize_triangles(
    device const TriVertex* vertices [[buffer(0)]],
    device const TriFace* faces      [[buffer(1)]],
    device uchar* output             [[buffer(2)]],
    device uchar* blur_map           [[buffer(3)]],
    constant uint& width             [[buffer(4)]],
    constant uint& height            [[buffer(5)]],
    constant uint& channels          [[buffer(6)]],
    constant uint& num_faces         [[buffer(7)]],
    device const uchar* tier_mask    [[buffer(8)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    /* tier_mask is used for blur_map only, NOT to skip rasterization */
    
    float px = (float)gid.x + 0.5f;
    float py = (float)gid.y + 0.5f;
    if (gid.x == 0) px = 0.0f;
    else if (gid.x == width - 1) px = (float)gid.x;
    if (gid.y == 0) py = 0.0f;
    else if (gid.y == height - 1) py = (float)gid.y;
    
    // Reverse iterating so frontmost faces draw last
    for (int i = num_faces - 1; i >= 0; i--) {
        TriFace f = faces[i];
        TriVertex v0 = vertices[f.v0];
        TriVertex v1 = vertices[f.v1];
        TriVertex v2 = vertices[f.v2];
        
        // Fast bbox reject
        float minx = min(v0.x, min(v1.x, v2.x));
        float maxx = max(v0.x, max(v1.x, v2.x));
        float miny = min(v0.y, min(v1.y, v2.y));
        float maxy = max(v0.y, max(v1.y, v2.y));
        
        if (px < minx || px > maxx || py < miny || py > maxy) continue;
        
        float d = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y);
        if (abs(d) < 1e-6f) continue;
        
        float inv_d = 1.0f / d;
        float u = ((v1.y - v2.y) * (px - v2.x) + (v2.x - v1.x) * (py - v2.y)) * inv_d;
        float v = ((v2.y - v0.y) * (px - v2.x) + (v0.x - v2.x) * (py - v2.y)) * inv_d;
        float w = 1.0f - u - v;
        
        if (u >= -0.001f && v >= -0.001f && w >= -0.001f) {
            u = max(0.0f, u);
            v = max(0.0f, v);
            w = max(0.0f, w);
            float sum = u + v + w;
            if (sum > 0) { u /= sum; v /= sum; w /= sum; }
            
            float r = u * v0.r + v * v1.r + w * v2.r;
            float g = u * v0.g + v * v1.g + w * v2.g;
            float b = u * v0.b + v * v1.b + w * v2.b;
            
            uint idx = (gid.y * width + gid.x) * channels;
            output[idx] = clamp((uchar)(r + 0.5f), (uchar)0, (uchar)255);
            if (channels >= 2) output[idx+1] = clamp((uchar)(g + 0.5f), (uchar)0, (uchar)255);
            if (channels >= 3) output[idx+2] = clamp((uchar)(b + 0.5f), (uchar)0, (uchar)255);
            
            // Area-based blur heuristic
            float area = 0.5f * abs(v0.x * (v1.y - v2.y) + v1.x * (v2.y - v0.y) + v2.x * (v0.y - v1.y));
            uchar passes = 0;
            if (area > 200.0f) passes = 2;
            else if (area > 50.0f) passes = 1;
            blur_map[gid.y * width + gid.x] = passes;
            return; // Found overlapping face, break
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 2: Edge-Preserving Blur
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void box_blur_3x3(
    device const uchar* input        [[buffer(0)]],
    device uchar* output             [[buffer(1)]],
    device const uchar* blur_map     [[buffer(2)]],
    constant uint& width             [[buffer(3)]],
    constant uint& height            [[buffer(4)]],
    constant uint& channels          [[buffer(5)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint idx = (gid.y * width + gid.x) * channels;
    uchar passes = blur_map[gid.y * width + gid.x];
    
    if (passes == 0) {
        output[idx] = input[idx];
        if (channels >= 2) output[idx+1] = input[idx+1];
        if (channels >= 3) output[idx+2] = input[idx+2];
        return;
    }
    
    int R = 1;
    int sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;
    
    int min_dy = gid.y < R ? -gid.y : -R;
    int max_dy = (gid.y + R >= height) ? (height - 1 - gid.y) : R;
    int min_dx = gid.x < R ? -gid.x : -R;
    int max_dx = (gid.x + R >= width) ? (width - 1 - gid.x) : R;
    
    for (int dy = min_dy; dy <= max_dy; dy++) {
        for (int dx = min_dx; dx <= max_dx; dx++) {
            uint nidx = ((gid.y + dy) * width + (gid.x + dx)) * channels;
            sum_r += input[nidx];
            if (channels >= 2) sum_g += input[nidx+1];
            if (channels >= 3) sum_b += input[nidx+2];
            sum_w++;
        }
    }
    
    output[idx] = (uchar)((sum_r + sum_w/2) / sum_w);
    if (channels >= 2) output[idx+1] = (uchar)((sum_g + sum_w/2) / sum_w);
    if (channels >= 3) output[idx+2] = (uchar)((sum_b + sum_w/2) / sum_w);
}
/* ══════════════════════════════════════════════════════════════
 *  Kernel 3a: Render Splats — Stamp Pass (SPLAT-CENTRIC)
 *  Grid: 1 thread per SPLAT (not per pixel!)
 *  Each splat "stamps" its Gaussian blob onto the accumulation buffers.
 *  Uses atomic_fetch_add_explicit on uint accumulators (fixed-point).
 *
 *  This is O(splats × radius²) ≈ 5M ops — vs the old O(pixels × splats) ≈ 100B ops.
 * ══════════════════════════════════════════════════════════════ */
kernel void render_splats_stamp(
    device const Splat2D* splats      [[buffer(0)]],
    device atomic_uint* accum_r       [[buffer(1)]],  /* fixed-point 16.8 accumulator */
    device atomic_uint* accum_g       [[buffer(2)]],
    device atomic_uint* accum_b       [[buffer(3)]],
    device atomic_uint* accum_w       [[buffer(4)]],  /* weight × 256 */
    constant uint& width              [[buffer(5)]],
    constant uint& height             [[buffer(6)]],
    constant uint& num_splats         [[buffer(7)]],
    uint tid                          [[thread_position_in_grid]]
) {
    if (tid >= num_splats) return;
    
    Splat2D sp = splats[tid];
    if (sp.alpha == 0) return;
    
    float sx = max(1.0f, (float)sp.sx);
    float sy = max(1.0f, (float)sp.sy);
    float inv_sx2 = 1.0f / (sx * sx);
    float inv_sy2 = 1.0f / (sy * sy);
    float alpha_f = (float)sp.alpha / 255.0f;
    
    float max_sigma = max(sx, sy);
    int radius = (int)(max_sigma * 3.0f);
    
    int x0 = max(0, (int)sp.x - radius);
    int y0 = max(0, (int)sp.y - radius);
    int x1 = min((int)width - 1,  (int)sp.x + radius);
    int y1 = min((int)height - 1, (int)sp.y + radius);
    
    for (int py = y0; py <= y1; py++) {
        float dy = (float)(py - (int)sp.y);
        float dy2 = dy * dy * inv_sy2;
        if (dy2 > 9.0f) continue;  /* early row reject */
        
        for (int px = x0; px <= x1; px++) {
            float dx = (float)(px - (int)sp.x);
            float dist_sq = dx * dx * inv_sx2 + dy2;
            if (dist_sq > 9.0f) continue;
            
            float w = alpha_f * exp(-0.5f * dist_sq);
            if (w < 0.005f) continue;
            
            /* Fixed-point: multiply by 256 and accumulate as uint */
            uint w_fixed = (uint)(w * 256.0f + 0.5f);
            uint pidx = (uint)py * width + (uint)px;
            
            atomic_fetch_add_explicit(&accum_r[pidx], (uint)((float)sp.r * w * 256.0f + 0.5f), memory_order_relaxed);
            atomic_fetch_add_explicit(&accum_g[pidx], (uint)((float)sp.g * w * 256.0f + 0.5f), memory_order_relaxed);
            atomic_fetch_add_explicit(&accum_b[pidx], (uint)((float)sp.b * w * 256.0f + 0.5f), memory_order_relaxed);
            atomic_fetch_add_explicit(&accum_w[pidx], w_fixed, memory_order_relaxed);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 3b: Render Splats — Normalize Pass (PER-PIXEL)
 *  Grid: 1 thread per pixel
 *  Reads accumulated weighted colors and divides by total weight.
 * ══════════════════════════════════════════════════════════════ */
kernel void render_splats_normalize(
    device const uint* accum_r       [[buffer(0)]],
    device const uint* accum_g       [[buffer(1)]],
    device const uint* accum_b       [[buffer(2)]],
    device const uint* accum_w       [[buffer(3)]],
    device uchar* output             [[buffer(4)]],
    constant uint& width             [[buffer(5)]],
    constant uint& height            [[buffer(6)]],
    constant uint& channels          [[buffer(7)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint pidx = gid.y * width + gid.x;
    uint w = accum_w[pidx];
    
    if (w > 0) {
        uint out_idx = pidx * channels;
        output[out_idx]     = (uchar)min(255u, accum_r[pidx] / w);
        if (channels >= 2) output[out_idx + 1] = (uchar)min(255u, accum_g[pidx] / w);
        if (channels >= 3) output[out_idx + 2] = (uchar)min(255u, accum_b[pidx] / w);
    }
}

/* Keep the old kernel for backward compatibility (image mode uses it) */
kernel void render_splats(
    device const Splat2D* splats     [[buffer(0)]],
    device uchar* output             [[buffer(1)]],
    device const uchar* fg_mask      [[buffer(2)]],
    constant uint& width             [[buffer(3)]],
    constant uint& height            [[buffer(4)]],
    constant uint& channels          [[buffer(5)]],
    constant uint& num_splats        [[buffer(6)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    if (fg_mask[gid.y * width + gid.x] == 1) return;
    
    float px = (float)gid.x;
    float py = (float)gid.y;
    
    float accum_r = 0, accum_g = 0, accum_b = 0;
    float total_weight = 0;
    
    uint scan_limit = num_splats;
    
    for (uint i = 0; i < scan_limit; i++) {
        Splat2D sp = splats[i];
        if (sp.alpha == 0) continue;
        
        float dx = px - (float)sp.x;
        float dy = py - (float)sp.y;
        float sx = max(1.0f, (float)sp.sx);
        float sy = max(1.0f, (float)sp.sy);
        
        float max_r = max(sx, sy) * 3.0f;
        if (abs(dx) > max_r || abs(dy) > max_r) continue;
        
        float dist_sq = (dx * dx) / (sx * sx) + (dy * dy) / (sy * sy);
        if (dist_sq > 9.0f) continue;
        
        float alpha_w = ((float)sp.alpha / 255.0f) * exp(-0.5f * dist_sq);
        if (alpha_w < 0.005f) continue;
        
        accum_r += (float)sp.r * alpha_w;
        accum_g += (float)sp.g * alpha_w;
        accum_b += (float)sp.b * alpha_w;
        total_weight += alpha_w;
    }
    
    if (total_weight > 0) {
        uint idx = (gid.y * width + gid.x) * channels;
        accum_r /= total_weight;
        accum_g /= total_weight;
        accum_b /= total_weight;
        
        output[idx] = clamp((uchar)(accum_r + 0.5f), (uchar)0, (uchar)255);
        if (channels >= 2) output[idx+1] = clamp((uchar)(accum_g + 0.5f), (uchar)0, (uchar)255);
        if (channels >= 3) output[idx+2] = clamp((uchar)(accum_b + 0.5f), (uchar)0, (uchar)255);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 4: Sobel Gradient
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void sobel_gradient(
    device const uchar* pixels       [[buffer(0)]],
    device float* gradient           [[buffer(1)]],
    constant uint& width             [[buffer(2)]],
    constant uint& height            [[buffer(3)]],
    constant uint& channels          [[buffer(4)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x == 0 || gid.x >= width - 1 || gid.y == 0 || gid.y >= height - 1) {
        if (gid.x < width && gid.y < height) gradient[gid.y * width + gid.x] = 0.0f;
        return;
    }
    
    float gx = 0, gy = 0;
    for (uint c = 0; c < channels; c++) {
        float dx = - (float)pixels[((gid.y-1)*width + (gid.x-1))*channels + c]
                   + (float)pixels[((gid.y-1)*width + (gid.x+1))*channels + c]
                   - 2.0f*(float)pixels[(gid.y*width + (gid.x-1))*channels + c]
                   + 2.0f*(float)pixels[(gid.y*width + (gid.x+1))*channels + c]
                   - (float)pixels[((gid.y+1)*width + (gid.x-1))*channels + c]
                   + (float)pixels[((gid.y+1)*width + (gid.x+1))*channels + c];
                   
        float dy = - (float)pixels[((gid.y-1)*width + (gid.x-1))*channels + c]
                   - 2.0f*(float)pixels[((gid.y-1)*width + gid.x)*channels + c]
                   - (float)pixels[((gid.y-1)*width + (gid.x+1))*channels + c]
                   + (float)pixels[((gid.y+1)*width + (gid.x-1))*channels + c]
                   + 2.0f*(float)pixels[((gid.y+1)*width + gid.x)*channels + c]
                   + (float)pixels[((gid.y+1)*width + (gid.x+1))*channels + c];
                   
        gx += dx * dx;
        gy += dy * dy;
    }
    
    gradient[gid.y * width + gid.x] = sqrt(gx + gy);
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 5: BGRA to RGB
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void bgra_to_rgb(
    device const uchar* bgra         [[buffer(0)]],
    device uchar* rgb                [[buffer(1)]],
    constant uint& width             [[buffer(2)]],
    constant uint& height            [[buffer(3)]],
    constant uint& stride            [[buffer(4)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint src_idx = gid.y * stride + gid.x * 4;
    uint dst_idx = (gid.y * width + gid.x) * 3;
    
    rgb[dst_idx]     = bgra[src_idx + 2]; // R
    rgb[dst_idx + 1] = bgra[src_idx + 1]; // G
    rgb[dst_idx + 2] = bgra[src_idx + 0]; // B
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 6: Frame Diff (P-frame delta encoding)
 *  Computes per-pixel difference: delta = current - reference + 128
 *  The +128 bias maps signed [-128,+127] into unsigned [0,255].
 *  A pixel is "changed" if any channel differs by more than a threshold.
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void frame_diff(
    device const uchar* current      [[buffer(0)]],
    device const uchar* reference    [[buffer(1)]],
    device uchar* delta              [[buffer(2)]],
    device atomic_uint* changed_count [[buffer(3)]],
    constant uint& width             [[buffer(4)]],
    constant uint& height            [[buffer(5)]],
    constant uint& channels          [[buffer(6)]],
    constant uint& threshold         [[buffer(7)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint idx = (gid.y * width + gid.x) * channels;
    bool changed = false;
    
    for (uint c = 0; c < channels; c++) {
        int diff = (int)current[idx + c] - (int)reference[idx + c];
        
        // Check if this pixel changed beyond threshold
        if (abs(diff) > (int)threshold) {
            changed = true;
        }
        
        // Store biased delta: 128 = no change, >128 = brighter, <128 = darker
        delta[idx + c] = (uchar)clamp(diff + 128, 0, 255);
    }
    
    if (!changed) {
        // Zero out unchanged pixels (exact 128 on all channels = no change)
        for (uint c = 0; c < channels; c++) {
            delta[idx + c] = 128;
        }
    } else {
        atomic_fetch_add_explicit(changed_count, 1, memory_order_relaxed);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 7: Region Classification (3-tier for splat sizing)
 *  Classifies each pixel's detail level from Sobel gradient:
 *    mask = 0 → SMOOTH (big splats: sky, ground, background)
 *    mask = 1 → MEDIUM (mid-size splats: buildings, clothes)
 *    mask = 2 → DENSE  (small splats: edges, grass, faces)
 *  Uses 3x3 dilation to prevent isolated pixels.
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void region_classify(
    device const float* gradient     [[buffer(0)]],
    device uchar* mask               [[buffer(1)]],
    constant uint& width             [[buffer(2)]],
    constant uint& height            [[buffer(3)]],
    constant float& threshold        [[buffer(4)]],
    uint2 gid                        [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint idx = gid.y * width + gid.x;
    
    /* Max gradient in 3x3 neighborhood (dilation) */
    float max_grad = 0.0f;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int nx = (int)gid.x + dx;
            int ny = (int)gid.y + dy;
            if (nx >= 0 && nx < (int)width && ny >= 0 && ny < (int)height) {
                float g = gradient[(uint)ny * width + (uint)nx];
                max_grad = max(max_grad, g);
            }
        }
    }
    
    /* 3-tier classification */
    float hi_thresh = threshold;         /* ~30: dense edge detail */
    float lo_thresh = threshold * 0.3f;  /* ~9: medium detail */
    
    if (max_grad > hi_thresh) {
        mask[idx] = 2; /* Dense → small splats */
    } else if (max_grad > lo_thresh) {
        mask[idx] = 1; /* Medium → medium splats */
    } else {
        mask[idx] = 0; /* Smooth → big splats */
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 13: Splat Color Sampling
 *  For each splat, averages source pixels within its radius
 *  to compute the best-fit color.
 *  Grid: 1 thread per splat
 *  SplatInput:  [uint16 x, uint16 y, uint8 radius]
 *  SplatOutput: [uint8 r, uint8 g, uint8 b]
 * ══════════════════════════════════════════════════════════════ */
kernel void splat_color_sample(
    device const uchar* splats_packed     [[buffer(0)]],
    device const uchar* source            [[buffer(1)]],
    device uchar* colors_out              [[buffer(2)]],
    constant uint& width                  [[buffer(3)]],
    constant uint& height                 [[buffer(4)]],
    constant uint& channels               [[buffer(5)]],
    constant uint& num_splats             [[buffer(6)]],
    uint tid                              [[thread_position_in_grid]]
) {
    if (tid >= num_splats) return;
    
    uint p_idx = tid * 5;
    int cx = (int)splats_packed[p_idx] | ((int)splats_packed[p_idx + 1] << 8);
    int cy = (int)splats_packed[p_idx + 2] | ((int)splats_packed[p_idx + 3] << 8);
    int r = (int)splats_packed[p_idx + 4];
    if (r < 1) r = 1;
    
    /* Average pixels in a circular region */
    float sum_r = 0, sum_g = 0, sum_b = 0;
    float count = 0;
    int r2 = r * r;
    
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) continue;
            int px = cx + dx;
            int py = cy + dy;
            if (px < 0 || px >= (int)width || py < 0 || py >= (int)height) continue;
            
            uint pidx = (uint)py * width * channels + (uint)px * channels;
            /* M17 fix: guard channel reads for grayscale (channels < 3). */
            sum_r += (float)source[pidx];
            sum_g += (channels >= 2) ? (float)source[pidx + 1] : (float)source[pidx];
            sum_b += (channels >= 3) ? (float)source[pidx + 2] : (float)source[pidx];
            count += 1.0f;
        }
    }
    
    if (count > 0) {
        sum_r /= count;
        sum_g /= count;
        sum_b /= count;
    }
    
    uint c_idx = tid * 3;
    colors_out[c_idx]     = (uchar)(sum_r + 0.5f);
    colors_out[c_idx + 1] = (uchar)(sum_g + 0.5f);
    colors_out[c_idx + 2] = (uchar)(sum_b + 0.5f);
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 8a: JFA Clear
 *  Clears the seed texture to (-1, -1) for all pixels.
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void jfa_clear(
    device int2* seed_tex             [[buffer(0)]],
    constant uint& width              [[buffer(1)]],
    constant uint& height             [[buffer(2)]],
    uint2 gid                         [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    seed_tex[gid.y * width + gid.x] = int2(-1, -1);
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 8b: JFA Seed
 *  Seeds vertex positions into the JFA texture.
 *  Grid: 1 thread per VERTEX (not per pixel!)
 * ══════════════════════════════════════════════════════════════ */
kernel void jfa_seed(
    device int2* seed_tex             [[buffer(0)]],
    device const float2* vertex_pos   [[buffer(1)]],
    constant uint& width              [[buffer(2)]],
    constant uint& height             [[buffer(3)]],
    constant uint& num_vertices       [[buffer(4)]],
    uint tid                          [[thread_position_in_grid]]
) {
    if (tid >= num_vertices) return;
    
    int vx = clamp((int)(vertex_pos[tid].x + 0.5f), 0, (int)width - 1);
    int vy = clamp((int)(vertex_pos[tid].y + 0.5f), 0, (int)height - 1);
    
    /* Write this vertex's position as the seed at its pixel location */
    seed_tex[vy * width + vx] = int2(vx, vy);
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 9: JFA Step
 *  One pass of the Jump Flooding Algorithm.
 *  Each pixel checks 8 neighbors at distance `step_size` and
 *  adopts the seed of the nearest neighbor.
 *  Run ceil(log2(max(W,H))) times, halving step_size each pass.
 *  Grid: 1 thread per pixel
 * ══════════════════════════════════════════════════════════════ */
kernel void jfa_step(
    device int2* seed_tex             [[buffer(0)]],
    device int2* seed_tex_out         [[buffer(1)]],
    constant uint& width              [[buffer(2)]],
    constant uint& height             [[buffer(3)]],
    constant int& step_size           [[buffer(4)]],
    uint2 gid                         [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint idx = gid.y * width + gid.x;
    float px = (float)gid.x;
    float py = (float)gid.y;
    
    int2 best_seed = seed_tex[idx];
    float best_dist = 1e20f;
    
    if (best_seed.x >= 0) {
        float dx = px - (float)best_seed.x;
        float dy = py - (float)best_seed.y;
        best_dist = dx * dx + dy * dy;
    }
    
    /* Check 9 neighbors (including self) at step_size distance */
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int nx = (int)gid.x + dx * step_size;
            int ny = (int)gid.y + dy * step_size;
            
            if (nx < 0 || nx >= (int)width || ny < 0 || ny >= (int)height) continue;
            
            int2 neighbor_seed = seed_tex[(uint)ny * width + (uint)nx];
            if (neighbor_seed.x < 0) continue; /* No seed assigned */
            
            float ndx = px - (float)neighbor_seed.x;
            float ndy = py - (float)neighbor_seed.y;
            float ndist = ndx * ndx + ndy * ndy;
            
            if (ndist < best_dist) {
                best_dist = ndist;
                best_seed = neighbor_seed;
            }
        }
    }
    
    seed_tex_out[idx] = best_seed;
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 10: Delaunay Triangle Extraction
 *  Scans the Voronoi diagram (seed_tex) to find where 3 different
 *  Voronoi cells meet — those 3 seeds form a Delaunay triangle.
 *  Uses a 2x2 pixel scan: checks if the 4 pixels at each grid
 *  position have 3+ distinct seeds → emit a triangle.
 *  Grid: 1 thread per 2x2 block (W/2 × H/2)
 * ══════════════════════════════════════════════════════════════ */
struct DelaunayTriOutput {
    uint v0; /* Index into vertex buffer */
    uint v1;
    uint v2;
};

kernel void delaunay_extract(
    device const int2* seed_tex       [[buffer(0)]],
    device const float2* vertex_pos   [[buffer(1)]],
    device atomic_uint* tri_count     [[buffer(2)]],
    device DelaunayTriOutput* tris    [[buffer(3)]],
    constant uint& width              [[buffer(4)]],
    constant uint& height             [[buffer(5)]],
    constant uint& num_vertices       [[buffer(6)]],
    constant uint& max_tris           [[buffer(7)]],
    uint2 gid                         [[thread_position_in_grid]]
) {
    /* Each thread processes a 2x2 pixel block */
    uint bx = gid.x * 2;
    uint by = gid.y * 2;
    if (bx + 1 >= width || by + 1 >= height) return;
    
    /* Read 4 Voronoi seeds from the 2x2 block */
    int2 s00 = seed_tex[by * width + bx];
    int2 s10 = seed_tex[by * width + (bx + 1)];
    int2 s01 = seed_tex[(by + 1) * width + bx];
    int2 s11 = seed_tex[(by + 1) * width + (bx + 1)];
    
    /* Collect unique seeds (max 4 from a 2x2 block) — inline, no lambda */
    int2 seeds[4];
    int count = 0;
    
    /* Add s00 */
    if (s00.x >= 0) {
        seeds[0] = s00;
        count = 1;
    }
    
    /* Add s10 if unique */
    if (s10.x >= 0) {
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (seeds[i].x == s10.x && seeds[i].y == s10.y) { dup = true; break; }
        }
        if (!dup && count < 4) seeds[count++] = s10;
    }
    
    /* Add s01 if unique */
    if (s01.x >= 0) {
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (seeds[i].x == s01.x && seeds[i].y == s01.y) { dup = true; break; }
        }
        if (!dup && count < 4) seeds[count++] = s01;
    }
    
    /* Add s11 if unique */
    if (s11.x >= 0) {
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (seeds[i].x == s11.x && seeds[i].y == s11.y) { dup = true; break; }
        }
        if (!dup && count < 4) seeds[count++] = s11;
    }
    
    /* Need at least 3 distinct seeds to form a triangle */
    if (count < 3) return;
    
    /* Emit triangles from unique seed combinations */
    for (int a = 0; a < count - 2; a++) {
        for (int b = a + 1; b < count - 1; b++) {
            for (int c = b + 1; c < count; c++) {
                /* Pixel-linearized vertex indices */
                uint va = (uint)seeds[a].y * width + (uint)seeds[a].x;
                uint vb = (uint)seeds[b].y * width + (uint)seeds[b].x;
                uint vc = (uint)seeds[c].y * width + (uint)seeds[c].x;
                
                /* Canonical ordering to deduplicate */
                if (va > vb) { uint t = va; va = vb; vb = t; }
                if (vb > vc) { uint t = vb; vb = vc; vc = t; }
                if (va > vb) { uint t = va; va = vb; vb = t; }
                
                uint ti = atomic_fetch_add_explicit(tri_count, 1, memory_order_relaxed);
                if (ti < max_tris) {
                    tris[ti].v0 = va;
                    tris[ti].v1 = vb;
                    tris[ti].v2 = vc;
                }
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 11: Triangle Color Fitting
 *  For each vertex, compute the average color of nearby source pixels.
 *  Grid: 1 thread per vertex
 * ══════════════════════════════════════════════════════════════ */
kernel void triangle_color_fit(
    device const float2* vertex_pos   [[buffer(0)]],
    device const uchar* source        [[buffer(1)]],
    device uchar3* vertex_colors      [[buffer(2)]],
    constant uint& width              [[buffer(3)]],
    constant uint& height             [[buffer(4)]],
    constant uint& channels           [[buffer(5)]],
    constant uint& num_vertices       [[buffer(6)]],
    uint tid                          [[thread_position_in_grid]]
) {
    if (tid >= num_vertices) return;
    
    float2 pos = vertex_pos[tid];
    int cx = clamp((int)(pos.x + 0.5f), 0, (int)width - 1);
    int cy = clamp((int)(pos.y + 0.5f), 0, (int)height - 1);
    
    /* Average color in a 3x3 neighborhood around the vertex */
    int sum_r = 0, sum_g = 0, sum_b = 0, cnt = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int nx = clamp(cx + dx, 0, (int)width - 1);
            int ny = clamp(cy + dy, 0, (int)height - 1);
            uint idx = (ny * width + nx) * channels;
            sum_r += source[idx];
            if (channels >= 2) sum_g += source[idx + 1];
            if (channels >= 3) sum_b += source[idx + 2];
            cnt++;
        }
    }
    
    vertex_colors[tid] = uchar3(
        (uchar)(sum_r / cnt),
        (uchar)(sum_g / cnt),
        (uchar)(sum_b / cnt)
    );
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel 12: Splat Fitting for Smooth Regions
 *  For each splat position (in mask=0 regions), fit a Gaussian splat
 *  by sampling the source image color and computing a radius based
 *  on the local gradient uniformity.
 *  Grid: 1 thread per splat
 * ══════════════════════════════════════════════════════════════ */
struct SplatOutput {
    float2 pos;
    uchar3 color;
    uchar radius;
    uchar alpha;
};

kernel void splat_fit_regions(
    device const float2* splat_pos    [[buffer(0)]],
    device const uchar* source        [[buffer(1)]],
    device const uchar* mask          [[buffer(2)]],
    device const float* gradient      [[buffer(3)]],
    device SplatOutput* splats_out    [[buffer(4)]],
    constant uint& width              [[buffer(5)]],
    constant uint& height             [[buffer(6)]],
    constant uint& channels           [[buffer(7)]],
    constant uint& num_splats         [[buffer(8)]],
    uint tid                          [[thread_position_in_grid]]
) {
    if (tid >= num_splats) return;
    
    float2 pos = splat_pos[tid];
    int cx = clamp((int)(pos.x + 0.5f), 0, (int)width - 1);
    int cy = clamp((int)(pos.y + 0.5f), 0, (int)height - 1);
    
    /* Only fit splats in smooth (mask=0) regions */
    if (mask[cy * width + cx] != 0) {
        splats_out[tid].alpha = 0; /* Disabled splat */
        return;
    }
    
    /* Sample source color at splat center */
    uint src_idx = (cy * width + cx) * channels;
    uchar r = source[src_idx];
    uchar g = channels >= 2 ? source[src_idx + 1] : r;
    uchar b = channels >= 3 ? source[src_idx + 2] : r;
    
    /* Compute radius from local gradient: lower gradient → larger splat */
    float local_grad = gradient[cy * width + cx];
    float radius_f = clamp(30.0f / (local_grad + 1.0f), 2.0f, 50.0f);
    
    splats_out[tid].pos = pos;
    splats_out[tid].color = uchar3(r, g, b);
    splats_out[tid].radius = (uchar)clamp((int)(radius_f + 0.5f), 2, 255);
    splats_out[tid].alpha = 255;
}

/* ══════════════════════════════════════════════════════════════
 *  Kernel: Composite Hybrid (Triangles + Splats)
 *  Grid: 2D (width × height), 1 thread per pixel
 *
 *  Pass 1 already rendered triangles into tri_buffer (RGB).
 *  Pass 2 already stamped splats into accum_r/g/b/w buffers.
 *
 *  This kernel composites them:
 *    - Where splats have coverage (accum_w > 0):
 *        splat_color = accum_rgb / accum_w
 *        output = splat_color (splats override triangle in their zones)
 *    - Where splats have no coverage:
 *        output = tri_buffer (triangle passthrough)
 *
 *  This gives crisp triangle edges for detail regions and
 *  smooth Gaussian gradients for smooth regions, with no overlap.
 * ══════════════════════════════════════════════════════════════ */
kernel void composite_hybrid(
    device const uchar* tri_buffer      [[buffer(0)]],   /* triangle rasterized RGB */
    device const uint*  splat_accum_r   [[buffer(1)]],
    device const uint*  splat_accum_g   [[buffer(2)]],
    device const uint*  splat_accum_b   [[buffer(3)]],
    device const uint*  splat_accum_w   [[buffer(4)]],
    device uchar* output                [[buffer(5)]],
    constant uint& width                [[buffer(6)]],
    constant uint& height               [[buffer(7)]],
    constant uint& channels             [[buffer(8)]],
    uint2 gid                           [[thread_position_in_grid]]
) {
    if (gid.x >= width || gid.y >= height) return;
    
    uint pidx = gid.y * width + gid.x;
    uint out_idx = pidx * channels;
    uint sw = splat_accum_w[pidx];
    
    if (sw > 0) {
        /* Splat zone: use Gaussian-blended color (smooth gradients) */
        output[out_idx] = (uchar)min(255u, splat_accum_r[pidx] / sw);
        if (channels >= 2) output[out_idx + 1] = (uchar)min(255u, splat_accum_g[pidx] / sw);
        if (channels >= 3) output[out_idx + 2] = (uchar)min(255u, splat_accum_b[pidx] / sw);
    } else {
        /* Triangle zone: use hardware-rasterized color (sharp edges) */
        output[out_idx] = tri_buffer[out_idx];
        if (channels >= 2) output[out_idx + 1] = tri_buffer[out_idx + 1];
        if (channels >= 3) output[out_idx + 2] = tri_buffer[out_idx + 2];
    }
}
