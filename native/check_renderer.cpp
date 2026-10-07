#include "renderer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace yami;
// Real geometry exercises depth reconstruction and ray hits, not shader text.
void check_world_effects(DisplayOptions options) {
    options.width = 384; options.height = 216;
    options.enhanced = false; // Start classic: enabling effects must work on this context.
    options.samples = 4;
    options.ambient_occlusion = options.reflections = options.bloom = options.sharpen = 0;
    const std::array<Vertex, 8> vertices{{
        {{-500,-80,-150},{0,0},{0,1,0}}, {{500,-80,-150},{0,0},{0,1,0}},
        {{500,-80,-1500},{0,0},{0,1,0}}, {{-500,-80,-1500},{0,0},{0,1,0}},
        {{-100,-80,-500},{0,0},{0,0,1}}, {{100,-80,-500},{0,0},{0,0,1}},
        {{100,250,-500},{0,0},{0,0,1}}, {{-100,250,-500},{0,0},{0,0,1}}
    }};
    const std::array<std::uint16_t, 6> floor_indices{{0,1,2,0,2,3}}, wall_indices{{4,5,6,4,6,7}};
    Renderer renderer(options);
    const auto floor = renderer.upload_mesh(vertices, floor_indices);
    const auto wall = renderer.upload_mesh(vertices, wall_indices);
    const int width = renderer.pixel_width(), height = renderer.pixel_height();
    const float logical_width = 768.f*width/height;
    const std::array<Vertex, 4> overlay_vertices{{
        {{logical_width-128,32,0},{0,0},{0,0,1}}, {{logical_width-32,32,0},{0,0},{0,0,1}},
        {{logical_width-32,128,0},{0,0},{0,0,1}}, {{logical_width-128,128,0},{0,0},{0,0,1}}
    }};
    const auto overlay = renderer.upload_mesh(overlay_vertices, floor_indices);
    Material material; material.cull = false;
    MaterialPass pass;
    DrawState state;
    state.diffuse_light = {.25f,.25f,.25f,1};
    const auto render = [&](bool lighting = false, bool draw_wall = true) {
        renderer.clear();
        renderer.camera(identity_matrix(), frustum_projection(.75f, float(width)/height, 1, 30000));
        state.lighting = lighting;
        state.color = {.35f,.35f,.35f,1};
        renderer.draw(floor, 0, material, pass, state);
        state.color = {.9f,.04f,.02f,1};
        if (draw_wall) renderer.draw(wall, 0, material, pass, state);
        renderer.finish_world();
        renderer.hud_camera();
        state.lighting = false;
        auto ui_pass = pass;
        ui_pass.depth_test = ui_pass.depth_write = false;
        state.color = {.125f,.75f,.25f,1};
        renderer.draw(overlay, 0, material, ui_pass, state);
        return renderer.capture_rgba();
    };
    const auto pixel = [&](const auto& frame, int x, int y, int channel) {
        return int(frame[(std::size_t(y)*width+x)*4+channel]);
    };
    const auto initial_classic = render();
    auto graphics = renderer.graphics_settings();
    for (const int samples : {4, 0, 4}) {
        graphics.samples = samples;
        graphics.enhanced = false;
        graphics.ambient_occlusion = graphics.reflections = 0;
        renderer.set_graphics(graphics);
        const auto baseline = render();
        assert(baseline.size() == std::size_t(width)*height*4);
        if (samples == 4) assert(baseline == initial_classic);
        graphics.enhanced = true;
        renderer.set_graphics(graphics);
        assert(render() == baseline); // Unlit geometry is unchanged with zero effects.
        for (int effect = 0; effect < 2; ++effect) {
            graphics.ambient_occlusion = effect == 0 ? 1.f : 0.f;
            graphics.reflections = effect == 1 ? 1.f : 0.f;
            renderer.set_graphics(graphics);
            const auto enhanced = render();
            const int hud_x = width-int(80.f*height/768), hud_y = int(80.f*height/768);
            assert(std::abs(pixel(enhanced, hud_x, hud_y, 0)-32) <= 1);
            assert(std::abs(pixel(enhanced, hud_x, hud_y, 1)-191) <= 1);
            assert(std::abs(pixel(enhanced, hud_x, hud_y, 2)-64) <= 1);
            for (int channel = 0; channel < 4; ++channel)
                assert(pixel(enhanced, 1, height-2, channel) == pixel(baseline, 1, height-2, channel));
            int response = 0;
            const float corner = .5f+vertices[4].position.y/(-vertices[4].position.z*.75f)*.5f;
            // Occlusion belongs to both surfaces meeting at the projected corner.
            const float y_begin = effect == 0 ? corner-.04f : .21f;
            const float y_end = effect == 0 ? corner+.04f : .30f;
            for (int y = int(height*y_begin); y < int(std::ceil(height*y_end)); ++y)
                for (int x = width*47/100; x < width*53/100; ++x) {
                    const int delta = effect == 0 ? pixel(baseline,x,y,0)-pixel(enhanced,x,y,0)
                                                 : pixel(enhanced,x,y,0)-pixel(baseline,x,y,0);
                    response = std::max(response, delta);
                }
            if (response < (effect == 0 ? 3 : 1))
                throw std::runtime_error(effect == 0 ? "Live AO did not darken floor/wall junction"
                                                    : "Live rough reflection did not return a real wall hit");
        }
        // Unknown legacy materials stay rough, not mirrors, even at maximum
        // strength. Real hits still respond to the slider; misses stay untouched.
        graphics.ambient_occlusion = 0;
        graphics.reflections = 1;
        renderer.set_graphics(graphics);
        const auto pavement = render();
        for (std::size_t i = 0; i < pavement.size(); ++i)
            assert(std::abs(int(pavement[i])-int(baseline[i])) <= (i%4 == 3 ? 0 : 6));
        graphics.reflections = .25f;
        renderer.set_graphics(graphics);
        const auto weaker = render();
        int pavement_response = 0, full_energy = 0, weaker_energy = 0;
        for (int y = int(height*.21f); y < int(std::ceil(height*.30f)); ++y)
            for (int x = width*47/100; x < width*53/100; ++x) {
                const int response = pixel(pavement,x,y,0)-pixel(baseline,x,y,0);
                pavement_response = std::max(pavement_response, response);
                full_energy += response;
                weaker_energy += pixel(weaker,x,y,0)-pixel(baseline,x,y,0);
            }
        assert(pavement_response > 0 && pavement_response <= 4);
        assert(full_energy > weaker_energy && weaker_energy >= 0);
        graphics.reflections = 0;
        renderer.set_graphics(graphics);
        const auto miss_baseline = render(false, false);
        graphics.reflections = 1;
        renderer.set_graphics(graphics);
        assert(render(false, false) == miss_baseline); // No wall: no fabricated environment/ghost.
        graphics.enhanced = false; // Nonzero strengths must not leak into classic mode.
        renderer.set_graphics(graphics);
        assert(render() == baseline);
        graphics.ambient_occlusion = graphics.reflections = 0;
        renderer.set_graphics(graphics);
        const auto classic_lighting = render(true);
        graphics.enhanced = true;
        renderer.set_graphics(graphics);
        assert(render(true) != classic_lighting);
        graphics.enhanced = false;
        renderer.set_graphics(graphics);
        assert(render(true) == classic_lighting);
    }
    renderer.release_mesh(overlay);
    renderer.release_mesh(wall);
    renderer.release_mesh(floor);
}

// Keep caster color out of this fixture: every changed pixel must be a real
// depth-map footprint on the raised, sloping receiver, not the actor itself.
void check_actor_shadows(DisplayOptions options) {
    options.width = 384; options.height = 216; options.enhanced = true;
    options.ambient_occlusion = options.reflections = options.bloom = options.sharpen = 0;
    options.shadows = 0;
    Renderer renderer(options);
    const int width = renderer.pixel_width(), height = renderer.pixel_height();
    const std::array<std::uint16_t, 6> indices{{0,1,2,0,2,3}};
    const std::array<Vertex, 4> receiver_vertices{{
        {{-900,26,-100},{0,0},{0,1,.12f}}, {{900,26,-100},{1,0},{0,1,.12f}},
        {{900,170,-1300},{1,1},{0,1,.12f}}, {{-900,170,-1300},{0,1},{0,1,.12f}}
    }};
    const std::array<Vertex, 4> rest_vertices{{
        {{-65,0,95},{0,0},{0,1,0}}, {{65,0,95},{1,0},{0,1,0}},
        {{65,0,-95},{1,1},{0,1,0}}, {{-65,0,-95},{0,1},{0,1,0}}
    }};
    const auto receiver = renderer.upload_mesh(receiver_vertices, indices);
    const auto caster = renderer.upload_mesh(rest_vertices, indices);
    const std::array<Vertex, 4> cloud_vertices{{
        {{-2000,-2000,-180},{0,0},{0,0,1}}, {{2000,-2000,-180},{1,0},{0,0,1}},
        {{2000,2000,-180},{1,1},{0,0,1}}, {{-2000,2000,-180},{0,1},{0,0,1}}
    }};
    const auto cloud = renderer.upload_mesh(cloud_vertices, indices);
    const float logical_width = 768.f*width/height;
    const std::array<Vertex, 4> hud_vertices{{
        {{logical_width-128,32,0},{0,0},{0,0,1}}, {{logical_width-32,32,0},{1,0},{0,0,1}},
        {{logical_width-32,128,0},{1,1},{0,0,1}}, {{logical_width-128,128,0},{0,1},{0,0,1}}
    }};
    const auto hud = renderer.upload_mesh(hud_vertices, indices);
    const std::array<std::uint8_t, 16> cutout_texels{{
        255,255,255,255, 255,255,255,0, 255,255,255,0, 255,255,255,0
    }};
    const auto flipped_texture = renderer.upload_texture(2, 2, cutout_texels, false);
    const auto raw_texture = renderer.upload_texture(2, 2, cutout_texels, false, false);
    Material material; material.cull = false;
    MaterialPass caster_pass, receiver_pass;
    DrawState caster_state, receiver_state;
    caster_state.model = pose_matrix({}, {-110,220,-600});
    receiver_state.lighting = true; receiver_state.diffuse_light = {.35f,.35f,.35f,1};
    ShadowBounds receiver_bounds, local_bounds;
    for (const auto& vertex : receiver_vertices) receiver_bounds.include(vertex.position);
    for (const auto& vertex : rest_vertices) local_bounds.include(vertex.position);
    int instances = 1;
    unsigned texture = 0;
    bool cloudy = false, cloud_fog = false, submit_casters = true;
    auto view = identity_matrix(); view.values[13] = -300;
    const auto projection = frustum_projection(.75f, float(width)/height, 1, 30000);
    const auto render = [&] {
        renderer.clear();
        renderer.camera(view, projection);
        ShadowBounds bounds;
        auto second = caster_state;
        second.model.values[12] += 280;
        if (instances > 0) bounds.include(local_bounds.transformed(caster_state.model));
        if (instances > 1) bounds.include(local_bounds.transformed(second.model));
        if (renderer.shadow_enabled() && bounds.valid)
            assert(renderer.shadow_caster_visible(bounds));
        if (renderer.begin_shadows(bounds, receiver_bounds)) {
            if (submit_casters && instances > 0)
                renderer.draw(caster, texture, material, caster_pass, caster_state);
            if (submit_casters && instances > 1)
                renderer.draw(caster, texture, material, caster_pass, second);
            renderer.end_shadows();
        }
        renderer.draw(receiver, 0, material, receiver_pass, receiver_state);
        DrawState overlay;
        overlay.ray_geometry = overlay.ray_primary = false;
        MaterialPass overlay_pass;
        overlay_pass.depth_test = overlay_pass.depth_write = false;
        if (cloudy) {
            overlay.color = {1,1,1,.45f};
            overlay.fog = cloud_fog;
            if (cloud_fog) overlay.model.values[14] = -1320; // Fog depth 1500, not the HUD.
            overlay_pass.blend = true;
            renderer.draw(cloud, 0, material, overlay_pass, overlay);
        }
        renderer.finish_world();
        renderer.hud_camera();
        overlay.color = {.125f,.75f,.25f,1}; overlay_pass.blend = false;
        overlay.fog = false; overlay.model = identity_matrix();
        renderer.draw(hud, 0, material, overlay_pass, overlay);
        return renderer.capture_rgba();
    };
    const auto red = [&](const auto& frame, int x, int y) {
        return int(frame[(std::size_t(y)*width+x)*4]);
    };
    struct Footprint { int count = 0, energy = 0; double x = 0, y = 0; };
    const auto footprint = [&](const auto& off, const auto& on) {
        Footprint result;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int delta = red(off,x,y)-red(on,x,y);
            if (delta > 3) {
                assert(red(off,x,y) > 0); // Never darken cleared background.
                ++result.count; result.energy += delta;
                result.x += x; result.y += y;
            }
        }
        if (result.count) { result.x /= result.count; result.y /= result.count; }
        return result;
    };
    auto graphics = renderer.graphics_settings();
    for (const int samples : {4,0,4}) {
        graphics.samples = samples; graphics.enhanced = true;
        graphics.shadows = 0; graphics.ray_tracing = false;
        renderer.set_graphics(graphics);
        assert(!renderer.shadow_enabled());
        const auto baseline = render();
        graphics.shadows = 1;
        renderer.set_graphics(graphics);
        assert(renderer.shadow_enabled());
        const auto full = render();
        const auto shaped = footprint(baseline, full);
        assert(shaped.count > 12 && shaped.count < width*height/8);
        assert(shaped.energy > shaped.count*8);
        for (std::size_t pixel = 0; pixel < baseline.size(); pixel += 4) {
            assert(full[pixel+3] == baseline[pixel+3]);
            if (baseline[pixel] == 0 && baseline[pixel+1] == 0 && baseline[pixel+2] == 0)
                for (int channel = 0; channel < 4; ++channel)
                    assert(full[pixel+channel] == baseline[pixel+channel]);
        }
        // Exact unshadowed receiver pixels and HUD/background survive depth pass.
        assert(red(full,1,height-2) == red(baseline,1,height-2));
        const int hud_x = width-int(80.f*height/768), hud_y = int(80.f*height/768);
        for (int channel = 0; channel < 4; ++channel)
            assert(full[(std::size_t(hud_y)*width+hud_x)*4+channel] ==
                   baseline[(std::size_t(hud_y)*width+hud_x)*4+channel]);
        graphics.ray_tracing = true;
        renderer.set_graphics(graphics);
        assert(render() == full); // GL shadows do not depend on Vulkan RT preference.
        graphics.shadows = .25f;
        renderer.set_graphics(graphics);
        const auto weak = footprint(baseline, render());
        assert(weak.energy > 0 && weak.energy < shaped.energy);
        graphics.shadows = 1;
        renderer.set_graphics(graphics);
        caster_state.model.values[12] += 140;
        const auto translated = footprint(baseline, render());
        assert(translated.count > 12 && translated.x > shaped.x+10);
        caster_state.model = pose_matrix({0,.38268343f,0,.92387953f}, {-110,220,-600});
        const auto rotated = render();
        assert(footprint(full,rotated).count+footprint(rotated,full).count > 12);
        caster_state.model = pose_matrix({}, {-110,220,-600});
        auto posed = rest_vertices;
        const auto bone_pose = pose_matrix({0,.25881905f,0,.96592583f}, {90,0,0});
        local_bounds = {};
        for (auto& vertex : posed) {
            vertex.position = transform_point(bone_pose,vertex.position);
            vertex.normal = transform_direction(bone_pose,vertex.normal);
            local_bounds.include(vertex.position);
        }
        renderer.update_mesh(caster, posed); // Same uploaded CPU-skinned stream as color draw.
        const auto animated = footprint(baseline, render());
        assert(animated.count > 12 && animated.x > shaped.x+5);
        renderer.update_mesh(caster, rest_vertices);
        local_bounds = {};
        for (const auto& vertex : rest_vertices) local_bounds.include(vertex.position);
        instances = 2;
        const auto doubled = footprint(baseline, render());
        assert(doubled.count > shaped.count*1.5 && doubled.x > shaped.x+10);
        instances = 1;
        // Every fixed-function alpha predicate is also obeyed by the depth shader.
        caster_pass.alpha_test = true; caster_pass.alpha_reference = .5f;
        for (unsigned predicate = 0x200; predicate <= 0x207; ++predicate) {
            caster_pass.alpha_function = predicate;
            const auto alpha = render();
            assert(alpha == (predicate >= 0x204 ? full : baseline));
        }
        caster_pass.alpha_function = 0x204;
        texture = flipped_texture;
        const auto cutout = render();
        const auto cut = footprint(baseline, cutout);
        assert(cut.count > 3 && cut.count < shaped.count*.8);
        texture = raw_texture;
        const auto unflipped = render();
        assert(footprint(cutout,unflipped).count+footprint(unflipped,cutout).count > 3);
        texture = flipped_texture;
        caster_state.uv_transform = {1,-1,0,1};
        const auto uv_flipped = render();
        assert(footprint(uv_flipped,unflipped).count+footprint(unflipped,uv_flipped).count <=
               std::max(2,cut.count/10)); // Equivalent UVs tolerate alpha-edge interpolation rounding.
        caster_state.uv_transform = {1,1,0,0};
        caster_state.color[3] = 0;
        assert(render() == baseline); // Material tint alpha participates in caster rejection.
        caster_state.color[3] = 1;
        texture = 0; caster_pass.alpha_test = false;
        for (int excluded = 0; excluded < 5; ++excluded) {
            caster_state.ray_geometry = excluded != 0;
            caster_state.ray_primary = excluded != 1;
            caster_pass.blend = excluded == 2;
            caster_pass.video = excluded == 3;
            caster_pass.depth_write = excluded != 4;
            assert(render() == baseline);
        }
        caster_state.ray_geometry = caster_state.ray_primary = true;
        caster_pass = {};
        // Rejected receivers must not get darkened by an otherwise valid map.
        for (int excluded = 0; excluded < 6; ++excluded) {
            receiver_state.ray_geometry = excluded != 0;
            receiver_state.ray_primary = excluded != 1;
            receiver_pass.blend = excluded == 2;
            receiver_pass.video = excluded == 3;
            receiver_pass.depth_write = excluded != 4;
            receiver_state.lighting = excluded != 5;
            graphics.shadows = 0; renderer.set_graphics(graphics);
            const auto excluded_baseline = render();
            graphics.shadows = 1; renderer.set_graphics(graphics);
            assert(render() == excluded_baseline);
        }
        receiver_state.ray_geometry = receiver_state.ray_primary = receiver_state.lighting = true;
        receiver_pass = {};
        assert(render() == full);
        submit_casters = false;
        assert(render() == baseline); // An empty submitted map cannot retain yesterday's actors.
        submit_casters = true; instances = 0;
        assert(render() == baseline); // Invalid/empty bounds also clear stale footprints.
        instances = 1; cloudy = true;
        for (bool fog : {false,true}) {
            cloud_fog = fog;
            const auto clouds = render();
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                if (red(baseline,x,y)-red(full,x,y) > 8)
                    assert(std::abs(red(clouds,x,y)-(.55f*red(full,x,y)+.45f*(fog ? 153 : 255))) <= 2);
        }
        cloudy = cloud_fog = false;
        graphics.shadows = 0; renderer.set_graphics(graphics);
        assert(render() == baseline);
        graphics.enhanced = false; renderer.set_graphics(graphics);
        const auto classic = render();
        graphics.shadows = 1; renderer.set_graphics(graphics);
        assert(!renderer.shadow_enabled() && render() == classic);
    }
    renderer.release_mesh(hud); renderer.release_mesh(cloud);
    renderer.release_mesh(caster); renderer.release_mesh(receiver);
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    try {
        DisplayOptions options;
        options.width = 192; options.height = 96;
        options.enhanced = false; // Keep the exact original gamma/orientation/alpha path.
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--gles") options.gles = true;
            if (std::string_view(argv[i]) == "--no-msaa") options.samples = 0;
            if (std::string_view(argv[i]) == "--vulkan") options.backend = GraphicsBackend::Vulkan;
        }
        {
            Renderer renderer(options);
            const std::array<Vertex,4> vertices{{
                {{-1,1,0},{0,1},{0,0,1}}, {{1,1,0},{1,1},{0,0,1}},
                {{1,-1,0},{1,0},{0,0,1}}, {{-1,-1,0},{0,0},{0,0,1}}
            }};
            const std::array<std::uint16_t,6> indices{{0,1,2,0,2,3}};
            const auto mesh = renderer.upload_mesh(vertices, indices);
            Material material;
            MaterialPass pass;
            DrawState state;
            auto render = [&](unsigned texture) {
                renderer.clear();
                renderer.camera(identity_matrix(), identity_matrix());
                renderer.draw(mesh, texture, material, pass, state);
                // A final UI scissor must not crop the MSAA resolve of the 3D scene.
                renderer.camera(identity_matrix(), interface_projection(), true);
                return renderer.capture_rgba();
            };
            auto pixel = [&](const auto& frame, int x, int y, int channel) {
                return int(frame[(std::size_t(y)*renderer.pixel_width()+x)*4+channel]);
            };
            state.color = {.5f,.5f,.5f,1};
            auto frame = render(0);
            assert(pixel(frame, 1, 1, 0) == 128);
            renderer.set_brightness(0);
            frame = render(0);
            assert(std::abs(pixel(frame, 1, 1, 0)-64) <= 1);
            renderer.set_brightness(65535);
            frame = render(0);
            assert(pixel(frame, 1, 1, 0) == 255 && pixel(frame, 1, 1, 3) == 255);
            renderer.set_brightness(129); // 257 gives the identity 16-bit gamma ramp.
            state.color = {1,1,1,1};
            const std::array<std::uint8_t,16> texels{{
                255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255
            }}; // canonical top-left rows
            const auto texture = renderer.upload_texture(2, 2, texels, false);
            frame = render(texture);
            const int width = renderer.pixel_width(), height = renderer.pixel_height();
            assert(pixel(frame, width/4, 3*height/4, 0) > 245);
            assert(pixel(frame, width/4, 3*height/4, 2) < 10);
            assert(pixel(frame, width/4, height/4, 2) > 245);
            assert(pixel(frame, width/4, height/4, 0) < 10);
            // The media path streams top-left RGBA rows and updates existing GPU
            // textures, including subregions, rather than replacing handles.
            const std::array<std::uint8_t, 4> yellow{{255,255,0,255}};
            renderer.update_texture(texture, 1, 1, yellow, 0, 0);
            frame = render(texture);
            assert(pixel(frame, width/4, 3*height/4, 0) > 245);
            assert(pixel(frame, width/4, 3*height/4, 1) > 245);
            assert(pixel(frame, width/4, height/4, 2) > 245);
            renderer.update_texture(texture, 2, 2, texels);
            assert(render(texture) != frame);
            const auto video_texture = renderer.upload_texture(2, 2, texels, false, false);
            frame = render(video_texture);
            assert(pixel(frame, width/4, 3*height/4, 2) > 245);
            assert(pixel(frame, width/4, height/4, 0) > 245);
            auto moved = vertices;
            for (auto& vertex : moved) vertex.position.x += 1;
            renderer.update_mesh(mesh, moved);
            frame = render(0);
            assert(pixel(frame, width/4, height/2, 0) == 0);
            assert(pixel(frame, 3*width/4, height/2, 0) == 255);
            renderer.update_mesh(mesh, vertices);
            // Rejected transparent fragments must leave the cleared framebuffer black.
            state.color = {1,1,1,0}; pass.alpha_test = true;
            pass.alpha_function = 0x204; pass.alpha_reference = .5f;
            frame = render(0);
            assert(pixel(frame, width/2, height/2, 0) == 0);
            renderer.present();
            renderer.release_mesh(mesh);
            std::cout << renderer.backend_name() << " renderer: " << width << 'x' << height
                      << " MSAA=" << renderer.samples() << " gamma/orientation/alpha/resolve/video/update PASS\n";
        }
        if (options.backend == GraphicsBackend::OpenGL) {
            check_world_effects(options);
            std::cout << "live classic/enhanced AO/rough-reflection/hit-miss/strength MSAA4/0/4 widescreen HUD PASS\n";
            check_actor_shadows(options);
            std::cout << "directional actor shadows/pose/instances/cutout/exclusions/cloud-alpha/live MSAA4/0/4 HUD PASS\n";
        }
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "renderer: " << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
