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
void check_temporal(Fixture& fixture) {
    auto graphics = fixture.renderer.graphics_settings();
    graphics.ray_tracing = false; graphics.shadows = 0; graphics.temporal_aa = false;
    fixture.renderer.set_graphics(graphics);
    const auto reference = fixture.frame(Fixture::Mode::Absent);
    const auto aliased = fixture.frame(Fixture::Mode::Opaque, 0, 0, true);
    graphics.temporal_aa = true;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    const auto first = fixture.frame(Fixture::Mode::Opaque, 0, 0, true);
    auto accumulated = first;
    for (int i = 0; i < 15; ++i) accumulated = fixture.frame(Fixture::Mode::Opaque, 0, 0, true);
    bool filtered_edge = false;
    const int foreground = aliased[(std::size_t(fixture.height*61/100)*fixture.width+fixture.width/2)*4];
    require(foreground > 8, "Static temporal fixture did not draw its blocker");
    // Restrict to the raised blocker's silhouette, away from floor and HUD.
    for (int yy = fixture.height*59/100; yy < fixture.height*65/100; ++yy)
        for (int xx = fixture.width*39/100; xx < fixture.width*61/100; ++xx) {
            const auto at = (std::size_t(yy)*fixture.width+xx)*4;
            if (accumulated[at] > 3 && accumulated[at] < foreground-3)
                filtered_edge = true;
        }
    require(filtered_edge && accumulated != first,
            "Temporal accumulation did not soften a static geometric silhouette");
    fixture.renderer.reset_history();
    for (int i = 0; i < 8; ++i) fixture.frame(Fixture::Mode::Opaque);
    const auto removed = fixture.frame(Fixture::Mode::Absent);
    // The previous occluder is above the floor in screen space. Its old silhouette
    // must become black immediately, not remain a temporal ghost.
    const int x = fixture.width/2;
    const int y = int(fixture.height*(.5f+100.f/(600.f*.75f)*.5f));
    for (int channel = 0; channel < 3; ++channel) {
        const auto at = (std::size_t(y)*fixture.width+x)*4+channel;
        require(reference[at] == 0 && removed[at] <= 2, "Removed dynamic silhouette left a temporal ghost");
    }
    for (int i = 0; i < 8; ++i) fixture.frame(Fixture::Mode::Opaque, 0, 0, true);
    const auto camera_move = fixture.frame(Fixture::Mode::Opaque, 0, 300, true);
    graphics.temporal_aa = false;
    fixture.renderer.set_graphics(graphics);
    const auto moved_reference = fixture.frame(Fixture::Mode::Opaque, 0, 300);
    for (int channel = 0; channel < 3; ++channel) {
        const auto at = (std::size_t(y)*fixture.width+x)*4+channel;
        require(moved_reference[at] == 0 && camera_move[at] <= 2, "Camera disocclusion retained old silhouette history");
    }
    graphics.temporal_aa = true; graphics.render_scale = .5f;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    const auto scaled = fixture.frame(Fixture::Mode::Absent);
    require(std::abs(fixture.patch(scaled, -90, -654)-fixture.patch(reference, -90, -654)) <= 3,
            "Temporal upscaling changed the interior of a flat receiver");
    std::cout << "temporal static-edge accumulation/dynamic/camera disocclusion/idempotent capture/render-scale/native HUD PASS\n";
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
    const auto reference = draw(false);
    compare(draw(true), reference, 2, "Native RT/TAA-off fine coplanar artwork was spatially blurred");
    if (!force_off && fixture.renderer.ray_tracing_available()) {
        graphics.ray_tracing = true; graphics.ambient_occlusion = 1;
        fixture.renderer.set_graphics(graphics);
        // The hemisphere leaves this plane: real ray queries are active, but
        // unobscured deterministic artwork must not become denoiser input.
        compare(draw(true), reference, 2, "RT-enabled fine coplanar artwork was spatially blurred");
    }
    graphics.ray_tracing = false; graphics.ambient_occlusion = 0; graphics.temporal_aa = true;
    fixture.renderer.set_graphics(graphics);
    fixture.renderer.reset_history();
    for (int frame = 0; frame < 12; ++frame) draw(true, float(frame)*.0005f);
    // A small continuous camera move never trips the >20-unit history reset.
    // The final stationary frame is exactly eight frames later: same camera,
    // same jitter phase, different retained history. Artwork must agree.
    const auto moving = draw(true, .006f);
    fixture.renderer.reset_history();
    auto fresh = moving;
    for (int frame = 0; frame < 8; ++frame) fresh = draw(true, .006f);
    compare(moving, fresh, 2, "Continuous camera reprojection retained adjacent fine artwork");
    fixture.renderer.release_mesh(quad);
    fixture.renderer.set_graphics(saved);
    std::cout << "fine coplanar artwork/native RT-off/RT-on/continuous camera PASS\n";
}
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
void check_moving_shadow_history(Fixture& fixture, bool force_off) {
    if (force_off || !fixture.renderer.ray_tracing_available()) return;
    const auto saved = fixture.renderer.graphics_settings();
    auto graphics = saved;
    graphics.render_scale = 1; graphics.samples = 0; graphics.ray_tracing = true;
    graphics.ambient_occlusion = graphics.reflections = graphics.indirect_lighting = 0;
    graphics.shadows = 1; graphics.bloom = graphics.sharpen = 0; graphics.exposure = 1;
    for (bool taa : {false, true}) {
        graphics.temporal_aa = taa; fixture.renderer.set_graphics(graphics);
        fixture.renderer.reset_history();
        const auto lit = fixture.frame(Fixture::Mode::Absent);
        for (int frame = 0; frame < 12; ++frame) fixture.frame(Fixture::Mode::Opaque);
        const auto moved = fixture.frame(Fixture::Mode::Opaque, 250);
        fixture.renderer.reset_history();
        const auto fresh = fixture.frame(Fixture::Mode::Opaque, 250);
        require(std::abs(fixture.patch(moved, -90, -654)-fixture.patch(lit, -90, -654)) <= 3,
                "Stationary receiver retained a moving blocker's old shadow");
        require(std::abs(fixture.patch(moved, 160, -654)-fixture.patch(fresh, 160, -654)) <= 3,
                "Stationary receiver delayed a moving blocker's new shadow");
    }
    fixture.renderer.set_graphics(saved);
    std::cout << "stationary receiver/moving blocker/RT short history/TAA history PASS\n";
}
} // namespace
int main(int argc, char** argv) {
    bool force_off = false, require_rt = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--rt-off") force_off = true;
        else if (std::string_view(argv[i]) == "--require-rt") require_rt = true;
        else { std::cerr << "Usage: check_vulkan [--rt-off | --require-rt]\n"; return 1; }
    }
    if (force_off && require_rt) { std::cerr << "Conflicting RT requirements\n"; return 1; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    int result = 0;
    try {
        Fixture fixture;
        std::cout << fixture.renderer.backend_name() << " drawable=" << fixture.width << 'x' << fixture.height
                  << " ray-query-capable=" << fixture.renderer.ray_tracing_available() << '\n';
        check_shadows(fixture, force_off, require_rt);
        check_texture_table_boundary(fixture, force_off);
        check_traced_artwork(fixture, force_off);
        check_temporal(fixture);
        check_upscale_sky_boundary(fixture);
        check_authored_artwork(fixture);
        check_noisy_illumination(fixture, force_off);
        check_msaa_metadata(fixture, force_off);
        check_moving_shadow_history(fixture, force_off);
        check_fine_artwork(fixture, force_off);
    } catch (const std::exception& error) {
        std::cerr << "Vulkan behavior: " << error.what() << '\n'; result = 1;
    }
    SDL_Quit();
    return result;
}
