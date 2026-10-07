#pragma once
#include <cmath>
#include <initializer_list>

namespace yami {
enum class GraphicsBackend { OpenGL, Vulkan };
struct GraphicsSettings {
    int samples = 4;
    float anisotropy = 16;
    bool enhanced = true, fullscreen = false;
    GraphicsBackend backend = GraphicsBackend::OpenGL;
    bool ray_tracing = true, temporal_aa = true;
    float shadows = .75f, indirect_lighting = .35f, exposure = 1.f;
    float render_scale = 1.f, roughness = .85f;
    float ambient_occlusion = .65f, reflections = .22f, bloom = .12f, sharpen = .18f;
    bool operator==(const GraphicsSettings&) const = default;
};
inline bool valid_graphics(const GraphicsSettings& settings) noexcept {
    if (settings.samples < 0 || !std::isfinite(settings.anisotropy) || settings.anisotropy < 1) return false;
    if (settings.backend != GraphicsBackend::OpenGL && settings.backend != GraphicsBackend::Vulkan) return false;
    for (float value : {settings.ambient_occlusion, settings.reflections, settings.bloom, settings.sharpen,
                       settings.shadows, settings.indirect_lighting})
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return std::isfinite(settings.exposure) && settings.exposure >= .1f && settings.exposure <= 4 &&
           std::isfinite(settings.render_scale) && settings.render_scale >= .5f && settings.render_scale <= 1 &&
           std::isfinite(settings.roughness) && settings.roughness >= .05f && settings.roughness <= 1;
}
} // namespace yami
