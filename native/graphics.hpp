#pragma once
#include <cmath>
#include <initializer_list>

namespace yami {
struct GraphicsSettings {
    int samples = 4;
    float anisotropy = 16;
    bool enhanced = true, fullscreen = false;
    float ambient_occlusion = .65f, reflections = .22f, bloom = .12f, sharpen = .18f;
    bool operator==(const GraphicsSettings&) const = default;
};
inline bool valid_graphics(const GraphicsSettings& settings) noexcept {
    if (settings.samples < 0 || !std::isfinite(settings.anisotropy) || settings.anisotropy < 1) return false;
    for (float value : {settings.ambient_occlusion, settings.reflections, settings.bloom, settings.sharpen})
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return true;
}
} // namespace yami
