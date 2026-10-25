#pragma once
#include <cmath>
#include <array>
#include <cstdint>
#include <initializer_list>

namespace yami {
enum class GraphicsBackend { OpenGL, Vulkan };
enum class TimeOfDay { Cycle, Day, Night };
struct Environment {
    float ambient = 1, direct = 1;
    std::array<float,3> sky{{1,1,1}}, fog{{.2f,.2f,.2f}};
    float emission = 0;
};
inline constexpr std::uint64_t environment_cycle_ms = 720000;
inline Environment environment_at(TimeOfDay mode, std::uint64_t elapsed) {
    const Environment day;
    const Environment night{.45f,0,{{.03f,.05f,.12f}},{{.01f,.02f,.06f}},1};
    if (mode==TimeOfDay::Day) return day;
    if (mode==TimeOfDay::Night) return night;
    const auto phase=elapsed%environment_cycle_ms;
    if (phase==0) return day;
    if (phase==environment_cycle_ms/2) return night;
    const float amount=phase<environment_cycle_ms/2 ?
        float(phase)/float(environment_cycle_ms/2) :
        float(environment_cycle_ms-phase)/float(environment_cycle_ms/2);
    const auto mix=[amount](float from,float to) { return from+(to-from)*amount; };
    Environment result;
    result.ambient=mix(day.ambient,night.ambient);
    result.direct=mix(day.direct,night.direct);
    result.emission=mix(day.emission,night.emission);
    for (int i=0;i<3;++i) {
        result.sky[i]=mix(day.sky[i],night.sky[i]);
        result.fog[i]=mix(day.fog[i],night.fog[i]);
    }
    return result;
}
struct GraphicsSettings {
    int samples = 4;
    float anisotropy = 16;
    bool enhanced = true, fullscreen = false;
    GraphicsBackend backend = GraphicsBackend::OpenGL;
    TimeOfDay time_of_day = TimeOfDay::Cycle;
    bool ray_tracing = true, temporal_aa = true, parallax = true;
    bool frame_generation = false; // Vulkan FSR 3.1.4 frame interpolation; needs temporal_aa (FSR)
    float shadows = .75f, indirect_lighting = .35f, exposure = 1.f;
    float render_scale = 1.f, roughness = .85f;
    float ambient_occlusion = .65f, reflections = .22f, bloom = .12f, sharpen = .18f;
    bool operator==(const GraphicsSettings&) const = default;
};
inline bool valid_graphics(const GraphicsSettings& settings) noexcept {
    if (settings.samples < 0 || !std::isfinite(settings.anisotropy) || settings.anisotropy < 1) return false;
    if (settings.backend != GraphicsBackend::OpenGL && settings.backend != GraphicsBackend::Vulkan) return false;
    if (settings.time_of_day != TimeOfDay::Cycle && settings.time_of_day != TimeOfDay::Day &&
        settings.time_of_day != TimeOfDay::Night) return false;
    for (float value : {settings.ambient_occlusion, settings.reflections, settings.bloom, settings.sharpen,
                       settings.shadows, settings.indirect_lighting})
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return std::isfinite(settings.exposure) && settings.exposure >= .1f && settings.exposure <= 4 &&
           std::isfinite(settings.render_scale) && settings.render_scale >= .5f && settings.render_scale <= 1 &&
           std::isfinite(settings.roughness) && settings.roughness >= .05f && settings.roughness <= 1;
}
} // namespace yami
