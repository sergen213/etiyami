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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
