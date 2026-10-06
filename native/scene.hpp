#pragma once
#include "assets.hpp"
#include "media.hpp"
#include "menu.hpp"
#include "renderer.hpp"
#include <map>
#include <memory>
#include <string>

namespace yami {
struct MeshResource {
    Mesh mesh;
    std::vector<Animation> animations;
    unsigned static_gpu = 0;
};
struct MaterialResource { Material material; std::vector<unsigned> textures; };
struct ModelResource {
    Model model;
    struct Part { MeshResource* mesh; MaterialResource* material; };
    std::vector<Part> parts;
};
class ModelInstance {
public:
    ModelInstance(Renderer&, const ModelResource&, bool animated);
    ~ModelInstance();
    ModelInstance(const ModelInstance&) = delete;
    ModelInstance& operator=(const ModelInstance&) = delete;
    const ModelResource& resource() const noexcept { return model_; }
    struct Part {
        unsigned gpu = 0;
        bool owned = false;
        std::size_t animation = static_cast<std::size_t>(-1), frame = static_cast<std::size_t>(-1);
        SkinningState skin;
    };
    std::vector<Part> parts;
private:
    Renderer& renderer_;
    const ModelResource& model_;
};
// Immutable CPU/GPU resources are cached per original resource identifier.
// Mutable skinning belongs to each instance; video storage is reused per source.
class SceneCache {
public:
    SceneCache(Renderer&, std::filesystem::path root);
    std::filesystem::path resolve(std::string_view name); // Scoped to this read-only asset tree.
    ModelResource& model(std::string_view descriptor); // game-relative model.dat path
    MaterialResource& material(std::string_view name); // data/materials/<name>.dat
    unsigned image(std::string_view filename); // original missing-image white semantics
    void draw(ModelInstance&, DrawState, std::size_t animation, std::uint32_t elapsed,
              std::uint32_t wall_clock);
    void draw_ui(std::span<const menu::Draw>, std::uint32_t wall_clock);
    bool draw_video(std::string_view material, std::uint32_t elapsed); // false at actual end
    void reset_video(std::string_view material);
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    struct Video {
        std::unique_ptr<VideoDecoder> decoder;
        unsigned texture = 0;
        int padding_x = 0, padding_y = 0;
        std::int64_t frame = -1;
        std::uint32_t started = 0;
        bool active = false;
    };
    Renderer& renderer_;
    std::filesystem::path root_;
    AssetDirectories directories_;
    std::map<std::string, std::unique_ptr<MeshResource>, std::less<>> meshes_;
    std::map<std::string, std::unique_ptr<ModelResource>, std::less<>> models_;
    std::map<std::string, std::unique_ptr<MaterialResource>, std::less<>> materials_;
    std::map<std::string, unsigned, std::less<>> images_;
    std::map<std::string, Video, std::less<>> videos_;
    unsigned quad_ = 0, white_ = 0;
    MeshResource& mesh(const std::filesystem::path&);
    bool video_texture(std::string_view, std::uint32_t time, bool loop, unsigned& texture);
    void draw_quad(const menu::Draw&, std::uint32_t time, bool loop);
};
} // namespace yami
