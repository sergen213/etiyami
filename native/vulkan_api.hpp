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
#define YAMI_VK_DEVICE(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit2) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) X(vkGetBufferDeviceAddress) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier2) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch)
#define YAMI_VK_RAY(X) X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureBuildSizesKHR) X(vkGetAccelerationStructureDeviceAddressKHR) X(vkCmdBuildAccelerationStructuresKHR)
struct VulkanApi {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define YAMI_VK_FIELD(name) PFN_##name name = nullptr;
    YAMI_VK_GLOBAL(YAMI_VK_FIELD)
    YAMI_VK_INSTANCE(YAMI_VK_FIELD)
    YAMI_VK_DEVICE(YAMI_VK_FIELD)
    YAMI_VK_RAY(YAMI_VK_FIELD)
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
};
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
struct VulkanRayMesh {
    VulkanBuffer vertices{}, indices{};
    std::uint32_t vertex_count = 0, index_count = 0;
    std::uint64_t revision = 0, key = 0;
};
struct VulkanRayInstance {
    VulkanRayMesh mesh{};
    Mat4 model{};
    std::uint32_t texture_index = 0;
    std::array<float,4> uv_transform{{1,1,0,0}}, color{{1,1,1,1}}, diffuse_light{{1,1,1,1}};
    std::uint32_t alpha_function = 0;
    float alpha_reference = 0;
    bool flip_vertical = false;
    bool alpha_test = false, cull = false;
    std::uint32_t cull_face = 0x405;
    bool lighting = false;
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
    bool history_valid = false;
};
} // namespace yami
