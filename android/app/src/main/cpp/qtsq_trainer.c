/*
 * QTSQ Phase D — GPU-First Vulkan Surfel Trainer (impl)
 *
 * Port of prototype/live_surfel/{surfel_render,surfel_loss,surfel_backward,
 * surfel_optim,surfel_online}.c to Vulkan compute, running on the Android
 * device's GPU. The optimizer runs on a background thread; the GL thread
 * reads the surfel host mirror for live preview.
 *
 * Author: Haruhito
 */

#include "qtsq_trainer.h"
#include "qtsq_surfel.h"
#include "qtsq_keystore.h"
#include "qtsq_shader_model.h"
#include "qtsq_tsdf.h"

#ifdef __ANDROID__

#include <vulkan/vulkan.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

#define TLOG(...) __android_log_print(ANDROID_LOG_INFO, "QTSQ_Trainer", __VA_ARGS__)
#define TERR(...) __android_log_print(ANDROID_LOG_ERROR, "QTSQ_Trainer", __VA_ARGS__)

/* Embedded SPIR-V shaders */
#include "shaders/surfel_forward_spv.h"
#include "shaders/surfel_loss_spv.h"
#include "shaders/surfel_backward_spv.h"
#include "shaders/surfel_adam_spv.h"

/* vec3 matching the host layout */
typedef struct { float x, y, z; } vec3_host;

/* ── Trainer context ── */
struct qtsq_trainer {
    /* Vulkan state (shared device from vk_compute_ctx) */
    VkDevice device;
    VkPhysicalDevice physical_device;
    VkQueue compute_queue;
    uint32_t queue_family_index;
    VkCommandPool cmd_pool;
    VkDescriptorPool desc_pool;
    VkPhysicalDeviceMemoryProperties mem_props;

    /* Pipelines */
    VkPipelineLayout pipe_layout_fwd;
    VkPipeline       pipe_fwd;
    VkPipelineLayout pipe_layout_loss;
    VkPipeline       pipe_loss;
    VkPipelineLayout pipe_layout_bwd;
    VkPipeline       pipe_bwd;
    VkPipelineLayout pipe_layout_adam;
    VkPipeline       pipe_adam;

    /* Descriptor set layouts */
    VkDescriptorSetLayout dsl_fwd;
    VkDescriptorSetLayout dsl_loss;
    VkDescriptorSetLayout dsl_bwd;
    VkDescriptorSetLayout dsl_adam;

    /* Buffers */
    VkBuffer surfel_buf, grad_buf, adam_m_buf, adam_v_buf;
    VkBuffer pred_buf, target_buf, dLdpred_buf, loss_buf;
    VkBuffer depth_buf, normal_buf, alpha_buf, ncgrad_buf;
    VkDeviceMemory surfel_mem, grad_mem, adam_m_mem, adam_v_mem;
    VkDeviceMemory pred_mem, target_mem, dLdpred_mem, loss_mem;
    VkDeviceMemory depth_mem, normal_mem, alpha_mem, ncgrad_mem;

    /* Host mirrors */
    float *host_surfels;    /* SURFEL_STRIDE_FLOATS * max_surfels */
    uint32_t *host_grads;   /* GRAD_STRIDE_FLOATS * max_surfels (uint bit-patterns) */
    float *host_adam_m;
    float *host_adam_v;
    vec3_host *host_pred;
    vec3_host *host_target;
    vec3_host *host_dLdpred;
    float host_loss;

    /* Surfel model state */
    int num_surfels;
    int cap_surfels;
    int max_surfels;        /* allocation cap */

    /* Training state */
    int step_count;
    float base_lr;
    float lam_n, lam_c;     /* Phase E: normal-consistency / depth-convergence weights */
    float *grad_accum;      /* EMA of |d_center| per surfel for density control */

    /* Phase E/G extras */
    float *host_alpha;      /* W*H opacity map readback (coverage, NC gate) */
    float coverage_pct;     /* % pixels with alpha>0.5 in the last sampled frame */
    float angle_spread_deg; /* max pairwise keyframe view-angle spread */
    float step_sleep_ms;    /* current inter-step sleep (adaptive thermal budget) */
    double last_step_ms;    /* measured duration of trainer_step (ms) */

    /* Attached capture session (not owned) */
    qtsq_capture_t *cap;

    /* Vulkan device reference (borrowed from global ctx) */
    int owns_device;

    /* ── Threading (Phase D §3): one writer (train thread), readers see the
     * snapshot. 'lock' guards host_surfels/host_loss/step_count and the
     * snapshot buffer. ── */
    pthread_mutex_t lock;
    pthread_t train_thread;
    volatile int loop_running;   /* thread alive?  */
    volatile int loop_stop;      /* stop requested */
    float *snap_surfels;         /* GL-visible mirror, max_surfels*13 */
    int    snap_count;
};

/* ── Push constant structs (must match shaders) ── */

typedef struct {
    float cam_pos[4];
    float cam_right[4];
    float cam_up[4];
    float cam_forward[4];
    float intrinsics[4];
    uint32_t dims[4];      /* W, H, num_surfels, unused */
} fwd_push_t;

typedef struct {
    uint32_t dims[4];   /* W, H, unused, unused */
    float    lam[4];    /* x = lam_n (normal consistency), rest unused */
} loss_push_t;

typedef struct {
    float cam_pos[4];
    float cam_right[4];
    float cam_up[4];
    float cam_forward[4];
    float intrinsics[4];
    uint32_t dims[4];
    float lam[4];       /* x = lam_n, y = lam_c (Phase E weights) */
} bwd_push_t;

typedef struct {
    float lr;
    float beta1;
    float beta2;
    float eps;
    float bc1;
    float bc2;
    uint32_t num_surfels;
    uint32_t step;
} adam_push_t;

/* ── Memory helpers ── */

static uint32_t find_mem_type(qtsq_trainer_t *t, uint32_t filter,
                               VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < t->mem_props.memoryTypeCount; i++) {
        if ((filter & (1 << i)) &&
            (t->mem_props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

static int create_buf(qtsq_trainer_t *t, VkDeviceSize size,
                       VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                       VkBuffer *buf, VkDeviceMemory *mem) {
    VkBufferCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(t->device, &ci, NULL, buf) != VK_SUCCESS) return -1;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(t->device, *buf, &req);

    VkMemoryAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_mem_type(t, req.memoryTypeBits, flags);
    if (ai.memoryTypeIndex == UINT32_MAX) return -1;
    if (vkAllocateMemory(t->device, &ai, NULL, mem) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(t->device, *buf, *mem, 0) != VK_SUCCESS) return -1;
    return 0;
}

static void upload_buf(qtsq_trainer_t *t, VkDeviceMemory mem,
                        const void *data, size_t size) {
    void *mapped;
    vkMapMemory(t->device, mem, 0, size, 0, &mapped);
    if (data) memcpy(mapped, data, size);
    else memset(mapped, 0, size);
    vkUnmapMemory(t->device, mem);
}

static void download_buf(qtsq_trainer_t *t, VkDeviceMemory mem,
                           void *data, size_t size) {
    void *mapped;
    vkMapMemory(t->device, mem, 0, size, 0, &mapped);
    memcpy(data, mapped, size);
    vkUnmapMemory(t->device, mem);
}

/* ── Pipeline creation helpers ── */

static int create_pipeline(qtsq_trainer_t *t,
                            const unsigned char *spirv, unsigned int spirv_len,
                            uint32_t num_bindings, uint32_t push_size,
                            VkDescriptorSetLayout *out_dsl,
                            VkPipelineLayout *out_pl,
                            VkPipeline *out_pipe) {
    /* DSL */
    VkDescriptorSetLayoutBinding bindings[8] = {0};
    for (uint32_t i = 0; i < num_bindings && i < 8; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl_ci = {0};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = num_bindings;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(t->device, &dsl_ci, NULL, out_dsl) != VK_SUCCESS)
        return -1;

    /* Pipeline layout */
    VkPushConstantRange push = {0};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = push_size;
    VkPipelineLayoutCreateInfo pl_ci = {0};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = out_dsl;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(t->device, &pl_ci, NULL, out_pl) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(t->device, *out_dsl, NULL);
        return -1;
    }

    /* Shader module */
    VkShaderModuleCreateInfo sm_ci = {0};
    sm_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_ci.codeSize = spirv_len;
    sm_ci.pCode = (const uint32_t *)spirv;
    VkShaderModule module;
    if (vkCreateShaderModule(t->device, &sm_ci, NULL, &module) != VK_SUCCESS) {
        vkDestroyPipelineLayout(t->device, *out_pl, NULL);
        vkDestroyDescriptorSetLayout(t->device, *out_dsl, NULL);
        return -1;
    }

    VkComputePipelineCreateInfo cp_ci = {0};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp_ci.stage.module = module;
    cp_ci.stage.pName = "main";
    cp_ci.layout = *out_pl;
    VkResult r = vkCreateComputePipelines(t->device, VK_NULL_HANDLE, 1, &cp_ci, NULL, out_pipe);
    vkDestroyShaderModule(t->device, module, NULL);
    if (r != VK_SUCCESS) {
        vkDestroyPipelineLayout(t->device, *out_pl, NULL);
        vkDestroyDescriptorSetLayout(t->device, *out_dsl, NULL);
        return -1;
    }
    return 0;
}

/* ── Dispatch helper ── */

static int gpu_dispatch(qtsq_trainer_t *t,
                         VkPipeline pipe, VkPipelineLayout pl,
                         VkDescriptorSet ds,
                         const void *push, uint32_t push_size,
                         uint32_t gx, uint32_t gy, uint32_t gz) {
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ca = {0};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = t->cmd_pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(t->device, &ca, &cmd) != VK_SUCCESS) return -1;

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    if (push && push_size > 0)
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    vkCmdDispatch(cmd, gx, gy, gz);

    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);

    VkFence fence;
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(t->device, &fi, NULL, &fence);
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(t->compute_queue, 1, &si, fence);
    vkWaitForFences(t->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(t->device, fence, NULL);
    vkFreeCommandBuffers(t->device, t->cmd_pool, 1, &cmd);
    return 0;
}

/* ── Allocate descriptor set ── */

static VkDescriptorSet alloc_ds(qtsq_trainer_t *t, VkDescriptorSetLayout dsl) {
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = t->desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(t->device, &ai, &ds) != VK_SUCCESS) return VK_NULL_HANDLE;
    return ds;
}

/* ── Bind buffers to descriptor set ── */

static void bind_bufs(qtsq_trainer_t *t, VkDescriptorSet ds,
                       const VkBuffer *bufs, VkDeviceSize *sizes, uint32_t count) {
    VkDescriptorBufferInfo infos[8];
    VkWriteDescriptorSet writes[8] = {0};
    for (uint32_t i = 0; i < count && i < 8; i++) {
        infos[i].buffer = bufs[i];
        infos[i].offset = 0;
        infos[i].range = sizes[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = ds;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(t->device, count, writes, 0, NULL);
}

/* ── Trainer lifecycle ── */

qtsq_trainer_t *trainer_create(qtsq_capture_t *cap) {
    if (!cap) return NULL;

    qtsq_trainer_t *t = (qtsq_trainer_t *)calloc(1, sizeof(qtsq_trainer_t));
    if (!t) return NULL;

    t->cap = cap;
    int W = cap->W, H = cap->H;
    int max_px = W * H;

    /* Init Vulkan (reuse the global compute context's device) */
    /* We create our own instance/device for the trainer to avoid contention */
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "QTSQ_Trainer";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS) {
        TERR("Failed to create Vulkan instance");
        free(t);
        return NULL;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, NULL);
    if (dev_count == 0) { vkDestroyInstance(instance, NULL); free(t); return NULL; }
    VkPhysicalDevice devs[4];
    if (dev_count > 4) dev_count = 4;
    vkEnumeratePhysicalDevices(instance, &dev_count, devs);
    t->physical_device = devs[0];
    vkGetPhysicalDeviceMemoryProperties(t->physical_device, &t->mem_props);

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(t->physical_device, &qf_count, NULL);
    VkQueueFamilyProperties *qf = (VkQueueFamilyProperties *)malloc(qf_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(t->physical_device, &qf_count, qf);
    t->queue_family_index = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { t->queue_family_index = i; break; }
    }
    free(qf);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = t->queue_family_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    if (vkCreateDevice(t->physical_device, &dci, NULL, &t->device) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        free(t);
        return NULL;
    }
    vkGetDeviceQueue(t->device, t->queue_family_index, 0, &t->compute_queue);
    t->owns_device = 1;

    /* Command pool */
    VkCommandPoolCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = t->queue_family_index;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(t->device, &cpi, NULL, &t->cmd_pool);

    /* Descriptor pool */
    VkDescriptorPoolSize ps = {0};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 128;
    VkDescriptorPoolCreateInfo dpi = {0};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 32;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(t->device, &dpi, NULL, &t->desc_pool);

    /* Create 4 pipelines */
    if (create_pipeline(t, surfel_forward_spv, surfel_forward_spv_len,
                        6, sizeof(fwd_push_t), &t->dsl_fwd, &t->pipe_layout_fwd, &t->pipe_fwd) != 0) {
        TERR("Failed to create forward pipeline");
        trainer_destroy(t);
        return NULL;
    }
    if (create_pipeline(t, surfel_loss_spv, surfel_loss_spv_len,
                        8, sizeof(loss_push_t), &t->dsl_loss, &t->pipe_layout_loss, &t->pipe_loss) != 0) {
        TERR("Failed to create loss pipeline");
        trainer_destroy(t);
        return NULL;
    }
    if (create_pipeline(t, surfel_backward_spv, surfel_backward_spv_len,
                        6, sizeof(bwd_push_t), &t->dsl_bwd, &t->pipe_layout_bwd, &t->pipe_bwd) != 0) {
        TERR("Failed to create backward pipeline");
        trainer_destroy(t);
        return NULL;
    }
    if (create_pipeline(t, surfel_adam_spv, surfel_adam_spv_len,
                        4, sizeof(adam_push_t), &t->dsl_adam, &t->pipe_layout_adam, &t->pipe_adam) != 0) {
        TERR("Failed to create adam pipeline");
        trainer_destroy(t);
        return NULL;
    }

    /* Allocate buffers */
    t->max_surfels = 8192;  /* initial cap */
    VkDeviceSize surfel_sz = (VkDeviceSize)t->max_surfels * SURFEL_STRIDE_FLOATS * sizeof(float);
    VkDeviceSize grad_sz = (VkDeviceSize)t->max_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    VkDeviceSize img_sz = (VkDeviceSize)max_px * sizeof(float) * 3;  /* vec3 */
    VkDeviceSize loss_sz = sizeof(float);

    VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /* Phase E deferred buffers */
    VkDeviceSize depth_sz = (VkDeviceSize)max_px * sizeof(float);        /* W*H depth */
    VkDeviceSize normal_sz = (VkDeviceSize)max_px * 3 * sizeof(float);   /* W*H*3 normal */
    VkDeviceSize ncgrad_sz = (VkDeviceSize)max_px * 4 * sizeof(float);   /* depth_grad + normal_grad */

    create_buf(t, surfel_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->surfel_buf, &t->surfel_mem);
    create_buf(t, grad_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->grad_buf, &t->grad_mem);
    create_buf(t, grad_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->adam_m_buf, &t->adam_m_mem);
    create_buf(t, grad_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->adam_v_buf, &t->adam_v_mem);
    create_buf(t, img_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->pred_buf, &t->pred_mem);
    create_buf(t, img_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->target_buf, &t->target_mem);
    create_buf(t, img_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->dLdpred_buf, &t->dLdpred_mem);
    create_buf(t, loss_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->loss_buf, &t->loss_mem);
    create_buf(t, depth_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->depth_buf, &t->depth_mem);
    create_buf(t, normal_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->normal_buf, &t->normal_mem);
    create_buf(t, depth_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->alpha_buf, &t->alpha_mem);
    create_buf(t, ncgrad_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &t->ncgrad_buf, &t->ncgrad_mem);

    /* Allocate host mirrors */
    t->host_surfels = (float *)calloc(t->max_surfels, SURFEL_STRIDE_FLOATS * sizeof(float));
    t->host_grads = (uint32_t *)calloc(t->max_surfels, GRAD_STRIDE_FLOATS * sizeof(uint32_t));
    t->host_adam_m = (float *)calloc(t->max_surfels, GRAD_STRIDE_FLOATS * sizeof(float));
    t->host_adam_v = (float *)calloc(t->max_surfels, GRAD_STRIDE_FLOATS * sizeof(float));
    t->host_pred = (vec3_host *)calloc(max_px, sizeof(vec3_host));
    t->host_target = (vec3_host *)calloc(max_px, sizeof(vec3_host));
    t->host_dLdpred = (vec3_host *)calloc(max_px, sizeof(vec3_host));
    t->grad_accum = (float *)calloc(t->max_surfels, sizeof(float));
    t->snap_surfels = (float *)calloc(t->max_surfels, SURFEL_STRIDE_FLOATS * sizeof(float));
    t->host_alpha = (float *)calloc(max_px, sizeof(float));

    pthread_mutex_init(&t->lock, NULL);
    t->loop_running = 0;
    t->loop_stop = 0;

    t->num_surfels = 0;
    t->cap_surfels = t->max_surfels;
    t->step_count = 0;
    t->base_lr = 0.001f;
    /* Phase E loss weights (paper-2 defaults for the online setting). */
    t->lam_n = 0.05f;
    t->lam_c = 7.0f;
    t->coverage_pct = 0.0f;
    t->angle_spread_deg = 0.0f;
    t->step_sleep_ms = 20.0f;

    TLOG("Trainer created: %dx%d, max_surfels=%d", W, H, t->max_surfels);
    return t;
}

void trainer_destroy(qtsq_trainer_t *t) {
    if (!t) return;

    /* Stop the background train loop before tearing anything down. */
    trainer_stop_loop(t);
    vkDeviceWaitIdle(t->device);

    pthread_mutex_destroy(&t->lock);

    /* Free buffers */
    VkBuffer bufs[] = {t->surfel_buf, t->grad_buf, t->adam_m_buf, t->adam_v_buf,
                       t->pred_buf, t->target_buf, t->dLdpred_buf, t->loss_buf,
                       t->depth_buf, t->normal_buf, t->alpha_buf, t->ncgrad_buf};
    VkDeviceMemory mems[] = {t->surfel_mem, t->grad_mem, t->adam_m_mem, t->adam_v_mem,
                             t->pred_mem, t->target_mem, t->dLdpred_mem, t->loss_mem,
                             t->depth_mem, t->normal_mem, t->alpha_mem, t->ncgrad_mem};
    for (int i = 0; i < 12; i++) {
        if (bufs[i]) vkDestroyBuffer(t->device, bufs[i], NULL);
        if (mems[i]) vkFreeMemory(t->device, mems[i], NULL);
    }

    /* Free pipelines */
    if (t->pipe_fwd) vkDestroyPipeline(t->device, t->pipe_fwd, NULL);
    if (t->pipe_layout_fwd) vkDestroyPipelineLayout(t->device, t->pipe_layout_fwd, NULL);
    if (t->dsl_fwd) vkDestroyDescriptorSetLayout(t->device, t->dsl_fwd, NULL);
    if (t->pipe_loss) vkDestroyPipeline(t->device, t->pipe_loss, NULL);
    if (t->pipe_layout_loss) vkDestroyPipelineLayout(t->device, t->pipe_layout_loss, NULL);
    if (t->dsl_loss) vkDestroyDescriptorSetLayout(t->device, t->dsl_loss, NULL);
    if (t->pipe_bwd) vkDestroyPipeline(t->device, t->pipe_bwd, NULL);
    if (t->pipe_layout_bwd) vkDestroyPipelineLayout(t->device, t->pipe_layout_bwd, NULL);
    if (t->dsl_bwd) vkDestroyDescriptorSetLayout(t->device, t->dsl_bwd, NULL);
    if (t->pipe_adam) vkDestroyPipeline(t->device, t->pipe_adam, NULL);
    if (t->pipe_layout_adam) vkDestroyPipelineLayout(t->device, t->pipe_layout_adam, NULL);
    if (t->dsl_adam) vkDestroyDescriptorSetLayout(t->device, t->dsl_adam, NULL);

    if (t->desc_pool) vkDestroyDescriptorPool(t->device, t->desc_pool, NULL);
    if (t->cmd_pool) vkDestroyCommandPool(t->device, t->cmd_pool, NULL);
    if (t->device) vkDestroyDevice(t->device, NULL);

    free(t->host_surfels);
    free(t->host_grads);
    free(t->host_adam_m);
    free(t->host_adam_v);
    free(t->host_pred);
    free(t->host_target);
    free(t->host_dLdpred);
    free(t->grad_accum);
    free(t->snap_surfels);
    free(t->host_alpha);
    free(t);
}

/* ── Surfel seeding ── */

static inline float frand_trainer(unsigned *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (float)((*s) >> 8) / (float)(1u << 24);
}

void trainer_seed_from_points(qtsq_trainer_t *t,
                              const float *xyz, const float *rgb, int n) {
    if (!t || !xyz || n <= 0) return;
    t->num_surfels = 0;

    for (int i = 0; i < n && t->num_surfels < t->max_surfels; i++) {
        int s = t->num_surfels;
        int base = s * SURFEL_STRIDE_FLOATS;
        t->host_surfels[base + 0] = xyz[i * 3 + 0];
        t->host_surfels[base + 1] = xyz[i * 3 + 1];
        t->host_surfels[base + 2] = xyz[i * 3 + 2];
        /* Identity quaternion [w,x,y,z] */
        t->host_surfels[base + 3] = 1.0f;
        t->host_surfels[base + 4] = 0.0f;
        t->host_surfels[base + 5] = 0.0f;
        t->host_surfels[base + 6] = 0.0f;
        /* Small isotropic scale */
        t->host_surfels[base + 7] = logf(0.05f);  /* log_su */
        t->host_surfels[base + 8] = logf(0.05f);  /* log_sv */
        /* Moderate opacity */
        t->host_surfels[base + 9] = 0.0f;  /* opacity_logit -> sigmoid=0.5 */
        /* Color */
        if (rgb) {
            t->host_surfels[base + 10] = rgb[i * 3 + 0];
            t->host_surfels[base + 11] = rgb[i * 3 + 1];
            t->host_surfels[base + 12] = rgb[i * 3 + 2];
        } else {
            t->host_surfels[base + 10] = 0.5f;
            t->host_surfels[base + 11] = 0.5f;
            t->host_surfels[base + 12] = 0.5f;
        }
        t->num_surfels++;
    }

    pthread_mutex_lock(&t->lock);
    TLOG("Seeded %d surfels from points", t->num_surfels);
    t->step_count = 0;
    pthread_mutex_unlock(&t->lock);

    /* Upload to GPU */
    VkDeviceSize sz = (VkDeviceSize)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float);
    upload_buf(t, t->surfel_mem, t->host_surfels, sz);

    /* Zero Adam moments */
    VkDeviceSize grad_sz = (VkDeviceSize)t->num_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    upload_buf(t, t->adam_m_mem, NULL, grad_sz);
    upload_buf(t, t->adam_v_mem, NULL, grad_sz);

    memset(t->grad_accum, 0, t->num_surfels * sizeof(float));

    /* Publish the seed as the first snapshot. */
    pthread_mutex_lock(&t->lock);
    memcpy(t->snap_surfels, t->host_surfels,
           (size_t)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float));
    t->snap_count = t->num_surfels;
    pthread_mutex_unlock(&t->lock);
}

void trainer_seed_random(qtsq_trainer_t *t, int count,
                         const cap_camera_t *cam, float spread) {
    if (!t || !cam || count <= 0) return;
    t->num_surfels = 0;

    unsigned rng = 0x9e3779b9u;
    for (int i = 0; i < count && t->num_surfels < t->max_surfels; i++) {
        int s = t->num_surfels;
        int base = s * SURFEL_STRIDE_FLOATS;
        /* Random position near camera */
        t->host_surfels[base + 0] = cam->pos.x + (frand_trainer(&rng) - 0.5f) * spread;
        t->host_surfels[base + 1] = cam->pos.y + (frand_trainer(&rng) - 0.5f) * spread;
        t->host_surfels[base + 2] = cam->pos.z + (frand_trainer(&rng) - 0.5f) * spread;
        /* Identity quaternion */
        t->host_surfels[base + 3] = 1.0f;
        t->host_surfels[base + 4] = 0.0f;
        t->host_surfels[base + 5] = 0.0f;
        t->host_surfels[base + 6] = 0.0f;
        t->host_surfels[base + 7] = logf(0.05f);
        t->host_surfels[base + 8] = logf(0.05f);
        t->host_surfels[base + 9] = 0.0f;
        t->host_surfels[base + 10] = 0.5f;
        t->host_surfels[base + 11] = 0.5f;
        t->host_surfels[base + 12] = 0.5f;
        t->num_surfels++;
    }

    pthread_mutex_lock(&t->lock);
    TLOG("Seeded %d random surfels", t->num_surfels);
    t->step_count = 0;
    pthread_mutex_unlock(&t->lock);

    VkDeviceSize sz = (VkDeviceSize)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float);
    upload_buf(t, t->surfel_mem, t->host_surfels, sz);
    VkDeviceSize grad_sz = (VkDeviceSize)t->num_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    upload_buf(t, t->adam_m_mem, NULL, grad_sz);
    upload_buf(t, t->adam_v_mem, NULL, grad_sz);
    memset(t->grad_accum, 0, t->num_surfels * sizeof(float));

    /* Publish the seed as the first snapshot. */
    pthread_mutex_lock(&t->lock);
    memcpy(t->snap_surfels, t->host_surfels,
           (size_t)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float));
    t->snap_count = t->num_surfels;
    pthread_mutex_unlock(&t->lock);
}

/* ── Training step ──
 * Called by the train thread (or manually while the loop is stopped).
 * Picks a random keyframe under the ring lock, runs the four GPU dispatches,
 * auto-runs density control every DENSIFY_EVERY steps, and refreshes the
 * GL-visible snapshot. */

#define DENSIFY_EVERY 50

float trainer_step(qtsq_trainer_t *t) {
    if (!t || !t->cap || t->num_surfels == 0) return 0.0f;

    int W = t->cap->W, H = t->cap->H;
    if (W <= 0 || H <= 0) return 0.0f;

    /* Pick a random keyframe from the capture ring (thread-safe sample) */
    pthread_mutex_lock(&t->lock);
    int cur_step = t->step_count;
    pthread_mutex_unlock(&t->lock);
    unsigned count = (unsigned)qtsq_capture_keyframe_count(t->cap);
    if (count == 0) return 0.0f;
    unsigned rng = (unsigned)((cur_step + 1) * 2654435761u);
    int r = (int)(rng % count);

    /* Scratch image for the sampled keyframe — freed after upload; a
     * discarded frame never leaves scratch memory (privacy invariant). */
    float *img = (float *)malloc((size_t)W * H * 3 * sizeof(float));
    if (!img) return 0.0f;
    cap_camera_t kf_cam;
    if (!qtsq_capture_sample(t->cap, r, &kf_cam, img)) { free(img); return 0.0f; }

    uint32_t ns = (uint32_t)t->num_surfels;

    /* Upload target image */
    VkDeviceSize img_sz = (VkDeviceSize)W * H * 3 * sizeof(float);
    upload_buf(t, t->target_mem, img, img_sz);
    free(img);

    /* Zero pred and dLdpred */
    upload_buf(t, t->pred_mem, NULL, img_sz);
    upload_buf(t, t->dLdpred_mem, NULL, img_sz);

    /* Zero loss */
    float zero = 0.0f;
    upload_buf(t, t->loss_mem, &zero, sizeof(float));

    /* Zero grads */
    VkDeviceSize grad_sz = (VkDeviceSize)t->num_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    upload_buf(t, t->grad_mem, NULL, grad_sz);

    /* Build push constants from camera */
    fwd_push_t fwd_pc;
    fwd_pc.cam_pos[0] = kf_cam.pos.x; fwd_pc.cam_pos[1] = kf_cam.pos.y;
    fwd_pc.cam_pos[2] = kf_cam.pos.z; fwd_pc.cam_pos[3] = 0;
    fwd_pc.cam_right[0] = kf_cam.right.x; fwd_pc.cam_right[1] = kf_cam.right.y;
    fwd_pc.cam_right[2] = kf_cam.right.z; fwd_pc.cam_right[3] = 0;
    fwd_pc.cam_up[0] = kf_cam.up.x; fwd_pc.cam_up[1] = kf_cam.up.y;
    fwd_pc.cam_up[2] = kf_cam.up.z; fwd_pc.cam_up[3] = 0;
    fwd_pc.cam_forward[0] = kf_cam.forward.x; fwd_pc.cam_forward[1] = kf_cam.forward.y;
    fwd_pc.cam_forward[2] = kf_cam.forward.z; fwd_pc.cam_forward[3] = 0;
    fwd_pc.intrinsics[0] = kf_cam.fx; fwd_pc.intrinsics[1] = kf_cam.fy;
    fwd_pc.intrinsics[2] = kf_cam.cx; fwd_pc.intrinsics[3] = kf_cam.cy;
    fwd_pc.dims[0] = W; fwd_pc.dims[1] = H; fwd_pc.dims[2] = ns; fwd_pc.dims[3] = 0;

    bwd_push_t bwd_pc;
    memcpy(&bwd_pc, &fwd_pc, sizeof(fwd_push_t));

    /* Phase E loss weights */
    loss_push_t loss_pc;
    loss_pc.dims[0] = W; loss_pc.dims[1] = H; loss_pc.dims[2] = 0; loss_pc.dims[3] = 0;
    loss_pc.lam[0] = t->lam_n; loss_pc.lam[1] = 0; loss_pc.lam[2] = 0; loss_pc.lam[3] = 0;

    /* Dispatch forward (color + depth + normal + alpha) */
    VkDeviceSize surfel_sz = (VkDeviceSize)ns * SURFEL_STRIDE_FLOATS * sizeof(float);
    VkDeviceSize depth_sz = (VkDeviceSize)W * H * sizeof(float);
    VkDeviceSize ncgrad_sz = (VkDeviceSize)W * H * 4 * sizeof(float);

    VkBuffer fwd_bufs[] = {t->surfel_buf, t->pred_buf, t->target_buf,
                           t->depth_buf, t->normal_buf, t->alpha_buf};
    VkDeviceSize fwd_sizes[] = {surfel_sz, img_sz, img_sz, depth_sz, img_sz, depth_sz};
    VkDescriptorSet fwd_ds = alloc_ds(t, t->dsl_fwd);
    bind_bufs(t, fwd_ds, fwd_bufs, fwd_sizes, 6);
    uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16;
    gpu_dispatch(t, t->pipe_fwd, t->pipe_layout_fwd, fwd_ds,
                 &fwd_pc, sizeof(fwd_pc), gx, gy, 1);
    vkFreeDescriptorSets(t->device, t->desc_pool, 1, &fwd_ds);

    /* Dispatch loss (L1 + λn·NC; writes dLdpred + nc_grad) */
    VkBuffer loss_bufs[] = {t->pred_buf, t->target_buf, t->dLdpred_buf, t->loss_buf,
                            t->depth_buf, t->normal_buf, t->alpha_buf, t->ncgrad_buf};
    VkDeviceSize loss_sizes[] = {img_sz, img_sz, img_sz, sizeof(float),
                                 depth_sz, img_sz, depth_sz, ncgrad_sz};
    VkDescriptorSet loss_ds = alloc_ds(t, t->dsl_loss);
    bind_bufs(t, loss_ds, loss_bufs, loss_sizes, 8);
    gpu_dispatch(t, t->pipe_loss, t->pipe_layout_loss, loss_ds,
                 &loss_pc, sizeof(loss_pc), gx, gy, 1);
    vkFreeDescriptorSets(t->device, t->desc_pool, 1, &loss_ds);

    /* Dispatch backward (photometric + NC chains + λc·DC, adds to loss) */
    bwd_pc.lam[0] = t->lam_n; bwd_pc.lam[1] = t->lam_c;
    bwd_pc.lam[2] = 0; bwd_pc.lam[3] = 0;
    VkBuffer bwd_bufs[] = {t->surfel_buf, t->dLdpred_buf, t->grad_buf, t->target_buf,
                           t->ncgrad_buf, t->loss_buf};
    VkDeviceSize bwd_sizes[] = {surfel_sz, img_sz, grad_sz, img_sz, ncgrad_sz, sizeof(float)};
    VkDescriptorSet bwd_ds = alloc_ds(t, t->dsl_bwd);
    bind_bufs(t, bwd_ds, bwd_bufs, bwd_sizes, 6);
    gpu_dispatch(t, t->pipe_bwd, t->pipe_layout_bwd, bwd_ds,
                 &bwd_pc, sizeof(bwd_pc), gx, gy, 1);
    vkFreeDescriptorSets(t->device, t->desc_pool, 1, &bwd_ds);

    /* Read back loss AFTER backward (now includes λc·DC) */
    download_buf(t, t->loss_mem, &t->host_loss, sizeof(float));

    /* Phase E/G: read back the opacity map for the coverage indicator. */
    download_buf(t, t->alpha_mem, t->host_alpha, depth_sz);

    /* Dispatch adam */
    adam_push_t adam_pc;
    adam_pc.lr = t->base_lr;
    adam_pc.beta1 = 0.9f;
    adam_pc.beta2 = 0.999f;
    adam_pc.eps = 1e-8f;
    adam_pc.bc1 = 1.0f - powf(0.9f, (float)(t->step_count + 1));
    adam_pc.bc2 = 1.0f - powf(0.999f, (float)(t->step_count + 1));
    adam_pc.num_surfels = ns;
    adam_pc.step = (uint32_t)t->step_count;

    VkBuffer adam_bufs[] = {t->surfel_buf, t->grad_buf, t->adam_m_buf, t->adam_v_buf};
    VkDeviceSize adam_sizes[] = {surfel_sz, grad_sz, grad_sz, grad_sz};
    VkDescriptorSet adam_ds = alloc_ds(t, t->dsl_adam);
    bind_bufs(t, adam_ds, adam_bufs, adam_sizes, 4);
    uint32_t adam_gx = (ns + 63) / 64;
    gpu_dispatch(t, t->pipe_adam, t->pipe_layout_adam, adam_ds,
                 &adam_pc, sizeof(adam_pc), adam_gx, 1, 1);
    vkFreeDescriptorSets(t->device, t->desc_pool, 1, &adam_ds);

    /* Read back surfels for host mirror (for GL preview + density control) */
    download_buf(t, t->surfel_mem, t->host_surfels, surfel_sz);

    pthread_mutex_lock(&t->lock);
    t->step_count++;
    /* Coverage: % of training pixels with alpha > 0.5 (Phase G indicator). */
    {
        long covered = 0;
        for (int i = 0; i < W * H; i++)
            if (t->host_alpha[i] > 0.5f) covered++;
        t->coverage_pct = 100.0f * (float)covered / (float)(W * H);
    }
    pthread_mutex_unlock(&t->lock);

    /* Density control on the train thread every DENSIFY_EVERY steps. */
    if (t->step_count % DENSIFY_EVERY == 0)
        trainer_density_control(t);

    /* Publish the GL-visible snapshot (thread-safe mirror). */
    pthread_mutex_lock(&t->lock);
    memcpy(t->snap_surfels, t->host_surfels,
           (size_t)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float));
    t->snap_count = t->num_surfels;
    pthread_mutex_unlock(&t->lock);

    return t->host_loss;
}

/* ── CPU density control (port of prototype density_control) ── */

void trainer_density_control(qtsq_trainer_t *t) {
    if (!t || t->num_surfels == 0) return;

    /* Read back grads for density signal */
    VkDeviceSize grad_sz = (VkDeviceSize)t->num_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    uint32_t *tmp_grads = (uint32_t *)malloc(grad_sz);
    download_buf(t, t->grad_mem, tmp_grads, grad_sz);

    /* Update grad_accum EMA */
    for (int i = 0; i < t->num_surfels; i++) {
        float gx = 0, gy = 0, gz = 0;
        memcpy(&gx, &tmp_grads[i * GRAD_STRIDE_FLOATS + 0], sizeof(float));
        memcpy(&gy, &tmp_grads[i * GRAD_STRIDE_FLOATS + 1], sizeof(float));
        memcpy(&gz, &tmp_grads[i * GRAD_STRIDE_FLOATS + 2], sizeof(float));
        float gm = sqrtf(gx*gx + gy*gy + gz*gz);
        t->grad_accum[i] = 0.9f * t->grad_accum[i] + 0.1f * gm;
    }
    free(tmp_grads);

    /* Prune: compact surfels with alpha >= prune_alpha */
    float prune_alpha = 0.005f;
    float clone_thresh = 0.002f;
    int w = 0;

    for (int i = 0; i < t->num_surfels; i++) {
        float logit = t->host_surfels[i * SURFEL_STRIDE_FLOATS + 9];
        float alpha = 1.0f / (1.0f + expf(-logit));
        if (alpha < prune_alpha) continue;
        if (w != i) {
            memcpy(&t->host_surfels[w * SURFEL_STRIDE_FLOATS],
                   &t->host_surfels[i * SURFEL_STRIDE_FLOATS],
                   SURFEL_STRIDE_FLOATS * sizeof(float));
            t->grad_accum[w] = t->grad_accum[i];
        }
        w++;
    }
    int kept = w;

    /* Clone high-gradient surfels */
    int out = kept;
    for (int i = 0; i < kept; i++) {
        if (t->grad_accum[i] > clone_thresh && out < t->max_surfels - 1) {
            memcpy(&t->host_surfels[out * SURFEL_STRIDE_FLOATS],
                   &t->host_surfels[i * SURFEL_STRIDE_FLOATS],
                   SURFEL_STRIDE_FLOATS * sizeof(float));
            /* Jitter along tangent */
            int base = out * SURFEL_STRIDE_FLOATS;
            float su = expf(t->host_surfels[base + 7]);
            /* Simple jitter on center.x */
            t->host_surfels[base + 0] += su * 0.5f * ((float)rand() / (float)RAND_MAX - 0.5f);
            t->grad_accum[out] = 0.0f;
            out++;
        }
    }

    t->num_surfels = out;
    TLOG("Density control: %d -> %d surfels", out, t->num_surfels);

    /* Re-upload surfels, zero adam moments */
    VkDeviceSize surfel_sz = (VkDeviceSize)t->num_surfels * SURFEL_STRIDE_FLOATS * sizeof(float);
    upload_buf(t, t->surfel_mem, t->host_surfels, surfel_sz);
    VkDeviceSize grad_sz2 = (VkDeviceSize)t->num_surfels * GRAD_STRIDE_FLOATS * sizeof(uint32_t);
    upload_buf(t, t->adam_m_mem, NULL, grad_sz2);
    upload_buf(t, t->adam_v_mem, NULL, grad_sz2);
}

/* Max pairwise angle (degrees) between the retained keyframes' view
 * directions — the Phase G "angle diversity" indicator. */
static float compute_angle_spread(const qtsq_capture_t *cap) {
    if (!cap) return 0.0f;
    int n = qtsq_capture_keyframe_count(cap);
    if (n < 2) return 0.0f;
    int m = n < 32 ? n : 32;
    cap_camera_t cams[32];
    int got = 0;
    for (int i = 0; i < m; i++) {
        if (qtsq_capture_sample(cap, i, &cams[got], NULL) == 1) got++;
    }
    if (got < 2) return 0.0f;
    float max_deg = 0.0f;
    for (int i = 0; i < got; i++) {
        for (int j = i + 1; j < got; j++) {
            const cap_vec3 *a = &cams[i].forward, *b = &cams[j].forward;
            float dot = a->x*b->x + a->y*b->y + a->z*b->z;
            if (dot > 1.0f) dot = 1.0f;
            if (dot < -1.0f) dot = -1.0f;
            float deg = acosf(dot) * (180.0f / 3.14159265f);
            if (deg > max_deg) max_deg = deg;
        }
    }
    return max_deg;
}

trainer_stats_t trainer_get_stats(const qtsq_trainer_t *t) {
    trainer_stats_t s = {0};
    if (!t) return s;
    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    s.step = t->step_count;
    s.surfel_count = t->num_surfels;
    s.loss = t->host_loss;
    s.keyframe_count = t->cap ? qtsq_capture_keyframe_count(t->cap) : 0;
    s.coverage_pct = t->coverage_pct;
    s.thermal_sleep_ms = t->step_sleep_ms;
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
    /* PSNR = -10 * log10(mse); for L1 loss, approximate */
    if (s.loss > 1e-12f)
        s.psnr = -10.0f * log10f(s.loss);
    else
        s.psnr = 99.0f;
    s.angle_spread_deg = compute_angle_spread(t->cap);
    return s;
}

const float *trainer_get_surfels(const qtsq_trainer_t *t, int *out_count) {
    if (!t) { if (out_count) *out_count = 0; return NULL; }
    if (out_count) *out_count = t->snap_count;
    return t->snap_surfels;
}

int trainer_snapshot_surfels(const qtsq_trainer_t *t,
                             float *out_surfels, int max_count) {
    if (!t || !out_surfels || max_count <= 0) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    int n = t->snap_count;
    if (n > max_count) n = max_count;
    if (n > 0)
        memcpy(out_surfels, t->snap_surfels,
               (size_t)n * SURFEL_STRIDE_FLOATS * sizeof(float));
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
    return n;
}

/* ── Background train loop (Phase D §3) ──
 * One writer: the loop thread owns trainer_step / trainer_density_control.
 * The GL thread reads the snapshot + stats. A simple 20 ms inter-step sleep
 * is the v1 thermal budget (Phase G refines this). */

/* Adaptive thermal budget (Phase G): keep the per-step wall time near the
 * 25 ms budget by adjusting the inter-step sleep (5..80 ms). */
static void adapt_sleep(qtsq_trainer_t *t, double step_ms) {
    float *sleep = &t->step_sleep_ms;
    if (step_ms > 45.0)      *sleep = fminf(*sleep + 5.0f, 80.0f);
    else if (step_ms < 15.0) *sleep = fmaxf(*sleep - 2.0f, 5.0f);
}

static void *train_loop_fn(void *arg) {
    qtsq_trainer_t *t = (qtsq_trainer_t *)arg;
    TLOG("Train loop started");
    struct timespec ts0, ts1;
    while (!t->loop_stop) {
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        trainer_step(t);
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        double ms = (double)(ts1.tv_sec - ts0.tv_sec) * 1000.0
                  + (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e6;
        t->last_step_ms = ms;
        adapt_sleep(t, ms);
        usleep((useconds_t)(t->step_sleep_ms * 1000.0));
    }
    TLOG("Train loop stopped (%d steps)", t->step_count);
    return NULL;
}

int trainer_start_loop(qtsq_trainer_t *t) {
    if (!t || t->loop_running) return -1;
    t->loop_stop = 0;
    if (pthread_create(&t->train_thread, NULL, train_loop_fn, t) != 0) return -1;
    t->loop_running = 1;
    return 0;
}

void trainer_stop_loop(qtsq_trainer_t *t) {
    if (!t || !t->loop_running) return;
    t->loop_stop = 1;
    pthread_join(t->train_thread, NULL);
    t->loop_running = 0;
}

/* ── On-device parity gate (Phase D §4) ──
 * Render keyframe 0 with the GPU forward pipeline and with the host
 * shader-model reference, report the max per-channel difference. GPU and
 * host must agree to ~1e-4 once the Vulkan path is correct. */

int trainer_parity_forward(qtsq_trainer_t *t, float *max_diff) {
    if (!t || !max_diff || !t->cap || t->num_surfels == 0) return -1;
    int W = t->cap->W, H = t->cap->H;
    if (W <= 0 || H <= 0) return -1;

    /* Sample keyframe 0 and its camera (same code path as trainer_step). */
    float *img = (float *)malloc((size_t)W * H * 3 * sizeof(float));
    if (!img) return -1;
    cap_camera_t cam;
    if (!qtsq_capture_sample(t->cap, 0, &cam, img)) { free(img); return -1; }

    /* Build the same push constants trainer_step uses. */
    fwd_push_t fwd_pc;
    fwd_pc.cam_pos[0] = cam.pos.x; fwd_pc.cam_pos[1] = cam.pos.y;
    fwd_pc.cam_pos[2] = cam.pos.z; fwd_pc.cam_pos[3] = 0;
    fwd_pc.cam_right[0] = cam.right.x; fwd_pc.cam_right[1] = cam.right.y;
    fwd_pc.cam_right[2] = cam.right.z; fwd_pc.cam_right[3] = 0;
    fwd_pc.cam_up[0] = cam.up.x; fwd_pc.cam_up[1] = cam.up.y;
    fwd_pc.cam_up[2] = cam.up.z; fwd_pc.cam_up[3] = 0;
    fwd_pc.cam_forward[0] = cam.forward.x; fwd_pc.cam_forward[1] = cam.forward.y;
    fwd_pc.cam_forward[2] = cam.forward.z; fwd_pc.cam_forward[3] = 0;
    fwd_pc.intrinsics[0] = cam.fx; fwd_pc.intrinsics[1] = cam.fy;
    fwd_pc.intrinsics[2] = cam.cx; fwd_pc.intrinsics[3] = cam.cy;

    pthread_mutex_lock(&t->lock);
    uint32_t ns = (uint32_t)t->num_surfels;
    pthread_mutex_unlock(&t->lock);
    if (ns == 0) { free(img); return -1; }
    fwd_pc.dims[0] = W; fwd_pc.dims[1] = H; fwd_pc.dims[2] = ns; fwd_pc.dims[3] = 0;

    /* GPU forward on the current surfel buffer. */
    VkDeviceSize img_sz = (VkDeviceSize)W * H * 3 * sizeof(float);
    upload_buf(t, t->pred_mem, NULL, img_sz);
    VkDeviceSize surfel_sz = (VkDeviceSize)ns * SURFEL_STRIDE_FLOATS * sizeof(float);
    VkBuffer bufs[] = {t->surfel_buf, t->pred_buf, t->target_buf};
    VkDeviceSize sizes[] = {surfel_sz, img_sz, img_sz};
    VkDescriptorSet ds = alloc_ds(t, t->dsl_fwd);
    bind_bufs(t, ds, bufs, sizes, 3);
    uint32_t gx = (W + 15) / 16, gy = (H + 15) / 16;
    int rc = gpu_dispatch(t, t->pipe_fwd, t->pipe_layout_fwd, ds,
                          &fwd_pc, sizeof(fwd_pc), gx, gy, 1);
    vkFreeDescriptorSets(t->device, t->desc_pool, 1, &ds);
    if (rc != 0) { free(img); return -1; }

    /* GPU pred readback. */
    float *gpu_pred = (float *)malloc(img_sz);
    if (!gpu_pred) { free(img); return -1; }
    download_buf(t, t->pred_mem, gpu_pred, img_sz);

    /* Host reference on the same surfels + camera. */
    float *host_pred = (float *)malloc(img_sz);
    float *surfels = (float *)malloc(surfel_sz);
    if (!host_pred || !surfels) {
        free(img); free(gpu_pred); free(host_pred); free(surfels);
        return -1;
    }
    pthread_mutex_lock(&t->lock);
    memcpy(surfels, t->host_surfels, surfel_sz);
    pthread_mutex_unlock(&t->lock);
    rc = sfm_render(surfels, (int)ns, &cam, host_pred);

    float md = 0.0f;
    if (rc == 0) {
        for (size_t i = 0; i < (size_t)W * H * 3; i++) {
            float d = fabsf(gpu_pred[i] - host_pred[i]);
            if (d > md) md = d;
        }
    }
    *max_diff = md;

    TLOG("parity_forward: max|gpu-host| = %g (%dx%d, %u surfels)",
         md, W, H, ns);

    free(img); free(gpu_pred); free(host_pred); free(surfels);
    return rc == 0 ? 0 : -1;
}

/* ── Save — fail-closed secure write (Phase D §6) ──
 * Converts the trainer's host mirror (13 floats/surfel, unconstrained form)
 * into qtsq_surfel_t — the layouts are identical — and routes through
 * qtsq_surfel_container_write, which sanitizes + keystore-encrypts and
 * refuses plaintext. The keystore must be initialized (app startup /
 * nativeKeystoreInit). */

int trainer_save(const qtsq_trainer_t *t, const char *path) {
    if (!t || !path || t->num_surfels == 0) return -1;

    if (!qtsq_keystore_is_initialized()) {
        TERR("trainer_save: keystore not initialized — cannot save encrypted .qtsq");
        return QTSQ_ERR_NOT_ENCRYPTED;
    }

    /* Snapshot the model under the lock. */
    int n = t->snap_count;
    if (n <= 0) return -1;
    qtsq_surfel_t *surfels = (qtsq_surfel_t *)malloc((size_t)n * sizeof(qtsq_surfel_t));
    if (!surfels) return QTSQ_ERR_ALLOC;

    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    for (int i = 0; i < n; i++) {
        const float *s = t->snap_surfels + (size_t)i * SURFEL_STRIDE_FLOATS;
        qtsq_surfel_t *o = &surfels[i];
        memcpy(o->center, s, 3 * sizeof(float));
        memcpy(o->quat, s + 3, 4 * sizeof(float));
        o->log_scale[0] = s[7];
        o->log_scale[1] = s[8];
        o->opacity_logit = s[9];
        memcpy(o->color, s + 10, 3 * sizeof(float));
    }
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);

    qtsq_surfel_model_t model;
    qtsq_surfel_model_init(&model);
    model.surfels = surfels;
    model.count = (uint32_t)n;
    model.capacity = (uint32_t)n;
    qtsq_surfel_model_recompute_aabb(&model);

    /* Phase F: fuse a mesh from posed depth maps of the keyframe cameras
     * (host CPU at save time). Depth maps are rendered from the trained
     * snapshot with the same sorted-composite math as the GPU forward. */
    qtsq_mesh_t mesh;
    memset(&mesh, 0, sizeof(mesh));
    if (t->cap) {
        int kfs = qtsq_capture_keyframe_count(t->cap);
        int W = t->cap->W, H = t->cap->H;
        if (kfs >= 3 && W > 0 && H > 0) {
            int use_kf = kfs < 24 ? kfs : 24;
            float **deps = (float **)calloc((size_t)use_kf, sizeof(float *));
            qtsq_tsdf_camera_t *tcams = (qtsq_tsdf_camera_t *)calloc(
                (size_t)use_kf, sizeof(qtsq_tsdf_camera_t));
            float *sf = (float *)malloc((size_t)n * SURFEL_STRIDE_FLOATS * sizeof(float));
            if (deps && tcams && sf) {
                pthread_mutex_lock((pthread_mutex_t*)&t->lock);
                memcpy(sf, t->snap_surfels,
                       (size_t)n * SURFEL_STRIDE_FLOATS * sizeof(float));
                pthread_mutex_unlock((pthread_mutex_t*)&t->lock);

                int got = 0;
                for (int i = 0; i < use_kf && got < use_kf; i++) {
                    cap_camera_t cam;
                    if (qtsq_capture_sample(t->cap, i, &cam, NULL) != 1) continue;
                    float *dep = (float *)malloc((size_t)W * H * sizeof(float));
                    if (!dep) continue;
                    if (sfm_render_full(sf, n, &cam, NULL, dep, NULL, NULL) != 0){
                        free(dep); continue;
                    }
                    deps[got] = dep;
                    qtsq_tsdf_camera_t *tc = &tcams[got];
                    memcpy(tc->pos, &cam.pos, 3 * sizeof(float));
                    memcpy(tc->right, &cam.right, 3 * sizeof(float));
                    memcpy(tc->up, &cam.up, 3 * sizeof(float));
                    memcpy(tc->forward, &cam.forward, 3 * sizeof(float));
                    tc->fx = cam.fx; tc->fy = cam.fy;
                    tc->cx = cam.cx; tc->cy = cam.cy;
                    tc->width = W; tc->height = H;
                    got++;
                }
                if (got >= 3) {
                    /* Grid sized to the frustum span, capped at 96³. */
                    float center[3], half[3];
                    qtsq_tsdf_grid_bounds(tcams, got, 0.3f, 5.0f, center, half);
                    float span = fmaxf(fmaxf(half[0], half[1]), half[2]);
                    float voxel = span / 48.0f;
                    if (voxel < 0.005f) voxel = 0.005f;
                    if (voxel > 0.05f) voxel = 0.05f;
                    int rc = qtsq_tsdf_reconstruct((const float *const *)deps,
                                                   tcams, got, W, H,
                                                   96, voxel, 4.0f * voxel,
                                                   &mesh);
                    TLOG("trainer_save: TSDF %d views -> %u verts / %u tris (rc=%d)",
                         got, mesh.nv, mesh.nt, rc);
                } else {
                    TLOG("trainer_save: only %d usable views — mesh skipped", got);
                }
            }
            if (deps) {
                for (int i = 0; i < use_kf; i++) free(deps[i]);
                free(deps);
            }
            free(tcams);
            free(sf);
        }
    }

    int result = mesh.nv > 0
        ? qtsq_surfel_container_write_mesh(path, &model, &mesh)
        : qtsq_surfel_container_write(path, &model);
    TLOG("trainer_save: %d surfels(, %u mesh tris) -> %s (rc=%d)",
         n, mesh.nt, path, result);

    qtsq_mesh_free(&mesh);
    qtsq_surfel_model_free(&model);
    return result;
}

#else
/* Non-Android stubs */
qtsq_trainer_t *trainer_create(qtsq_capture_t *cap) { (void)cap; return NULL; }
void trainer_seed_from_points(qtsq_trainer_t *t, const float *xyz, const float *rgb, int n) { (void)t; (void)xyz; (void)rgb; (void)n; }
void trainer_seed_random(qtsq_trainer_t *t, int count, const cap_camera_t *cam, float spread) { (void)t; (void)count; (void)cam; (void)spread; }
float trainer_step(qtsq_trainer_t *t) { (void)t; return 0; }
void trainer_density_control(qtsq_trainer_t *t) { (void)t; }
int trainer_start_loop(qtsq_trainer_t *t) { (void)t; return -1; }
void trainer_stop_loop(qtsq_trainer_t *t) { (void)t; }
trainer_stats_t trainer_get_stats(const qtsq_trainer_t *t) { (void)t; trainer_stats_t s = {0}; return s; }
const float *trainer_get_surfels(const qtsq_trainer_t *t, int *out_count) { (void)t; if (out_count) *out_count = 0; return NULL; }
int trainer_snapshot_surfels(const qtsq_trainer_t *t, float *out, int max) { (void)t; (void)out; (void)max; return 0; }
int trainer_parity_forward(qtsq_trainer_t *t, float *max_diff) { (void)t; (void)max_diff; return -1; }
int trainer_save(const qtsq_trainer_t *t, const char *path) { (void)t; (void)path; return -1; }
void trainer_destroy(qtsq_trainer_t *t) { (void)t; }
#endif
