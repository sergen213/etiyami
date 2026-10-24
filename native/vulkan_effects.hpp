#pragma once
#include "vulkan_api.hpp"
#include <memory>

namespace yami {
// All calls occur after the core's previous-frame fence. Geometry and textures
// remain core-owned; acceleration structures and temporal images live here.
class VulkanEffects {
public:
    explicit VulkanEffects(VulkanContext&);
    ~VulkanEffects();
    VulkanEffects(const VulkanEffects&) = delete;
    VulkanEffects& operator=(const VulkanEffects&) = delete;
    const VulkanImage& render(const VulkanEffectsFrame&);
    void reset_history() noexcept;
    std::uint64_t acceleration_builds() const noexcept;
    std::uint64_t ray_query_frames() const noexcept;
    // Frame generation (VulkanEffectsFrame::generation_format): true after a render() that upscaled with FSR and
    // owns the optical-flow/interpolation contexts; interpolate() is then valid once, in the same command buffer.
    bool frame_generation() const noexcept;
    // Usage the caller's interpolation source (HUD-less display image) and target images were created with.
    static constexpr VkImageUsageFlags generation_source_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    static constexpr VkImageUsageFlags generation_target_usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    // Writes the frame halfway between the previous and this source (display size, generation_format) into
    // target, leaving source shader-read-only and target GENERAL; FSR 3.1.4 keeps its own copy of source for
    // the next call. False: history reset (first/discontinuous frame), so target must not be shown.
    bool interpolate(VkCommandBuffer, VulkanImage& source, VulkanImage& target);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
