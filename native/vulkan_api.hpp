#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "assets.hpp"
#include "graphics.hpp"
#include <array>
#include <span>

namespace yami {
#define YAMI_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define YAMI_VK_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define YAMI_VK_DEVICE(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit2) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) X(vkGetBufferDeviceAddress) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier2) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDrawIndirect) X(vkCmdUpdateBuffer)
#define YAMI_VK_RAY(X) X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureBuildSizesKHR) X(vkGetAccelerationStructureDeviceAddressKHR) X(vkCmdBuildAccelerationStructuresKHR)
#define YAMI_VK_QUERY(X) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2)
struct VulkanApi {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define YAMI_VK_FIELD(name) PFN_##name name = nullptr;
    YAMI_VK_GLOBAL(YAMI_VK_FIELD)
    YAMI_VK_INSTANCE(YAMI_VK_FIELD)
    YAMI_VK_DEVICE(YAMI_VK_FIELD)
    YAMI_VK_RAY(YAMI_VK_FIELD)
    YAMI_VK_QUERY(YAMI_VK_FIELD)
#undef YAMI_VK_FIELD
};
struct VulkanContext {
    VulkanApi& vk;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    bool ray_query = false;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t scratch_alignment = 256;
    VkInstance instance = VK_NULL_HANDLE;
    // Device enables FSR's storage-image/subgroup features; effects may clear it.
    bool fsr = false;
    // --gpu-timings: renderer-owned timestamp pool (null: off); per-frame acceleration builds the renderer resets.
    VkQueryPool timings = VK_NULL_HANDLE;
    std::uint32_t blas_builds = 0, blas_updates = 0, tlas_builds = 0, tlas_updates = 0;
};
// --gpu-timings stages: query 2*stage is written before the stage's commands, 2*stage+1 after them.
enum GpuStage : std::uint32_t { GpuWorld, GpuAcceleration, GpuClassify, GpuEffects, GpuSun, GpuTemporal, GpuFsrInputs, GpuFsr,
    GpuDisplay, GpuComposite, GpuGeneration, GpuUi, GpuFrame, GpuPresent, GpuPresentGenerated, GpuPresentReal, GpuQueryStages };
inline void gpu_timestamp(VulkanContext& c, VkCommandBuffer cmd, GpuStage stage, bool end) {
    if (c.timings) c.vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c.timings, 2*stage+end);
}
// Host-mapped memory: the first type with `preferred` flags, falling back to the first with `required` flags when
// none exists or that allocation fails (e.g. a full 256 MiB BAR heap without resizable BAR).
inline VkResult allocate_host_memory(const VulkanContext& c, VkMemoryAllocateInfo info, std::uint32_t bits,
                                     VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required, VkDeviceMemory& memory) {
    const auto find = [&](VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < c.memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (c.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return ~0u;
    };
    if (const auto i = find(preferred); i != ~0u) {
        info.memoryTypeIndex = i;
        if (c.vk.vkAllocateMemory(c.device, &info, nullptr, &memory) == VK_SUCCESS) return VK_SUCCESS;
    }
    info.memoryTypeIndex = find(required);
    return info.memoryTypeIndex == ~0u ? VK_ERROR_FEATURE_NOT_PRESENT : c.vk.vkAllocateMemory(c.device, &info, nullptr, &memory);
}
// CPU-written, GPU-read buffers prefer VRAM the CPU can map (resizable BAR); CPU-read buffers prefer cached system RAM.
constexpr VkMemoryPropertyFlags host_memory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags host_upload_memory = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | host_memory;
constexpr VkMemoryPropertyFlags host_readback_memory = VK_MEMORY_PROPERTY_HOST_CACHED_BIT | host_memory;
struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};
struct VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct VulkanBarrierMask { VkPipelineStageFlags2 stages; VkAccessFlags2 access; };
inline VulkanBarrierMask vulkan_barrier_mask(VkImageLayout layout) {
    constexpr auto shader=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    switch(layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,0};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return {shader,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return {VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_GENERAL:
        return {shader|VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT|
                VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return {VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,0};
    default: return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};
    }
}
struct VulkanRayMesh {
    VulkanBuffer vertices{}, indices{};
    std::uint32_t vertex_count = 0, index_count = 0;
    std::uint64_t revision = 0, key = 0;
};
struct VulkanRayInstance {
    VulkanRayMesh mesh{};
    Mat4 model{};
    // Last frame's pose of this draw (vertex buffer and model); the current ones when it has no previous draw.
    VulkanBuffer previous_vertices{};
    Mat4 previous_model{};
    std::uint32_t texture_index = 0;
    std::array<float,4> uv_transform{{1,1,0,0}}, color{{1,1,1,1}}, diffuse_light{{1,1,1,1}};
    std::uint32_t alpha_function = 0;
    float alpha_reference = 0;
    bool flip_vertical = false;
    bool alpha_test = false, cull = false;
    std::uint32_t cull_face = 0x405;
    bool lighting = false;
    float emission = 0;
    float window_emission = 0;
};
struct VulkanEffectsFrame {
    VkCommandBuffer command = VK_NULL_HANDLE;
    const VulkanImage *color = nullptr, *depth = nullptr, *normal = nullptr;
    VkExtent2D extent{}, output_extent{};
    Mat4 view{}, projection{}, previous_view_projection{};
    std::array<float,2> jitter{};
    std::span<const VulkanRayInstance> instances;
    std::span<const VkDescriptorImageInfo> textures;
    GraphicsSettings settings{};
    Environment environment{};
    bool history_valid = false;
    // Sample-zero integer prior UV/depth/token (clip-W for perspective,
    // 1+device depth for orthographic), and RGBA32_SINT prior authored oct/current
    // center depth/current physical oct/prior physical oct (invalid oct: 0x80008000).
    const VulkanImage *motion = nullptr, *previous_normal = nullptr;
    // Final retained MSAA ownership/authored normal/same packed facet metadata, absent at 1x.
    const VulkanImage *ms_motion = nullptr, *ms_normal = nullptr, *ms_previous_normal = nullptr;
    // Frame generation: format of the HUD-less display image to interpolate (FSR only); UNDEFINED disables.
    VkFormat generation_format = VK_FORMAT_UNDEFINED;
    // Every ray-geometry world draw carries the raster's distance fog (DrawState::fog).
    bool fog = false;
};
} // namespace yami
