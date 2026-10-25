#include "renderer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace yami;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    Renderer renderer;
    unsigned floor, blocker, cutout, overlay;
    int width, height;
    float aspect;
    Material material;
    MaterialPass pass;
    std::array<Vertex, 4> blocker_vertices{{
        {{-75,100,-675},{0,0},{0,1,0}}, {{75,100,-675},{1,0},{0,1,0}},
        {{75,100,-525},{1,1},{0,1,0}}, {{-75,100,-525},{0,1},{0,1,0}}
    }};
    static DisplayOptions options() {
        DisplayOptions options;
        options.backend = GraphicsBackend::Vulkan;
        options.width = 384; options.height = 216; options.samples = 0;
        options.enhanced = true; options.temporal_aa = false;
        options.ambient_occlusion = options.reflections = options.bloom = options.sharpen = 0;
        options.indirect_lighting = 0; options.shadows = 0; options.exposure = 1;
        return options;
    }
    Fixture() : renderer(options()), width(renderer.pixel_width()), height(renderer.pixel_height()),
                aspect(float(width)/height) {
        material.cull = false;
        const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
        const std::array<Vertex, 4> floor_vertices{{
            {{-700,-80,-200},{0,0},{0,1,0}}, {{700,-80,-200},{1,0},{0,1,0}},
            {{700,-80,-1600},{1,1},{0,1,0}}, {{-700,-80,-1600},{0,1},{0,1,0}}
        }};
        floor = renderer.upload_mesh(floor_vertices, indices);
        blocker = renderer.upload_mesh(blocker_vertices, indices);
        const std::array<std::uint8_t, 4> transparent{{255,255,255,0}};
        cutout = renderer.upload_texture(1, 1, transparent, false);
        const std::array<Vertex, 4> overlay_vertices{{
            {{24,24,0},{0,0},{0,0,1}}, {{104,24,0},{0,0},{0,0,1}},
            {{104,104,0},{0,0},{0,0,1}}, {{24,104,0},{0,0},{0,0,1}}
        }};
        overlay = renderer.upload_mesh(overlay_vertices, indices);
    }
    ~Fixture() {
        renderer.release_mesh(overlay);
        renderer.release_mesh(blocker);
        renderer.release_mesh(floor);
    }
    enum class Mode { Absent, Opaque, Cutout, Excluded, Blended, TwoInstances };
    std::vector<std::uint8_t> frame(Mode mode, float translation = 0, float camera_x = 0,
                                    bool static_blocker = false) {
        renderer.clear();
        auto view = identity_matrix(); view.values[12] = camera_x;
        renderer.camera(view, frustum_projection(.75f, aspect, 1, 30000));
        DrawState state;
        state.lighting = true; state.temporal_static = true;
        state.color = {.5f,.5f,.5f,1}; state.diffuse_light = {.5f,.5f,.5f,1};
        renderer.draw(floor, 0, material, pass, state);
        if (mode != Mode::Absent) {
            auto blocker_pass = pass;
            state.temporal_static = static_blocker;
            state.model.values[12] = translation;
            state.ray_geometry = mode != Mode::Excluded;
            if (mode == Mode::Cutout) {
                blocker_pass.alpha_test = true;
                blocker_pass.alpha_function = 0x204; blocker_pass.alpha_reference = .5f;
            }
            if (mode == Mode::Blended) {
                blocker_pass.blend = true; blocker_pass.depth_write = false;
                state.color[3] = 0;
            }
            renderer.draw(blocker, mode == Mode::Cutout ? cutout : 0, material, blocker_pass, state);
            if (mode == Mode::TwoInstances) {
                state.model.values[12] = translation + 250;
                renderer.draw(blocker, 0, material, blocker_pass, state);
            }
        }
        renderer.finish_world();
        renderer.hud_camera();
        DrawState ui; ui.color = {.125f,.75f,.25f,1}; ui.ray_geometry = false;
        auto ui_pass = pass; ui_pass.depth_test = ui_pass.depth_write = false;
        renderer.draw(overlay, 0, material, ui_pass, ui);
        auto result = renderer.capture_rgba();
        require(renderer.capture_rgba() == result, "Capturing twice advanced the temporal frame");
        renderer.present();
        require(result.size() == std::size_t(width)*height*4, "Vulkan capture is not drawable-sized RGBA");
        const int hud_x = int(64.f*height/768), hud_y = int(64.f*height/768);
        const auto hud = (std::size_t(hud_y)*width+hud_x)*4;
        require(std::abs(int(result[hud])-32) <= 1 && std::abs(int(result[hud+1])-191) <= 1 &&
                std::abs(int(result[hud+2])-64) <= 1, "World treatment changed native-resolution HUD");
        return result;
    }
    int patch(const std::vector<std::uint8_t>& image, float x, float z, float camera_x = 0) const {
        const int px = int(width*(.5f+(x+camera_x)/(-z*.75f*aspect)*.5f));
        const int py = int(height*(.5f-80.f/(-z*.75f)*.5f));
        require(px > 3 && px < width-3 && py > 3 && py < height-3, "Fixture patch outside drawable");
        int sum = 0;
        for (int y = py-2; y <= py+2; ++y)
            for (int xx = px-2; xx <= px+2; ++xx) sum += image[(std::size_t(y)*width+xx)*4];
        return sum/25;
    }
};
void check_shadows(Fixture& fixture, bool force_off, bool require_rt) {
    const bool available = fixture.renderer.ray_tracing_available();
    require(!require_rt || available, "--require-rt requested but hardware ray queries are unavailable");
    auto graphics = fixture.renderer.graphics_settings();
    graphics.ray_tracing = !force_off;
    graphics.shadows = 0;
    fixture.renderer.set_graphics(graphics);
    const auto baseline = fixture.frame(Fixture::Mode::Opaque);
    graphics.shadows = 1;
    fixture.renderer.set_graphics(graphics);
    const auto shadow = fixture.frame(Fixture::Mode::Opaque);
    if (force_off || !available) {
        require(shadow == baseline, "RT-off/unsupported device still changed pixels with shadow strength");
        std::cout << "RT shadow checks SKIPPED: " << (force_off ? "explicitly disabled" : "device unsupported") << '\n';
        return;
    }
    const int lit = fixture.patch(baseline, -90, -654);
    require(lit-fixture.patch(shadow, -90, -654) >= 4, "Hardware shadow failed to darken its geometric footprint");
    require(std::abs(fixture.patch(shadow, 220, -654)-fixture.patch(baseline, 220, -654)) <= 2,
            "Shadow darkened an unoccluded receiver");
    for (const auto mode : {Fixture::Mode::Cutout, Fixture::Mode::Excluded, Fixture::Mode::Blended}) {
        const auto clear = fixture.frame(mode);
        require(std::abs(fixture.patch(clear, -90, -654)-lit) <= 2,
                "Transparent cutout/blended/excluded geometry occluded a ray");
    }
    const std::array<std::uint8_t, 4> opaque{{255,255,255,255}}, transparent{{255,255,255,0}};
    fixture.renderer.update_texture(fixture.cutout, 1, 1, opaque);
    require(lit-fixture.patch(fixture.frame(Fixture::Mode::Cutout), -90, -654) >= 4,
            "Updating cutout opacity did not update acceleration-scene alpha sampling");
    fixture.renderer.update_texture(fixture.cutout, 1, 1, transparent);
    require(std::abs(fixture.patch(fixture.frame(Fixture::Mode::Cutout), -90, -654)-lit) <= 2,
            "Old cutout opacity leaked after video-style texture update");
    const auto transformed = fixture.frame(Fixture::Mode::Opaque, 250);
    require(std::abs(fixture.patch(transformed, -90, -654)-lit) <= 2,
            "Moving an instance left a stale shadow at the old transform");
    const int translated_lit = fixture.patch(baseline, 160, -654);
    require(translated_lit-fixture.patch(transformed, 160, -654) >= 4,
            "Transformed instance shadow did not move with its mesh");
    const auto shared_instances = fixture.frame(Fixture::Mode::TwoInstances);
    require(lit-fixture.patch(shared_instances, -90, -654) >= 4 &&
            translated_lit-fixture.patch(shared_instances, 160, -654) >= 4,
            "Two instances sharing a mesh did not both cast shadows");
    auto moved = fixture.blocker_vertices;
    for (auto& vertex : moved) vertex.position.x += 250;
    fixture.renderer.update_mesh(fixture.blocker, moved);
    const auto dynamic = fixture.frame(Fixture::Mode::Opaque);
    require(std::abs(fixture.patch(dynamic, -90, -654)-lit) <= 2 &&
            translated_lit-fixture.patch(dynamic, 160, -654) >= 4,
            "Dynamic vertex upload retained old acceleration geometry");
    fixture.renderer.update_mesh(fixture.blocker, fixture.blocker_vertices);
    graphics.ray_tracing = false;
    fixture.renderer.set_graphics(graphics);
    require(fixture.frame(Fixture::Mode::Opaque) == baseline, "Disabling RT retained its shadow/history");
    std::cout << "hardware shadows/cutout opacity/exclusion/transforms/shared instances/dynamic vertices/RT-off PASS\n";
}
void check_texture_table_boundary(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "RT artwork-table boundary SKIPPED: RT off/unavailable\n";
        return;
    }
    auto graphics = fixture.renderer.graphics_settings();
    graphics.ray_tracing = true; graphics.temporal_aa = false; graphics.render_scale = 1;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = graphics.shadows = 0;
    fixture.renderer.set_graphics(graphics);
    std::array<unsigned, 300> textures{};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const std::array<std::uint8_t, 4> texel{{
            std::uint8_t(i%256), std::uint8_t(i/256), 255, 255
        }};
        textures[i] = fixture.renderer.upload_texture(1, 1, texel, false);
    }
    const std::array<std::uint8_t, 4> opaque{{255,255,255,255}}, transparent{{255,255,255,0}};
    fixture.renderer.update_texture(textures.back(), 1, 1, opaque);
    const auto draw = [&] {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), frustum_projection(.75f, fixture.aspect, 1, 30000));
        DrawState state;
        state.lighting = true; state.temporal_static = true;
        state.color = {.5f,.5f,.5f,1}; state.diffuse_light = {.5f,.5f,.5f,1};
        fixture.renderer.draw(fixture.floor, 0, fixture.material, fixture.pass, state);
        auto cutout_pass = fixture.pass;
        cutout_pass.alpha_test = true;
        cutout_pass.alpha_function = 0x204; cutout_pass.alpha_reference = .5f;
        // Real distinct texture submissions exceed the old 256-artwork table.
        // Only the last instance can intersect the local receiver's shadow ray.
        for (std::size_t i = 0; i < textures.size(); ++i) {
            state.model.values[12] = i+1 == textures.size() ? 0.f : 30000.f+200.f*float(i);
            fixture.renderer.draw(fixture.blocker, textures[i], fixture.material, cutout_pass, state);
        }
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba();
        fixture.renderer.present();
        return image;
    };
    const auto baseline = draw();
    const int lit = fixture.patch(baseline, -90, -654);
    graphics.shadows = 1;
    fixture.renderer.set_graphics(graphics);
    const auto shadow = draw();
    require(lit-fixture.patch(shadow, -90, -654) >= 4,
            "Late-index opaque artwork did not cast a shadow beyond 256 textures");
    fixture.renderer.update_texture(textures.front(), 1, 1, transparent);
    require(lit-fixture.patch(draw(), -90, -654) >= 4,
            "An earlier unrelated texture changed the late artwork shadow binding");
    fixture.renderer.update_texture(textures.back(), 1, 1, transparent);
    require(std::abs(fixture.patch(draw(), -90, -654)-lit) <= 2,
            "Late-index cutout opacity did not remove its shadow beyond 256 textures");
    fixture.renderer.update_texture(textures.back(), 1, 1, opaque);
    require(lit-fixture.patch(draw(), -90, -654) >= 4,
            "Late-index artwork opacity restore did not restore the geometric shadow");
    std::cout << "300 distinct artwork textures/late cutout shadow binding/update PASS\n";
}
void check_upscale_sky_boundary(Fixture& fixture) {
    auto graphics = fixture.renderer.graphics_settings();
    graphics.ray_tracing = false; graphics.temporal_aa = false;
    graphics.ambient_occlusion = graphics.reflections = graphics.shadows = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0;
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> sky_vertices{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{0,0},{0,0,1}},
        {{1,1,0},{0,0},{0,0,1}}, {{-1,1,0},{0,0},{0,0,1}}
    }};
    auto world_vertices = sky_vertices;
    world_vertices[1].position.x = world_vertices[2].position.x = 0;
    const auto sky = fixture.renderer.upload_mesh(sky_vertices, indices);
    const auto world = fixture.renderer.upload_mesh(world_vertices, indices);
    const auto draw = [&] {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        auto sky_pass = fixture.pass;
        sky_pass.depth_test = sky_pass.depth_write = false;
        DrawState state;
        state.color = {.125f,.25f,.5f,1}; state.ray_geometry = false;
        fixture.renderer.draw(sky, 0, fixture.material, sky_pass, state);
        state.color = {.6f,.3f,.15f,1}; state.lighting = true;
        state.diffuse_light = {.5f,.5f,.5f,1};
        state.ray_geometry = true; state.temporal_static = true;
        fixture.renderer.draw(world, 0, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba();
        fixture.renderer.present();
        return image;
    };
    const int boundary = fixture.width/2, y = fixture.height/2;
    const auto pixel = [&](const auto& image, int x, int channel) {
        return int(image[(std::size_t(y)*fixture.width+x)*4+channel]);
    };
    int world_low_exposure = 0;
    for (const float exposure : {.5f, 4.f}) {
        graphics.exposure = exposure; graphics.render_scale = 1;
        fixture.renderer.set_graphics(graphics);
        const auto native = draw();
        graphics.render_scale = .5f;
        fixture.renderer.set_graphics(graphics);
        const auto scaled = draw();
        // These adjacent pixels select different nearest source coverage.
        // Upscaling must not blend distinct surface coverage into a halo;
        // both world and sky remain in the same HDR encoding until tone mapping.
        for (int channel = 0; channel < 3; ++channel) {
            require(std::abs(pixel(scaled, boundary, channel)-pixel(native, boundary, channel)) <= 1,
                    "Half-resolution upscale bled world coverage into sky at its boundary");
            require(std::abs(pixel(scaled, boundary-1, channel)-pixel(native, boundary-1, channel)) <= 2,
                    "Half-resolution upscale bled sky coverage into the world edge");
        }
        if (exposure == .5f) world_low_exposure = pixel(native, boundary-8, 0);
        else require(pixel(native, boundary-8, 0)-world_low_exposure >= 16,
                     "Boundary fixture did not exercise actual world exposure");
    }
    fixture.renderer.release_mesh(world);
    fixture.renderer.release_mesh(sky);
    std::cout << "half-resolution world/sky boundary at low/high exposure PASS\n";
}
void check_traced_artwork(Fixture& fixture, bool force_off) {
    const bool hardware = !force_off && fixture.renderer.ray_tracing_available();
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.ray_tracing = false;
    graphics.temporal_aa = false; graphics.render_scale = 1; graphics.exposure = 1;
    graphics.roughness = .05f;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = graphics.shadows = 0;
    fixture.renderer.set_graphics(graphics);
    auto source = fixture.blocker_vertices;
    for (auto& vertex : source) vertex.position.y = -70;
    fixture.renderer.update_mesh(fixture.blocker, source);
    const std::array<std::uint8_t, 4> red{{255,0,0,255}}, green{{0,255,0,255}};
    std::vector<std::size_t> patch;
    // Floor y=-80, source y=-70: these primary rays cross y=-70 before
    // z=-525, so only the neutral floor is visible. Mirror rays hit the source
    // at z=1.125*floor_z (619..669), about 70 units away; GI reaches its
    // underside just 10 units up, within the 60-unit hemisphere ray limit.
    // Select actual pixel centers rather than a rounded patch crossing the edge.
    for (int y = 0; y < fixture.height/2; ++y) {
        const float distance = 80.f/(.75f*(1-2*(y+.5f)/fixture.height));
        if (distance < 550 || distance > 595) continue;
        for (int x = 0; x < fixture.width; ++x) {
            const float world_x = (2*(x+.5f)/fixture.width-1)*distance*.75f*fixture.aspect;
            if (std::abs(world_x) <= 25) patch.push_back((std::size_t(y)*fixture.width+x)*4);
        }
    }
    require(patch.size() >= 8, "Traced artwork fixture has no usable visible floor patch");
    const auto measure = [&](const auto& artwork, int frames) {
        fixture.renderer.update_texture(fixture.cutout, 1, 1, artwork);
        std::array<double, 3> sum{};
        for (int frame = 0; frame < frames; ++frame) {
            // Fresh history, but advancing frame noise: average real hits without
            // borrowing the preceding artwork's color or requiring exact RNG rays.
            fixture.renderer.reset_history();
            fixture.renderer.clear();
            fixture.renderer.camera(identity_matrix(), frustum_projection(.75f, fixture.aspect, 1, 30000));
            DrawState state;
            state.temporal_static = true; state.color = {.25f,.25f,.25f,1};
            fixture.renderer.draw(fixture.floor, 0, fixture.material, fixture.pass, state);
            state.color = {1,1,1,1};
            fixture.renderer.draw(fixture.blocker, fixture.cutout, fixture.material, fixture.pass, state);
            fixture.renderer.finish_world();
            const auto image = fixture.renderer.capture_rgba();
            require(image.size() == std::size_t(fixture.width)*fixture.height*4,
                    "Traced artwork capture is not drawable-sized RGBA");
            for (const auto at : patch)
                for (int channel = 0; channel < 3; ++channel) sum[channel] += image[at+channel];
            fixture.renderer.present();
        }
        for (auto& channel : sum) channel /= double(patch.size())*frames;
        return sum;
    };
    const auto baseline = measure(red, 1), green_baseline = measure(green, 1);
    for (int channel = 0; channel < 3; ++channel) {
        require(baseline[channel] > 16 && baseline[channel] < 128 &&
                std::abs(baseline[channel]-baseline[0]) <= 1 &&
                std::abs(green_baseline[channel]-baseline[channel]) <= 1,
                "Zero-strength artwork fixture is not an unchanged neutral floor");
    }
    for (const bool reflection : {true, false}) {
        graphics.reflections = reflection ? 1.f : 0.f;
        graphics.indirect_lighting = reflection ? 0.f : 1.f;
        graphics.ray_tracing = false;
        fixture.renderer.set_graphics(graphics);
        const auto off_red = measure(red, 1), off_green = measure(green, 1);
        for (int channel = 0; channel < 3; ++channel)
            require(std::abs(off_red[channel]-baseline[channel]) <= 1 &&
                    std::abs(off_green[channel]-baseline[channel]) <= 1,
                    reflection ? "RT-off reflection reached artwork beyond the screen-ray range" :
                                 "RT-off GI still transferred source artwork color");
        if (!hardware) continue;
        graphics.ray_tracing = true;
        fixture.renderer.set_graphics(graphics);
        const auto reflected_red = measure(red, 16), reflected_green = measure(green, 16);
        const double minimum = reflection ? 16 : 4;
        std::cout << (reflection ? "traced reflection" : "one-bounce GI") << " floor baseline=" << baseline[0]
                  << " red=" << reflected_red[0] << ',' << reflected_red[1] << ',' << reflected_red[2]
                  << " green=" << reflected_green[0] << ',' << reflected_green[1] << ',' << reflected_green[2] << '\n';
        require(reflected_red[0]-baseline[0] >= minimum &&
                reflected_green[1]-baseline[1] >= minimum &&
                reflected_red[0]-reflected_red[1] >= minimum &&
                reflected_green[1]-reflected_green[0] >= minimum &&
                reflected_red[0]-reflected_green[0] >= minimum &&
                reflected_green[1]-reflected_red[1] >= minimum,
                reflection ? "Traced reflection did not carry updated red/green source texture color" :
                             "One-bounce GI did not carry updated red/green source texture color");
        require(std::abs(reflected_red[1]-baseline[1]) <= 2 &&
                std::abs(reflected_green[0]-baseline[0]) <= 2 &&
                std::abs(reflected_red[2]-baseline[2]) <= 2 &&
                std::abs(reflected_green[2]-baseline[2]) <= 2,
                "Traced source color contaminated channels with no source radiance");
        graphics.reflections = graphics.indirect_lighting = 0;
        fixture.renderer.set_graphics(graphics);
        const auto zero_red = measure(red, 1), zero_green = measure(green, 1);
        for (int channel = 0; channel < 3; ++channel)
            require(std::abs(zero_red[channel]-baseline[channel]) <= 1 &&
                    std::abs(zero_green[channel]-baseline[channel]) <= 1,
                    "Zero strength retained traced source artwork color");
    }
    const std::array<std::uint8_t, 4> transparent{{255,255,255,0}};
    fixture.renderer.update_texture(fixture.cutout, 1, 1, transparent);
    fixture.renderer.update_mesh(fixture.blocker, fixture.blocker_vertices);
    fixture.renderer.set_graphics(saved);
    if (hardware) std::cout << "independent textured ray reflection/one-bounce GI/update/zero-strength/RT-off PASS\n";
    else std::cout << "RT reflection/GI artwork checks SKIPPED: "
                   << (force_off ? "explicitly disabled" : "device unsupported") << " (RT-off boundaries PASS)\n";
}
void check_authored_artwork(Fixture& fixture) {
    auto graphics = fixture.renderer.graphics_settings();
    graphics.enhanced = true; graphics.ray_tracing = false;
    graphics.temporal_aa = false; graphics.render_scale = 1; graphics.exposure = 1;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = graphics.shadows = 0;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const float light_length = std::sqrt(1.34f);
    const Vec3 normal{.5f/light_length, 1/light_length, .3f/light_length};
    const std::array<Vertex, 4> vertices{{
        {{-1,-1,0},{0,.5f},normal}, {{1,-1,0},{1,.5f},normal},
        {{1,1,0},{1,.5f},normal}, {{-1,1,0},{0,.5f},normal}
    }};
    const auto quad = fixture.renderer.upload_mesh(vertices, indices);
    std::array<std::uint8_t, 8> texels{{102,153,204,153, 204,102,153,153}};
    const auto artwork = fixture.renderer.upload_texture(2, 1, texels, false, false);
    const std::array<float, 4> tint{{.8f,.65f,.7f,.9f}};
    const auto draw = [&](float gain, unsigned texture, int fog_layers = 0,
                          float fog_alpha = .2f, bool fog_cutout = false) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        for (int panel = 0; panel < 3; ++panel) {
            DrawState state;
            state.model.values[0] = 1.f/3;
            state.model.values[12] = float(panel-1)*2.f/3;
            state.color = {tint[0]*gain,tint[1]*gain,tint[2]*gain,tint[3]};
            state.ray_geometry = panel != 1; state.temporal_static = panel != 1;
            auto pass = fixture.pass;
            if (panel == 2) {
                // Original GL clamps .456 + .8 * diffuse * aligned light to one.
                state.lighting = true; state.diffuse_light = {1,1,1,1};
                pass.alpha_test = true; pass.alpha_function = 0x204; pass.alpha_reference = .5f;
            }
            fixture.renderer.draw(quad, texture, fixture.material, pass, state);
        }
        // The recovered sky/cloud sheets are authored white alpha overlays, not
        // linear-light emitters. Cover world and excluded sky; leave the third
        // artwork/cutout panel untouched to check actual overlay coverage.
        auto fog_pass = fixture.pass;
        fog_pass.blend = true; fog_pass.depth_test = fog_pass.depth_write = false;
        fog_pass.alpha_test = fog_cutout; fog_pass.alpha_function = 0x204;
        fog_pass.alpha_reference = .5f;
        for (int layer = 0; layer < fog_layers; ++layer)
            for (int panel = 0; panel < 2; ++panel) {
                DrawState fog;
                fog.model.values[0] = 1.f/3;
                fog.model.values[12] = float(panel-1)*2.f/3;
                fog.color = {1,1,1,fog_alpha}; fog.ray_geometry = false;
                fixture.renderer.draw(quad, 0, fixture.material, fog_pass, fog);
            }
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba();
        fixture.renderer.present();
        return image;
    };
    const auto pixel = [&](const auto& image, int panel, int texel, int channel) {
        const int x = int(fixture.width*(float(panel)/3 + float(2*texel+1)/12));
        return int(image[(std::size_t(fixture.height/2)*fixture.width+x)*4+channel]);
    };
    const auto reference = draw(1, artwork);
    for (int panel = 0; panel < 3; ++panel)
        for (int texel = 0; texel < 2; ++texel)
            for (int channel = 0; channel < 3; ++channel) {
                const int expected = int(std::lround(texels[texel*4+channel]*
                                                     (panel == 2 ? 1.f : tint[channel])));
                require(std::abs(pixel(reference, panel, texel, channel)-expected) <= (panel == 2 ? 12 : 5),
                        "Enhanced world/sky artwork no longer matches authored tint and clamped legacy lighting");
            }
    const auto half_white = draw(0, 0, 1, .5f);
    const auto layered_fog = draw(1, artwork, 2);
    const auto rejected_fog = draw(1, artwork, 2, .2f, true);
    for (int panel = 0; panel < 3; ++panel)
        for (int texel = 0; texel < 2; ++texel)
            for (int channel = 0; channel < 3; ++channel) {
                if (panel < 2) {
                    require(std::abs(pixel(half_white, panel, texel, channel)-128) <= 2,
                            "Authored half-white alpha no longer composites to half brightness");
                    const float retained = .8f*.8f;
                    const float authored = texels[texel*4+channel]*tint[channel];
                    const int expected = int(std::lround(authored*retained+255*(1-retained)));
                    require(std::abs(pixel(layered_fog, panel, texel, channel)-expected) <= 5,
                            "Layered authored cloud alpha washed out underlying artwork");
                    const int contrast = pixel(layered_fog, panel, 1, channel)-
                                         pixel(layered_fog, panel, 0, channel);
                    const int original = pixel(reference, panel, 1, channel)-
                                         pixel(reference, panel, 0, channel);
                    require(std::abs(contrast-original*retained) <= 5,
                            "Layered authored cloud alpha failed to preserve source contrast");
                } else require(std::abs(pixel(layered_fog, panel, texel, channel)-
                                        pixel(reference, panel, texel, channel)) <= 1,
                               "Cloud sheets changed artwork outside their coverage");
                require(std::abs(pixel(rejected_fog, panel, texel, channel)-
                                 pixel(reference, panel, texel, channel)) <= 1,
                        "Cutout-rejected cloud sheets still changed underlying artwork");
            }
    // Alpha .6 passes GREATER .5; decoding alpha like RGB would incorrectly discard it.
    texels[3] = texels[7] = 102;
    fixture.renderer.update_texture(artwork, 2, 1, texels);
    const auto rejected = draw(1, artwork);
    for (int texel = 0; texel < 2; ++texel)
        for (int channel = 0; channel < 3; ++channel)
            require(pixel(rejected, 2, texel, channel) <= 1, "Texture alpha cutout ignored its authored threshold");
    texels[3] = texels[7] = 153;
    fixture.renderer.update_texture(artwork, 2, 1, texels);
    graphics.exposure = .5f; fixture.renderer.set_graphics(graphics);
    const auto dim = draw(1, artwork);
    graphics.exposure = 2; fixture.renderer.set_graphics(graphics);
    const auto bright = draw(1, artwork);
    for (int panel = 0; panel < 3; ++panel)
        for (int texel = 0; texel < 2; ++texel)
            for (int channel = 0; channel < 3; ++channel)
                require(pixel(bright, panel, texel, channel)-pixel(dim, panel, texel, channel) >= 24,
                        "World or excluded sky artwork bypassed enhanced exposure");
    // Both untextured gains exceed one in every RGB channel. Low exposure must
    // distinguish them, proving HDR survives the world target without fixing a curve.
    graphics.exposure = .1f; fixture.renderer.set_graphics(graphics);
    const auto hdr_low = draw(2, 0), hdr_high = draw(4, 0);
    for (int panel = 0; panel < 2; ++panel)
        for (int channel = 0; channel < 3; ++channel)
            require(pixel(hdr_high, panel, 0, channel)-pixel(hdr_low, panel, 0, channel) >= 16,
                    "World or excluded sky HDR was clamped before exposure");
    fixture.renderer.release_mesh(quad);
    std::cout << "authored textured tint/clamped lighting/artist alpha/layered cloud contrast/coverage/cutout/world and sky exposure/HDR headroom PASS\n";
}
void check_fine_artwork(Fixture& fixture, bool force_off) {
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.samples = 0; graphics.render_scale = 1;
    graphics.temporal_aa = false; graphics.ray_tracing = false; graphics.exposure = 1;
    graphics.ambient_occlusion = graphics.reflections = graphics.shadows = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> vertices{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{1,0},{0,0,1}},
        {{1,1,0},{1,1},{0,0,1}}, {{-1,1,0},{0,1},{0,0,1}}
    }};
    const auto quad = fixture.renderer.upload_mesh(vertices, indices);
    std::vector<std::uint8_t> texels(std::size_t(fixture.width)*4);
    for (int x = 0; x < fixture.width; ++x) {
        const auto at = std::size_t(x)*4;
        texels[at] = x%4<2 ? 48 : 176;
        texels[at+1] = x%4<2 ? 160 : 64;
        texels[at+2] = 96; texels[at+3] = 255;
    }
    const auto texture = fixture.renderer.upload_texture(fixture.width, 1, texels, false, false);
    const auto draw = [&](bool eligible, float camera_x = 0) {
        fixture.renderer.clear();
        auto view = identity_matrix(); view.values[12] = camera_x;
        fixture.renderer.camera(view, identity_matrix());
        DrawState state; state.ray_geometry = eligible; state.temporal_static = eligible;
        state.color = {.8f,.9f,.7f,1};
        fixture.renderer.draw(quad, texture, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto result = fixture.renderer.capture_rgba(); fixture.renderer.present();
        return result;
    };
    const auto compare = [&](const auto& image, const auto& reference, int tolerance, const char* message) {
        int maximum = 0;
        for (int x = 16; x < fixture.width-16; ++x)
            for (int c = 0; c < 3; ++c) {
                const auto at = (std::size_t(fixture.height/2)*fixture.width+x)*4+c;
                maximum = std::max(maximum, std::abs(int(image[at])-int(reference[at])));
            }
        std::cout << message << " max-channel-error=" << maximum << '\n';
        require(maximum <= tolerance, message);
    };
    // Same textured raster/geometry, neutral native display; only the local
    // Renderer sharpening control changes, never persisted preferences.
    const auto neutral = draw(true);
    int darkest = 255, brightest = 0;
    for (int x = 16; x < fixture.width-16; ++x) {
        const auto at = (std::size_t(fixture.height/2)*fixture.width+x)*4;
        darkest = std::min(darkest, int(neutral[at]));
        brightest = std::max(brightest, int(neutral[at]));
    }
    require(brightest-darkest > 32, "Neutral sharpening fixture lacks high-contrast artwork");
    graphics.sharpen = .18f;
    fixture.renderer.set_graphics(graphics);
    const auto sharpened = draw(true);
    require(sharpened.size() == neutral.size(), "Neutral sharpening changed capture dimensions");
    int neutral_error = 0;
    for (std::size_t at = 0; at < neutral.size(); at += 4) {
        for (int c = 0; c < 3; ++c)
            neutral_error = std::max(neutral_error, std::abs(int(sharpened[at+c])-int(neutral[at+c])));
        require(sharpened[at+3] == neutral[at+3], "Neutral sharpening changed raster alpha");
    }
    std::cout << "Neutral native TAA-off artwork sharpen=0/.18 max-channel-error="
              << neutral_error << " source-contrast=" << brightest-darkest << '\n';
    require(neutral_error <= 1, "Neutral sharpening invented deterministic artwork contrast");
    graphics.sharpen = 0;
    fixture.renderer.set_graphics(graphics);
    const auto reference = draw(false);
    compare(draw(true), reference, 2, "Native RT/TAA-off fine coplanar artwork was spatially blurred");
    if (!force_off && fixture.renderer.ray_tracing_available()) {
        graphics.ray_tracing = true; graphics.ambient_occlusion = 1;
        fixture.renderer.set_graphics(graphics);
        // The hemisphere leaves this plane: real ray queries are active, but
        // unobscured deterministic artwork must not become denoiser input.
        compare(draw(true), reference, 2, "RT-enabled fine coplanar artwork was spatially blurred");
    }
    fixture.renderer.release_mesh(quad);
    fixture.renderer.set_graphics(saved);
    std::cout << "fine coplanar artwork/native RT-off/RT-on PASS\n";
}
struct TemporalPixels {
    int width, height, frames = 0;
    std::vector<double> sum, square;
    TemporalPixels(int w, int h) : width(w), height(h), sum(std::size_t(w)*h*3),
                                  square(sum.size()) {}
    void add(const std::vector<std::uint8_t>& image, int drawable_width, int left, int bottom) {
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c) {
                    const auto at = (std::size_t(y)*width+x)*3+c;
                    const double value = image[(std::size_t(bottom+y)*drawable_width+left+x)*4+c];
                    sum[at] += value; square[at] += value*value;
                }
        ++frames;
    }
    double variance() const {
        double result = 0;
        for (std::size_t at = 0; at < sum.size(); ++at) {
            const double mean = sum[at]/frames;
            result += std::max(0., square[at]/frames-mean*mean);
        }
        return result/sum.size();
    }
    double error(const TemporalPixels& other) const {
        double result = 0;
        for (std::size_t at = 0; at < sum.size(); ++at)
            result += std::abs(sum[at]/frames-other.sum[at]/other.frames);
        return result/sum.size();
    }
    double stripe_contrast() const {
        double result = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width-2; ++x) {
                const auto at = (std::size_t(y)*width+x)*3;
                result += std::abs(sum[at]-sum[at+6])/frames;
            }
        return result/(height*(width-2));
    }
};
void check_noisy_illumination(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) return;
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.samples = 0; graphics.render_scale = 1; graphics.temporal_aa = false;
    graphics.ray_tracing = true; graphics.ambient_occlusion = 1;
    graphics.reflections = graphics.shadows = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> vertices{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{1,0},{0,0,1}},
        {{1,1,0},{1,1},{0,0,1}}, {{-1,1,0},{0,1},{0,0,1}}
    }};
    const auto quad = fixture.renderer.upload_mesh(vertices, indices);
    const auto draw = [&](bool stationary) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        DrawState receiver; receiver.temporal_static = stationary;
        receiver.color = {.5f,.5f,.5f,1};
        fixture.renderer.draw(quad, 0, fixture.material, fixture.pass, receiver);
        // A clipped square two units above the receiver subtends only part of
        // its hemisphere. Each genuine AO ray can hit or miss independently.
        DrawState blocker; blocker.model.values[14] = 2;
        blocker.model.values[0] = blocker.model.values[5] = .8f;
        fixture.renderer.draw(quad, 0, fixture.material, fixture.pass, blocker);
        fixture.renderer.finish_world();
        auto result = fixture.renderer.capture_rgba(); fixture.renderer.present(); return result;
    };
    const auto fluctuation = [&](bool stationary) {
        fixture.renderer.reset_history();
        auto previous = draw(stationary);
        for (int frame = 0; frame < 8; ++frame) previous = draw(stationary);
        double change = 0, brightness = 0;
        int count = 0;
        for (int frame = 0; frame < 12; ++frame) {
            const auto current = draw(stationary);
            for (int y = fixture.height/3; y < fixture.height*2/3; ++y)
                for (int x = fixture.width/3; x < fixture.width*2/3; ++x) {
                    const auto at = (std::size_t(y)*fixture.width+x)*4;
                    change += std::abs(int(current[at])-int(previous[at]));
                    brightness += current[at]; ++count;
                }
            previous = current;
        }
        require(brightness/count < 126, "Noisy AO fixture did not trace its actual occluder");
        return change/count;
    };
    const double spatial = fluctuation(false), temporal = fluctuation(true);
    std::cout << "AO frame fluctuation spatial=" << spatial << " temporal=" << temporal << '\n';
    // Unfiltered hit/miss AO on this receiver varies by tens of display levels.
    // Dynamic surfaces still receive a spatial filter; static ones additionally
    // benefit from short RT history without enabling camera jitter/TAA.
    require(spatial > .1 && spatial < 10, "Dynamic stochastic AO lost its spatial noise filter");
    require(temporal < spatial*.8, "Static stochastic AO lost temporal noise stabilization");
    fixture.renderer.release_mesh(quad);
    fixture.renderer.set_graphics(saved);
    std::cout << "actual stochastic AO/spatial noise bound/short-history stabilization PASS\n";
}
void check_smooth_normal_lighting_noise(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "ordinary-FOV smooth-normal AO noise SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.samples = 8; graphics.temporal_aa = false;
    graphics.ray_tracing = true; graphics.ambient_occlusion = 1; graphics.exposure = 1;
    graphics.reflections = graphics.indirect_lighting = graphics.shadows = 0;
    graphics.bloom = graphics.sharpen = 0;
    constexpr float half_height = .75f, distance = 2;
    const float half_width = distance*half_height*fixture.aspect;
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    std::array<Vertex, 4> vertices{{
        {{-half_width,-distance*half_height,-distance},{0,0},{0,0,1}},
        {{half_width,-distance*half_height,-distance},{1,0},{0,0,1}},
        {{half_width,distance*half_height,-distance},{1,1},{0,0,1}},
        {{-half_width,distance*half_height,-distance},{0,1},{0,0,1}}
    }};
    const auto receiver = fixture.renderer.upload_mesh(vertices, indices);
    // Behind the camera, hence absent from raster coverage, but reached within
    // AO's ten-unit falloff by genuine rays leaving the visible receiver.
    const std::array<Vertex, 4> blocker_vertices{{
        {{.5f,-2,.5f},{0,0},{0,0,-1}}, {{5.5f,-2,.5f},{1,0},{0,0,-1}},
        {{5.5f,2,.5f},{1,1},{0,0,-1}}, {{.5f,2,.5f},{0,1},{0,0,-1}}
    }};
    const auto blocker = fixture.renderer.upload_mesh(blocker_vertices, indices);
    std::vector<std::uint8_t> texels(std::size_t(fixture.width)*4);
    const int art_left = fixture.width*65/100-32, bottom = fixture.height/2-8;
    const int stroke = art_left+32;
    for (int x = 0; x < fixture.width; ++x) {
        const bool patterned = x >= fixture.width/2;
        texels[x*4] = patterned ? (x%4<2 ? 112 : 144) : 128;
        texels[x*4+1] = patterned ? (x%4<2 ? 152 : 128) : 128;
        texels[x*4+2] = patterned ? (x%4<2 ? 120 : 136) : 128;
        if (x >= stroke && x < stroke+3)
            texels[x*4] = texels[x*4+1] = texels[x*4+2] = 24;
        texels[x*4+3] = 255;
    }
    const auto texture = fixture.renderer.upload_texture(fixture.width, 1, texels, false, false);
    const auto draw = [&](bool occluder) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), frustum_projection(half_height, fixture.aspect, 1, 1000));
        DrawState state; state.temporal_static = false; state.temporal_id = 0x7A06;
        fixture.renderer.draw(receiver, texture, fixture.material, fixture.pass, state);
        if (occluder) {
            state = {}; state.temporal_static = true; state.temporal_id = 0x7A07;
            fixture.renderer.draw(blocker, 0, fixture.material, fixture.pass, state);
        }
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba(); fixture.renderer.present(); return image;
    };
    struct Samples {
        TemporalPixels smooth{32,16}, artwork{64,16};
    };
    const auto measure = [&](bool retain, bool occluder) {
        fixture.renderer.reset_history();
        if (retain) for (int frame = 0; frame < 64; ++frame) draw(occluder);
        Samples result;
        for (int frame = 0; frame < 64; ++frame) {
            if (!retain) fixture.renderer.reset_history();
            const auto image = draw(occluder);
            result.smooth.add(image, fixture.width, fixture.width/4-16, bottom);
            result.artwork.add(image, fixture.width, art_left, bottom);
        }
        return result;
    };
    const auto mean_rgb = [](const TemporalPixels& samples) {
        double total = 0;
        for (double value : samples.sum) total += value/samples.frames;
        return total/samples.sum.size();
    };
    for (bool smooth_normal : {false, true}) {
        const Vec3 normal = smooth_normal ? Vec3{.9f,0,std::sqrt(1-.9f*.9f)} : Vec3{0,0,1};
        for (auto& vertex : vertices) vertex.normal = normal;
        fixture.renderer.update_mesh(receiver, vertices);
        for (float scale : {1.f, .5f}) {
            graphics.render_scale = scale; graphics.ambient_occlusion = 0;
            fixture.renderer.set_graphics(graphics);
            const auto neutral = measure(false, false);
            graphics.ambient_occlusion = 1; fixture.renderer.set_graphics(graphics);
            const auto absent = measure(false, false);
            const auto fresh = measure(false, true), retained = measure(true, true);
            const double effect = fresh.smooth.error(absent.smooth);
            double mean = 0;
            for (double value : fresh.artwork.sum) mean += value/fresh.artwork.frames;
            mean /= fresh.artwork.sum.size();
            const double detail = fresh.artwork.stripe_contrast();
            const double contrast = retained.artwork.stripe_contrast()/detail;
            const float chord_ratio = normal.x*2*half_width/
                (fixture.width*scale*distance*.001f);
            std::cout << "ordinary-FOV " << (smooth_normal ? "authored-smooth" : "geometric")
                      << " normal AO scale=" << scale << " samples=" << fixture.renderer.samples()
                      << " source-chord/tolerance=" << chord_ratio << " actual-caster-effect=" << effect
                      << " meanRGB neutral/absent/present=" << mean_rgb(neutral.smooth)
                      << '/' << mean_rgb(absent.smooth) << '/' << mean_rgb(fresh.smooth)
                      << " blocker-absent-AO=" << absent.smooth.error(neutral.smooth)
                      << " smooth-variance fresh/retained=" << fresh.smooth.variance()
                      << '/' << retained.smooth.variance()
                      << " physical-mean-error=" << retained.artwork.error(fresh.artwork)
                      << " painted-detail-retained=" << contrast << std::endl;
            require(effect >= 1, "Ordinary-FOV AO fixture did not respond to its actual ray blocker");
            require(absent.smooth.error(neutral.smooth) <= 1,
                    "Authored smooth normal sent AO rays into its own physical receiver facet");
            require(fresh.smooth.variance() > .1,
                    "Ordinary-FOV smooth receiver lacks genuine consumer-visible AO noise");
            require(detail > 2 && mean > 16, "Ordinary-FOV AO fixture lacks visible painted detail");
            require(retained.artwork.error(fresh.artwork) <= mean*.05 &&
                    contrast >= .95 && contrast <= 1.05,
                    "Ordinary-FOV AO history changed physical painted mean/detail by over five percent");
            require(retained.smooth.variance() < fresh.smooth.variance()*.7,
                    "Same physical ordinary-FOV smooth surface lost temporal AO noise convergence");
        }
    }
    fixture.renderer.release_mesh(receiver); fixture.renderer.release_mesh(blocker);
    fixture.renderer.set_graphics(saved);
    std::cout << "ordinary-FOV smooth-normal/full-strength AO/MSAA8/native and half/painted detail PASS\n";
}
void check_ray_origin_translation(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "physical RT ray-origin translation SKIPPED: "
                  << (force_off ? "explicitly disabled" : "device unsupported") << '\n';
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.temporal_aa = false; graphics.ray_tracing = true;
    graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1; graphics.roughness = .85f;
    constexpr float half_height = .05f, left_w = 700, right_w = 1000, coverage = 1.3f;
    const Vec3 far_origin{-4432.91f,-13.8573f,2805.53f};
    const float left_x = -left_w*half_height*fixture.aspect*coverage;
    const float right_x = right_w*half_height*fixture.aspect*coverage;
    const float length = std::hypot(right_w-left_w, right_x-left_x);
    const Vec3 first_normal{(right_w-left_w)/length,0,(right_x-left_x)/length};
    // This is the true outward normal of both triangles, not an authored smooth
    // normal. Sun, cosine-hemisphere and reflected rays all leave this convex
    // plane: there is no other geometry and therefore no legitimate ray hit.
    require(first_normal.x*.5f+first_normal.z*.3f > 0 && first_normal.z > 0,
            "Ray-origin MISS fixture does not face the sun and camera");
    std::array<Vertex, 4> vertices{{
        {{left_x,-left_w*half_height*coverage,-left_w},{0,0},first_normal},
        {{right_x,-right_w*half_height*coverage,-right_w},{1,0},first_normal},
        {{right_x,right_w*half_height*coverage,-right_w},{1,1},first_normal},
        {{left_x,left_w*half_height*coverage,-left_w},{0,1},first_normal}
    }};
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const auto plane = fixture.renderer.upload_mesh(vertices, indices);
    struct Restore {
        Renderer& renderer;
        GraphicsSettings graphics;
        unsigned plane;
        ~Restore() { renderer.release_mesh(plane); renderer.set_graphics(graphics); }
    } restore{fixture.renderer, saved, plane};
    const std::array<std::uint8_t, 4> grey{{128,128,128,255}};
    const auto texture = fixture.renderer.upload_texture(1, 1, grey, false, false);
    const auto draw = [&](Vec3 origin, bool full_rt) {
        graphics.ambient_occlusion = graphics.shadows = graphics.reflections =
            graphics.indirect_lighting = full_rt ? 1.f : 0.f;
        fixture.renderer.set_graphics(graphics);
        // Every image is fresh source lighting: no TAA/history, fixed zero jitter.
        // RNG phases may differ, but every physical ray must miss at every phase.
        fixture.renderer.reset_history();
        fixture.renderer.clear();
        auto view = identity_matrix();
        view.values[12] = -origin.x; view.values[13] = -origin.y; view.values[14] = -origin.z;
        fixture.renderer.camera(view, frustum_projection(half_height, fixture.aspect, 1, 12000));
        DrawState state; state.lighting = true;
        state.model.values[12] = origin.x; state.model.values[13] = origin.y;
        state.model.values[14] = origin.z;
        fixture.renderer.draw(plane, texture, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba();
        fixture.renderer.present();
        require(image.size() == std::size_t(fixture.width)*fixture.height*4,
                "Ray-origin output is not native drawable-sized RGBA");
        return image;
    };
    const int roi_width = fixture.width/2, roi_height = fixture.height/2;
    const int left = fixture.width/4, bottom = fixture.height/4;
    bool contrast_ok = true, source_ok = true, miss_ok = true;
    for (int orientation = 0; orientation < 2; ++orientation) {
        Vec3 normal = first_normal;
        if (orientation == 1) {
            const float normal_length = std::sqrt(.72f*.72f+.4f*.4f+.56f*.56f);
            normal = {-.72f/normal_length,.4f/normal_length,.56f/normal_length};
            // Intersect four overscan corner rays with n.P = -plane_constant.
            // Set overscan analytically so the extrema remain W700 and W1000,
            // despite the true plane having slope in BOTH X and Y.
            const float projected_slope = half_height*(std::abs(normal.x)*fixture.aspect+std::abs(normal.y));
            const float plane_coverage = normal.z*(right_w-left_w)/((right_w+left_w)*projected_slope);
            const float plane_constant = 2*left_w*right_w*normal.z/(left_w+right_w);
            require(plane_coverage > 1.05f, "Second ray-origin plane does not cover the native drawable");
            for (auto& vertex : vertices) {
                const float sx = (vertex.uv.x*2-1)*half_height*fixture.aspect*plane_coverage;
                const float sy = (vertex.uv.y*2-1)*half_height*plane_coverage;
                const float w = plane_constant/(normal.z-normal.x*sx-normal.y*sy);
                vertex.position.x = sx*w; vertex.position.y = sy*w;
                vertex.position.z = (-plane_constant-normal.x*vertex.position.x-normal.y*vertex.position.y)/normal.z;
                vertex.normal = normal;
            }
            fixture.renderer.update_mesh(plane, vertices);
        }
        const float dot_sun = (normal.x*.5f+normal.y+normal.z*.3f)/std::sqrt(.5f*.5f+1+.3f*.3f);
        const float front_distance = -normal.x*vertices[0].position.x-
            normal.y*vertices[0].position.y-normal.z*vertices[0].position.z;
        const float dot_camera_offset = normal.x*far_origin.x+normal.y*far_origin.y+normal.z*far_origin.z;
        require(dot_sun > 0 && normal.z > 0 && front_distance > 0,
                "Ray-origin orientation does not have outward sun/reflected/hemisphere MISS rays");
    for (float scale : {1.f, .5f})
        for (int requested_samples : {4, 8}) {
            graphics.render_scale = scale; graphics.samples = requested_samples;
            TemporalPixels near_pixels(roi_width, roi_height), far_pixels(roi_width, roi_height);
            int translation_max = 0, neutral_translation_max = 0, miss_max = 0, roi_miss_max = 0;
            int neutral_min = 255, neutral_max = 0, far_min = 255, far_max = 0;
            int darkened_pixels = 0, neighbor_step = 0, worst_frame = 0;
            std::size_t worst_at = 0;
            std::array<int, 3> worst_full{}, worst_neutral{};
            for (int frame = 0; frame < 16; ++frame) {
                const auto near_neutral = draw({}, false), near_full = draw({}, true);
                const auto far_neutral = draw(far_origin, false), far_full = draw(far_origin, true);
                near_pixels.add(near_full, fixture.width, left, bottom);
                far_pixels.add(far_full, fixture.width, left, bottom);
                for (int y = 0; y < fixture.height; ++y)
                    for (int x = 0; x < fixture.width; ++x) {
                        const auto at = (std::size_t(y)*fixture.width+x)*4;
                        bool darkened = false;
                        for (int c = 0; c < 3; ++c) {
                            neutral_min = std::min({neutral_min, int(near_neutral[at+c]), int(far_neutral[at+c])});
                            neutral_max = std::max({neutral_max, int(near_neutral[at+c]), int(far_neutral[at+c])});
                            far_min = std::min(far_min, int(far_full[at+c]));
                            far_max = std::max(far_max, int(far_full[at+c]));
                            translation_max = std::max(translation_max,
                                std::abs(int(near_full[at+c])-int(far_full[at+c])));
                            neutral_translation_max = std::max(neutral_translation_max,
                                std::abs(int(near_neutral[at+c])-int(far_neutral[at+c])));
                            const int error = std::max(
                                std::abs(int(near_full[at+c])-int(near_neutral[at+c])),
                                std::abs(int(far_full[at+c])-int(far_neutral[at+c])));
                            if (error > miss_max || (frame == 0 && at == 0 && c == 0)) {
                                miss_max = error; worst_at = at; worst_frame = frame;
                                const bool far = std::abs(int(far_full[at+c])-int(far_neutral[at+c])) >=
                                                 std::abs(int(near_full[at+c])-int(near_neutral[at+c]));
                                for (int channel = 0; channel < 3; ++channel) {
                                    worst_full[channel] = (far ? far_full : near_full)[at+channel];
                                    worst_neutral[channel] = (far ? far_neutral : near_neutral)[at+channel];
                                }
                            }
                            if (x >= left && x < left+roi_width && y >= bottom && y < bottom+roi_height)
                                roi_miss_max = std::max(roi_miss_max, error);
                            darkened |= int(far_neutral[at+c])-int(far_full[at+c]) > 2;
                            if (x+1 < fixture.width)
                                neighbor_step = std::max(neighbor_step, std::abs(int(far_full[at+c])-int(far_full[at+4+c])));
                            if (y+1 < fixture.height)
                                neighbor_step = std::max(neighbor_step,
                                    std::abs(int(far_full[at+c])-int(far_full[at+fixture.width*4+c])));
                        }
                        darkened_pixels += darkened;
                    }
            }
            std::cout << "physical convex-plane MISS orientation=" << orientation
                      << " normal=" << normal.x << ',' << normal.y << ',' << normal.z
                      << " dotSun=" << dot_sun << " dotCameraOffset=" << dot_camera_offset
                      << " camera-front-distance=" << front_distance << " origin0/far scale=" << scale
                      << " requested-samples=" << requested_samples << " samples=" << fixture.renderer.samples()
                      << " fresh-frames=16 native=" << fixture.width << 'x' << fixture.height
                      << " translation-maxRGB=" << translation_max
                      << " neutral-translation-maxRGB=" << neutral_translation_max
                      << " fullRT/neutral-maxRGB=" << miss_max << " interior-maxRGB=" << roi_miss_max
                      << " neutral-range=" << neutral_min << ':' << neutral_max
                      << " translated-range=" << far_min << ':' << far_max
                      << " darkened-pixel-frames=" << darkened_pixels << " neighbor-step=" << neighbor_step
                      << " interior-temporal-variance0/far=" << near_pixels.variance() << '/' << far_pixels.variance()
                      << " worst-frame/pixel=" << worst_frame << '/' << (worst_at/4)%fixture.width
                      << ',' << (worst_at/4)/fixture.width << " full/neutral-RGB=";
            for (int c = 0; c < 3; ++c) std::cout << (c ? "," : "") << worst_full[c] << '/' << worst_neutral[c];
            std::cout << '\n';
            contrast_ok &= neutral_min >= 64 && neutral_max <= 192 && neutral_max-neutral_min <= 2;
            source_ok &= neutral_translation_max <= 2;
            miss_ok &= translation_max <= 2 && miss_max <= 2 && roi_miss_max <= 2;
        }
    }
    // Measure every orientation/configuration before failing: the first plane's
    // strict boundary failure must not hide the second orientation's evidence.
    require(contrast_ok, "Ray-origin MISS fixture lacks uniform bright midtone shadow contrast");
    require(source_ok, "Camera/mesh translation changed the neutral authored source");
    require(miss_ok, "Full-strength RT self-occluded a convex plane or changed under physical translation");
    std::cout << "physical RT ray-origin translation/two true-plane orientations/native MISS output/MSAA4 and 8/scales1 and .5 PASS\n";
}
void check_untracked_overlay_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "untracked fog physical lighting controls SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.temporal_aa = false; graphics.ray_tracing = true;
    graphics.samples = 0; graphics.render_scale = 1; graphics.ambient_occlusion = 1;
    graphics.reflections = graphics.indirect_lighting = 0;
    graphics.shadows = graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> vertices{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{1,0},{0,0,1}},
        {{1,1,0},{1,1},{0,0,1}}, {{-1,1,0},{0,1},{0,0,1}}
    }};
    const auto quad = fixture.renderer.upload_mesh(vertices, indices);
    auto fog_pass = fixture.pass;
    fog_pass.blend = true; fog_pass.blend_source = 0x302; fog_pass.blend_destination = 0x303;
    fog_pass.depth_test = fog_pass.depth_write = false;
    const std::array<float, 4> white{{1,1,1,.4f}};
    const auto draw = [&](const std::array<float, 4>& fog, bool occluder = false,
                          bool excluded = false) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        DrawState receiver; receiver.temporal_static = true; receiver.temporal_id = 0x7A01;
        receiver.ray_geometry = !excluded; receiver.color = {.22f,.36f,.48f,1};
        fixture.renderer.draw(quad, 0, fixture.material, fixture.pass, receiver);
        if (occluder) {
            // Reuse the genuine hit/miss AO fixture: clipped from raster, not rays.
            DrawState blocker; blocker.model.values[14] = 2;
            blocker.model.values[0] = blocker.model.values[5] = .8f;
            fixture.renderer.draw(quad, 0, fixture.material, fixture.pass, blocker);
        }
        if (fog[3] > 0) {
            DrawState overlay; overlay.ray_geometry = false; overlay.color = fog;
            fixture.renderer.draw(quad, 0, fixture.material, fog_pass, overlay);
        }
        fixture.renderer.finish_world();
        auto result = fixture.renderer.capture_rgba(); fixture.renderer.present(); return result;
    };
    // Fully covered, uniform interior.
    const int left = fixture.width/3, bottom = fixture.height/3;
    const int width = fixture.width/3, height = fixture.height/3;
    const auto maximum_error = [&](const std::vector<std::uint8_t>& a,
                                    const std::vector<std::uint8_t>& b) {
        int maximum = 0;
        for (int y = bottom; y < bottom+height; ++y)
            for (int x = left; x < left+width; ++x)
                for (int c = 0; c < 3; ++c) {
                    const auto at = (std::size_t(y)*fixture.width+x)*4+c;
                    maximum = std::max(maximum, std::abs(int(a[at])-int(b[at])));
                }
        return maximum;
    };
    const auto measure = [&](bool keep_history) {
        fixture.renderer.reset_history();
        if (keep_history)
            for (int frame = 0; frame < 32; ++frame) draw(white, true);
        TemporalPixels result(width, height);
        for (int frame = 0; frame < 64; ++frame) {
            if (!keep_history) fixture.renderer.reset_history();
            result.add(draw(white, true), fixture.width, left, bottom);
        }
        return result;
    };
    const auto fresh = measure(false), retained = measure(true);
    graphics.ambient_occlusion = 0;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    const auto neutral_image = draw(white, true);
    TemporalPixels neutral(width, height); neutral.add(neutral_image, fixture.width, left, bottom);
    const double ao_strength = fresh.error(neutral);
    std::cout << "constant untracked fog actual-AO meanRGB=" << ao_strength
              << " variance fresh=" << fresh.variance() << " retained=" << retained.variance()
              << " physical-mean-error=" << retained.error(fresh) << '\n';
    require(ao_strength >= 1, "Fog-covered AO control did not trace its actual occluder");
    require(fresh.variance() > .1, "Fog-covered AO control did not produce genuine stochastic noise");
    require(retained.variance() < fresh.variance()*.75,
            "Constant untracked fog lost the opaque receiver's physical lighting history");
    require(retained.error(neutral) >= ao_strength*.75 && retained.error(fresh) <= 12,
            "Fog-covered temporal filtering suppressed the physical AO strength or changed its mean");

    // The same fog must not turn excluded (-1) geometry into a light receiver.
    fixture.renderer.reset_history();
    const auto excluded_neutral = draw(white, true, true);
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 1;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    for (int frame = 0; frame < 32; ++frame) draw(white, true, true);
    const auto excluded_lit = draw(white, true, true);
    fixture.renderer.reset_history();
    const auto excluded_fresh = draw(white, true, true);
    const int excluded_error = std::max(maximum_error(excluded_lit, excluded_neutral),
                                        maximum_error(excluded_fresh, excluded_neutral));
    std::cout << "excluded fog-covered receiver full-strength AO/reflection/GI maxRGB="
              << excluded_error << '\n';
    require(excluded_error <= 2, "Untracked fog promoted excluded geometry into traced lighting");
    fixture.renderer.release_mesh(quad);
    fixture.renderer.set_graphics(saved);
    std::cout << "untracked fog physical lighting ownership PASS\n";
}

void check_curved_actor_illumination(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "curved dynamic actor RT illumination SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.samples = 0; graphics.render_scale = 1;
    graphics.temporal_aa = false; graphics.ray_tracing = true; graphics.exposure = 1;
    graphics.roughness = .8f; graphics.shadows = graphics.bloom = graphics.sharpen = 0;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 1;
    fixture.renderer.set_graphics(graphics);
    constexpr int segments = 24;
    std::vector<Vertex> vertices((segments+1)*2);
    std::vector<std::uint16_t> indices;
    indices.reserve(segments*6);
    for (int segment = 0; segment < segments; ++segment) {
        const auto a = std::uint16_t(segment*2);
        indices.insert(indices.end(), {a, std::uint16_t(a+2), std::uint16_t(a+3),
                                       a, std::uint16_t(a+3), std::uint16_t(a+1)});
    }
    const auto pose_vertices = [&](int pose) {
        const float bulge = .18f+.002f*std::sin(float(pose)*.3f);
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = float(segment)/segments, theta = (u-.5f)*2.1f;
            const float nx = bulge*std::sin(theta)/.65f, nz = std::cos(theta);
            const float length = std::sqrt(nx*nx+nz*nz);
            for (int side = 0; side < 2; ++side) {
                auto& vertex = vertices[segment*2+side];
                vertex.position = {.65f*std::sin(theta)+float(pose)/fixture.width,
                                   side ? .65f : -.65f, -.25f+bulge*std::cos(theta)};
                vertex.uv = {u, float(side)}; vertex.normal = {nx/length, 0, nz/length};
            }
        }
    };
    pose_vertices(0);
    const auto actor = fixture.renderer.upload_mesh(vertices, indices);
    const std::array<std::uint16_t, 6> plane_indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> plane{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{1,0},{0,0,1}},
        {{1,1,0},{1,1},{0,0,1}}, {{-1,1,0},{0,1},{0,0,1}}
    }};
    const auto source = fixture.renderer.upload_mesh(plane, plane_indices);
    std::array<std::uint8_t, 64*4> texels{};
    for (int x = 0; x < 64; ++x) {
        texels[x*4] = x%8<4 ? 100 : 180;
        texels[x*4+1] = x%8<4 ? 170 : 110;
        texels[x*4+2] = 130; texels[x*4+3] = 255;
    }
    const auto texture = fixture.renderer.upload_texture(64, 1, texels, false, false);
    const std::array<std::uint8_t, 4> emitter_texel{{255,150,70,255}};
    const auto emitter = fixture.renderer.upload_texture(1, 1, emitter_texel, false, false);
    const auto draw = [&](int pose, bool visible = true) {
        fixture.renderer.clear();
        pose_vertices(pose);
        fixture.renderer.update_mesh(actor, vertices);
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        DrawState background;
        background.model.values[14] = .5f; background.ray_geometry = false;
        background.color = {.07f,.13f,.29f,1};
        fixture.renderer.draw(source, 0, fixture.material, fixture.pass, background);
        if (visible) {
            DrawState state; state.temporal_static = false; state.temporal_id = 0x7102;
            // Half a pixel of model motion plus half a pixel of uploaded vertex
            // motion per pose. The tracked patch follows a full physical pixel.
            state.model.values[12] = float(pose)/fixture.width;
            state.color = {.65f,.65f,.65f,1};
            fixture.renderer.draw(actor, texture, fixture.material, fixture.pass, state);
        }
        // Outside clip depth, but genuinely reached by AO/GI/reflection rays.
        // Its bounded footprint produces real hit/miss noise, not a fake RNG.
        DrawState emitter_state;
        emitter_state.model.values[0] = emitter_state.model.values[5] = 1.2f;
        emitter_state.model.values[14] = 2;
        emitter_state.color = {1.8f,.8f,.4f,1};
        fixture.renderer.draw(source, emitter, fixture.material, fixture.pass, emitter_state);
        fixture.renderer.finish_world();
        auto result = fixture.renderer.capture_rgba(); fixture.renderer.present(); return result;
    };
    const int left = fixture.width/2-24, bottom = fixture.height/2-12;
    const auto measure = [&](bool keep_history, bool moving) {
        fixture.renderer.reset_history();
        if (keep_history)
            for (int frame = 0; frame < 32; ++frame) draw(moving ? frame : 0);
        TemporalPixels result(48, 24);
        for (int frame = 0; frame < 16; ++frame) {
            if (!keep_history) fixture.renderer.reset_history();
            const int pose = moving ? frame+32 : 0;
            result.add(draw(pose), fixture.width, left+pose, bottom);
        }
        return result;
    };
    // Independently prove that all three full-strength effects actually alter
    // the textured curved receiver; variance reduction cannot pass by disabling
    // AO, GI or reflections, or by flattening the actor to its raster base.
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    fixture.renderer.set_graphics(graphics);
    const auto base = measure(false, false);
    for (int effect = 0; effect < 3; ++effect) {
        graphics.ambient_occlusion = effect == 0 ? 1.f : 0.f;
        graphics.indirect_lighting = effect == 1 ? 1.f : 0.f;
        graphics.reflections = effect == 2 ? 1.f : 0.f;
        fixture.renderer.set_graphics(graphics);
        const auto lit = measure(false, false);
        std::cout << "curved actor " << (effect == 0 ? "AO" : effect == 1 ? "GI" : "reflection")
                  << " actual framebuffer effect=" << lit.error(base) << '\n';
        require(lit.error(base) >= 1,
                "Curved actor fixture did not exercise a full-strength traced illumination effect");
    }
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 1;
    for (int requested_samples : {0, 8}) {
        graphics.samples = requested_samples;
        fixture.renderer.set_graphics(graphics);
        // Unsupported MSAA8 may downgrade, but must not skip temporal lighting.
        require(fixture.renderer.samples() <= requested_samples || requested_samples == 0,
                "Renderer selected more MSAA samples than requested");
        for (bool moving : {false, true}) {
            const auto spatial = measure(false, moving), temporal = measure(true, moving);
            std::cout << "curved textured dynamic actor " << (moving ? "animated" : "stationary")
                      << " samples=" << fixture.renderer.samples()
                      << " variance fresh=" << spatial.variance() << " retained=" << temporal.variance()
                      << " physical-mean-error=" << temporal.error(spatial) << '\n';
            require(spatial.variance() > .25,
                    "Curved actor fixture did not generate consumer-visible stochastic noise");
            require(temporal.variance() < spatial.variance()*.7,
                    "Stable dynamic actor identity lost temporal AO/GI/reflection noise reduction");
            require(temporal.error(spatial) <= 12,
                    "Dynamic illumination history changed the physical mean or left textured pose trails");
        }
        fixture.renderer.reset_history();
        for (int frame = 0; frame < 16; ++frame) draw(0);
        const auto removed = draw(0, false);
        fixture.renderer.reset_history();
        const auto empty = draw(0, false);
        int ghost = 0;
        for (int y = bottom; y < bottom+24; ++y)
            for (int x = left; x < left+48; ++x)
                for (int c = 0; c < 3; ++c) {
                    const auto at = (std::size_t(y)*fixture.width+x)*4+c;
                    ghost = std::max(ghost, std::abs(int(removed[at])-int(empty[at])));
                }
        require(ghost <= 2, "Removed curved actor left old stochastic radiance on the background");
    }
    fixture.renderer.release_mesh(actor); fixture.renderer.release_mesh(source);
    fixture.renderer.set_graphics(saved);
    std::cout << "curved textured dynamic actor/full-strength AO GI reflection/prior vertex-model pose/TAA off/MSAA8/removal PASS\n";
}

void check_oblique_half_resolution_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "oblique half-resolution perspective history SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    constexpr float half_height = .75f, render_scale = .5f;
    graphics.enhanced = true; graphics.temporal_aa = false; graphics.render_scale = render_scale; graphics.ray_tracing = true;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = graphics.shadows = 1;
    graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1; graphics.roughness = .35f;
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> floor_vertices{{
        {{-700,-80,-200},{0,0},{0,1,0}}, {{700,-80,-200},{1,0},{0,1,0}},
        {{700,-80,-1600},{1,1},{0,1,0}}, {{-700,-80,-1600},{0,1},{0,1,0}}
    }};
    auto source_vertices = fixture.blocker_vertices;
    const auto by_depth = [](const Vertex& a, const Vertex& b) { return a.position.z < b.position.z; };
    const float original_back_z =
        std::min_element(source_vertices.begin(), source_vertices.end(), by_depth)->position.z;
    // Thirty units above the receiver: even an infinite plane misses 25% of
    // cosine-hemisphere GI rays within their real sixty-unit distance limit.
    for (auto& vertex : source_vertices) {
        vertex.position.y = -50;
        if (vertex.position.z == original_back_z) vertex.position.z = -1000;
        else vertex.position.z = -380;
    }
    const auto floor = fixture.renderer.upload_mesh(floor_vertices, indices);
    const auto source = fixture.renderer.upload_mesh(source_vertices, indices);
    std::array<std::uint8_t, 256*4> texels{};
    for (int x = 0; x < 256; ++x) {
        texels[x*4] = x%4<2 ? 96 : 192;
        texels[x*4+1] = x%4<2 ? 180 : 108;
        texels[x*4+2] = 140; texels[x*4+3] = 255;
    }
    const auto texture = fixture.renderer.upload_texture(256, 1, texels, false, false);
    const auto& source_front = std::max_element(source_vertices.begin(), source_vertices.end(), by_depth)->position;
    const float source_back_distance =
        -std::min_element(source_vertices.begin(), source_vertices.end(), by_depth)->position.z;
    const float source_front_distance = -source_front.z;
    const float floor_y = floor_vertices.front().position.y;
    const float source_front_edge =
        fixture.height*(.5f+source_front.y/(source_front_distance*half_height)*.5f);
    // Reserve source texels for jitter (.5), bilinear center reach (1),
    // any MSAA sample offset (.5), and a further .5 safety margin.
    constexpr float source_margin = 2.5f;
    // Choose the physical floor location independently of drawable density.
    constexpr float target_floor_distance = 470;
    const int bottom = int(std::floor(fixture.height*(.5f+floor_y/
                                                     (target_floor_distance*half_height)*.5f)));
    const float actual_source_margin = (source_front_edge-(bottom+.5f))*render_scale;
    require(actual_source_margin >= source_margin,
            "Oblique history ROI crosses the caster's mixed-geometry reconstruction footprint");
    const int left = fixture.width/2-17;
    const float patch_z = -floor_y/(half_height*(1-2*(bottom+.5f)/fixture.height));
    const float units_per_pixel = 2*patch_z*half_height*fixture.aspect/fixture.width;
    const float source_height = source_front.y-floor_y;
    const float reflected_z = patch_z*(1+source_height/-floor_y);
    const float gi_depth_gap = std::max(0.f, source_front_distance-patch_z);
    const float closest_gi_distance = std::sqrt(source_height*source_height+gi_depth_gap*gi_depth_gap);
    // Project the real caster along the same world light as effects.frag.
    const Vec3 light_direction{.5f, 1.f, .3f};
    const auto by_width = [](const Vertex& a, const Vertex& b) { return a.position.x < b.position.x; };
    const auto source_width = std::minmax_element(source_vertices.begin(), source_vertices.end(), by_width);
    const float shadow_min_x = source_width.first->position.x-source_height*light_direction.x/light_direction.y;
    const float shadow_max_x = source_width.second->position.x-source_height*light_direction.x/light_direction.y;
    const float shadow_min_z = -source_back_distance-source_height*light_direction.z/light_direction.y;
    const float shadow_max_z = -source_front_distance-source_height*light_direction.z/light_direction.y;
    const float native_margin = source_margin/render_scale;
    float shadow_margin = shadow_max_x-shadow_min_x;
    // Include every measured pixel, all four poses, and the full AA/upscale
    // footprint: changing hard-shadow visibility is not stochastic ray noise.
    for (float x : {left+.5f-native_margin, left+23+.5f+3+native_margin})
        for (float y : {bottom+.5f-native_margin, bottom+.5f+native_margin}) {
            const float distance = -floor_y/(half_height*(1-2*y/fixture.height));
            const float world_x = (2*x/fixture.width-1)*distance*half_height*fixture.aspect;
            const float world_z = -distance;
            require(world_x >= shadow_min_x && world_x <= shadow_max_x &&
                    world_z >= shadow_min_z && world_z <= shadow_max_z,
                    "Oblique history ROI crosses the caster's hard-shadow reconstruction footprint");
            shadow_margin = std::min({shadow_margin, world_x-shadow_min_x, shadow_max_x-world_x,
                                      world_z-shadow_min_z, shadow_max_z-world_z});
        }
    std::cout << "oblique perspective ROI height=" << fixture.height
              << " source-front-world=" << source_front.x << ',' << source_front.y << ',' << source_front.z
              << " half-height=" << half_height << " render-scale=" << render_scale
              << " source-front-edge=" << source_front_edge << " center=" << bottom+.5f
              << " source-margin=" << actual_source_margin << " floor-z=" << patch_z
              << " mirror-hit-z=" << reflected_z << " nearest-GI=" << closest_gi_distance
              << " shadow-world-margin=" << shadow_margin << '\n';
    require(reflected_z >= source_front_distance && reflected_z <= source_back_distance &&
            closest_gi_distance < 60,
            "Interior oblique ROI no longer reaches its real reflection/GI caster");
    const auto draw = [&](int pose) {
        fixture.renderer.clear();
        auto moved_vertices = floor_vertices;
        for (auto& vertex : moved_vertices) vertex.position.x += pose*units_per_pixel*.5f;
        fixture.renderer.update_mesh(floor, moved_vertices);
        fixture.renderer.camera(identity_matrix(), frustum_projection(half_height, fixture.aspect, 1, 30000));
        DrawState state; state.temporal_id = 0x7103; state.temporal_static = false;
        state.model.values[12] = pose*units_per_pixel*.5f; state.color = {.7f,.7f,.7f,1};
        fixture.renderer.draw(floor, texture, fixture.material, fixture.pass, state);
        state = {}; state.temporal_static = true; state.temporal_id = 0x7104;
        state.color = {1,.65f,.35f,1};
        fixture.renderer.draw(source, 0, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba(); fixture.renderer.present(); return image;
    };
    const auto measure = [&](bool retain, bool moving) {
        fixture.renderer.reset_history();
        if (retain)
            for (int frame = 0; frame < 32; ++frame) draw(moving ? frame%4 : 0);
        TemporalPixels result{24, 1};
        for (int frame = 0; frame < 16; ++frame) {
            if (!retain) fixture.renderer.reset_history();
            const int pose = moving ? frame%4 : 0;
            result.add(draw(pose), fixture.width, left+pose, bottom);
        }
        return result;
    };
    for (int requested_samples : {4, 8}) {
        graphics.samples = requested_samples;
        fixture.renderer.set_graphics(graphics);
        for (bool moving : {false, true}) {
            const auto fresh = measure(false, moving), retained = measure(true, moving);
            std::cout << "oblique perspective half-resolution " << (moving ? "vertex+model" : "stationary")
                      << " samples=" << fixture.renderer.samples()
                      << " variance fresh=" << fresh.variance() << " retained=" << retained.variance()
                      << " physical-error=" << retained.error(fresh)
                      << " contrast=" << fresh.stripe_contrast() << '/'
                      << retained.stripe_contrast() << '\n';
            require(fresh.variance() > .1,
                    "Oblique perspective fixture did not exercise visible texture/illumination noise");
            require(retained.variance() < fresh.variance()*.75,
                    "Half-resolution MSAA perspective history rejected the same oblique surface");
            require(retained.error(fresh) <= 10 && fresh.stripe_contrast() > 2 &&
                    retained.stripe_contrast() >= fresh.stripe_contrast()*.7,
                    "Oblique previous vertex/model correspondence smeared physical artwork or illumination");
        }
    }
    fixture.renderer.release_mesh(floor); fixture.renderer.release_mesh(source);
    fixture.renderer.set_graphics(saved);
    std::cout << "perspective oblique floor/half resolution/MSAA4 and 8/full-strength RT/vertex-model convergence PASS\n";
}

void check_msaa_metadata(Fixture& fixture, bool force_off) {
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.samples = 4; graphics.render_scale = 1; graphics.temporal_aa = false;
    graphics.ray_tracing = !force_off; graphics.shadows = 0;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> plane{{
        {{-1,-1,0},{0,0},{0,0,1}}, {{1,-1,0},{1,0},{0,0,1}},
        {{1,1,0},{1,1},{0,0,1}}, {{-1,1,0},{0,1},{0,0,1}}
    }};
    auto edge = plane;
    edge[0].position.x = -.137f; edge[3].position.x = .193f;
    for (auto& vertex : edge) vertex.position.z = -.2f;
    const auto world = fixture.renderer.upload_mesh(plane, indices);
    const auto silhouette = fixture.renderer.upload_mesh(edge, indices);
    const auto draw = [&] {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), identity_matrix());
        DrawState state; state.temporal_static = true; state.color = {.2f,.2f,.2f,1};
        fixture.renderer.draw(world, 0, fixture.material, fixture.pass, state);
        state.ray_geometry = false; state.temporal_static = false; state.color = {.7f,.7f,.7f,1};
        fixture.renderer.draw(silhouette, 0, fixture.material, fixture.pass, state);
        // Clipped raster geometry remains a real, large ray shadow caster.
        state = {}; state.model.values[0] = state.model.values[5] = 10;
        state.model.values[14] = 1.2f;
        fixture.renderer.draw(world, 0, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto result = fixture.renderer.capture_rgba(); fixture.renderer.present(); return result;
    };
    const auto unshadowed = draw();
    bool coverage = false;
    for (int y = 8; y < fixture.height-8; ++y)
        for (int x = fixture.width/3; x < fixture.width*2/3; ++x) {
            const int value = unshadowed[(std::size_t(y)*fixture.width+x)*4];
            coverage |= value > 53 && value < 176;
        }
    require(coverage, "Enhanced MSAA discarded averaged artwork silhouette coverage");
    if (!force_off && fixture.renderer.ray_tracing_available()) {
        graphics.shadows = 1; fixture.renderer.set_graphics(graphics);
        const auto positive = draw();
        // Excluded normals cannot contaminate the selected world sample.
        for (auto& vertex : edge) vertex.normal = {0,0,-1};
        fixture.renderer.update_mesh(silhouette, edge);
        fixture.renderer.reset_history();
        const auto negative = draw();
        int changed = 0;
        for (std::size_t at = 0; at < positive.size(); at += 4)
            changed += std::abs(int(positive[at])-int(negative[at])) > 2;
        std::cout << "MSAA excluded-normal contamination pixels=" << changed << '\n';
        require(changed == 0, "Enhanced MSAA averaged unrelated normals against sample-zero depth");
    }
    fixture.renderer.release_mesh(world); fixture.renderer.release_mesh(silhouette);
    fixture.renderer.set_graphics(saved);
    std::cout << "enhanced MSAA silhouette coverage/coherent selected geometry PASS\n";
}
void check_reflection_noise(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "finite-source reflection convergence/motion SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.samples = 8; graphics.temporal_aa = false;
    graphics.render_scale = 1; graphics.ray_tracing = true; graphics.exposure = 1;
    graphics.roughness = .85f; graphics.reflections = 0;
    graphics.ambient_occlusion = graphics.indirect_lighting = graphics.shadows = 0;
    graphics.bloom = graphics.sharpen = 0;
    fixture.renderer.set_graphics(graphics);
    constexpr float half_height = .75f, distance = 2;
    const float half_width = distance*half_height*fixture.aspect;
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> receiver_vertices{{
        {{-1.1f*half_width,-1.1f*distance*half_height,-distance},{-.05f,-.05f},{0,0,1}},
        {{1.1f*half_width,-1.1f*distance*half_height,-distance},{1.05f,-.05f},{0,0,1}},
        {{1.1f*half_width,1.1f*distance*half_height,-distance},{1.05f,1.05f},{0,0,1}},
        {{-1.1f*half_width,1.1f*distance*half_height,-distance},{-.05f,1.05f},{0,0,1}}
    }};
    const std::array<Vertex, 4> red_vertices{{
        {{-3.2f,-3.2f,2},{0,0},{0,0,-1}}, {{3.2f,-3.2f,2},{1,0},{0,0,-1}},
        {{3.2f,3.2f,2},{1,1},{0,0,-1}}, {{-3.2f,3.2f,2},{0,1},{0,0,-1}}
    }};
    const std::array<Vertex, 4> green_vertices{{
        {{-6,-6,5},{0,0},{0,0,-1}}, {{6,-6,5},{1,0},{0,0,-1}},
        {{6,6,5},{1,1},{0,0,-1}}, {{-6,6,5},{0,1},{0,0,-1}}
    }};
    const auto receiver = fixture.renderer.upload_mesh(receiver_vertices, indices);
    const auto red = fixture.renderer.upload_mesh(red_vertices, indices);
    const auto green = fixture.renderer.upload_mesh(green_vertices, indices);
    const std::array<std::uint8_t, 4> white{{255,255,255,255}};
    const auto source_texture = fixture.renderer.upload_texture(1, 1, white, false, false);
    std::vector<std::uint8_t> texels(std::size_t(fixture.width)*4);
    const int smooth_left = fixture.width/2-16, art_left = fixture.width*75/100-32;
    const int bottom = fixture.height/2-8, stroke = art_left+32;
    for (int x = 0; x < fixture.width; ++x) {
        texels[x*4] = texels[x*4+1] = 56;
        texels[x*4+2] = x < fixture.width*60/100 ? 56 : (x%4<2 ? 112 : 144);
        if (x >= stroke && x < stroke+3) texels[x*4+2] = 24;
        texels[x*4+3] = 255;
    }
    const auto texture = fixture.renderer.upload_texture(fixture.width, 1, texels, false, false);
    const float source_color = std::pow(4.f,1.f/2.2f);
    const auto draw = [&](bool moved) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), frustum_projection(half_height,fixture.aspect,1,1000));
        DrawState state; state.temporal_static = true; state.temporal_id = 0x7A0A;
        fixture.renderer.draw(receiver, texture, fixture.material, fixture.pass, state);
        // Both emitters remain behind the camera and in the actual acceleration
        // scene. Only RED's model changes; no raster owner, texture epoch, mesh,
        // material, instance ID, camera or graphics setting changes at the move.
        state.temporal_id = 0x7A0B; state.color = {source_color,0,0,1};
        state.model.values[12] = moved ? 100.f : 0.f;
        fixture.renderer.draw(red, source_texture, fixture.material, fixture.pass, state);
        state.temporal_id = 0x7A0C; state.color = {0,source_color,0,1};
        state.model = identity_matrix();
        fixture.renderer.draw(green, source_texture, fixture.material, fixture.pass, state);
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba(); fixture.renderer.present(); return image;
    };
    struct Samples {
        TemporalPixels smooth{32,16}, artwork{64,16};
        std::array<double,3> linear{}, byte_bound{};
    };
    const auto add = [&](Samples& samples, const std::vector<std::uint8_t>& image) {
        samples.smooth.add(image, fixture.width, smooth_left, bottom);
        samples.artwork.add(image, fixture.width, art_left, bottom);
        for (int y = 0; y < samples.smooth.height; ++y)
            for (int x = 0; x < samples.smooth.width; ++x)
                for (int c = 0; c < 3; ++c) {
                    const double displayed = image[(std::size_t(bottom+y)*fixture.width+smooth_left+x)*4+c]/255.;
                    samples.linear[c] += std::pow(displayed,2.2);
                    // Exact upper derivative bound for the half-byte interval.
                    samples.byte_bound[c] += (1.1/255)*std::pow(std::min(1.,displayed+.5/255),1.2);
                }
    };
    const auto mean = [](const Samples& samples, int c) {
        return samples.linear[c]/(samples.smooth.frames*samples.smooth.width*samples.smooth.height);
    };
    const auto bound = [](const Samples& samples, int c) {
        return samples.byte_bound[c]/(samples.smooth.frames*samples.smooth.width*samples.smooth.height);
    };
    const auto measure_fresh = [&](bool moved) {
        Samples result;
        for (int frame = 0; frame < 64; ++frame) {
            fixture.renderer.reset_history();
            add(result,draw(moved));
        }
        return result;
    };
    // Independent neutral raster controls repeat the same meshes and IDs with
    // RED in each model position. A behind-camera source must not alter artwork.
    const auto neutral = measure_fresh(false), neutral_moved = measure_fresh(true);
    graphics.reflections = 1; fixture.renderer.set_graphics(graphics);
    const auto fresh = measure_fresh(false);
    fixture.renderer.reset_history();
    for (int frame = 0; frame < 128; ++frame) draw(false);
    Samples retained, old_red;
    for (int frame = 0; frame < 64; ++frame) {
        const auto image = draw(false); add(retained,image);
        if (frame >= 32) add(old_red,image);
    }
    std::array<Samples,4> moving;
    constexpr std::array<int,4> observed{{1,2,4,8}};
    for (int frame = 1; frame <= 8; ++frame) {
        const auto image = draw(true); // NO reset: exercise genuine source innovation.
        for (std::size_t i = 0; i < observed.size(); ++i)
            if (frame == observed[i]) add(moving[i],image);
    }
    // Only now destroy runtime history for the independent new-source truth.
    const auto fresh_green = measure_fresh(true);
    const double red_signal = mean(old_red,0)-mean(fresh_green,0);
    const double green_signal = mean(fresh_green,1)-mean(old_red,1);
    const double red_physical = mean(fresh,0)-mean(neutral,0);
    const double green_physical = mean(fresh_green,1)-mean(neutral,1);
    // Match the retained artwork's AA/history regime as well as its physical
    // source. Run this control last, leaving every source/motion RNG phase above
    // unchanged; neither emitter supplies any blue radiance.
    graphics.reflections = 0; fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    for (int frame = 0; frame < 128; ++frame) draw(false);
    Samples retained_neutral;
    for (int frame = 0; frame < 64; ++frame) add(retained_neutral,draw(false));
    double blue_error = 0, blue_detail = 0, blue_control_regime_error = 0;
    for (std::size_t at = 2; at < neutral.artwork.sum.size(); at += 3) {
        blue_error = std::max(blue_error,std::abs(retained.artwork.sum[at]/retained.artwork.frames-
                                                retained_neutral.artwork.sum[at]/retained_neutral.artwork.frames));
        blue_error = std::max(blue_error,std::abs(fresh.artwork.sum[at]/fresh.artwork.frames-
                                                neutral.artwork.sum[at]/neutral.artwork.frames));
        blue_control_regime_error = std::max(blue_control_regime_error,
            std::abs(retained_neutral.artwork.sum[at]/retained_neutral.artwork.frames-
                     neutral.artwork.sum[at]/neutral.artwork.frames));
        blue_detail = std::max(blue_detail,std::abs(neutral.artwork.sum[at]/neutral.artwork.frames-56));
    }
    std::cout << "finite-source reflection samples=" << fixture.renderer.samples()
              << " FOV-half-height=.75 roughness=.85 strength=1 linear-source=4"
              << " variance fresh/reset64=" << fresh.smooth.variance()
              << " warm128+retained64=" << retained.smooth.variance()
              << " ratio=" << retained.smooth.variance()/fresh.smooth.variance()
              << " fixed-.9-EMA-floor=" << .1/1.9 << " count59-EMA-floor=" << 1./119
              << " linear-meanRGB neutral/fresh/retained=";
    for (int c = 0; c < 3; ++c)
        std::cout << (c ? "," : "") << mean(neutral,c) << '/' << mean(fresh,c) << '/' << mean(retained,c);
    std::cout << " linear-physical-red/green=" << red_physical << '/' << green_physical
              << " neutral-model-repeat-error=" << neutral.smooth.error(neutral_moved.smooth)
              << " painted-blue-matched-maxerror=" << blue_error << " painted-blue-detail=" << blue_detail
              << " neutral-blue-fresh/retained-regime-maxdifference=" << blue_control_regime_error << std::endl;
    for (std::size_t i = 0; i < observed.size(); ++i)
        std::cout << "reflection source-model-move frame=" << observed[i]
                  << " linearRGB=" << mean(moving[i],0) << ',' << mean(moving[i],1) << ',' << mean(moving[i],2)
                  << " oldRed/newGreen=" << mean(old_red,0) << '/' << mean(fresh_green,0)
                  << ',' << mean(old_red,1) << '/' << mean(fresh_green,1)
                  << " remaining-red=" << (mean(moving[i],0)-mean(fresh_green,0))/red_signal
                  << " green-error=" << std::abs(mean(moving[i],1)-mean(fresh_green,1))/green_signal
                  << " linear-byte-boundRG=" << bound(moving[i],0)+bound(fresh_green,0)
                  << ',' << bound(moving[i],1)+bound(fresh_green,1) << std::endl;
    const auto& fourth = moving[2];
    const bool static_pass = retained.smooth.variance() < fresh.smooth.variance()*.025;
    const bool motion_pass =
        std::abs(mean(fourth,0)-mean(fresh_green,0)) <= red_signal*.15+bound(fourth,0)+bound(fresh_green,0) &&
        std::abs(mean(fourth,1)-mean(fresh_green,1)) <= green_signal*.15+bound(fourth,1)+bound(fresh_green,1);
    std::cout << "reflection gates steady-floor=" << (static_pass ? "PASS" : "FAIL")
              << " frame4-real-source-response=" << (motion_pass ? "PASS" : "FAIL") << std::endl;
    fixture.renderer.release_mesh(green); fixture.renderer.release_mesh(red);
    fixture.renderer.release_mesh(receiver); fixture.renderer.set_graphics(saved);
    require(neutral.smooth.error(neutral_moved.smooth) <= 1 &&
            neutral.artwork.error(neutral_moved.artwork) <= 1,
            "Reflection source model changed the primary raster receiver/artwork");
    require(red_physical > .01 && green_physical > .01 && red_signal > .01 && green_signal > .01 &&
            fresh.smooth.variance() > .25,
            "Finite reflection fixture lacks meaningful genuine hit/miss variance or red/green source signal");
    require(blue_detail >= 32 && blue_error <= 1,
            "Reflection convergence erased painted detail in a channel with exactly zero source radiance");
    for (int c = 0; c < 3; ++c)
        require(std::abs(mean(retained,c)-mean(fresh,c)) <=
                    std::abs(mean(fresh,c)-mean(neutral,c))*.15+bound(retained,c)+bound(fresh,c),
                "Reflection convergence changed the physical linear mean/strength");
    require(static_pass, "Stationary finite-source reflection retained the fixed-EMA consumer noise floor");
    require(motion_pass, "Unchanged receiver retained old red reflection after the real source moved aside");
    std::cout << "finite-source reflection stationary convergence/real source innovation/linear mean/painted detail PASS\n";
}

void check_rare_reflection_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "rare stationary finite-source reflection SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = graphics.ray_tracing = true; graphics.temporal_aa = false;
    graphics.samples = 8; graphics.render_scale = 1; graphics.exposure = 1;
    graphics.roughness = .85f; graphics.reflections = 0;
    graphics.shadows = graphics.ambient_occlusion = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0;
    fixture.renderer.set_graphics(graphics);
    const std::array<std::uint16_t,6> indices{{0,1,2,0,2,3}};
    const float half_width = 2*.75f*fixture.aspect;
    const std::array<Vertex,4> receiver_vertices{{
        {{-1.1f*half_width,-1.65f,-2},{0,0},{0,0,1}},
        {{1.1f*half_width,-1.65f,-2},{1,0},{0,0,1}},
        {{1.1f*half_width,1.65f,-2},{1,1},{0,0,1}},
        {{-1.1f*half_width,1.65f,-2},{0,1},{0,0,1}}
    }};
    // Four world units from the receiver: half-angle atan(.35/3.94).
    // Unlike the original 3.2-wide emitter, this narrow solid angle permits
    // legitimate whole-kernel misses. No mesh/model/material ever changes.
    const std::array<Vertex,4> source_vertices{{
        {{-.35f,-.35f,2},{0,0},{0,0,-1}}, {{.35f,-.35f,2},{1,0},{0,0,-1}},
        {{.35f,.35f,2},{1,1},{0,0,-1}}, {{-.35f,.35f,2},{0,1},{0,0,-1}}
    }};
    const auto receiver = fixture.renderer.upload_mesh(receiver_vertices,indices);
    const auto source = fixture.renderer.upload_mesh(source_vertices,indices);
    const int left = fixture.width/2-16, bottom = fixture.height/2-8;
    const auto draw = [&] {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(),frustum_projection(.75f,fixture.aspect,1,1000));
        DrawState state; state.temporal_static = true; state.temporal_id = 0x7A14;
        state.color = {56.f/255,56.f/255,56.f/255,1};
        fixture.renderer.draw(receiver,0,fixture.material,fixture.pass,state);
        state.temporal_id = 0x7A15; state.color = {std::pow(16.f,1.f/2.2f),0,0,1};
        fixture.renderer.draw(source,0,fixture.material,fixture.pass,state);
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba(); fixture.renderer.present(); return image;
    };
    fixture.renderer.reset_history();
    const auto neutral = draw();
    graphics.reflections = 1; fixture.renderer.set_graphics(graphics);
    TemporalPixels fresh{32,16}, retained{32,16};
    std::array<bool,32*16> previous_zero{};
    int zeros=0, hits=0, zero_to_hit=0;
    double fresh_linear=0, retained_linear=0, neutral_linear=0, byte_bound=0;
    const auto add = [&](TemporalPixels& result, const std::vector<std::uint8_t>& image,
                         double& linear, bool detect) {
        result.add(image,fixture.width,left,bottom);
        for (int y=0;y<16;++y) for (int x=0;x<32;++x) {
            const auto at = (std::size_t(bottom+y)*fixture.width+left+x)*4;
            const double displayed = image[at]/255.;
            linear += std::pow(displayed,2.2);
            byte_bound += (1.1/255)*std::pow(std::min(1.,displayed+.5/255),1.2);
            if (detect) {
                const auto index = std::size_t(y)*32+x;
                const bool zero = image[at]==neutral[at];
                zeros += zero; hits += image[at]>neutral[at]+1;
                zero_to_hit += result.frames>1 && previous_zero[index] && image[at]>neutral[at]+1;
                previous_zero[index] = zero;
                neutral_linear += std::pow(neutral[at]/255.,2.2);
            }
        }
    };
    for (int frame=0;frame<256;++frame) {
        fixture.renderer.reset_history();
        add(fresh,draw(),fresh_linear,true);
    }
    fixture.renderer.reset_history();
    for (int frame=0;frame<256;++frame) draw();
    for (int frame=0;frame<256;++frame) add(retained,draw(),retained_linear,false);
    constexpr double count=256.*32*16;
    const double physical = (fresh_linear-neutral_linear)/count;
    const double mean_error = std::abs(retained_linear-fresh_linear)/count;
    std::cout << "rare unchanged reflection emitter-half=.35 z=2 receiver-z=-2"
              << " half-angle=" << std::atan(.35/3.94) << " linear-source=16"
              << " roughness=.85 reflection-strength=1 samples=" << fixture.renderer.samples()
              << " consumer-zero=" << zeros << " consumer-hit=" << hits
              << " stationary-consumer-zero-to-hit=" << zero_to_hit
              << " variance fresh/reset256=" << fresh.variance()
              << " warm256+retained256=" << retained.variance()
              << " ratio=" << retained.variance()/fresh.variance()
              << " linear-physical-red=" << physical << " linear-mean-error=" << mean_error
              << " linear-byte-bound=" << byte_bound/count << std::endl;
    fixture.renderer.release_mesh(source); fixture.renderer.release_mesh(receiver);
    fixture.renderer.set_graphics(saved);
    require(zeros>1000 && hits>1000 && zero_to_hit>100,
            "Rare unchanged emitter lacks observed consumer zero-to-hit episodes");
    require(physical>.0002 && fresh.variance()>.05,
            "Rare reflection fixture lacks meaningful physical signal/Monte Carlo noise");
    require(mean_error<=physical*.15+byte_bound/count,
            "Rare stationary reflection changed its physical linear mean/strength");
    require(retained.variance()<fresh.variance()*.025,
            "Stationary whole-kernel reflection misses collapsed retained confidence");
    std::cout << "rare unchanged reflected-emitter consumer zero-to-hit/convergence PASS\n";
}

void check_static_shadow_noise(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) {
        std::cout << "static Sun shadow-edge noise SKIPPED: RT off/unavailable\n";
        return;
    }
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.samples = 8; graphics.temporal_aa = false;
    graphics.ray_tracing = true; graphics.shadows = 1; graphics.exposure = 1;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.bloom = graphics.sharpen = 0;
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    // Local meshes leave the small moving/mixed-shadow fixtures unchanged.
    // Extend the receiver past the measurement border: no sky coverage in its ROI.
    const std::array<Vertex, 4> floor_vertices{{
        {{-4000,-80,-200},{0,0},{0,1,0}}, {{4000,-80,-200},{1,0},{0,1,0}},
        {{4000,-80,-6000},{1,1},{0,1,0}}, {{-4000,-80,-6000},{0,1},{0,1,0}}
    }};
    const std::array<Vertex, 4> caster_vertices{{
        {{-150,100,-1000},{0,0},{0,1,0}}, {{150,100,-1000},{1,0},{0,1,0}},
        {{150,100,-380},{1,1},{0,1,0}}, {{-150,100,-380},{0,1},{0,1,0}}
    }};
    const auto floor = fixture.renderer.upload_mesh(floor_vertices, indices);
    const auto caster = fixture.renderer.upload_mesh(caster_vertices, indices);
    const auto draw = [&](bool present) {
        fixture.renderer.clear();
        fixture.renderer.camera(identity_matrix(), frustum_projection(.75f, fixture.aspect, 1, 30000));
        DrawState state; state.lighting = true; state.temporal_static = true;
        state.temporal_id = 0x7A08;
        state.color = {.5f,.5f,.5f,1}; state.diffuse_light = {.5f,.5f,.5f,1};
        fixture.renderer.draw(floor, 0, fixture.material, fixture.pass, state);
        if (present) {
            state.temporal_id = 0x7A09;
            fixture.renderer.draw(caster, 0, fixture.material, fixture.pass, state);
        }
        fixture.renderer.finish_world();
        auto image = fixture.renderer.capture_rgba(); fixture.renderer.present(); return image;
    };
    struct Point { double x, y; };
    const auto project = [&](double x, double z) {
        return Point{fixture.width*(.5+x/(-z*.75*fixture.aspect)*.5),
                     fixture.height*(.5-80/(-z*.75)*.5)};
    };
    // Independent geometry: Sun (.5,1,.3), floor -80, caster +100,
    // and the unchanged .06 normal ray-origin offset give height 179.94.
    const std::array<Point, 4> polygon{{
        project(-150-89.97, -1000-53.982), project(150-89.97, -1000-53.982),
        project(150-89.97, -380-53.982), project(-150-89.97, -380-53.982)
    }};
    double twice_area = 0;
    double min_x = fixture.width, min_y = fixture.height, max_x = 0, max_y = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i+1)%polygon.size()];
        twice_area += a.x*b.y-a.y*b.x;
        min_x = std::min(min_x, a.x); max_x = std::max(max_x, a.x);
        min_y = std::min(min_y, a.y); max_y = std::max(max_y, a.y);
    }
    const double physical_area = std::abs(twice_area)*.5;
    const int left = int(std::floor(min_x))-4, bottom = int(std::floor(min_y))-4;
    const int width = int(std::ceil(max_x))+4-left, height = int(std::ceil(max_y))+4-bottom;
    require(left >= 0 && bottom >= 0 && left+width <= fixture.width &&
            bottom+height <= fixture.height, "Static Sun physical shadow ROI outside drawable");
    const auto inside = [&](Point p) {
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const auto a = polygon[i], b = polygon[(i+1)%polygon.size()];
            const double cross = (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
            if (cross*twice_area < 0) return false;
        }
        return true;
    };
    const auto edge_distance = [&](Point p) {
        double distance = fixture.width;
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const auto a = polygon[i], b = polygon[(i+1)%polygon.size()];
            const double dx = b.x-a.x, dy = b.y-a.y;
            const double t = std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/(dx*dx+dy*dy), 0., 1.);
            distance = std::min(distance, std::hypot(p.x-a.x-t*dx, p.y-a.y-t*dy));
        }
        return distance;
    };
    struct Samples {
        TemporalPixels pixels;
        std::vector<double> linear;
        Samples(int w, int h) : pixels(w,h), linear(std::size_t(w)*h) {}
    };
    const auto measure = [&](bool retain, bool present) {
        fixture.renderer.reset_history();
        if (retain) for (int frame = 0; frame < 64; ++frame) draw(present);
        Samples result(width, height);
        for (int frame = 0; frame < 64; ++frame) {
            if (!retain) fixture.renderer.reset_history();
            const auto image = draw(present);
            result.pixels.add(image, fixture.width, left, bottom);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    result.linear[std::size_t(y)*width+x] +=
                        std::pow(image[(std::size_t(bottom+y)*fixture.width+left+x)*4]/255., 2.2)/64;
        }
        return result;
    };
    // This neutral floor is below the HDR shoulder. Gamma 2.2 decoding
    // recovers linear gain: a real fully occluded Sun multiplies it by .3.
    const double neutral_rgb = 255*(.496+.4/std::sqrt(1+.5*.5+.3*.3));
    for (float scale : {1.f, .5f}) {
        graphics.render_scale = scale; fixture.renderer.set_graphics(graphics);
        const auto neutral = measure(true, false);
        const auto fresh = measure(false, true), retained = measure(true, true);
        double neutral_mean = 0, fresh_area = 0, retained_area = 0, quantization_area = 0;
        for (std::size_t at = 0; at < neutral.linear.size(); ++at) {
            const double lit = neutral.linear[at];
            neutral_mean += neutral.pixels.sum[at*3]/64;
            require(lit > .1, "Static Sun no-caster ROI lacks its actual neutral receiver");
            fresh_area += (lit-fresh.linear[at])/(.7*lit);
            retained_area += (lit-retained.linear[at])/(.7*lit);
            // Each displayed byte is rounded within half a level. Use the
            // largest derivative in [0,lit+half-level] for both decoded images.
            const double lit_display = std::pow(lit, 1/2.2);
            quantization_area += (2.2/255)*std::pow(std::min(1.,lit_display+.5/255),1.2)/(.7*lit);
        }
        neutral_mean /= neutral.linear.size();
        double fresh_variance = 0, retained_variance = 0;
        int edge_pixels = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                if (edge_distance({left+x+.5, bottom+y+.5}) > 2/scale) continue;
                ++edge_pixels;
                for (int c = 0; c < 3; ++c) {
                    const auto at = (std::size_t(y)*width+x)*3+c;
                    const double f = fresh.pixels.sum[at]/64;
                    const double r = retained.pixels.sum[at]/64;
                    fresh_variance += std::max(0.,fresh.pixels.square[at]/64-f*f);
                    retained_variance += std::max(0.,retained.pixels.square[at]/64-r*r);
                }
            }
        require(edge_pixels > 0, "Static Sun independent physical edge ROI is empty");
        fresh_variance /= edge_pixels*3; retained_variance /= edge_pixels*3;
        // Explicit independent finite-source quadrature, not an arbitrary
        // widened quality percentage. Bilinear reconstruction preserves the
        // integral of these fixed-grid cell values within the padded ROI.
        double grid_area = 0;
        const int pitch = int(1/scale);
        for (int y = 0; y < fixture.height; y += pitch)
            for (int x = 0; x < fixture.width; x += pitch)
                if (inside({x+pitch*.5, y+pitch*.5})) grid_area += pitch*pitch;
        const double tolerance = physical_area*.05+std::abs(grid_area-physical_area)+quantization_area;
        std::cout << "static Sun shadow-edge scale=" << scale << " samples=" << fixture.renderer.samples()
                  << " frames fresh/reset=" << fresh.pixels.frames << " retained=" << retained.pixels.frames
                  << " warmup=64 edge-pixels=" << edge_pixels
                  << " variance fresh/reset64=" << fresh_variance << " retained64=" << retained_variance
                  << " neutral-meanRGB=" << neutral_mean
                  << " area physical/grid/fresh/retained=" << physical_area << '/' << grid_area
                  << '/' << fresh_area << '/' << retained_area
                  << " coverage-mean physical/fresh/retained=" << physical_area/neutral.linear.size()
                  << '/' << fresh_area/neutral.linear.size() << '/' << retained_area/neutral.linear.size()
                  << " area-tolerance 5%+grid+byte=" << tolerance << std::endl;
        require(std::abs(neutral_mean-neutral_rgb) <= 1,
                "Static Sun no-caster image changed the original neutral floor transfer");
        require(fresh_area > physical_area*.8 && retained_area > physical_area*.8,
                "Static Sun lost real opaque caster shadow strength/area");
        require(std::abs(fresh_area-physical_area) <= tolerance &&
                std::abs(retained_area-physical_area) <= tolerance,
                "Static Sun changed independent physical shadow area/mean beyond five percent plus finite sampling");
        require(retained_variance <= .1,
                "Stationary real Sun shadow edge retained consumer-visible noise");
    }
    fixture.renderer.release_mesh(caster); fixture.renderer.release_mesh(floor);
    fixture.renderer.set_graphics(saved);
    std::cout << "static Sun shadow-edge/RT history/MSAA8/native and half/independent physical extent PASS\n";
}

void check_mixed_shadow_coverage_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) return;
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.enhanced = true; graphics.render_scale = .5f; graphics.samples = 0;
    graphics.temporal_aa = false; graphics.ray_tracing = true;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.shadows = 1; graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    const auto lit = fixture.frame(Fixture::Mode::Absent);
    fixture.renderer.reset_history();
    const auto fresh = fixture.frame(Fixture::Mode::Opaque);
    auto retained = fresh;
    for (int frame = 0; frame < 24; ++frame) retained = fixture.frame(Fixture::Mode::Opaque);
    int darkest_receiver = 255;
    for (std::size_t at = 0; at < fresh.size(); at += 4)
        if (lit[at] > 100 && fresh[at] > 0)
            darkest_receiver = std::min(darkest_receiver, int(fresh[at]));
    int mixed_pixels = 0, maximum = 0, mixed_maximum = 0;
    for (std::size_t at = 0; at < fresh.size(); at += 4) {
        const int darkening = int(lit[at])-int(fresh[at]);
        // Exclude fully shadowed interiors as well as untouched lit pixels.
        // Remaining display levels prove actual shadow/lit reconstruction mix.
        const bool mixed = lit[at] > 100 && darkening >= 10 && darkening <= 60 &&
                           fresh[at] > darkest_receiver+3;
        mixed_pixels += mixed;
        for (int c = 0; c < 3; ++c) {
            const int error = std::abs(int(retained[at+c])-int(fresh[at+c]));
            maximum = std::max(maximum, error);
            if (mixed) mixed_maximum = std::max(mixed_maximum, error);
        }
    }
    std::cout << "fixed-jitter half-resolution shadow mixed-pixels=" << mixed_pixels
              << " darkest-receiver=" << darkest_receiver << " retained/fresh max-RGB-error=" << maximum
              << " mixed-max-RGB-error=" << mixed_maximum << '\n';
    require(mixed_pixels > 0, "Shadow coverage fixture did not reconstruct a real mixed shadow edge");
    require(maximum <= 3 && mixed_maximum <= 3,
            "Resolved shadow coverage was reused as selected illumination and brightened with history");
    fixture.renderer.set_graphics(saved);
    std::cout << "fixed-pose/mixed-shadow coverage/half-resolution/no-TAA illumination history PASS\n";
}

void check_moving_shadow_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) return;
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.render_scale = 1; graphics.samples = 0; graphics.ray_tracing = true; graphics.temporal_aa = false;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.shadows = 1; graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    const auto lit = fixture.frame(Fixture::Mode::Absent);
    for (int frame = 0; frame < 12; ++frame) fixture.frame(Fixture::Mode::Opaque);
    const auto moved = fixture.frame(Fixture::Mode::Opaque, 250);
    fixture.renderer.reset_history();
    const auto fresh = fixture.frame(Fixture::Mode::Opaque, 250);
    std::cout << "moving shadow old-patch lit/moved/fresh=" << fixture.patch(lit, -90, -654) << '/'
              << fixture.patch(moved, -90, -654) << '/' << fixture.patch(fresh, -90, -654)
              << " new-patch lit/moved/fresh=" << fixture.patch(lit, 160, -654) << '/'
              << fixture.patch(moved, 160, -654) << '/' << fixture.patch(fresh, 160, -654) << '\n';
    require(std::abs(fixture.patch(moved, -90, -654)-fixture.patch(lit, -90, -654)) <= 3,
            "Stationary receiver retained a moving blocker's old shadow");
    require(fixture.patch(lit, 160, -654)-fixture.patch(moved, 160, -654) >= 4,
            "Moving blocker failed to shadow its new receiver footprint");
    require(std::abs(fixture.patch(moved, 160, -654)-fixture.patch(fresh, 160, -654)) <= 3,
            "Stationary receiver delayed a moving blocker's new shadow");
    fixture.renderer.set_graphics(saved);
    std::cout << "stationary receiver/moving blocker/RT short history PASS\n";
}
} // namespace
void check_day_night(Fixture& fixture) {
    auto& renderer=fixture.renderer;
    const auto original=renderer.graphics_settings();
    auto rt_settings=original; rt_settings.ray_tracing=true; rt_settings.shadows=1;
    renderer.set_graphics(rt_settings);
    const std::array<std::uint8_t,4> white{{255,255,255,255}};
    const auto texture=renderer.upload_texture(1,1,white,false);
    const auto frame=[&](float emission) {
        renderer.reset_history();
        renderer.clear();
        renderer.camera(identity_matrix(),frustum_projection(.75f,fixture.aspect,1,30000));
        DrawState state;
        state.lighting=state.temporal_static=state.fog=true;
        state.diffuse_light={.5f,.5f,.5f,1}; state.emission=emission;
        renderer.draw(fixture.floor,texture,fixture.material,fixture.pass,state);
        renderer.finish_world();
        renderer.hud_camera();
        DrawState ui; ui.color={.125f,.75f,.25f,1}; ui.ray_geometry=false;
        auto pass=fixture.pass; pass.depth_test=pass.depth_write=false;
        renderer.draw(fixture.overlay,0,fixture.material,pass,ui);
        auto result=renderer.capture_rgba(); renderer.present(); return result;
    };
    renderer.set_environment(environment_at(TimeOfDay::Day,0));
    const auto day=frame(0);
    require(day==frame(0),"Fixed daylight was not deterministic");
    renderer.set_environment(environment_at(TimeOfDay::Night,0));
    const auto night=frame(0),lamps=frame(1);
    require(night!=day,"Night did not change the lit/fogged world");
    std::uint64_t night_sum=0,lamp_sum=0;
    for(std::size_t i=0;i<night.size();i+=4) { night_sum+=night[i]; lamp_sum+=lamps[i]; }
    require(lamp_sum>night_sum+1000,"Night lamp emission did not brighten the world");
    const int hud_x=int(64.f*fixture.height/768),hud_y=hud_x;
    for(const auto* image:{&day,&night,&lamps}) {
        const auto at=(std::size_t(hud_y)*fixture.width+hud_x)*4;
        require(std::abs(int((*image)[at])-32)<=1 && std::abs(int((*image)[at+1])-191)<=1 &&
                std::abs(int((*image)[at+2])-64)<=1,"Environment changed native-resolution HUD");
    }
    auto classic_settings=original; classic_settings.enhanced=false;
    renderer.set_graphics(classic_settings); renderer.set_environment(environment_at(TimeOfDay::Day,0));
    require(frame(0)==frame(0),"Classic fixed daylight was not deterministic");
    renderer.set_graphics(original); renderer.set_environment(environment_at(TimeOfDay::Day,0));
    std::cout<<"day/night environment, emissive lamps and unchanged HUD PASS\n";
}
void check_local_lights(Fixture& fixture) {
    if (!fixture.renderer.ray_tracing_available()) return;
    auto& renderer=fixture.renderer;
    const auto original=renderer.graphics_settings();
    auto settings=original;
    settings.ray_tracing=true; settings.temporal_aa=false; settings.shadows=0;
    settings.ambient_occlusion=settings.reflections=settings.indirect_lighting=0;
    renderer.set_graphics(settings);
    renderer.set_environment(environment_at(TimeOfDay::Night,0));
    const auto render=[&](Fixture::Mode mode,bool illuminated,float light_x=0,bool reset=true) {
        if(reset) renderer.reset_history(); renderer.clear();
        renderer.camera(identity_matrix(),frustum_projection(.75f,fixture.aspect,1,30000));
        if (illuminated) {
            LocalLight light;
            light.position={light_x,200,-654}; light.color={1,1,1}; light.intensity=40; light.radius=500;
            renderer.submit_light(light);
        }
        DrawState state; state.lighting=state.temporal_static=true;
        renderer.draw(fixture.floor,0,fixture.material,fixture.pass,state);
        if (mode!=Fixture::Mode::Absent) renderer.draw(fixture.blocker,0,fixture.material,fixture.pass,state);
        renderer.finish_world();
        auto result=renderer.capture_rgba(); renderer.present(); return result;
    };
    const auto dark=render(Fixture::Mode::Absent,false);
    const auto lit=render(Fixture::Mode::Absent,true);
    std::cout<<"local dark/lit near="<<fixture.patch(dark,-90,-654)<<'/'<<fixture.patch(lit,-90,-654)
             <<" far="<<fixture.patch(dark,500,-654)<<'/'<<fixture.patch(lit,500,-654)<<'\n';
    require(fixture.patch(lit,-90,-654)>fixture.patch(dark,-90,-654)+10,
            "Local point light did not illuminate a dark receiver");
    require(std::abs(fixture.patch(lit,500,-654)-fixture.patch(dark,500,-654))<=2,
            "Local point light escaped its radius");
    const auto shadow=render(Fixture::Mode::Opaque,true);
    require(fixture.patch(lit,-90,-654)-fixture.patch(shadow,-90,-654)>=8,
            "Opaque geometry did not block local-light visibility");
    require(std::abs(fixture.patch(shadow,220,-654)-fixture.patch(lit,220,-654))<=2,
            "Local-light shadow darkened an unoccluded receiver");
    settings.temporal_aa=true; renderer.set_graphics(settings);
    render(Fixture::Mode::Absent,true,0,true);
    const auto moved=render(Fixture::Mode::Absent,true,250,false);
    const auto fresh=render(Fixture::Mode::Absent,true,250,true);
    require(std::abs(fixture.patch(moved,160,-654)-fixture.patch(fresh,160,-654))<=3 &&
            std::abs(fixture.patch(moved,-90,-654)-fixture.patch(fresh,-90,-654))<=3,
            "Moving local light left stale illumination in temporal history");
    renderer.set_graphics(original); renderer.set_environment(environment_at(TimeOfDay::Day,0));
    std::cout<<"local point illumination/radius/shadow visibility/moving-light history PASS\n";
}

int main(int argc, char** argv) {
    bool force_off = false, require_rt = false, temporal_only = false, oblique_only = false, environment_only = false;
    bool shadow_only = false, overlay_only = false, ray_origin_only = false, lighting_noise_only = false;
    bool shadow_noise_only = false, reflection_noise_only = false, rare_reflection_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--rt-off") force_off = true;
        else if (std::string_view(argv[i]) == "--require-rt") require_rt = true;
        else if (std::string_view(argv[i]) == "--temporal-only") temporal_only = true;
        else if (std::string_view(argv[i]) == "--oblique-only") oblique_only = true;
        else if (std::string_view(argv[i]) == "--shadow-only") shadow_only = true;
        else if (std::string_view(argv[i]) == "--overlay-only") overlay_only = true;
        else if (std::string_view(argv[i]) == "--ray-origin-only") ray_origin_only = true;
        else if (std::string_view(argv[i]) == "--lighting-noise-only") lighting_noise_only = true;
        else if (std::string_view(argv[i]) == "--shadow-noise-only") shadow_noise_only = true;
        else if (std::string_view(argv[i]) == "--reflection-noise-only") reflection_noise_only = true;
        else if (std::string_view(argv[i]) == "--rare-reflection-only") rare_reflection_only = true;
        else if (std::string_view(argv[i]) == "--environment-only") environment_only = true;
        else { std::cerr << "Usage: check_vulkan [--rt-off | --require-rt] [--temporal-only | --oblique-only | --shadow-only | --overlay-only | --ray-origin-only | --lighting-noise-only | --shadow-noise-only | --reflection-noise-only | --rare-reflection-only]\n"; return 1; }
    }
    if (force_off && require_rt) { std::cerr << "Conflicting RT requirements\n"; return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    int result = 0;
    try {
        Fixture fixture;
        check_local_lights(fixture);
        check_day_night(fixture);
        if (environment_only) return 0;
        std::cout << fixture.renderer.backend_name() << " drawable=" << fixture.width << 'x' << fixture.height
                  << " ray-query-capable=" << fixture.renderer.ray_tracing_available() << '\n';
        require(!require_rt || fixture.renderer.ray_tracing_available(),
                "--require-rt requested but hardware ray queries are unavailable");
        if (rare_reflection_only) check_rare_reflection_history(fixture, force_off);
        else if (reflection_noise_only) check_reflection_noise(fixture, force_off);
        else if (shadow_noise_only) {
            check_static_shadow_noise(fixture, force_off);
            check_moving_shadow_history(fixture, force_off);
        }
        else if (lighting_noise_only) check_smooth_normal_lighting_noise(fixture, force_off);
        else if (ray_origin_only) check_ray_origin_translation(fixture, force_off);
        else if (overlay_only) check_untracked_overlay_history(fixture, force_off);
        else if (shadow_only) {
            check_mixed_shadow_coverage_history(fixture, force_off);
            check_moving_shadow_history(fixture, force_off);
        }
        else if (!oblique_only) {
            if (!temporal_only) {
                check_shadows(fixture, force_off, require_rt);
                check_texture_table_boundary(fixture, force_off);
                check_traced_artwork(fixture, force_off);
                check_upscale_sky_boundary(fixture);
                check_authored_artwork(fixture);
                check_msaa_metadata(fixture, force_off);
                check_ray_origin_translation(fixture, force_off);
                check_rare_reflection_history(fixture, force_off);
            }
            check_noisy_illumination(fixture, force_off);
            check_smooth_normal_lighting_noise(fixture, force_off);
            check_reflection_noise(fixture, force_off);
            check_static_shadow_noise(fixture, force_off);
            check_mixed_shadow_coverage_history(fixture, force_off);
            check_moving_shadow_history(fixture, force_off);
            check_fine_artwork(fixture, force_off);
            check_untracked_overlay_history(fixture, force_off);
            check_curved_actor_illumination(fixture, force_off);
        }
        if (!shadow_only && !overlay_only && !ray_origin_only && !lighting_noise_only && !shadow_noise_only && !reflection_noise_only && !rare_reflection_only)
            check_oblique_half_resolution_history(fixture, force_off);
    } catch (const std::exception& error) {
        std::cerr << "Vulkan behavior: " << error.what() << '\n'; result = 1;
    }
    SDL_Quit();
    return result;
}
