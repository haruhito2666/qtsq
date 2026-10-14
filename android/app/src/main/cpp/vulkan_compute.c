/*
 * QTSQ Vulkan Compute — GPU-accelerated decompression (IFS iteration)
 *
 * Used only for decompression where each block independently applies
 * a known transform: val = contrast * downsample_sym(domain) + brightness.
 * No searching, no branching — ideal GPU workload.
 *
 * Compression uses the CPU path with NEON SIMD.
 */

#include "vulkan_compute.h"

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define VK_TAG "QTSQ_Vulkan"
#define VKLOG(...) __android_log_print(ANDROID_LOG_INFO, VK_TAG, __VA_ARGS__)
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, VK_TAG, __VA_ARGS__)

/* Embedded SPIR-V shader for decompression */
#include "shaders/decompress_spv.h"

/* Push constant layout — must match decompress.comp */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t range_size;
    uint32_t domain_size;
    uint32_t blocks_x;
    uint32_t blocks_y;
    uint32_t total_blocks;
} gpu_decomp_pc_t;

/* Vulkan compute context */
struct vk_compute_ctx {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue compute_queue;
    uint32_t queue_family_index;

    VkDescriptorSetLayout desc_set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorPool desc_pool;
    VkCommandPool cmd_pool;

    VkPhysicalDeviceMemoryProperties mem_props;
    int initialized;
};

static uint32_t find_memory_type(vk_compute_ctx_t *ctx, uint32_t type_filter,
                                  VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < ctx->mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (ctx->mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int create_buffer(vk_compute_ctx_t *ctx, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkMemoryPropertyFlags mem_flags,
                          VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo buf_info = {0};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = size;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(ctx->device, &buf_info, NULL, buffer) != VK_SUCCESS) return -1;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(ctx->device, *buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info = {0};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(ctx, mem_req.memoryTypeBits, mem_flags);

    if (alloc_info.memoryTypeIndex == UINT32_MAX) return -1;
    if (vkAllocateMemory(ctx->device, &alloc_info, NULL, memory) != VK_SUCCESS) return -1;
    if (vkBindBufferMemory(ctx->device, *buffer, *memory, 0) != VK_SUCCESS) return -1;

    return 0;
}

vk_compute_ctx_t *vk_compute_init(void) {
    vk_compute_ctx_t *ctx = (vk_compute_ctx_t *)calloc(1, sizeof(vk_compute_ctx_t));
    if (!ctx) return NULL;

    /* ── Instance ── */
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "QTSQ";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo inst_info = {0};
    inst_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    inst_info.pApplicationInfo = &app_info;

    if (vkCreateInstance(&inst_info, NULL, &ctx->instance) != VK_SUCCESS) {
        VKERR("Failed to create Vulkan instance");
        free(ctx);
        return NULL;
    }

    /* ── Physical device ── */
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, NULL);
    if (device_count == 0) {
        VKERR("No Vulkan physical devices");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    VkPhysicalDevice devices[8];
    if (device_count > 8) device_count = 8;
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);
    ctx->physical_device = devices[0];

    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &ctx->mem_props);

    VkPhysicalDeviceProperties dev_props;
    vkGetPhysicalDeviceProperties(ctx->physical_device, &dev_props);
    VKLOG("GPU: %s (Vulkan %d.%d)",
          dev_props.deviceName,
          VK_VERSION_MAJOR(dev_props.apiVersion),
          VK_VERSION_MINOR(dev_props.apiVersion));

    /* ── Queue family with compute ── */
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_count, NULL);
    VkQueueFamilyProperties *queue_props =
        (VkQueueFamilyProperties *)malloc(queue_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_count, queue_props);

    ctx->queue_family_index = UINT32_MAX;
    for (uint32_t i = 0; i < queue_count; i++) {
        if (queue_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            ctx->queue_family_index = i;
            break;
        }
    }
    free(queue_props);

    if (ctx->queue_family_index == UINT32_MAX) {
        VKERR("No compute queue family");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    /* ── Logical device + queue ── */
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {0};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = ctx->queue_family_index;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo dev_info = {0};
    dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = &queue_info;

    if (vkCreateDevice(ctx->physical_device, &dev_info, NULL, &ctx->device) != VK_SUCCESS) {
        VKERR("Failed to create Vulkan device");
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        return NULL;
    }

    vkGetDeviceQueue(ctx->device, ctx->queue_family_index, 0, &ctx->compute_queue);

    /* ── Descriptor set layout (3 storage buffers: current, next, transforms) ── */
    VkDescriptorSetLayoutBinding bindings[3] = {0};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = {0};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(ctx->device, &layout_info, NULL,
                                     &ctx->desc_set_layout) != VK_SUCCESS) {
        VKERR("Failed to create descriptor set layout");
        goto cleanup_device;
    }

    /* ── Push constant range ── */
    VkPushConstantRange push_range = {0};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(gpu_decomp_pc_t);

    VkPipelineLayoutCreateInfo pipe_layout_info = {0};
    pipe_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_info.setLayoutCount = 1;
    pipe_layout_info.pSetLayouts = &ctx->desc_set_layout;
    pipe_layout_info.pushConstantRangeCount = 1;
    pipe_layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(ctx->device, &pipe_layout_info, NULL,
                                &ctx->pipeline_layout) != VK_SUCCESS) {
        VKERR("Failed to create pipeline layout");
        goto cleanup_desc_layout;
    }

    /* ── Shader module from embedded SPIR-V ── */
    VkShaderModuleCreateInfo shader_info = {0};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = decompress_spv_size;
    shader_info.pCode = decompress_spv;

    VkShaderModule shader_module;
    if (vkCreateShaderModule(ctx->device, &shader_info, NULL, &shader_module) != VK_SUCCESS) {
        VKERR("Failed to create shader module");
        goto cleanup_pipe_layout;
    }

    /* ── Compute pipeline ── */
    VkComputePipelineCreateInfo compute_info = {0};
    compute_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    compute_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute_info.stage.module = shader_module;
    compute_info.stage.pName = "main";
    compute_info.layout = ctx->pipeline_layout;

    if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1,
                                  &compute_info, NULL, &ctx->pipeline) != VK_SUCCESS) {
        VKERR("Failed to create compute pipeline");
        vkDestroyShaderModule(ctx->device, shader_module, NULL);
        goto cleanup_pipe_layout;
    }
    vkDestroyShaderModule(ctx->device, shader_module, NULL);

    /* ── Descriptor pool ── */
    VkDescriptorPoolSize pool_size = {0};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 64;

    VkDescriptorPoolCreateInfo pool_info = {0};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 16;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(ctx->device, &pool_info, NULL, &ctx->desc_pool) != VK_SUCCESS) {
        VKERR("Failed to create descriptor pool");
        goto cleanup_pipeline;
    }

    /* ── Command pool ── */
    VkCommandPoolCreateInfo cmd_info = {0};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmd_info.queueFamilyIndex = ctx->queue_family_index;
    cmd_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(ctx->device, &cmd_info, NULL, &ctx->cmd_pool) != VK_SUCCESS) {
        VKERR("Failed to create command pool");
        goto cleanup_desc_pool;
    }

    ctx->initialized = 1;
    VKLOG("Vulkan decompression initialized");
    return ctx;

cleanup_desc_pool:
    vkDestroyDescriptorPool(ctx->device, ctx->desc_pool, NULL);
cleanup_pipeline:
    vkDestroyPipeline(ctx->device, ctx->pipeline, NULL);
cleanup_pipe_layout:
    vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
cleanup_desc_layout:
    vkDestroyDescriptorSetLayout(ctx->device, ctx->desc_set_layout, NULL);
cleanup_device:
    vkDestroyDevice(ctx->device, NULL);
    vkDestroyInstance(ctx->instance, NULL);
    free(ctx);
    return NULL;
}

void vk_compute_destroy(vk_compute_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->initialized) {
        vkDeviceWaitIdle(ctx->device);
        vkDestroyCommandPool(ctx->device, ctx->cmd_pool, NULL);
        vkDestroyDescriptorPool(ctx->device, ctx->desc_pool, NULL);
        vkDestroyPipeline(ctx->device, ctx->pipeline, NULL);
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->desc_set_layout, NULL);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroyInstance(ctx->instance, NULL);
    }
    free(ctx);
}

int vk_compute_decompress_iteration(
    vk_compute_ctx_t *ctx,
    const float *current,
    const void *transforms,
    float *next,
    uint32_t num_blocks,
    const vk_decompress_params_t *params)
{
    if (!ctx || !ctx->initialized || !current || !transforms || !next) return -1;

    uint32_t pixel_count = params->width * params->height;
    VkDeviceSize pixel_buf_size = (VkDeviceSize)pixel_count * sizeof(float);
    VkDeviceSize transform_buf_size = (VkDeviceSize)num_blocks * 8;  /* 8 bytes per transform */

    VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /* ── Create 3 buffers: current(in), next(out), transforms(in) ── */
    VkBuffer current_buf, next_buf, transform_buf;
    VkDeviceMemory current_mem, next_mem, transform_mem;

    if (create_buffer(ctx, pixel_buf_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       host_visible, &current_buf, &current_mem) != 0) {
        VKERR("Failed to create current buffer");
        return -1;
    }

    if (create_buffer(ctx, pixel_buf_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       host_visible, &next_buf, &next_mem) != 0) {
        VKERR("Failed to create next buffer");
        goto cleanup_cur;
    }

    if (create_buffer(ctx, transform_buf_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       host_visible, &transform_buf, &transform_mem) != 0) {
        VKERR("Failed to create transform buffer");
        goto cleanup_next;
    }

    /* ── Upload data ── */
    void *mapped;

    /* Upload current iteration state */
    vkMapMemory(ctx->device, current_mem, 0, pixel_buf_size, 0, &mapped);
    memcpy(mapped, current, (size_t)pixel_buf_size);
    vkUnmapMemory(ctx->device, current_mem);

    /* Upload (pre-zero) next buffer */
    vkMapMemory(ctx->device, next_mem, 0, pixel_buf_size, 0, &mapped);
    memcpy(mapped, current, (size_t)pixel_buf_size);  /* start from current */
    vkUnmapMemory(ctx->device, next_mem);

    /* Upload transforms */
    vkMapMemory(ctx->device, transform_mem, 0, transform_buf_size, 0, &mapped);
    memcpy(mapped, transforms, (size_t)transform_buf_size);
    vkUnmapMemory(ctx->device, transform_mem);

    /* ── Descriptor set ── */
    VkDescriptorSet desc_set;
    VkDescriptorSetAllocateInfo alloc_info = {0};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = ctx->desc_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &ctx->desc_set_layout;

    if (vkAllocateDescriptorSets(ctx->device, &alloc_info, &desc_set) != VK_SUCCESS) {
        VKERR("Failed to allocate descriptor set");
        goto cleanup_all;
    }

    VkDescriptorBufferInfo buf_infos[3] = {0};
    buf_infos[0].buffer = current_buf;
    buf_infos[0].offset = 0;
    buf_infos[0].range = pixel_buf_size;
    buf_infos[1].buffer = next_buf;
    buf_infos[1].offset = 0;
    buf_infos[1].range = pixel_buf_size;
    buf_infos[2].buffer = transform_buf;
    buf_infos[2].offset = 0;
    buf_infos[2].range = transform_buf_size;

    VkWriteDescriptorSet writes[3] = {0};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = desc_set;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 3, writes, 0, NULL);

    /* ── Command buffer ── */
    VkCommandBuffer cmd_buf;
    VkCommandBufferAllocateInfo cmd_alloc = {0};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx->cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(ctx->device, &cmd_alloc, &cmd_buf) != VK_SUCCESS) {
        VKERR("Failed to allocate command buffer");
        goto cleanup_desc;
    }

    /* ── Record + dispatch ── */
    VkCommandBufferBeginInfo begin_info = {0};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd_buf, &begin_info);

    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipeline);
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                             ctx->pipeline_layout, 0, 1, &desc_set, 0, NULL);

    gpu_decomp_pc_t pc = {0};
    pc.width = params->width;
    pc.height = params->height;
    pc.range_size = params->range_size;
    pc.domain_size = params->domain_size;
    pc.blocks_x = params->blocks_x;
    pc.blocks_y = params->blocks_y;
    pc.total_blocks = num_blocks;

    vkCmdPushConstants(cmd_buf, ctx->pipeline_layout,
                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    /* Decompression is lightweight per-block: dispatch all at once.
     * Each workgroup = 1 thread processing 16×16 = 256 pixels.
     * ~50K blocks finish in <50ms on Adreno 750. */
    vkCmdDispatch(cmd_buf, num_blocks, 1, 1);

    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_buf,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT,
                          0, 1, &barrier, 0, NULL, 0, NULL);

    vkEndCommandBuffer(cmd_buf);

    /* ── Submit and wait ── */
    VkFence fence;
    VkFenceCreateInfo fence_info = {0};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx->device, &fence_info, NULL, &fence);

    VkSubmitInfo submit_info = {0};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf;

    vkQueueSubmit(ctx->compute_queue, 1, &submit_info, fence);
    vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(ctx->device, fence, NULL);

    /* ── Read back result ── */
    vkMapMemory(ctx->device, next_mem, 0, pixel_buf_size, 0, &mapped);
    memcpy(next, mapped, (size_t)pixel_buf_size);
    vkUnmapMemory(ctx->device, next_mem);

    /* Cleanup */
    vkFreeCommandBuffers(ctx->device, ctx->cmd_pool, 1, &cmd_buf);
    vkFreeDescriptorSets(ctx->device, ctx->desc_pool, 1, &desc_set);
    vkDestroyBuffer(ctx->device, transform_buf, NULL);
    vkFreeMemory(ctx->device, transform_mem, NULL);
    vkDestroyBuffer(ctx->device, next_buf, NULL);
    vkFreeMemory(ctx->device, next_mem, NULL);
    vkDestroyBuffer(ctx->device, current_buf, NULL);
    vkFreeMemory(ctx->device, current_mem, NULL);
    return 0;

cleanup_desc:
    vkFreeDescriptorSets(ctx->device, ctx->desc_pool, 1, &desc_set);
cleanup_all:
    vkDestroyBuffer(ctx->device, transform_buf, NULL);
    vkFreeMemory(ctx->device, transform_mem, NULL);
cleanup_next:
    vkDestroyBuffer(ctx->device, next_buf, NULL);
    vkFreeMemory(ctx->device, next_mem, NULL);
cleanup_cur:
    vkDestroyBuffer(ctx->device, current_buf, NULL);
    vkFreeMemory(ctx->device, current_mem, NULL);
    return -1;
}

/* ═══════════════════════════════════════════════════════════
 *  GPU Triangle Rasterization Pipeline
 *  Each pixel is a parallel GPU thread that checks all faces.
 * ═══════════════════════════════════════════════════════════ */

#include "shaders/rasterize_spv.h"
#include "shaders/splat_render_spv.h"

/* Helper: create a one-shot compute pipeline from SPIR-V */
static int create_oneshot_pipeline(vk_compute_ctx_t *ctx,
                                    const unsigned char *spirv, unsigned int spirv_len,
                                    uint32_t num_bindings, uint32_t push_constant_size,
                                    VkDescriptorSetLayout *out_layout,
                                    VkPipelineLayout *out_pipe_layout,
                                    VkPipeline *out_pipeline) {
    /* Descriptor set layout */
    VkDescriptorSetLayoutBinding bindings[8] = {0};
    for (uint32_t i = 0; i < num_bindings && i < 8; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl_info = {0};
    dsl_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_info.bindingCount = num_bindings;
    dsl_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(ctx->device, &dsl_info, NULL, out_layout) != VK_SUCCESS)
        return -1;

    /* Pipeline layout with push constants */
    VkPushConstantRange push = {0};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = push_constant_size;
    VkPipelineLayoutCreateInfo pl_info = {0};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = out_layout;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(ctx->device, &pl_info, NULL, out_pipe_layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(ctx->device, *out_layout, NULL);
        return -1;
    }

    /* Shader module */
    VkShaderModuleCreateInfo sm_info = {0};
    sm_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm_info.codeSize = spirv_len;
    sm_info.pCode = (const uint32_t *)spirv;
    VkShaderModule module;
    if (vkCreateShaderModule(ctx->device, &sm_info, NULL, &module) != VK_SUCCESS) {
        vkDestroyPipelineLayout(ctx->device, *out_pipe_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, *out_layout, NULL);
        return -1;
    }

    VkComputePipelineCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = *out_pipe_layout;
    VkResult r = vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &ci, NULL, out_pipeline);
    vkDestroyShaderModule(ctx->device, module, NULL);
    if (r != VK_SUCCESS) {
        vkDestroyPipelineLayout(ctx->device, *out_pipe_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, *out_layout, NULL);
        return -1;
    }
    return 0;
}

/* Helper: run a compute dispatch with given pipeline, descriptors, push constants */
static int gpu_dispatch(vk_compute_ctx_t *ctx,
                        VkPipeline pipeline, VkPipelineLayout pipe_layout,
                        VkDescriptorSet desc_set,
                        const void *push_data, uint32_t push_size,
                        uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) {
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc = {0};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx->cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(ctx->device, &cmd_alloc, &cmd) != VK_SUCCESS)
        return -1;

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipe_layout, 0, 1, &desc_set, 0, NULL);
    if (push_data && push_size > 0)
        vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push_data);
    vkCmdDispatch(cmd, groups_x, groups_y, groups_z);

    VkMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);

    VkFence fence;
    VkFenceCreateInfo fi = {0};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(ctx->device, &fi, NULL, &fence);
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx->compute_queue, 1, &si, fence);
    vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(ctx->device, fence, NULL);
    vkFreeCommandBuffers(ctx->device, ctx->cmd_pool, 1, &cmd);
    return 0;
}

typedef struct { uint32_t width, height, num_faces, num_verts, channels, has_tier_mask; } raster_pc_t;

int vk_compute_rasterize(
    vk_compute_ctx_t *ctx,
    const vk_vertex_t *verts, uint32_t nv,
    const vk_face_t *faces, uint32_t nf,
    uint32_t width, uint32_t height,
    uint8_t *output_rgb,
    uint8_t *blur_map,
    const uint8_t *tier_mask)
{
    if (!ctx || !ctx->initialized || !verts || !faces || !output_rgb) return -1;
    VKLOG("GPU rasterize: %u verts, %u faces, %ux%u", nv, nf, width, height);

    uint32_t pixel_count = width * height;

    /* Create pipeline */
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    if (create_oneshot_pipeline(ctx, rasterize_spv, rasterize_spv_len,
                                6, sizeof(raster_pc_t), &dsl, &pl, &pipe) != 0) {
        VKERR("GPU rasterize: pipeline creation failed");
        return -1;
    }

    VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /* Buffer sizes */
    VkDeviceSize out_size    = (VkDeviceSize)pixel_count * 4;  /* RGBA uint32 per pixel */
    VkDeviceSize vert_size   = (VkDeviceSize)nv * 3 * sizeof(float);  /* x,y,rgb_packed */
    VkDeviceSize face_size   = (VkDeviceSize)nf * 3 * sizeof(uint32_t);  /* planar v0,v1,v2 */
    VkDeviceSize bbox_size   = (VkDeviceSize)nf * sizeof(uint64_t);  /* uvec2 per face */
    VkDeviceSize blur_size   = (VkDeviceSize)pixel_count * 4;  /* 1 uint32 per pixel for correct atomicMax */
    VkDeviceSize mask_size   = (VkDeviceSize)((pixel_count + 3) / 4) * 4;

    VkBuffer out_buf, vert_buf, face_buf, bbox_buf, blur_buf, mask_buf;
    VkDeviceMemory out_mem, vert_mem, face_mem, bbox_mem, blur_mem, mask_mem;

    if (create_buffer(ctx, out_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &out_buf, &out_mem) != 0 ||
        create_buffer(ctx, vert_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &vert_buf, &vert_mem) != 0 ||
        create_buffer(ctx, face_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &face_buf, &face_mem) != 0 ||
        create_buffer(ctx, bbox_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &bbox_buf, &bbox_mem) != 0 ||
        create_buffer(ctx, blur_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &blur_buf, &blur_mem) != 0 ||
        create_buffer(ctx, mask_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &mask_buf, &mask_mem) != 0) {
        VKERR("GPU rasterize: buffer creation failed");
        vkDestroyPipeline(ctx->device, pipe, NULL);
        vkDestroyPipelineLayout(ctx->device, pl, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
        return -1;
    }

    /* Upload vertex data: interleaved x, y, rgb_packed as float */
    void *mapped;
    vkMapMemory(ctx->device, vert_mem, 0, vert_size, 0, &mapped);
    float *vdata = (float *)mapped;
    for (uint32_t i = 0; i < nv; i++) {
        vdata[i * 3 + 0] = verts[i].x;
        vdata[i * 3 + 1] = verts[i].y;
        uint32_t rgb = (uint32_t)verts[i].r | ((uint32_t)verts[i].g << 8) | ((uint32_t)verts[i].b << 16);
        union { uint32_t u; float f; } conv; conv.u = rgb;
        vdata[i * 3 + 2] = conv.f;
    }
    vkUnmapMemory(ctx->device, vert_mem);

    /* Upload face data (planar: all v0s, all v1s, all v2s) and compute bounding boxes */
    vkMapMemory(ctx->device, face_mem, 0, face_size, 0, &mapped);
    uint32_t *fdata = (uint32_t *)mapped;
    for (uint32_t i = 0; i < nf; i++) fdata[i] = faces[i].v0;
    for (uint32_t i = 0; i < nf; i++) fdata[nf + i] = faces[i].v1;
    for (uint32_t i = 0; i < nf; i++) fdata[nf * 2 + i] = faces[i].v2;
    vkUnmapMemory(ctx->device, face_mem);

    /* Compute and upload bounding boxes */
    vkMapMemory(ctx->device, bbox_mem, 0, bbox_size, 0, &mapped);
    uint32_t *bbdata = (uint32_t *)mapped;  /* pairs of uint32 */
    for (uint32_t i = 0; i < nf; i++) {
        float x0 = verts[faces[i].v0].x, y0 = verts[faces[i].v0].y;
        float x1 = verts[faces[i].v1].x, y1 = verts[faces[i].v1].y;
        float x2 = verts[faces[i].v2].x, y2 = verts[faces[i].v2].y;
        float minx = x0, miny = y0, maxx = x0, maxy = y0;
        if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
        if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
        if (y1 < miny) miny = y1; if (y1 > maxy) maxy = y1;
        if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
        uint32_t iminx = (uint32_t)(minx < 0.0f ? 0.0f : minx); if (iminx >= width) iminx = width - 1;
        uint32_t iminy = (uint32_t)(miny < 0.0f ? 0.0f : miny); if (iminy >= height) iminy = height - 1;
        uint32_t imaxx = (uint32_t)(maxx < 0.0f ? 0.0f : maxx + 0.5f); if (imaxx >= width) imaxx = width - 1;
        uint32_t imaxy = (uint32_t)(maxy < 0.0f ? 0.0f : maxy + 0.5f); if (imaxy >= height) imaxy = height - 1;
        bbdata[i * 2 + 0] = iminx | (iminy << 16);
        bbdata[i * 2 + 1] = imaxx | (imaxy << 16);
    }
    vkUnmapMemory(ctx->device, bbox_mem);

    /* Initialize output and zoom out */
    vkMapMemory(ctx->device, out_mem, 0, out_size, 0, &mapped);
    if (output_rgb) {
        uint32_t *m_out = (uint32_t *)mapped;
        for (uint32_t i = 0; i < pixel_count; i++) {
            m_out[i] = (uint32_t)output_rgb[i * 3 + 0] | 
                      ((uint32_t)output_rgb[i * 3 + 1] << 8) | 
                      ((uint32_t)output_rgb[i * 3 + 2] << 16) | (0xFFu << 24);
        }
    } else {
        memset(mapped, 0, (size_t)out_size);
    }
    vkUnmapMemory(ctx->device, out_mem);
    vkMapMemory(ctx->device, blur_mem, 0, blur_size, 0, &mapped);
    memset(mapped, 0, (size_t)blur_size);
    vkUnmapMemory(ctx->device, blur_mem);

    /* Upload tier mask */
    vkMapMemory(ctx->device, mask_mem, 0, mask_size, 0, &mapped);
    if (tier_mask) {
        uint32_t *mdata = (uint32_t *)mapped;
        uint32_t npacked = (pixel_count + 3) / 4;
        memset(mdata, 0, npacked * 4);
        for (uint32_t i = 0; i < pixel_count; i++) {
            mdata[i / 4] |= ((uint32_t)tier_mask[i] << ((i % 4) * 8));
        }
    } else {
        memset(mapped, 0, (size_t)mask_size);
    }
    vkUnmapMemory(ctx->device, mask_mem);

    /* Descriptor set */
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo ds_alloc = {0};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = ctx->desc_pool;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(ctx->device, &ds_alloc, &ds) != VK_SUCCESS) {
        VKERR("GPU rasterize: descriptor alloc failed");
        goto rast_cleanup;
    }

    VkDescriptorBufferInfo buf_infos[6] = {
        {out_buf, 0, out_size}, {vert_buf, 0, vert_size},
        {face_buf, 0, face_size}, {bbox_buf, 0, bbox_size},
        {blur_buf, 0, blur_size}, {mask_buf, 0, mask_size}
    };
    VkWriteDescriptorSet writes[6] = {0};
    for (int i = 0; i < 6; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = ds;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 6, writes, 0, NULL);

    vkMapMemory(ctx->device, blur_mem, 0, blur_size, 0, &mapped);
    memset(mapped, 0, blur_size);
    vkUnmapMemory(ctx->device, blur_mem);

    /* Dispatch: 1 thread per face, workgroups of 64 
     * NOTE: tier_mask is DISABLED on GPU (has_tier_mask=0) because the extra
     * global memory reads per pixel on 2.2M+ faces overwhelms mobile GPUs.
     * Splat fg_mask (from blur_map) handles layering on the CPU side instead. */
    raster_pc_t pc = {width, height, nf, nv, 3, 0u};
    uint32_t gx = (nf + 63) / 64;
    uint32_t gy = 1;
    VKLOG("GPU rasterize: dispatching %u workgroups (threads=faces)", gx);

    if (gpu_dispatch(ctx, pipe, pl, ds, &pc, sizeof(pc), gx, gy, 1) != 0) {
        VKERR("GPU rasterize: dispatch failed");
        goto rast_cleanup;
    }

    /* Read back output */
    vkMapMemory(ctx->device, out_mem, 0, out_size, 0, &mapped);
    uint32_t *rgba = (uint32_t *)mapped;
    for (uint32_t i = 0; i < pixel_count; i++) {
        output_rgb[i * 3 + 0] = (uint8_t)(rgba[i] & 0xFF);
        output_rgb[i * 3 + 1] = (uint8_t)((rgba[i] >> 8) & 0xFF);
        output_rgb[i * 3 + 2] = (uint8_t)((rgba[i] >> 16) & 0xFF);
    }
    vkUnmapMemory(ctx->device, out_mem);

    /* Read back blur map */
    if (blur_map) {
        vkMapMemory(ctx->device, blur_mem, 0, blur_size, 0, &mapped);
        uint32_t *bm = (uint32_t *)mapped;
        for (uint32_t i = 0; i < pixel_count; i++) {
            blur_map[i] = (uint8_t)(bm[i] > 255 ? 255 : bm[i]);
        }
        vkUnmapMemory(ctx->device, blur_mem);
    }

    VKLOG("GPU rasterize: complete");
    vkFreeDescriptorSets(ctx->device, ctx->desc_pool, 1, &ds);
    vkDestroyBuffer(ctx->device, mask_buf, NULL); vkFreeMemory(ctx->device, mask_mem, NULL);
    vkDestroyBuffer(ctx->device, blur_buf, NULL); vkFreeMemory(ctx->device, blur_mem, NULL);
    vkDestroyBuffer(ctx->device, bbox_buf, NULL); vkFreeMemory(ctx->device, bbox_mem, NULL);
    vkDestroyBuffer(ctx->device, face_buf, NULL); vkFreeMemory(ctx->device, face_mem, NULL);
    vkDestroyBuffer(ctx->device, vert_buf, NULL); vkFreeMemory(ctx->device, vert_mem, NULL);
    vkDestroyBuffer(ctx->device, out_buf, NULL); vkFreeMemory(ctx->device, out_mem, NULL);
    vkDestroyPipeline(ctx->device, pipe, NULL);
    vkDestroyPipelineLayout(ctx->device, pl, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
    return 0;

rast_cleanup:
    vkDestroyBuffer(ctx->device, mask_buf, NULL); vkFreeMemory(ctx->device, mask_mem, NULL);
    vkDestroyBuffer(ctx->device, blur_buf, NULL); vkFreeMemory(ctx->device, blur_mem, NULL);
    vkDestroyBuffer(ctx->device, bbox_buf, NULL); vkFreeMemory(ctx->device, bbox_mem, NULL);
    vkDestroyBuffer(ctx->device, face_buf, NULL); vkFreeMemory(ctx->device, face_mem, NULL);
    vkDestroyBuffer(ctx->device, vert_buf, NULL); vkFreeMemory(ctx->device, vert_mem, NULL);
    vkDestroyBuffer(ctx->device, out_buf, NULL); vkFreeMemory(ctx->device, out_mem, NULL);
    vkDestroyPipeline(ctx->device, pipe, NULL);
    vkDestroyPipelineLayout(ctx->device, pl, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
    return -1;
}

/* ═══════════════════════════════════════════════════════════
 *  GPU Splat Rendering
 * ═══════════════════════════════════════════════════════════ */

typedef struct { uint32_t width, height, num_splats, has_fg_mask; } splat_pc_t;

int vk_compute_render_splats(
    vk_compute_ctx_t *ctx,
    const vk_splat_t *splats, uint32_t num_splats,
    uint32_t width, uint32_t height,
    uint8_t *image_rgb,
    const uint8_t *fg_mask)
{
    if (!ctx || !ctx->initialized || !splats || !image_rgb || num_splats == 0) return -1;
    VKLOG("GPU splats: %u splats, %ux%u", num_splats, width, height);

    uint32_t pixel_count = width * height;

    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe;
    if (create_oneshot_pipeline(ctx, splat_render_spv, splat_render_spv_len,
                                3, sizeof(splat_pc_t), &dsl, &pl, &pipe) != 0) {
        VKERR("GPU splats: pipeline creation failed");
        return -1;
    }

    VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkDeviceSize img_size   = (VkDeviceSize)pixel_count * 4;
    VkDeviceSize splat_size = (VkDeviceSize)num_splats * 16;  /* uvec4 = 16 bytes per splat */
    VkDeviceSize mask_size  = (VkDeviceSize)((pixel_count + 3) / 4) * 4;

    VkBuffer img_buf, spl_buf, mask_buf;
    VkDeviceMemory img_mem, spl_mem, mask_mem;

    if (create_buffer(ctx, img_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &img_buf, &img_mem) != 0 ||
        create_buffer(ctx, splat_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &spl_buf, &spl_mem) != 0 ||
        create_buffer(ctx, mask_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_vis, &mask_buf, &mask_mem) != 0) {
        VKERR("GPU splats: buffer creation failed");
        vkDestroyPipeline(ctx->device, pipe, NULL);
        vkDestroyPipelineLayout(ctx->device, pl, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
        return -1;
    }

    /* Upload image as RGBA */
    void *mapped;
    vkMapMemory(ctx->device, img_mem, 0, img_size, 0, &mapped);
    uint32_t *rgba = (uint32_t *)mapped;
    for (uint32_t i = 0; i < pixel_count; i++) {
        rgba[i] = (uint32_t)image_rgb[i * 3] |
                  ((uint32_t)image_rgb[i * 3 + 1] << 8) |
                  ((uint32_t)image_rgb[i * 3 + 2] << 16) |
                  (0xFFu << 24);
    }
    vkUnmapMemory(ctx->device, img_mem);

    /* Upload splats as uvec4 (16 bytes each) packed */
    vkMapMemory(ctx->device, spl_mem, 0, splat_size, 0, &mapped);
    uint32_t *sdata = (uint32_t *)mapped;
    for (uint32_t i = 0; i < num_splats; i++) {
        /* uvec4.x = x | (y << 16) */
        sdata[i * 4 + 0] = (uint32_t)splats[i].x | ((uint32_t)splats[i].y << 16);
        /* uvec4.y = sx | (sy << 8) | (theta << 16) | (alpha << 24) */
        sdata[i * 4 + 1] = (uint32_t)splats[i].sx | ((uint32_t)splats[i].sy << 8) |
                            ((uint32_t)splats[i].theta << 16) | ((uint32_t)splats[i].alpha << 24);
        /* uvec4.z = r | (g << 8) | (b << 16) */
        sdata[i * 4 + 2] = (uint32_t)splats[i].r | ((uint32_t)splats[i].g << 8) |
                            ((uint32_t)splats[i].b << 16);
        /* uvec4.w = unused */
        sdata[i * 4 + 3] = 0;
    }
    vkUnmapMemory(ctx->device, spl_mem);

    /* Upload fg mask */
    vkMapMemory(ctx->device, mask_mem, 0, mask_size, 0, &mapped);
    if (fg_mask) {
        uint32_t *mdata = (uint32_t *)mapped;
        uint32_t npacked = (pixel_count + 3) / 4;
        memset(mdata, 0, npacked * 4);
        for (uint32_t i = 0; i < pixel_count; i++) {
            if (fg_mask[i])
                mdata[i / 4] |= (0xFFu << ((i % 4) * 8));
        }
    } else {
        memset(mapped, 0, (size_t)mask_size);
    }
    vkUnmapMemory(ctx->device, mask_mem);

    /* Descriptor set */
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo ds_alloc = {0};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = ctx->desc_pool;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(ctx->device, &ds_alloc, &ds) != VK_SUCCESS) {
        VKERR("GPU splats: descriptor alloc failed");
        goto splat_cleanup;
    }

    VkDescriptorBufferInfo buf_infos[3] = {
        {img_buf, 0, img_size}, {spl_buf, 0, splat_size}, {mask_buf, 0, mask_size}
    };
    VkWriteDescriptorSet ws[3] = {0};
    for (int i = 0; i < 3; i++) {
        ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[i].dstSet = ds;
        ws[i].dstBinding = (uint32_t)i;
        ws[i].descriptorCount = 1;
        ws[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(ctx->device, 3, ws, 0, NULL);

    splat_pc_t pc = {width, height, num_splats, fg_mask ? 1u : 0u};
    uint32_t gx = (width + 15) / 16;
    uint32_t gy = (height + 15) / 16;
    VKLOG("GPU splats: dispatching %u×%u workgroups", gx, gy);

    if (gpu_dispatch(ctx, pipe, pl, ds, &pc, sizeof(pc), gx, gy, 1) != 0) {
        VKERR("GPU splats: dispatch failed");
        goto splat_cleanup;
    }

    /* Read back */
    vkMapMemory(ctx->device, img_mem, 0, img_size, 0, &mapped);
    rgba = (uint32_t *)mapped;
    for (uint32_t i = 0; i < pixel_count; i++) {
        image_rgb[i * 3 + 0] = (uint8_t)(rgba[i] & 0xFF);
        image_rgb[i * 3 + 1] = (uint8_t)((rgba[i] >> 8) & 0xFF);
        image_rgb[i * 3 + 2] = (uint8_t)((rgba[i] >> 16) & 0xFF);
    }
    vkUnmapMemory(ctx->device, img_mem);

    VKLOG("GPU splats: complete");
    vkFreeDescriptorSets(ctx->device, ctx->desc_pool, 1, &ds);
    vkDestroyBuffer(ctx->device, mask_buf, NULL); vkFreeMemory(ctx->device, mask_mem, NULL);
    vkDestroyBuffer(ctx->device, spl_buf, NULL); vkFreeMemory(ctx->device, spl_mem, NULL);
    vkDestroyBuffer(ctx->device, img_buf, NULL); vkFreeMemory(ctx->device, img_mem, NULL);
    vkDestroyPipeline(ctx->device, pipe, NULL);
    vkDestroyPipelineLayout(ctx->device, pl, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
    return 0;

splat_cleanup:
    vkDestroyBuffer(ctx->device, mask_buf, NULL); vkFreeMemory(ctx->device, mask_mem, NULL);
    vkDestroyBuffer(ctx->device, spl_buf, NULL); vkFreeMemory(ctx->device, spl_mem, NULL);
    vkDestroyBuffer(ctx->device, img_buf, NULL); vkFreeMemory(ctx->device, img_mem, NULL);
    vkDestroyPipeline(ctx->device, pipe, NULL);
    vkDestroyPipelineLayout(ctx->device, pl, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, dsl, NULL);
    return -1;
}

#else
/* Non-Android stubs */
#include <stddef.h>
vk_compute_ctx_t *vk_compute_init(void) { return NULL; }
void vk_compute_destroy(vk_compute_ctx_t *ctx) { (void)ctx; }
int vk_compute_decompress_iteration(vk_compute_ctx_t *ctx, const float *cur,
    const void *tf, float *next, uint32_t n, const vk_decompress_params_t *p) {
    (void)ctx; (void)cur; (void)tf; (void)next; (void)n; (void)p;
    return -1;
}
int vk_compute_rasterize(vk_compute_ctx_t *ctx, const vk_vertex_t *v, uint32_t nv,
    const vk_face_t *f, uint32_t nf, uint32_t w, uint32_t h, uint8_t *out, uint8_t *bm, const uint8_t *tm) {
    (void)ctx;(void)v;(void)nv;(void)f;(void)nf;(void)w;(void)h;(void)out;(void)bm;(void)tm;
    return -1;
}
int vk_compute_render_splats(vk_compute_ctx_t *ctx, const vk_splat_t *s, uint32_t ns,
    uint32_t w, uint32_t h, uint8_t *img, const uint8_t *fm) {
    (void)ctx;(void)s;(void)ns;(void)w;(void)h;(void)img;(void)fm;
    return -1;
}
#endif

