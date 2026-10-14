/*
 * QTSQ Metal GPU Engine — Objective-C++ Implementation
 * Bridging C-structs to Metal GPU Shaders
 */

#include "qtsq_metal.h"

#ifdef __APPLE__

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct qtsq_metal_ctx {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    
    id<MTLComputePipelineState> pso_rasterize;
    id<MTLComputePipelineState> pso_blur;
    id<MTLComputePipelineState> pso_splats;
    id<MTLComputePipelineState> pso_sobel;
    id<MTLComputePipelineState> pso_bgra_rgb;
    id<MTLComputePipelineState> pso_frame_diff;
    /* Phase 9: GPU Delaunay + Segmented Pipeline */
    id<MTLComputePipelineState> pso_region_classify;
    id<MTLComputePipelineState> pso_jfa_clear;
    id<MTLComputePipelineState> pso_jfa_seed;
    id<MTLComputePipelineState> pso_jfa_step;
    id<MTLComputePipelineState> pso_delaunay_extract;
    id<MTLComputePipelineState> pso_triangle_color_fit;
    id<MTLComputePipelineState> pso_splat_fit_regions;
    id<MTLComputePipelineState> pso_splat_color_sample;
    /* Two-pass splat-centric rendering for real-time video */
    id<MTLComputePipelineState> pso_splats_stamp;
    id<MTLComputePipelineState> pso_splats_normalize;
    /* Hybrid triangle+splat compositing */
    id<MTLComputePipelineState> pso_composite_hybrid;
    
    /* Persistent GPU buffers for video rendering (allocated once, reused per frame) */
    id<MTLBuffer> vr_splats;      /* splat upload buffer */
    id<MTLBuffer> vr_accum_r;     /* fixed-point R accumulator */
    id<MTLBuffer> vr_accum_g;     /* fixed-point G accumulator */
    id<MTLBuffer> vr_accum_b;     /* fixed-point B accumulator */
    id<MTLBuffer> vr_accum_w;     /* fixed-point weight accumulator */
    id<MTLBuffer> vr_output;      /* output pixel buffer */
    uint32_t vr_splat_capacity;   /* current capacity of vr_splats */
    uint32_t vr_pixel_count;      /* current pixel count for accum buffers */
    uint8_t  vr_channels;         /* current channel count */
    
    /* Persistent hybrid render buffers (triangles + splats) */
    id<MTLBuffer> hr_vertices;    /* tri_vertex_t upload buffer */
    id<MTLBuffer> hr_faces;       /* tri_face_t upload buffer */
    id<MTLBuffer> hr_tri_output;  /* triangle rasterization output RGB */
    uint32_t hr_vert_capacity;    /* current vertex capacity */
    uint32_t hr_face_capacity;    /* current face capacity */
};

// Internal error logger
static void log_metal_error(NSError *error, const char *msg) {
    if (error) {
        fprintf(stderr, "[QTSQ Metal] %s: %s\n", msg, [[error localizedDescription] UTF8String]);
    } else {
        fprintf(stderr, "[QTSQ Metal] %s\n", msg);
    }
}

static qtsq_metal_ctx_t *global_metal_ctx = NULL;
static pthread_once_t metal_once = PTHREAD_ONCE_INIT;

static void metal_init_once(void) {
    global_metal_ctx = qtsq_metal_init();
}

/* Thread-safe singleton: worker threads in qtsq_dispatch can call this
 * concurrently; pthread_once guarantees exactly one init. */
qtsq_metal_ctx_t* qtsq_metal_global(void) {
    pthread_once(&metal_once, metal_init_once);
    return global_metal_ctx;
}

qtsq_metal_ctx_t* qtsq_metal_init(void) {
    /* H17 fix: only redirect stderr to the debug log when QTSQ_DEBUG is set.
     * Previously this ran unconditionally on every init, hijacking process
     * stderr globally and leaking the prior fd. */
    if (getenv("QTSQ_DEBUG")) {
        freopen("/tmp/qtsq_debug.log", "a", stderr);
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        fprintf(stderr, "[QTSQ Metal] Failed to access Apple GPU! Falling back to CPU if supported.\n");
        return NULL;
    }
    
    id<MTLCommandQueue> commandQueue = [device newCommandQueue];
    if (!commandQueue) {
        fprintf(stderr, "[QTSQ Metal] Failed to create command queue!\n");
        return NULL;
    }
    
    // Load default library built alongside the framework
    id<MTLLibrary> defaultLibrary = [device newDefaultLibrary];
    if (!defaultLibrary) {
        // Look within the specific bundle if running as a framework
        NSBundle *bundle = [NSBundle bundleForClass:NSClassFromString(@"QTSQMetalDummyClassWaitDoesntExist")]; 
        if (!bundle) bundle = [NSBundle mainBundle];
        
        NSError *libErr = nil;
        NSString *libPath = [bundle pathForResource:@"default" ofType:@"metallib"];
        if (libPath) {
            defaultLibrary = [device newLibraryWithURL:[NSURL fileURLWithPath:libPath] error:&libErr];
        }
        
        if (!defaultLibrary) {
            /* Try multiple search paths — include Resources/ and the folder
             * next to the executable so both bin/QTSQStudio and
             * QTSQStudio.app resolve the same metallib. */
            NSString *execPath = [[NSBundle mainBundle] executablePath];
            NSString *binDir = [execPath stringByDeletingLastPathComponent];
            NSArray *searchPaths = @[
                [binDir stringByAppendingPathComponent:@"default.metallib"],
                [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"default.metallib"],
                [binDir stringByAppendingPathComponent:@"../Resources/default.metallib"],
                @"bin/default.metallib",
            ];
            
            for (NSString *path in searchPaths) {
                if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
                    defaultLibrary = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&libErr];
                    if (defaultLibrary) {
                        fprintf(stderr, "[QTSQ Metal] Loaded metallib from: %s\n", [path UTF8String]);
                        break;
                    }
                }
            }
        }
        
            /* Last resort: compile from source .metal file at runtime.
             * H17 fix: removed hardcoded /Users/priyanshuboruah/... path.
             * Search relative to the executable + bundle resources only. */
            if (!defaultLibrary) {
                NSString *execPath2 = [[NSBundle mainBundle] executablePath];
                NSString *srcDir = [execPath2 stringByDeletingLastPathComponent];
                NSArray *sourcePaths = @[
                    [srcDir stringByAppendingPathComponent:@"qtsq_shaders.metal"],
                    @"src/qtsq_shaders.metal",
                    [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"qtsq_shaders.metal"],
                ];
            for (NSString *path in sourcePaths) {
                if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
                    NSString *source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&libErr];
                    if (source) {
                        MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
                        defaultLibrary = [device newLibraryWithSource:source options:opts error:&libErr];
                        if (defaultLibrary) {
                            fprintf(stderr, "[QTSQ Metal] Compiled shaders from source: %s\n", [path UTF8String]);
                            break;
                        } else {
                            fprintf(stderr, "[QTSQ Metal] Shader compile error: %s\n", [[libErr localizedDescription] UTF8String]);
                        }
                    }
                }
            }
        }
        
        if (!defaultLibrary) {
            fprintf(stderr, "[QTSQ Metal] FATAL: Could not locate compiled .metallib shaders!\n");
            return NULL;
        }
    }
    
    qtsq_metal_ctx_t *ctx = (qtsq_metal_ctx_t *)malloc(sizeof(qtsq_metal_ctx_t));
    ctx->device = device;
    ctx->commandQueue = commandQueue;
    
    NSError *error = nil;
    
    id<MTLFunction> fn_rast = [defaultLibrary newFunctionWithName:@"rasterize_triangles"];
    ctx->pso_rasterize = [device newComputePipelineStateWithFunction:fn_rast error:&error];
    if (error) log_metal_error(error, "Failed to compile rasterize_triangles");
    
    id<MTLFunction> fn_blur = [defaultLibrary newFunctionWithName:@"box_blur_3x3"];
    ctx->pso_blur = [device newComputePipelineStateWithFunction:fn_blur error:&error];
    if (error) log_metal_error(error, "Failed to compile box_blur_3x3");
    
    id<MTLFunction> fn_splat = [defaultLibrary newFunctionWithName:@"render_splats"];
    ctx->pso_splats = [device newComputePipelineStateWithFunction:fn_splat error:&error];
    if (error) log_metal_error(error, "Failed to compile render_splats");
    
    id<MTLFunction> fn_sobel = [defaultLibrary newFunctionWithName:@"sobel_gradient"];
    ctx->pso_sobel = [device newComputePipelineStateWithFunction:fn_sobel error:&error];
    if (error) log_metal_error(error, "Failed to compile sobel_gradient");
    
    id<MTLFunction> fn_bgra = [defaultLibrary newFunctionWithName:@"bgra_to_rgb"];
    ctx->pso_bgra_rgb = [device newComputePipelineStateWithFunction:fn_bgra error:&error];
    if (error) log_metal_error(error, "Failed to compile bgra_to_rgb");
    
    id<MTLFunction> fn_diff = [defaultLibrary newFunctionWithName:@"frame_diff"];
    ctx->pso_frame_diff = [device newComputePipelineStateWithFunction:fn_diff error:&error];
    if (error) log_metal_error(error, "Failed to compile frame_diff");
    
    /* Phase 9: GPU Delaunay + Segmented Pipeline kernels */
    id<MTLFunction> fn_classify = [defaultLibrary newFunctionWithName:@"region_classify"];
    ctx->pso_region_classify = fn_classify ? [device newComputePipelineStateWithFunction:fn_classify error:&error] : nil;
    if (error) log_metal_error(error, "Failed to compile region_classify");
    
    id<MTLFunction> fn_jfa_clear = [defaultLibrary newFunctionWithName:@"jfa_clear"];
    ctx->pso_jfa_clear = fn_jfa_clear ? [device newComputePipelineStateWithFunction:fn_jfa_clear error:&error] : nil;
    
    id<MTLFunction> fn_jfa_seed = [defaultLibrary newFunctionWithName:@"jfa_seed"];
    ctx->pso_jfa_seed = fn_jfa_seed ? [device newComputePipelineStateWithFunction:fn_jfa_seed error:&error] : nil;
    
    id<MTLFunction> fn_jfa_step = [defaultLibrary newFunctionWithName:@"jfa_step"];
    ctx->pso_jfa_step = fn_jfa_step ? [device newComputePipelineStateWithFunction:fn_jfa_step error:&error] : nil;
    
    id<MTLFunction> fn_delaunay = [defaultLibrary newFunctionWithName:@"delaunay_extract"];
    ctx->pso_delaunay_extract = fn_delaunay ? [device newComputePipelineStateWithFunction:fn_delaunay error:&error] : nil;
    
    id<MTLFunction> fn_tri_fit = [defaultLibrary newFunctionWithName:@"triangle_color_fit"];
    ctx->pso_triangle_color_fit = fn_tri_fit ? [device newComputePipelineStateWithFunction:fn_tri_fit error:&error] : nil;
    
    id<MTLFunction> fn_splat_fit = [defaultLibrary newFunctionWithName:@"splat_fit_regions"];
    ctx->pso_splat_fit_regions = fn_splat_fit ? [device newComputePipelineStateWithFunction:fn_splat_fit error:&error] : nil;
    
    id<MTLFunction> fn_splat_cs = [defaultLibrary newFunctionWithName:@"splat_color_sample"];
    ctx->pso_splat_color_sample = fn_splat_cs ? [device newComputePipelineStateWithFunction:fn_splat_cs error:&error] : nil;
    
    /* Two-pass splat-centric rendering for real-time video */
    id<MTLFunction> fn_stamp = [defaultLibrary newFunctionWithName:@"render_splats_stamp"];
    ctx->pso_splats_stamp = fn_stamp ? [device newComputePipelineStateWithFunction:fn_stamp error:&error] : nil;
    if (error && fn_stamp) log_metal_error(error, "Failed to compile render_splats_stamp");
    error = nil;
    
    id<MTLFunction> fn_norm = [defaultLibrary newFunctionWithName:@"render_splats_normalize"];
    ctx->pso_splats_normalize = fn_norm ? [device newComputePipelineStateWithFunction:fn_norm error:&error] : nil;
    if (error && fn_norm) log_metal_error(error, "Failed to compile render_splats_normalize");
    error = nil;
    
    /* Hybrid triangle+splat composite kernel */
    id<MTLFunction> fn_composite = [defaultLibrary newFunctionWithName:@"composite_hybrid"];
    ctx->pso_composite_hybrid = fn_composite ? [device newComputePipelineStateWithFunction:fn_composite error:&error] : nil;
    if (error && fn_composite) log_metal_error(error, "Failed to compile composite_hybrid");
    error = nil;
    
    if (!ctx->pso_rasterize || !ctx->pso_blur || !ctx->pso_splats || !ctx->pso_sobel || !ctx->pso_bgra_rgb || !ctx->pso_frame_diff) {
        qtsq_metal_destroy(ctx);
        return NULL;
    }
    
    /* Phase 10 splat pipeline */
    if (ctx->pso_region_classify && ctx->pso_splat_color_sample) {
        fprintf(stderr, "[QTSQ Metal] Phase 10 Pure Splat pipeline ready.\n");
    }
    
    fprintf(stderr, "[QTSQ Metal] Initialized GPU Compute Engine correctly (%s).\n", [[device name] UTF8String]);
    return ctx;
}

void qtsq_metal_destroy(qtsq_metal_ctx_t *ctx) {
    if (!ctx) return;
    /* M18 fix: ARC id<> fields held in a malloc'd struct are NOT released
     * by ARC when free() is called. Nil them out so ARC releases each one
     * (MTLDevice, commandQueue, all pipeline states, persistent buffers). */
    ctx->device = nil;
    ctx->commandQueue = nil;
    ctx->pso_rasterize = nil;
    ctx->pso_blur = nil;
    ctx->pso_splats = nil;
    ctx->pso_sobel = nil;
    ctx->pso_bgra_rgb = nil;
    ctx->pso_frame_diff = nil;
    ctx->pso_region_classify = nil;
    ctx->pso_jfa_clear = nil;
    ctx->pso_jfa_seed = nil;
    ctx->pso_jfa_step = nil;
    ctx->pso_delaunay_extract = nil;
    ctx->pso_triangle_color_fit = nil;
    ctx->pso_splat_fit_regions = nil;
    ctx->pso_splat_color_sample = nil;
    ctx->pso_splats_stamp = nil;
    ctx->pso_splats_normalize = nil;
    ctx->pso_composite_hybrid = nil;
    ctx->vr_splats = nil;
    ctx->vr_accum_r = nil;
    ctx->vr_accum_g = nil;
    ctx->vr_accum_b = nil;
    ctx->vr_accum_w = nil;
    free(ctx);
}

// ══════════════════════════════════════════
// Rasterization Dispatch
// ══════════════════════════════════════════
int qtsq_metal_rasterize(qtsq_metal_ctx_t *ctx,
                         const tri_vertex_t *verts, uint32_t nv,
                         const tri_face_t *faces, uint32_t nf,
                         uint32_t w, uint32_t h, uint8_t ch,
                         uint8_t *output, uint8_t *blur_map, const uint8_t *tier_mask) {
    if (!ctx || !ctx->pso_rasterize) return -1;
    if (nf == 0 || nv == 0 || w == 0 || h == 0) return 0;
    
    // Naive gather compute shader is O(P*F). Fallback to fast CPU scatter if scene is too complex.
    if ((uint64_t)w * h * nf > 500000000ULL) return -1;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_rasterize];
        
        // Safe Shared Memory Buffers (copies, because malloc pointers are not 4096-page aligned)
        // M13 fix: use size_t for buffer sizes to prevent uint32 overflow.
        size_t out_len = (size_t)w * (size_t)h * (size_t)ch;
        size_t blur_len = (size_t)w * (size_t)h;
        id<MTLBuffer> b_verts = [ctx->device newBufferWithBytes:verts length:nv * sizeof(tri_vertex_t) options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_faces = [ctx->device newBufferWithBytes:faces length:nf * sizeof(tri_face_t) options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_out = [ctx->device newBufferWithLength:out_len options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_blur = [ctx->device newBufferWithLength:blur_len options:MTLResourceStorageModeShared];
        
        id<MTLBuffer> b_tier = nil;
        if (tier_mask) {
            b_tier = [ctx->device newBufferWithBytes:tier_mask length:w * h options:MTLResourceStorageModeShared];
        } else {
            b_tier = [ctx->device newBufferWithLength:w * h options:MTLResourceStorageModeShared];
            memset([b_tier contents], 2, w * h);
        }
        
        [encoder setBuffer:b_verts offset:0 atIndex:0];
        [encoder setBuffer:b_faces offset:0 atIndex:1];
        [encoder setBuffer:b_out offset:0 atIndex:2];
        [encoder setBuffer:b_blur offset:0 atIndex:3];
        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:4];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:5];
        uint32_t cch = ch;
        [encoder setBytes:&cch length:sizeof(uint32_t) atIndex:6];
        [encoder setBytes:&nf length:sizeof(uint32_t) atIndex:7];
        [encoder setBuffer:b_tier offset:0 atIndex:8];
        
        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tgSize = ctx->pso_rasterize.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt(tgSize);
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            NSLog(@"[QTSQ Metal] Kernel aborted: %@", commandBuffer.error.localizedDescription);
            return -1;
        }
        
        // Copy back outputs
        memcpy(output, [b_out contents], w * h * ch);
        if (blur_map) memcpy(blur_map, [b_blur contents], w * h);
    }
    return 0;
}

// ══════════════════════════════════════════
// Render Splats Dispatch
// ══════════════════════════════════════════
int qtsq_metal_render_splats(qtsq_metal_ctx_t *ctx,
                             const splat2d_t *splats, uint32_t count,
                             uint32_t w, uint32_t h, uint8_t ch,
                             uint8_t *output, const uint8_t *fg_mask) {
    if (!ctx || !ctx->pso_splats) return -1;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_splats];
        
        id<MTLBuffer> b_splats = [ctx->device newBufferWithBytes:splats length:count * sizeof(splat2d_t) options:MTLResourceStorageModeShared];
        
        /* CRITICAL: Pre-populate with existing triangle-rendered output so splats
         * alpha-blend ON TOP of the base layer instead of replacing with black. */
        size_t out_bytes = (size_t)w * h * ch;
        id<MTLBuffer> b_out = [ctx->device newBufferWithBytes:output length:out_bytes options:MTLResourceStorageModeShared];
        
        id<MTLBuffer> b_fg = nil;
        if (fg_mask) {
            b_fg = [ctx->device newBufferWithBytes:fg_mask length:w * h options:MTLResourceStorageModeShared];
        } else {
            /* Must allocate a FULL-SIZE zero buffer — the shader reads fg_mask[y*w+x]
             * for every pixel. A 1-byte dummy causes out-of-bounds GPU reads that
             * produce black horizontal bars from GPU memory garbage. */
            b_fg = [ctx->device newBufferWithLength:w * h options:MTLResourceStorageModeShared];
            memset([b_fg contents], 0, w * h);
        }
        
        [encoder setBuffer:b_splats offset:0 atIndex:0];
        [encoder setBuffer:b_out offset:0 atIndex:1];
        [encoder setBuffer:b_fg offset:0 atIndex:2];

        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:3];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:4];
        uint32_t cch = ch;
        [encoder setBytes:&cch length:sizeof(uint32_t) atIndex:5];
        [encoder setBytes:&count length:sizeof(uint32_t) atIndex:6];
        
        NSUInteger tgSize = ctx->pso_splats.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt((double)tgSize);
        if (wd < 1) wd = 1;
        if (wd > 32) wd = 32;  /* Safety cap */
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        MTLSize gridSize = MTLSizeMake((w + wd - 1) / wd, (h + wd - 1) / wd, 1);
        
        [encoder dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        /* Check for GPU errors — fall back to CPU if command buffer was aborted */
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            fprintf(stderr, "[QTSQ Metal] Splat render GPU error — falling back to CPU\n");
            return -1;
        }
        
        memcpy(output, [b_out contents], out_bytes);
    }
    return 0;
}

// ══════════════════════════════════════════
// Fast Splat-Centric Render (Two-Pass) for Video
// Uses PERSISTENT GPU buffers — allocated once, reused every frame.
// ══════════════════════════════════════════
int qtsq_metal_render_splats_fast(qtsq_metal_ctx_t *ctx,
                                   const splat2d_t *splats, uint32_t count,
                                   uint32_t w, uint32_t h, uint8_t ch,
                                   uint8_t *output) {
    if (!ctx || !ctx->pso_splats_stamp || !ctx->pso_splats_normalize) {
        return qtsq_metal_render_splats(ctx, splats, count, w, h, ch, output, NULL);
    }
    
    @autoreleasepool {
        /* M13 fix: use size_t for pixel_count to prevent uint32 overflow. */
        size_t pixel_count = (size_t)w * (size_t)h;
        size_t out_bytes = pixel_count * ch;
        size_t accum_bytes = pixel_count * sizeof(uint32_t);
        size_t splat_bytes = (size_t)count * sizeof(splat2d_t);

        /* ── Allocate persistent buffers on first call or dimension change ── */
        if (ctx->vr_pixel_count != (uint32_t)pixel_count || ctx->vr_channels != ch) {
            ctx->vr_accum_r = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_g = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_b = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_w = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_output  = [ctx->device newBufferWithLength:out_bytes   options:MTLResourceStorageModeShared];
            ctx->vr_pixel_count = pixel_count;
            ctx->vr_channels = ch;
            fprintf(stderr, "[QTSQ Metal] Allocated persistent video buffers: %u×%u (%zu bytes each)\n",
                    w, h, accum_bytes);
        }
        
        if (ctx->vr_splat_capacity < count) {
            ctx->vr_splats = [ctx->device newBufferWithLength:splat_bytes options:MTLResourceStorageModeShared];
            ctx->vr_splat_capacity = count;
            fprintf(stderr, "[QTSQ Metal] Allocated persistent splat buffer: %u splats (%zu bytes)\n",
                    count, splat_bytes);
        }
        
        /* ── Per-frame: only update splat data and zero accumulators ── */
        memcpy([ctx->vr_splats contents], splats, splat_bytes);
        memset([ctx->vr_accum_r contents], 0, accum_bytes);
        memset([ctx->vr_accum_g contents], 0, accum_bytes);
        memset([ctx->vr_accum_b contents], 0, accum_bytes);
        memset([ctx->vr_accum_w contents], 0, accum_bytes);
        
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        
        /* ── Pass 1: Stamp (1D dispatch, 1 thread per splat) ── */
        {
            id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
            [encoder setComputePipelineState:ctx->pso_splats_stamp];
            
            [encoder setBuffer:ctx->vr_splats  offset:0 atIndex:0];
            [encoder setBuffer:ctx->vr_accum_r offset:0 atIndex:1];
            [encoder setBuffer:ctx->vr_accum_g offset:0 atIndex:2];
            [encoder setBuffer:ctx->vr_accum_b offset:0 atIndex:3];
            [encoder setBuffer:ctx->vr_accum_w offset:0 atIndex:4];
            [encoder setBytes:&w length:sizeof(uint32_t) atIndex:5];
            [encoder setBytes:&h length:sizeof(uint32_t) atIndex:6];
            [encoder setBytes:&count length:sizeof(uint32_t) atIndex:7];
            
            NSUInteger tgSize = ctx->pso_splats_stamp.maxTotalThreadsPerThreadgroup;
            if (tgSize > 256) tgSize = 256;
            MTLSize threadgroupSize = MTLSizeMake(tgSize, 1, 1);
            MTLSize gridSize = MTLSizeMake((count + tgSize - 1) / tgSize, 1, 1);
            
            [encoder dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
            [encoder endEncoding];
        }
        
        /* ── Pass 2: Normalize (2D dispatch, 1 thread per pixel) ── */
        {
            id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
            [encoder setComputePipelineState:ctx->pso_splats_normalize];
            
            [encoder setBuffer:ctx->vr_accum_r offset:0 atIndex:0];
            [encoder setBuffer:ctx->vr_accum_g offset:0 atIndex:1];
            [encoder setBuffer:ctx->vr_accum_b offset:0 atIndex:2];
            [encoder setBuffer:ctx->vr_accum_w offset:0 atIndex:3];
            [encoder setBuffer:ctx->vr_output  offset:0 atIndex:4];
            [encoder setBytes:&w  length:sizeof(uint32_t) atIndex:5];
            [encoder setBytes:&h  length:sizeof(uint32_t) atIndex:6];
            uint32_t cch = ch;
            [encoder setBytes:&cch length:sizeof(uint32_t) atIndex:7];
            
            NSUInteger wd = 16;
            MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
            MTLSize gridSize = MTLSizeMake((w + wd - 1) / wd, (h + wd - 1) / wd, 1);
            
            [encoder dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
            [encoder endEncoding];
        }
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            fprintf(stderr, "[QTSQ Metal] Fast splat render GPU error\n");
            return -1;
        }
        
        memcpy(output, [ctx->vr_output contents], out_bytes);
    }
    return 0;
}

// ══════════════════════════════════════════
// Sobel Detection Dispatch
// ══════════════════════════════════════════
int qtsq_metal_sobel(qtsq_metal_ctx_t *ctx,
                     const uint8_t *pixels, float *gradient,
                     uint32_t w, uint32_t h, uint8_t ch) {
    if (!ctx || !ctx->pso_sobel) return -1;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_sobel];
        
        id<MTLBuffer> b_pix = [ctx->device newBufferWithBytes:pixels length:w * h * ch options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_grad = [ctx->device newBufferWithLength:w * h * sizeof(float) options:MTLResourceStorageModeShared];
        
        [encoder setBuffer:b_pix offset:0 atIndex:0];
        [encoder setBuffer:b_grad offset:0 atIndex:1];
        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:2];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:3];
        uint32_t cch = ch;
        [encoder setBytes:&cch length:sizeof(uint32_t) atIndex:4];
        
        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tgSize = ctx->pso_sobel.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt(tgSize);
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        memcpy(gradient, [b_grad contents], w * h * sizeof(float));
    }
    return 0;
}

// ══════════════════════════════════════════
// Box Blur Dispatch
// ══════════════════════════════════════════
int qtsq_metal_blur(qtsq_metal_ctx_t *ctx,
                    const uint8_t *input, uint8_t *output,
                    uint32_t w, uint32_t h, uint8_t ch,
                    const uint8_t *blur_map) {
    if (!ctx || !ctx->pso_blur) return -1;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_blur];
        
        size_t pixel_bytes = (size_t)w * h * ch;
        id<MTLBuffer> b_in = [ctx->device newBufferWithBytes:input length:pixel_bytes options:MTLResourceStorageModeShared];
        /* CRITICAL: Pre-populate with input so unblurred pixels retain their original
         * values instead of becoming black (the blur kernel only writes blurred pixels). */
        id<MTLBuffer> b_out = [ctx->device newBufferWithBytes:input length:pixel_bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_map = [ctx->device newBufferWithBytes:blur_map length:w * h options:MTLResourceStorageModeShared];
        
        [encoder setBuffer:b_in offset:0 atIndex:0];
        [encoder setBuffer:b_out offset:0 atIndex:1];
        [encoder setBuffer:b_map offset:0 atIndex:2];
        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:3];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:4];
        uint32_t cch = ch;
        [encoder setBytes:&cch length:sizeof(uint32_t) atIndex:5];
        
        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tgSize = ctx->pso_blur.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt(tgSize);
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            fprintf(stderr, "[QTSQ Metal] Blur GPU error — falling back to CPU\n");
            return -1;
        }
        
        memcpy(output, [b_out contents], pixel_bytes);
    }
    return 0;
}

// ══════════════════════════════════════════
// BGRA to RGB Dispatch
// ══════════════════════════════════════════
int qtsq_metal_bgra_to_rgb(qtsq_metal_ctx_t *ctx,
                           const uint8_t *bgra, uint8_t *rgb,
                           uint32_t w, uint32_t h, uint32_t stride) {
    if (!ctx || !ctx->pso_bgra_rgb) return -1;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_bgra_rgb];
        
        id<MTLBuffer> b_bgra = [ctx->device newBufferWithBytes:bgra length:stride * h options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_rgb = [ctx->device newBufferWithLength:w * h * 3 options:MTLResourceStorageModeShared];
        
        [encoder setBuffer:b_bgra offset:0 atIndex:0];
        [encoder setBuffer:b_rgb offset:0 atIndex:1];
        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:2];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:3];
        [encoder setBytes:&stride length:sizeof(uint32_t) atIndex:4];
        
        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tgSize = ctx->pso_bgra_rgb.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt(tgSize);
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        memcpy(rgb, [b_rgb contents], w * h * 3);
    }
    return 0;
}

// ══════════════════════════════════════════
// Frame Diff for P-frame Encoding
// ══════════════════════════════════════════
int qtsq_metal_frame_diff(qtsq_metal_ctx_t *ctx,
                           const uint8_t *current, const uint8_t *reference,
                           uint8_t *delta, float *change_ratio,
                           uint32_t w, uint32_t h, uint8_t ch, uint32_t threshold) {
    if (!ctx || !ctx->pso_frame_diff) return -1;
    
    size_t pixel_count = (size_t)w * h;
    size_t buffer_size = pixel_count * ch;
    
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:ctx->pso_frame_diff];
        
        id<MTLBuffer> b_current = [ctx->device newBufferWithBytes:current length:buffer_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_ref = [ctx->device newBufferWithBytes:reference length:buffer_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_delta = [ctx->device newBufferWithLength:buffer_size options:MTLResourceStorageModeShared];
        
        // Atomic counter for changed pixels
        uint32_t zero = 0;
        id<MTLBuffer> b_counter = [ctx->device newBufferWithBytes:&zero length:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        
        [encoder setBuffer:b_current offset:0 atIndex:0];
        [encoder setBuffer:b_ref offset:0 atIndex:1];
        [encoder setBuffer:b_delta offset:0 atIndex:2];
        [encoder setBuffer:b_counter offset:0 atIndex:3];
        [encoder setBytes:&w length:sizeof(uint32_t) atIndex:4];
        [encoder setBytes:&h length:sizeof(uint32_t) atIndex:5];
        
        uint32_t ch32 = ch;
        [encoder setBytes:&ch32 length:sizeof(uint32_t) atIndex:6];
        [encoder setBytes:&threshold length:sizeof(uint32_t) atIndex:7];
        
        MTLSize gridSize = MTLSizeMake(w, h, 1);
        NSUInteger tgSize = ctx->pso_frame_diff.maxTotalThreadsPerThreadgroup;
        NSUInteger wd = (NSUInteger)sqrt(tgSize);
        MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
        
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        memcpy(delta, [b_delta contents], buffer_size);
        
        // Read back changed pixel count
        uint32_t *counter_ptr = (uint32_t *)[b_counter contents];
        if (change_ratio) {
            *change_ratio = (float)(*counter_ptr) / (float)pixel_count;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 9: Region Classification dispatch
 * ══════════════════════════════════════════════════════════════ */
int qtsq_metal_classify_regions(qtsq_metal_ctx_t *ctx,
                                 const float *gradient, uint8_t *mask,
                                 uint32_t w, uint32_t h, float threshold) {
    if (!ctx || !ctx->pso_region_classify) return -1;
    
    size_t pixel_count = (size_t)w * h;
    @autoreleasepool {
        id<MTLBuffer> b_grad = [ctx->device newBufferWithBytes:gradient length:pixel_count * sizeof(float) options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_mask = [ctx->device newBufferWithBytes:mask length:pixel_count options:MTLResourceStorageModeShared];
        
        id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx->pso_region_classify];
        [enc setBuffer:b_grad offset:0 atIndex:0];
        [enc setBuffer:b_mask offset:0 atIndex:1];
        [enc setBytes:&w length:sizeof(w) atIndex:2];
        [enc setBytes:&h length:sizeof(h) atIndex:3];
        [enc setBytes:&threshold length:sizeof(threshold) atIndex:4];
        
        MTLSize grid = MTLSizeMake(w, h, 1);
        MTLSize tg = MTLSizeMake(16, 16, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        
        memcpy(mask, [b_mask contents], pixel_count);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 9: GPU Delaunay (JFA + Extract)
 * ══════════════════════════════════════════════════════════════ */
int qtsq_metal_gpu_delaunay(qtsq_metal_ctx_t *ctx,
                             const float *vertex_pos, uint32_t num_vertices,
                             uint32_t *tris_out, uint32_t *num_tris_out,
                             uint32_t max_tris, uint32_t w, uint32_t h) {
    if (!ctx || !ctx->pso_jfa_clear || !ctx->pso_jfa_seed || !ctx->pso_jfa_step || !ctx->pso_delaunay_extract) return -1;
    
    size_t pixel_count = (size_t)w * h;
    
    @autoreleasepool {
        /* Allocate JFA textures (two for ping-pong) */
        size_t seed_size = pixel_count * sizeof(int32_t) * 2; /* int2 = 2 × int32 */
        id<MTLBuffer> b_seed_a = [ctx->device newBufferWithLength:seed_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_seed_b = [ctx->device newBufferWithLength:seed_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_verts = [ctx->device newBufferWithBytes:vertex_pos length:num_vertices * sizeof(float) * 2 options:MTLResourceStorageModeShared];
        
        MTLSize pixel_grid = MTLSizeMake(w, h, 1);
        MTLSize tg = MTLSizeMake(16, 16, 1);
        
        /* Step 1a: JFA Clear — set all pixels to (-1,-1) */
        {
            id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_jfa_clear];
            [enc setBuffer:b_seed_a offset:0 atIndex:0];
            [enc setBytes:&w length:sizeof(w) atIndex:1];
            [enc setBytes:&h length:sizeof(h) atIndex:2];
            [enc dispatchThreads:pixel_grid threadsPerThreadgroup:tg];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
        }
        
        /* Step 1b: JFA Seed — write vertex positions (1 thread per vertex) */
        {
            id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_jfa_seed];
            [enc setBuffer:b_seed_a offset:0 atIndex:0];
            [enc setBuffer:b_verts offset:0 atIndex:1];
            [enc setBytes:&w length:sizeof(w) atIndex:2];
            [enc setBytes:&h length:sizeof(h) atIndex:3];
            [enc setBytes:&num_vertices length:sizeof(num_vertices) atIndex:4];
            MTLSize vert_grid = MTLSizeMake(num_vertices, 1, 1);
            MTLSize vert_tg = MTLSizeMake(256, 1, 1);
            [enc dispatchThreads:vert_grid threadsPerThreadgroup:vert_tg];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
        }
        
        /* Step 2: JFA Steps — log2(max(W,H)) passes */
        uint32_t max_dim = w > h ? w : h;
        int step = 1;
        while (step < (int)max_dim) step <<= 1;
        step >>= 1;
        
        id<MTLBuffer> src = b_seed_a;
        id<MTLBuffer> dst = b_seed_b;
        
        while (step >= 1) {
            id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_jfa_step];
            [enc setBuffer:src offset:0 atIndex:0];
            [enc setBuffer:dst offset:0 atIndex:1];
            [enc setBytes:&w length:sizeof(w) atIndex:2];
            [enc setBytes:&h length:sizeof(h) atIndex:3];
            [enc setBytes:&step length:sizeof(step) atIndex:4];
            [enc dispatchThreads:pixel_grid threadsPerThreadgroup:tg];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            
            /* Ping-pong swap */
            id<MTLBuffer> tmp = src;
            src = dst;
            dst = tmp;
            step >>= 1;
        }
        /* After all passes, `src` contains the final Voronoi diagram */
        
        /* Step 3: Delaunay Extract — find triangles from Voronoi */
        uint32_t zero = 0;
        id<MTLBuffer> b_tri_count = [ctx->device newBufferWithBytes:&zero length:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_tris = [ctx->device newBufferWithLength:max_tris * sizeof(uint32_t) * 3 options:MTLResourceStorageModeShared];
        
        MTLSize extract_grid = MTLSizeMake(w / 2, h / 2, 1);
        {
            id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_delaunay_extract];
            [enc setBuffer:src offset:0 atIndex:0];
            [enc setBuffer:b_verts offset:0 atIndex:1];
            [enc setBuffer:b_tri_count offset:0 atIndex:2];
            [enc setBuffer:b_tris offset:0 atIndex:3];
            [enc setBytes:&w length:sizeof(w) atIndex:4];
            [enc setBytes:&h length:sizeof(h) atIndex:5];
            [enc setBytes:&num_vertices length:sizeof(num_vertices) atIndex:6];
            [enc setBytes:&max_tris length:sizeof(max_tris) atIndex:7];
            [enc dispatchThreads:extract_grid threadsPerThreadgroup:tg];
            [enc endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
        }
        
        /* Read back results */
        uint32_t *count_ptr = (uint32_t *)[b_tri_count contents];
        uint32_t tri_count = *count_ptr;
        if (tri_count > max_tris) tri_count = max_tris;
        
        memcpy(tris_out, [b_tris contents], tri_count * sizeof(uint32_t) * 3);
        if (num_tris_out) *num_tris_out = tri_count;
        
        fprintf(stderr, "[QTSQ Metal] GPU Delaunay: %u vertices → %u triangles\n", num_vertices, tri_count);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 9: Triangle Color Fitting dispatch
 * ══════════════════════════════════════════════════════════════ */
int qtsq_metal_fit_triangle_colors(qtsq_metal_ctx_t *ctx,
                                    const float *vertex_pos, uint32_t num_vertices,
                                    const uint8_t *source, uint8_t *colors_out,
                                    uint32_t w, uint32_t h, uint8_t ch) {
    if (!ctx || !ctx->pso_triangle_color_fit) return -1;
    
    @autoreleasepool {
        size_t src_size = (size_t)w * h * ch;
        id<MTLBuffer> b_verts = [ctx->device newBufferWithBytes:vertex_pos length:num_vertices * sizeof(float) * 2 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_src = [ctx->device newBufferWithBytes:source length:src_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_colors = [ctx->device newBufferWithLength:num_vertices * 3 options:MTLResourceStorageModeShared];
        uint32_t ch32 = ch;
        
        id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx->pso_triangle_color_fit];
        [enc setBuffer:b_verts offset:0 atIndex:0];
        [enc setBuffer:b_src offset:0 atIndex:1];
        [enc setBuffer:b_colors offset:0 atIndex:2];
        [enc setBytes:&w length:sizeof(w) atIndex:3];
        [enc setBytes:&h length:sizeof(h) atIndex:4];
        [enc setBytes:&ch32 length:sizeof(ch32) atIndex:5];
        [enc setBytes:&num_vertices length:sizeof(num_vertices) atIndex:6];
        
        MTLSize grid = MTLSizeMake(num_vertices, 1, 1);
        MTLSize tg = MTLSizeMake(256, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        
        memcpy(colors_out, [b_colors contents], num_vertices * 3);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 9: Splat Fitting dispatch
 * ══════════════════════════════════════════════════════════════ */
int qtsq_metal_fit_splats(qtsq_metal_ctx_t *ctx,
                           const float *splat_pos, uint32_t num_splats,
                           const uint8_t *source, const uint8_t *mask,
                           const float *gradient,
                           uint8_t *splats_out, uint32_t *num_active_out,
                           uint32_t w, uint32_t h, uint8_t ch) {
    if (!ctx || !ctx->pso_splat_fit_regions) return -1;
    
    size_t pixel_count = (size_t)w * h;
    /* SplatOutput struct: float2 pos + uchar3 color + uchar radius + uchar alpha = 12 bytes */
    size_t splat_out_size = num_splats * 12;
    
    @autoreleasepool {
        id<MTLBuffer> b_pos = [ctx->device newBufferWithBytes:splat_pos length:num_splats * sizeof(float) * 2 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_src = [ctx->device newBufferWithBytes:source length:pixel_count * ch options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_mask = [ctx->device newBufferWithBytes:mask length:pixel_count options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_grad = [ctx->device newBufferWithBytes:gradient length:pixel_count * sizeof(float) options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_out = [ctx->device newBufferWithLength:splat_out_size options:MTLResourceStorageModeShared];
        uint32_t ch32 = ch;
        
        id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx->pso_splat_fit_regions];
        [enc setBuffer:b_pos offset:0 atIndex:0];
        [enc setBuffer:b_src offset:0 atIndex:1];
        [enc setBuffer:b_mask offset:0 atIndex:2];
        [enc setBuffer:b_grad offset:0 atIndex:3];
        [enc setBuffer:b_out offset:0 atIndex:4];
        [enc setBytes:&w length:sizeof(w) atIndex:5];
        [enc setBytes:&h length:sizeof(h) atIndex:6];
        [enc setBytes:&ch32 length:sizeof(ch32) atIndex:7];
        [enc setBytes:&num_splats length:sizeof(num_splats) atIndex:8];
        
        MTLSize grid = MTLSizeMake(num_splats, 1, 1);
        MTLSize tg = MTLSizeMake(256, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        
        memcpy(splats_out, [b_out contents], splat_out_size);
        
        /* Count active splats (alpha > 0) */
        if (num_active_out) {
            uint32_t active = 0;
            uint8_t *buf = (uint8_t *)[b_out contents];
            for (uint32_t i = 0; i < num_splats; i++) {
                /* alpha is at offset 11 in the 12-byte struct */
                if (buf[i * 12 + 11] > 0) active++;
            }
            *num_active_out = active;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  Phase 10: Splat Color Sampling (GPU)
 *  For each splat, averages source pixels within its radius.
 *  splat_packed: 5 bytes per splat [uint16 x][uint16 y][uint8 radius]
 *  colors_out: 3 bytes per splat [r][g][b]
 * ══════════════════════════════════════════════════════════════ */
int qtsq_metal_splat_color_sample(qtsq_metal_ctx_t *ctx,
                                   const uint8_t *splat_packed, uint32_t num_splats,
                                   const uint8_t *source, uint8_t *colors_out,
                                   uint32_t w, uint32_t h, uint8_t ch) {
    if (!ctx || !ctx->pso_splat_color_sample || num_splats == 0) return -1;
    
    size_t pixel_data_size = (size_t)w * h * ch;
    
    @autoreleasepool {
        id<MTLBuffer> b_splats = [ctx->device newBufferWithBytes:splat_packed length:num_splats * 5 options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_source = [ctx->device newBufferWithBytes:source length:pixel_data_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_colors = [ctx->device newBufferWithLength:num_splats * 3 options:MTLResourceStorageModeShared];
        
        uint32_t w32 = w, h32 = h, ch32 = (uint32_t)ch, ns32 = num_splats;
        
        id<MTLCommandBuffer> cmd = [ctx->commandQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:ctx->pso_splat_color_sample];
        [enc setBuffer:b_splats offset:0 atIndex:0];
        [enc setBuffer:b_source offset:0 atIndex:1];
        [enc setBuffer:b_colors offset:0 atIndex:2];
        [enc setBytes:&w32 length:sizeof(w32) atIndex:3];
        [enc setBytes:&h32 length:sizeof(h32) atIndex:4];
        [enc setBytes:&ch32 length:sizeof(ch32) atIndex:5];
        [enc setBytes:&ns32 length:sizeof(ns32) atIndex:6];
        
        MTLSize grid = MTLSizeMake(num_splats, 1, 1);
        MTLSize tg = MTLSizeMake(256, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        
        /* Read back RGB colors */
        memcpy(colors_out, [b_colors contents], num_splats * 3);
    }
    return 0;
}

// ══════════════════════════════════════════
// Hybrid Triangle + Splat Render (Video)
// 3-pass GPU pipeline with persistent buffers:
//   Pass 1: Rasterize triangles (compute kernel)
//   Pass 2: Stamp splats (compute kernel)
//   Pass 3: Composite (blend splat zones over triangle base)
// ══════════════════════════════════════════
int qtsq_metal_render_hybrid(qtsq_metal_ctx_t *ctx,
                              const tri_vertex_t *verts, uint32_t num_verts,
                              const tri_face_t *faces, uint32_t num_faces,
                              const splat2d_t *splats, uint32_t num_splats,
                              uint32_t w, uint32_t h, uint8_t ch,
                              uint8_t *output) {
    if (!ctx || !ctx->pso_rasterize || !ctx->pso_composite_hybrid) {
        fprintf(stderr, "[QTSQ Metal] Hybrid render: missing PSOs\n");
        return -1;
    }
    
    @autoreleasepool {
        uint32_t pixel_count = w * h;
        size_t out_bytes = (size_t)pixel_count * ch;
        size_t accum_bytes = (size_t)pixel_count * sizeof(uint32_t);
        
        /* ── Allocate/resize persistent pixel buffers ── */
        if (ctx->vr_pixel_count != pixel_count || ctx->vr_channels != ch) {
            ctx->vr_accum_r = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_g = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_b = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_accum_w = [ctx->device newBufferWithLength:accum_bytes options:MTLResourceStorageModeShared];
            ctx->vr_output  = [ctx->device newBufferWithLength:out_bytes   options:MTLResourceStorageModeShared];
            ctx->hr_tri_output = [ctx->device newBufferWithLength:out_bytes options:MTLResourceStorageModeShared];
            ctx->vr_pixel_count = pixel_count;
            ctx->vr_channels = ch;
            fprintf(stderr, "[QTSQ Metal] Hybrid: allocated pixel buffers %ux%u\n", w, h);
        }
        
        /* ── Allocate/resize persistent geometry buffers ── */
        if (ctx->hr_vert_capacity < num_verts) {
            ctx->hr_vertices = [ctx->device newBufferWithLength:num_verts * sizeof(tri_vertex_t) options:MTLResourceStorageModeShared];
            ctx->hr_vert_capacity = num_verts;
        }
        if (ctx->hr_face_capacity < num_faces) {
            ctx->hr_faces = [ctx->device newBufferWithLength:num_faces * sizeof(tri_face_t) options:MTLResourceStorageModeShared];
            ctx->hr_face_capacity = num_faces;
        }
        if (ctx->vr_splat_capacity < num_splats && num_splats > 0) {
            ctx->vr_splats = [ctx->device newBufferWithLength:num_splats * sizeof(splat2d_t) options:MTLResourceStorageModeShared];
            ctx->vr_splat_capacity = num_splats;
        }
        
        /* ── Per-frame: upload geometry + zero accumulators ── */
        memcpy([ctx->hr_vertices contents], verts, num_verts * sizeof(tri_vertex_t));
        memcpy([ctx->hr_faces contents], faces, num_faces * sizeof(tri_face_t));
        if (num_splats > 0) {
            memcpy([ctx->vr_splats contents], splats, num_splats * sizeof(splat2d_t));
        }
        memset([ctx->hr_tri_output contents], 0, out_bytes);
        memset([ctx->vr_accum_r contents], 0, accum_bytes);
        memset([ctx->vr_accum_g contents], 0, accum_bytes);
        memset([ctx->vr_accum_b contents], 0, accum_bytes);
        memset([ctx->vr_accum_w contents], 0, accum_bytes);
        
        id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
        
        /* ── Pass 1: Triangle Rasterization ── */
        {
            id<MTLComputeCommandEncoder> enc = [commandBuffer computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_rasterize];
            
            [enc setBuffer:ctx->hr_vertices  offset:0 atIndex:0];
            [enc setBuffer:ctx->hr_faces     offset:0 atIndex:1];
            [enc setBuffer:ctx->hr_tri_output offset:0 atIndex:2];
            /* blur_map — just pass tri_output as dummy (unused for non-blur) */
            [enc setBuffer:ctx->hr_tri_output offset:0 atIndex:3];
            [enc setBytes:&w length:sizeof(uint32_t) atIndex:4];
            [enc setBytes:&h length:sizeof(uint32_t) atIndex:5];
            uint32_t cch = ch;
            [enc setBytes:&cch length:sizeof(uint32_t) atIndex:6];
            [enc setBytes:&num_faces length:sizeof(uint32_t) atIndex:7];
            /* tier_mask — pass tri_output as dummy */
            [enc setBuffer:ctx->hr_tri_output offset:0 atIndex:8];
            
            NSUInteger wd = 16;
            MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
            MTLSize gridSize = MTLSizeMake((w + wd - 1) / wd, (h + wd - 1) / wd, 1);
            [enc dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
            [enc endEncoding];
        }
        
        /* ── Pass 2: Splat Stamp (skip if no splats) ── */
        if (num_splats > 0 && ctx->pso_splats_stamp) {
            id<MTLComputeCommandEncoder> enc = [commandBuffer computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_splats_stamp];
            
            [enc setBuffer:ctx->vr_splats  offset:0 atIndex:0];
            [enc setBuffer:ctx->vr_accum_r offset:0 atIndex:1];
            [enc setBuffer:ctx->vr_accum_g offset:0 atIndex:2];
            [enc setBuffer:ctx->vr_accum_b offset:0 atIndex:3];
            [enc setBuffer:ctx->vr_accum_w offset:0 atIndex:4];
            [enc setBytes:&w length:sizeof(uint32_t) atIndex:5];
            [enc setBytes:&h length:sizeof(uint32_t) atIndex:6];
            [enc setBytes:&num_splats length:sizeof(uint32_t) atIndex:7];
            
            NSUInteger tgSize = ctx->pso_splats_stamp.maxTotalThreadsPerThreadgroup;
            if (tgSize > 256) tgSize = 256;
            MTLSize threadgroupSize = MTLSizeMake(tgSize, 1, 1);
            MTLSize gridSize = MTLSizeMake((num_splats + tgSize - 1) / tgSize, 1, 1);
            [enc dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
            [enc endEncoding];
        }
        
        /* ── Pass 3: Composite (blend splat zones over triangle base) ── */
        {
            id<MTLComputeCommandEncoder> enc = [commandBuffer computeCommandEncoder];
            [enc setComputePipelineState:ctx->pso_composite_hybrid];
            
            [enc setBuffer:ctx->hr_tri_output offset:0 atIndex:0];
            [enc setBuffer:ctx->vr_accum_r    offset:0 atIndex:1];
            [enc setBuffer:ctx->vr_accum_g    offset:0 atIndex:2];
            [enc setBuffer:ctx->vr_accum_b    offset:0 atIndex:3];
            [enc setBuffer:ctx->vr_accum_w    offset:0 atIndex:4];
            [enc setBuffer:ctx->vr_output     offset:0 atIndex:5];
            [enc setBytes:&w length:sizeof(uint32_t) atIndex:6];
            [enc setBytes:&h length:sizeof(uint32_t) atIndex:7];
            uint32_t cch = ch;
            [enc setBytes:&cch length:sizeof(uint32_t) atIndex:8];
            
            NSUInteger wd = 16;
            MTLSize threadgroupSize = MTLSizeMake(wd, wd, 1);
            MTLSize gridSize = MTLSizeMake((w + wd - 1) / wd, (h + wd - 1) / wd, 1);
            [enc dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
            [enc endEncoding];
        }
        
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
        
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            fprintf(stderr, "[QTSQ Metal] Hybrid render GPU error\n");
            return -1;
        }
        
        memcpy(output, [ctx->vr_output contents], out_bytes);
    }
    return 0;
}

#endif /* __APPLE__ */
