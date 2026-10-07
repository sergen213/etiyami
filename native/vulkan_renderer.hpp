#pragma once
#include "renderer.hpp"
#include <memory>

namespace yami {
class VulkanRenderer {
public:
    explicit VulkanRenderer(const DisplayOptions&);
    ~VulkanRenderer() noexcept;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    SDL_Window* window() const noexcept;
    int pixel_width() const noexcept;
    int pixel_height() const noexcept;
    int samples() const noexcept;
    float anisotropy() const noexcept;
    int max_samples() const noexcept;
    float max_anisotropy() const noexcept;
    void set_graphics(const GraphicsSettings&);
    GraphicsSettings graphics_settings() const noexcept;
    void resize();
    void clear();
    void set_brightness(std::uint16_t) noexcept;
    void camera(const Mat4&, const Mat4&, bool interface = false);
    void hud_camera();
    void finish_world();
    unsigned upload_mesh(std::span<const Vertex>, std::span<const std::uint16_t>);
    void update_mesh(unsigned, std::span<const Vertex>);
    void release_mesh(unsigned) noexcept;
    unsigned upload_texture(int, int, std::span<const std::uint8_t>, bool mipmaps = true, bool flip_vertical = true);
    void update_texture(unsigned, int, int, std::span<const std::uint8_t>, int x = 0, int y = 0);
    void draw(unsigned, unsigned, const Material&, const MaterialPass&, const DrawState&);
    void present();
    std::vector<std::uint8_t> capture_rgba();
    void reset_history();
    const char* backend_name() const noexcept;
    bool ray_tracing_available() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace yami
