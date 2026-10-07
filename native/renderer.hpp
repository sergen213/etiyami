#pragma once
#include "assets.hpp"
#include "graphics.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <memory>
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
class VulkanRenderer;
struct DrawState {
    Mat4 model = identity_matrix();
    bool lighting = false, fog = false;
    bool ray_geometry = true, ray_primary = true, temporal_static = false;
    std::array<float, 4> color{{1, 1, 1, 1}};
    std::array<float, 4> diffuse_light{{1, 1, 1, 1}};
    std::array<float, 4> uv_transform{{1, 1, 0, 0}}; // scale.xy, offset.xy
};
struct ShadowBounds {
    Vec3 min{}, max{};
    bool valid = false;
    void include(Vec3);
    void include(const ShadowBounds&);
    ShadowBounds transformed(const Mat4&) const;
};
// Backend selected at construction; original geometry/material semantics remain
// shared. All GPU resources belong to this renderer and are released before it.
class Renderer {
public:
    explicit Renderer(const DisplayOptions&);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    SDL_Window* window() const noexcept;
    int pixel_width() const noexcept;
    int pixel_height() const noexcept;
    int samples() const noexcept;
    float anisotropy() const noexcept;
    int max_samples() const noexcept;
    float max_anisotropy() const noexcept;
    const char* backend_name() const noexcept;
    bool ray_tracing_available() const noexcept;
    void reset_history();
    void set_graphics(const GraphicsSettings&);
    GraphicsSettings graphics_settings() const noexcept;
    void resize();
    void clear();
    void set_brightness(std::uint16_t ramp) noexcept;
    void camera(const Mat4& view, const Mat4& projection, bool interface = false);
    void hud_camera();
    void finish_world();
    bool shadow_enabled() const noexcept;
    bool shadow_caster_visible(const ShadowBounds&) const;
    bool begin_shadows(const ShadowBounds& casters, const ShadowBounds& receivers);
    void end_shadows();
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
#if YAMI_HAS_VULKAN
    std::unique_ptr<VulkanRenderer> vulkan_;
#endif
    GraphicsSettings desired_;
    bool gles_ = false;
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
    unsigned shadow_program_ = 0, shadow_framebuffer_ = 0, shadow_texture_ = 0;
    int shadow_size_ = 0;
    std::array<int, 7> shadow_uniforms_{};
    int shadow_model_uniform_ = -1, shadow_strength_uniform_ = -1, shadow_filter_uniform_ = -1;
    Mat4 shadow_matrix_ = identity_matrix(), projection_ = identity_matrix();
    float shadow_bias_ = 0;
    bool shadow_pass_ = false, shadow_valid_ = false, shadow_drawn_ = false;
    struct ShadowState {
        std::array<bool, 5> enabled{};
        std::array<int, 4> viewport{}, scissor{};
        std::array<unsigned char, 4> color_mask{};
        bool depth_mask = true;
        int read_framebuffer = 0, draw_framebuffer = 0, program = 0, vao = 0;
        int active_texture = 0, texture = 0, shadow_texture = 0;
        int depth_function = 0, cull_face = 0, front_face = 0, blend_source = 0, blend_destination = 0;
        int blend_source_alpha = 0, blend_destination_alpha = 0;
        float polygon_factor = 0, polygon_units = 0;
    } shadow_state_;
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
