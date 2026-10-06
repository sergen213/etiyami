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
    const auto render = [&](bool lighting = false) {
        renderer.clear();
        renderer.camera(identity_matrix(), frustum_projection(.75f, float(width)/height, 1, 30000));
        state.lighting = lighting;
        state.color = {.35f,.35f,.35f,1};
        renderer.draw(floor, 0, material, pass, state);
        state.color = {.9f,.04f,.02f,1};
        renderer.draw(wall, 0, material, pass, state);
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
            if (response < 3)
                throw std::runtime_error(effect == 0 ? "Live AO did not darken floor/wall junction"
                                                    : "Live reflection did not return red wall onto floor");
        }
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

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr << SDL_GetError() << '\n'; return 1; }
    try {
        DisplayOptions options;
        options.width = 192; options.height = 96;
        options.enhanced = false; // Keep the exact original gamma/orientation/alpha path.
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--gles") options.gles = true;
            if (std::string_view(argv[i]) == "--no-msaa") options.samples = 0;
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
            // Rejected transparent fragments must leave the cleared framebuffer black.
            state.color = {1,1,1,0}; pass.alpha_test = true;
            pass.alpha_function = 0x204; pass.alpha_reference = .5f;
            frame = render(0);
            assert(pixel(frame, width/2, height/2, 0) == 0);
            renderer.present();
            renderer.release_mesh(mesh);
            std::cout << (options.gles ? "GLES" : "GL") << " renderer: " << width << 'x' << height
                      << " MSAA=" << renderer.samples() << " gamma/orientation/alpha/resolve PASS\n";
        }
        check_world_effects(options);
        std::cout << "live classic/enhanced AO/reflection MSAA4/0/4 widescreen HUD PASS\n";
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "renderer: " << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
