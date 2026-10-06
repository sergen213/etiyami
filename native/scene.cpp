#include "scene.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace yami {
namespace {
constexpr std::array<std::uint16_t, 6> quad_indices{{0, 1, 2, 0, 2, 3}};
constexpr std::array<Vertex, 4> quad_vertices{{
    {{0,1,0},{0,1},{0,0,1}}, {{1,1,0},{1,1},{0,0,1}},
    {{1,0,0},{1,0},{0,0,1}}, {{0,0,0},{0,0},{0,0,1}}
}};
std::string model_key(const std::filesystem::path& path) { return path.generic_string(); }
}
ModelInstance::ModelInstance(Renderer& renderer, const ModelResource& model, bool animated)
    : renderer_(renderer), model_(model) {
    parts.reserve(model.parts.size());
    try {
        for (const auto& source : model.parts) {
            parts.emplace_back();
            auto& part = parts.back();
            if (animated && !source.mesh->mesh.bones.empty()) {
                prepare_skinning(source.mesh->mesh, part.skin);
                part.gpu = renderer_.upload_mesh(part.skin.vertices, source.mesh->mesh.indices);
                part.owned = true;
            } else {
                if (!source.mesh->static_gpu)
                    source.mesh->static_gpu = renderer_.upload_mesh(source.mesh->mesh.vertices, source.mesh->mesh.indices);
                part.gpu = source.mesh->static_gpu;
            }
        }
    } catch (...) {
        for (const auto& part : parts) if (part.owned) renderer_.release_mesh(part.gpu);
        throw;
    }
}
ModelInstance::~ModelInstance() {
    for (const auto& part : parts) if (part.owned) renderer_.release_mesh(part.gpu);
}
SceneCache::SceneCache(Renderer& renderer, std::filesystem::path root)
    : renderer_(renderer), root_(std::move(root)) {
    quad_ = renderer_.upload_mesh(quad_vertices, quad_indices);
}
std::filesystem::path SceneCache::resolve(std::string_view name) {
    return resolve_asset(root_, std::string(name), &directories_);
}
MeshResource& SceneCache::mesh(const std::filesystem::path& path) {
    const auto key = model_key(path);
    if (const auto found = meshes_.find(key); found != meshes_.end()) return *found->second;
    auto resource = std::make_unique<MeshResource>();
    resource->mesh = read_mesh(path);
    resource->animations.reserve(resource->mesh.animation_names.size());
    for (const auto& name : resource->mesh.animation_names)
        resource->animations.push_back(read_animation(animation_path(path, name, &directories_), resource->mesh));
    return *meshes_.emplace(key, std::move(resource)).first->second;
}
ModelResource& SceneCache::model(std::string_view descriptor) {
    if (const auto found = models_.find(descriptor); found != models_.end()) return *found->second;
    const auto path = resolve(descriptor);
    auto resource = std::make_unique<ModelResource>();
    resource->model = read_model(path);
    resource->parts.reserve(resource->model.parts.size());
    for (const auto& part : resource->model.parts)
        resource->parts.push_back({&mesh(mesh_path(path, part, &directories_)), &material(part.material_name)});
    return *models_.emplace(std::string(descriptor), std::move(resource)).first->second;
}
MaterialResource& SceneCache::material(std::string_view name) {
    if (const auto found = materials_.find(name); found != materials_.end()) return *found->second;
    auto resource = std::make_unique<MaterialResource>();
    resource->material = read_material(resolve("data/materials/"+std::string(name)+".dat"));
    resource->textures.resize(resource->material.passes.size());
    return *materials_.emplace(std::string(name), std::move(resource)).first->second;
}
unsigned SceneCache::image(std::string_view name) {
    if (const auto found = images_.find(name); found != images_.end()) return found->second;
    std::filesystem::path path;
    try { path = resolve(name); }
    catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory && error.code() != std::errc::is_a_directory)
            throw;
        // 00417c30's real supported fopen-failure result: opaque 2x2 white RGB.
        if (!white_) {
            constexpr std::array<std::uint8_t, 16> white{{255,255,255,255,255,255,255,255,
                                                        255,255,255,255,255,255,255,255}};
            white_ = renderer_.upload_texture(2, 2, white);
        }
        std::cerr << "Original white-image behavior: " << name << '\n';
        images_.emplace(std::string(name), white_);
        return white_;
    }
    bool flip_vertical = true;
    auto extension = path.extension().string();
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = char(c+'a'-'A');
    if (extension == ".tga") {
        std::array<std::uint8_t, 18> header{};
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(header.data()), header.size()))
            throw std::runtime_error("Truncated TGA header: "+path.string());
        // 00417a30 ignores origin bit. The one original top-first TGA is therefore
        // inverted relative to FFmpeg's canonical top-left image; retain that fact.
        flip_vertical = (header[17]&0x20) == 0;
    }
    VideoDecoder decoder(path);
    VideoFrame frame;
    if (!decoder.next(frame)) throw std::runtime_error("Image contains no frame: "+path.string());
    const auto texture = renderer_.upload_texture(frame.width, frame.height, frame.rgba, true, flip_vertical);
    images_.emplace(std::string(name), texture);
    return texture;
}
bool SceneCache::video_texture(std::string_view name, std::uint32_t time, bool loop, unsigned& texture) {
    auto found = videos_.find(name);
    if (found == videos_.end()) {
        Video video;
        video.decoder = std::make_unique<VideoDecoder>(resolve(name));
        const int width = video.decoder->width(), height = video.decoder->height();
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
            video.decoder->frame_count() <= 0 || !(video.decoder->fps() > 0))
            throw std::runtime_error("AVI lacks valid dimensions/frame-count/rate: "+std::string(name));
        // Original 004087d0 centers the DIB in a power-of-two black canvas.
        const int canvas_width = static_cast<int>(std::bit_ceil(static_cast<unsigned>(width)));
        const int canvas_height = static_cast<int>(std::bit_ceil(static_cast<unsigned>(height)));
        video.padding_x = (canvas_width-width)/2;
        video.padding_y = (canvas_height-height)/2;
        std::vector<std::uint8_t> black(static_cast<std::size_t>(canvas_width)*canvas_height*4);
        for (std::size_t i = 3; i < black.size(); i += 4) black[i] = 255;
        video.texture = renderer_.upload_texture(canvas_width, canvas_height, black, false);
        found = videos_.emplace(std::string(name), std::move(video)).first;
    }
    auto& video = found->second;
    texture = video.texture;
    if (!video.active) { video.started = time; video.active = true; }
    const auto elapsed = loop ? static_cast<std::uint32_t>(time-video.started) : time;
    // AVIStreamSampleToTime(length)/length at00408876 is an integer ms divisor.
    const auto duration_ms = static_cast<std::uint64_t>(std::llround(video.decoder->duration()*1000));
    const auto count = video.decoder->frame_count();
    const auto frame_ms = duration_ms/static_cast<std::uint64_t>(count);
    if (!frame_ms) throw std::runtime_error("Invalid original AVI frame duration: "+std::string(name));
    std::int64_t target = elapsed/frame_ms;
    if (target >= count) {
        if (!loop) return false;
        // Original resets its baseline on wrap rather than retaining overshoot.
        video.started = time;
        target = 0;
    }
    if (target < video.frame) { video.decoder->seek(0); video.frame = -1; }
    VideoFrame frame;
    while (video.frame < target) {
        if (!video.decoder->next(frame)) throw std::runtime_error("AVI ends before its declared frame count: "+std::string(name));
        ++video.frame;
    }
    if (!frame.rgba.empty())
        renderer_.update_texture(texture, frame.width, frame.height, frame.rgba, video.padding_x, video.padding_y);
    return true;
}
void SceneCache::draw(ModelInstance& instance, DrawState state, std::size_t animation,
                      std::uint32_t elapsed, std::uint32_t wall_clock) {
    const auto& model = instance.resource();
    for (std::size_t n = 0; n < model.parts.size(); ++n) {
        auto& part = instance.parts[n];
        const auto& source = model.parts[n];
        if (part.owned) {
            if (animation >= source.mesh->animations.size())
                throw std::runtime_error("Entity animation ordinal exceeds model mesh animation list");
            const auto& clip = source.mesh->animations[animation];
            const auto frame = animation_frame(clip, elapsed);
            if (animation != part.animation || frame != part.frame) {
                skin_mesh(source.mesh->mesh, clip, elapsed, model.model.scale, model.model.offset, part.skin);
                renderer_.update_mesh(part.gpu, part.skin.vertices);
                part.animation = animation; part.frame = frame;
            }
        }
        auto& material = *source.material;
        for (std::size_t p = 0; p < material.material.passes.size(); ++p) {
            const auto& pass = material.material.passes[p];
            auto& texture = material.textures[p];
            if (pass.video) video_texture(pass.texture_name, wall_clock, true, texture);
            else if (!texture) texture = image(pass.texture_name);
            renderer_.draw(part.gpu, texture, material.material, pass, state);
        }
    }
}
void SceneCache::draw_quad(const menu::Draw& quad, std::uint32_t time, bool loop) {
    if (!quad.visible) return;
    DrawState state;
    state.color = quad.color;
    const auto& p = quad.positions;
    auto& m = state.model.values;
    m[0] = (p[2].x-p[3].x)*1024; m[1] = (p[2].y-p[3].y)*768;
    m[4] = (p[0].x-p[3].x)*1024; m[5] = (p[0].y-p[3].y)*768;
    m[12] = p[3].x*1024; m[13] = p[3].y*768;
    state.uv_transform = {quad.uv[2].x-quad.uv[3].x, quad.uv[0].y-quad.uv[3].y,
                          quad.uv[3].x, quad.uv[3].y};
    if (quad.material.empty()) {
        Material material;
        material.cull = false;
        MaterialPass pass;
        pass.depth_test = false; pass.depth_write = false;
        pass.blend = true;
        renderer_.draw(quad_, quad.texture.empty() ? 0 : image(quad.texture), material, pass, state);
        return;
    }
    auto& material = this->material(quad.material);
    for (std::size_t p = 0; p < material.material.passes.size(); ++p) {
        const auto& pass = material.material.passes[p];
        unsigned texture;
        if (!quad.texture.empty()) texture = image(quad.texture);
        else {
            auto& cached = material.textures[p];
            if (pass.video) { if (!video_texture(pass.texture_name, time, loop, cached)) continue; }
            else if (!cached) cached = image(pass.texture_name);
            texture = cached;
        }
        renderer_.draw(quad_, texture, material.material, pass, state);
    }
}
void SceneCache::draw_ui(std::span<const menu::Draw> quads, std::uint32_t time) {
    for (const auto& quad : quads) draw_quad(quad, time, true);
}
bool SceneCache::draw_video(std::string_view name, std::uint32_t elapsed) {
    auto& resource = material(name);
    if (resource.material.passes.empty() || !resource.material.passes[0].video)
        throw std::runtime_error("Cutscene material does not contain video: "+std::string(name));
    unsigned texture;
    if (!video_texture(resource.material.passes[0].texture_name, elapsed, false, texture)) return false;
    menu::Draw quad;
    quad.material = name;
    quad.positions = {{{0,1},{1,1},{1,0},{0,0}}};
    // 0041659c..004165e8 exact UVs crop the centered512x384 source in512x512.
    quad.uv = {{{0,-0.125f},{1,-0.125f},{1,-0.875f},{0,-0.875f}}};
    draw_quad(quad, elapsed, false);
    return true;
}
void SceneCache::reset_video(std::string_view name) {
    auto& resource = material(name);
    for (const auto& pass : resource.material.passes) if (pass.video) {
        const auto found = videos_.find(pass.texture_name);
        if (found != videos_.end()) {
            found->second.decoder->seek(0);
            found->second.frame = -1;
            found->second.active = false;
        }
    }
}
} // namespace yami
