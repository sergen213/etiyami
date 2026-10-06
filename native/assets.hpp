#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace yami {
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec2 { float x = 0, y = 0; };
struct Quaternion { float x = 0, y = 0, z = 0, w = 1; };
// Original OpenGL column-major storage; translation occupies elements 12..14.
struct Mat4 { std::array<float, 16> values{}; };
struct Vertex { Vec3 position; Vec2 uv; Vec3 normal; };
static_assert(sizeof(Vertex) == 32, "Original mesh stride");
Mat4 identity_matrix();
Mat4 multiply(const Mat4& left, const Mat4& right);
Vec3 transform_point(const Mat4&, Vec3);
Vec3 transform_direction(const Mat4&, Vec3);
Mat4 pose_matrix(Quaternion, Vec3);

struct Bone {
    std::string name;
    int parent = -1;
    Mat4 bind_matrix; // Loaded verbatim: original skinning right-multiplies this matrix.
    std::vector<std::uint16_t> vertices; // Rigid ownership, not weighted influences.
};
struct BonePose { Quaternion rotation; Vec3 translation; };
struct Animation {
    std::vector<std::string> bone_names;
    std::vector<std::size_t> bone_indices;
    std::uint32_t frame_count = 0;
    std::vector<BonePose> frames; // Frame-major, then animation bone order.
};
struct Mesh {
    std::uint32_t vertex_format = 0; // Exported marker; original always reads stride 32.
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<Bone> bones;
    std::vector<std::string> animation_names; // Action slots in source order; aliases are intentional.
    // Original overwrites multiply-owned vertices in ascending bone order.
    std::vector<int> vertex_bones;
};
struct ModelPart { std::string mesh_name, material_name; };
struct Model {
    bool has_bounds = false;
    Vec3 bounds_min, bounds_max; // First/second exporter endpoints (+d8/+e4), not sorted.
    std::array<float, 4> exported_bounds_extra{}; // Loader reads and ignores these.
    bool has_collision_box = false;
    float scale = 1;
    // Original +bc/+c0/+c4 dimensions and +c8/+cc/+d0 offset, already scaled.
    // Animated outer draw matrix subtracts collision_box.y * .5 from world Y;
    // static draws ignore descriptor scale/offset and use the full scene matrix.
    Vec3 collision_box, offset;
    std::vector<ModelPart> parts;
};
// Original model +fc/+100/+104: signed (second-first)*.5; collision dimensions
// override these with scaled dimensions*.5. Empty exporter sentinels stay negative.
// No mesh-derived fallback; absent sections leave constructor extents at zero.
Vec3 model_half_extents(const Model&) noexcept;
struct MaterialPass {
    bool video = false;
    std::string texture_name; // Relative to game/, not to the material directory.
    bool alpha_test = false;
    std::uint32_t alpha_function = 0x200;
    float alpha_reference = 0;
    bool blend = false;
    std::uint32_t blend_source = 0x302, blend_destination = 0x303;
    std::array<bool, 4> color_write{{true, true, true, true}};
    bool depth_test = true;
    std::uint32_t depth_function = 0x203;
    bool depth_write = true;
    // Bytes 40..55 are unreferenced by 0040ffc0/0042df40. Texture properties
    // are constructor constants: MODULATE, REPEAT, LINEAR_MIPMAP_NEAREST min,
    // LINEAR mag (the original's invalid mipmapped mag request is ignored).
    // Serialized padding/unreferenced state bytes are preserved, not interpreted.
    std::array<std::uint8_t, 56> exported_state{};
};
struct Material {
    bool cull = true;
    std::uint32_t cull_face = 0x405;
    bool polygon_offset = false;
    float polygon_offset_factor = 0, polygon_offset_units = 0;
    std::array<std::uint8_t, 20> exported_state{};
    std::vector<MaterialPass> passes;
};

// Every reader checks lengths, numeric finiteness, indices/names and exact EOF.
// Errors throw std::runtime_error with the source path and byte offset.
Model read_model(const std::filesystem::path&);
Mesh read_mesh(const std::filesystem::path&);
Animation read_animation(const std::filesystem::path&);
Animation read_animation(const std::filesystem::path&, const Mesh&);
// Decodes exported filename lines, intentionally fixing the original fscanf
// whitespace bug without changing media or silently repairing exporter paths.
Material read_material(const std::filesystem::path&);
// Resolve original Windows case-insensitive names safely beneath a supplied root.
// Name contract: UTF-8; exhaustive shipped binary names and filesystem paths
// are ASCII (also valid CP1254). No heuristic encoding conversion occurs here.
// Optional cache belongs to one read-only asset lifetime; never reuse it after
// changing/replacing the asset tree. Only verified non-symlink directories enter it.
using AssetDirectories = std::map<std::filesystem::path, std::filesystem::path>;
std::filesystem::path resolve_asset(const std::filesystem::path& root, const std::string& name,
                                    AssetDirectories* directories = nullptr);
std::filesystem::path mesh_path(const std::filesystem::path& model_file, const ModelPart&,
                               AssetDirectories* directories = nullptr);
std::filesystem::path animation_path(const std::filesystem::path& mesh_file, const std::string& name,
                                    AssetDirectories* directories = nullptr);

// Allocate caller-owned buffers once using prepare_skinning; subsequent calls do
// not allocate or copy unchanged vertex/UV arrays. Static meshes remain verbatim.
struct SkinningState { std::vector<Mat4> local, final; std::vector<Vertex> vertices; };
void prepare_skinning(const Mesh&, SkinningState&);
std::size_t animation_frame(const Animation&, std::uint32_t elapsed_milliseconds);
// Exact original 30fps discrete frames, no interpolation. Parent composition is
// child * parent * grandparent, NOT the usual parent * child convention.
// Pass Model.scale/offset verbatim; conversion is already included here.
// Caller applies original upright instance matrix and the half-height Y shift.
void skin_mesh(const Mesh&, const Animation&, std::uint32_t elapsed_milliseconds,
               float model_scale, Vec3 model_offset, SkinningState&);
} // namespace yami
