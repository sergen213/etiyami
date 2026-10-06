#pragma once
#include "assets.hpp"
#include "graphics.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace yami {
struct DisplayOptions : GraphicsSettings {
    int width = 1600, height = 900;
    bool gles = false;
};
struct Viewport { int x, y, width, height; };
Viewport fit_original_interface(int width, int height);
Mat4 frustum_projection(float half_height, float aspect, float near_plane, float far_plane);
Mat4 interface_projection();
struct DrawState {
    Mat4 model = identity_matrix();
    bool lighting = false, fog = false;
    std::array<float, 4> color{{1, 1, 1, 1}};
    std::array<float, 4> diffuse_light{{1, 1, 1, 1}};
    std::array<float, 4> uv_transform{{1, 1, 0, 0}}; // scale.xy, offset.xy
};
// Original geometry/material semantics, with modern GL/GLES shaders and scalable
// viewports. All GPU resources belong to this context and are released before it.
class Renderer {
public:
    explicit Renderer(const DisplayOptions&);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    SDL_Window* window() const noexcept { return window_; }
    int pixel_width() const noexcept { return width_; }
    int pixel_height() const noexcept { return height_; }
    int samples() const noexcept { return samples_; }
    float anisotropy() const noexcept { return anisotropy_; }
    int max_samples() const noexcept { return max_samples_; }
    float max_anisotropy() const noexcept { return max_anisotropy_; }
    void set_graphics(const GraphicsSettings&);
    GraphicsSettings graphics_settings() const noexcept;
    void resize();
    void clear();
    void set_brightness(std::uint16_t ramp) noexcept { brightness_step_ = unsigned(ramp)+128; }
    void camera(const Mat4& view, const Mat4& projection, bool interface = false);
    void hud_camera();
    void finish_world();
    unsigned upload_mesh(std::span<const Vertex>, std::span<const std::uint16_t>);
    void update_mesh(unsigned mesh, std::span<const Vertex>);
    void release_mesh(unsigned mesh) noexcept;
    unsigned upload_texture(int width, int height, std::span<const std::uint8_t> rgba,
                            bool mipmaps = true, bool flip_vertical = true);
    void update_texture(unsigned texture, int width, int height,
                        std::span<const std::uint8_t> rgba, int x = 0, int y = 0);
    void draw(unsigned mesh, unsigned texture, const Material&, const MaterialPass&,
              const DrawState&);
    void present();
    std::vector<std::uint8_t> capture_rgba(); // Finished frame, bottom-left; call before present().
private:
    struct Geometry {
        unsigned vao, vertices, indices;
        std::size_t vertex_count, index_count;
        unsigned next_free = 0;
    };
    struct Texture { unsigned id; int width, height; bool mipmaps, flip_vertical; };
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    unsigned program_ = 0;
    unsigned post_program_ = 0, post_vao_ = 0, effect_program_ = 0;
    unsigned draw_framebuffer_ = 0, resolve_framebuffer_ = 0, resolve_texture_ = 0;
    unsigned resolve_depth_ = 0, effect_framebuffer_ = 0, effect_texture_ = 0;
    unsigned color_buffer_ = 0, depth_buffer_ = 0;
    unsigned brightness_step_ = 257;
    int brightness_uniform_ = -1, copy_uniform_ = -1;
    std::array<int, 3> effect_uniforms_{};
    std::array<float, 4> effects_{}, projection_info_{};
    std::array<float, 3> world_up_{{0, 1, 0}};
    bool enhanced_ = true, world_camera_ = false, world_finished_ = false;
    bool frame_finished_ = false;
    std::vector<Geometry> meshes_;
    unsigned free_mesh_ = 0;
    std::vector<Texture> textures_;
    Mat4 view_ = identity_matrix();
    Mat4 last_model_, last_model_view_;
    bool model_valid_ = false, normal_valid_ = false;
    std::array<float, 4> light_{{0.5f, 1, 0.3f, 0}};
    std::array<int, 15> uniforms_{};
    int width_ = 0, height_ = 0, samples_ = 0, max_samples_ = 0, max_texture_size_ = 0;
    int requested_samples_ = 0;
    float anisotropy_ = 1, max_anisotropy_ = 1;
    void cleanup() noexcept;
    void finish_frame();
    void allocate_targets(bool resizeTextures);
};
} // namespace yami
