#include "assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace yami {
namespace {
bool whitespace(std::uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
std::uint32_t little_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
float little_float(const std::uint8_t* p) {
    const auto bits = little_u32(p);
    float f;
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "Assets require IEEE754 binary32");
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : path_(path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) fail("cannot open file");
        const auto end = file.tellg();
        if (end < 0 || static_cast<std::uintmax_t>(end) > bytes_.max_size() ||
            static_cast<std::uintmax_t>(end) > std::uintmax_t(std::numeric_limits<std::streamsize>::max()))
            fail("invalid file size");
        bytes_.resize(static_cast<std::size_t>(end));
        file.seekg(0);
        if (!bytes_.empty() && !file.read(reinterpret_cast<char*>(bytes_.data()),
                                         static_cast<std::streamsize>(bytes_.size())))
            fail("short file read");
    }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(path_.string() + ": byte " + std::to_string(offset_) + ": " + reason);
    }
    std::size_t remaining() const { return bytes_.size() - offset_; }
    void require(std::size_t count, std::size_t stride = 1) const {
        if (!stride || count > remaining() / stride) fail("truncated or excessive section length");
    }
    const std::uint8_t* take(std::size_t count) {
        require(count);
        const auto* result = bytes_.data() + offset_;
        offset_ += count;
        return result;
    }
    std::uint32_t u32() { return little_u32(take(4)); }
    std::uint16_t u16() {
        const auto* p = take(2);
        return std::uint16_t(std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8));
    }
    float f32() {
        const auto value = little_float(take(4));
        if (!std::isfinite(value)) fail("nonfinite float");
        return value;
    }
    bool section() {
        const auto value = u32();
        if (value > 1) fail("invalid section presence flag");
        return value != 0;
    }
    std::size_t count(std::size_t minimum_stride) {
        const auto value = u32();
        if (value > std::uint32_t(std::numeric_limits<std::int32_t>::max())) fail("negative original count");
        require(value, minimum_stride);
        return value;
    }
    // fscanf("%s") leaves the delimiter; mesh/animation binary transitions call
    // getc exactly once (00420e6d/00420e9b,004017f6). Never skip extra bytes.
    std::string token(bool binary_follows = false) {
        while (remaining() && whitespace(bytes_[offset_])) ++offset_;
        const auto start = offset_;
        while (remaining() && !whitespace(bytes_[offset_])) {
            if (!bytes_[offset_] || offset_ - start >= 255) fail("invalid or oversized token");
            ++offset_;
        }
        if (offset_ == start) fail("missing token");
        std::string value(reinterpret_cast<const char*>(bytes_.data() + start), offset_ - start);
        if (binary_follows) {
            if (!remaining()) fail("missing token/binary delimiter");
            ++offset_;
        }
        return value;
    }
    std::string filename_line() {
        const auto start = offset_;
        while (remaining() && bytes_[offset_] != '\n') {
            if (!bytes_[offset_] || offset_ - start >= 255) fail("invalid or oversized texture filename");
            ++offset_;
        }
        if (!remaining()) fail("missing texture filename newline");
        auto length = offset_ - start;
        if (length && bytes_[offset_ - 1] == '\r') --length;
        if (!length) fail("empty texture filename");
        std::string value(reinterpret_cast<const char*>(bytes_.data() + start), length);
        ++offset_;
        return value;
    }
    void finish(bool text_tail = false) {
        if (text_tail) while (remaining() && whitespace(bytes_[offset_])) ++offset_;
        if (remaining()) fail("unconsumed trailing bytes");
    }
private:
    std::filesystem::path path_;
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};
Vec3 vector3(Reader& r) { return {r.f32(), r.f32(), r.f32()}; }
Vec3 scaled(Vec3 v, float scale) { return {v.x * scale, v.y * scale, v.z * scale}; }
void finite_vector(Reader& r, Vec3 v) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) r.fail("scaled vector overflow");
}
bool state_flag(Reader& r, const std::uint8_t* bytes, std::size_t offset) {
    if (bytes[offset] > 1) r.fail("invalid serialized render-state boolean");
    return bytes[offset] != 0;
}
float state_float(Reader& r, const std::uint8_t* bytes, std::size_t offset) {
    const float value = little_float(bytes + offset);
    if (!std::isfinite(value)) r.fail("nonfinite serialized render-state float");
    return value;
}
std::string folded(std::string s) {
    // Windows resource identifiers in the shipped data are ASCII; don't depend
    // on the host's locale for case-insensitive matching.
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return s;
}
void validate_hierarchy(Reader& r, const Mesh& mesh) {
    std::vector<std::uint8_t> marks(mesh.bones.size());
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        int bone = static_cast<int>(i);
        while (bone >= 0 && marks[std::size_t(bone)] == 0) {
            marks[std::size_t(bone)] = 1;
            bone = mesh.bones[std::size_t(bone)].parent;
        }
        if (bone >= 0 && marks[std::size_t(bone)] == 1) r.fail("cyclic bone hierarchy");
        bone = static_cast<int>(i);
        while (bone >= 0 && marks[std::size_t(bone)] == 1) {
            marks[std::size_t(bone)] = 2;
            bone = mesh.bones[std::size_t(bone)].parent;
        }
    }
}
} // namespace

Mat4 identity_matrix() {
    Mat4 result;
    result.values[0] = result.values[5] = result.values[10] = result.values[15] = 1;
    return result;
}
Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result;
    // 004137c0: original column-major product. Return by value also makes
    // aliasing safe, unlike a naive in-place implementation of this routine.
    for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
            result.values[column * 4 + row] = static_cast<float>(
                double(left.values[row]) * right.values[column * 4] +
                double(left.values[4 + row]) * right.values[column * 4 + 1] +
                double(left.values[12 + row]) * right.values[column * 4 + 3] +
                double(left.values[8 + row]) * right.values[column * 4 + 2]);
    return result;
}
Vec3 transform_direction(const Mat4& m, Vec3 v) {
    const auto& a = m.values; // 00413d80: no translation or normal renormalization.
    return {static_cast<float>(double(v.x) * a[0] + double(v.y) * a[4] + double(v.z) * a[8]),
            static_cast<float>(double(v.y) * a[5] + double(v.z) * a[9] + double(v.x) * a[1]),
            static_cast<float>(double(v.y) * a[6] + double(v.z) * a[10] + double(v.x) * a[2])};
}
Vec3 transform_point(const Mat4& m, Vec3 v) {
    const auto& a = m.values; // 00413d20: affine, no perspective divide.
    return {static_cast<float>(double(v.x) * a[0] + double(v.y) * a[4] + double(v.z) * a[8] + a[12]),
            static_cast<float>(double(v.y) * a[5] + double(v.z) * a[9] + double(v.x) * a[1] + a[13]),
            static_cast<float>(double(v.y) * a[6] + double(v.z) * a[10] + double(v.x) * a[2] + a[14])};
}
Mat4 pose_matrix(Quaternion q, Vec3 translation) {
    auto m = identity_matrix(); // 00423520 followed by 00423370; no normalization.
    auto& a = m.values;
    // 00423520 stores one product at binary32 before each combined expression;
    // the other product remains in x87's 53-bit accumulator until final store.
    a[0] = static_cast<float>(1 - 2 * (double(q.y) * q.y + float(q.z * q.z)));
    a[1] = static_cast<float>(2 * (double(q.x) * q.y + float(q.z * q.w)));
    a[2] = static_cast<float>(2 * (float(q.x * q.z) - double(q.y) * q.w));
    a[4] = static_cast<float>(2 * (float(q.x * q.y) - double(q.z) * q.w));
    a[5] = static_cast<float>(1 - 2 * (double(q.x) * q.x + float(q.z * q.z)));
    a[6] = static_cast<float>(2 * (float(q.y * q.z) + double(q.x) * q.w));
    a[8] = static_cast<float>(2 * (double(q.x) * q.z + float(q.y * q.w)));
    a[9] = static_cast<float>(2 * (float(q.y * q.z) - double(q.x) * q.w));
    a[10] = static_cast<float>(1 - 2 * (double(q.x) * q.x + float(q.y * q.y)));
    a[12] = translation.x; a[13] = translation.y; a[14] = translation.z;
    return m;
}

Model read_model(const std::filesystem::path& path) {
    Reader r(path);
    Model model;
    model.has_bounds = r.section();
    if (model.has_bounds) {
        model.bounds_min = vector3(r); model.bounds_max = vector3(r);
        for (auto& f : model.exported_bounds_extra) f = r.f32();
        // 0040ecc0 uses signed (second - first) * .5 verbatim. Empty exporter
        // models retain first=+999999, second=-999999; these are not malformed.
    }
    model.has_collision_box = r.section();
    if (model.has_collision_box) {
        model.scale = r.f32();
        model.collision_box = scaled(vector3(r), model.scale);
        if (r.section()) model.offset = scaled(vector3(r), model.scale);
        finite_vector(r, model.collision_box); finite_vector(r, model.offset);
    }
    const auto count = r.count(4);
    model.parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        model.parts.push_back({r.token(), r.token()});
    r.finish(true);
    return model;
}
Vec3 model_half_extents(const Model& model) noexcept {
    if (model.has_collision_box) return scaled(model.collision_box, 0.5f);
    // No sorting/absolute value: +d8..+e0 and +e4..+ec are source-order endpoints.
    if (model.has_bounds) {
        return {
            static_cast<float>((double(model.bounds_max.x) - model.bounds_min.x) * 0.5),
            static_cast<float>((double(model.bounds_max.y) - model.bounds_min.y) * 0.5),
            static_cast<float>((double(model.bounds_max.z) - model.bounds_min.z) * 0.5)
        };
    }
    return {};
}

Mesh read_mesh(const std::filesystem::path& path) {
    Reader r(path);
    Mesh mesh;
    if (r.section()) {
        const auto count = r.count(32);
        mesh.vertex_format = r.u32();
        if (mesh.vertex_format != 3) r.fail("unsupported vertex format marker");
        r.require(count, 32);
        mesh.vertices.resize(count);
        for (auto& v : mesh.vertices) {
            v.position = vector3(r); v.uv = {r.f32(), r.f32()}; v.normal = vector3(r);
        }
    }
    if (r.section()) {
        const auto count = r.count(2);
        if (count % 3) r.fail("triangle index count is not a multiple of three");
        mesh.indices.resize(count);
        for (auto& index : mesh.indices) {
            index = r.u16();
            if (index >= mesh.vertices.size()) r.fail("vertex index outside mesh");
        }
    }
    mesh.vertex_bones.assign(mesh.vertices.size(), -1);
    if (r.section()) {
        const auto count = r.count(74);
        if (count > std::size_t(std::numeric_limits<int>::max())) r.fail("excessive bone count");
        mesh.bones.reserve(count);
        std::unordered_map<std::string, int> names;
        for (std::size_t i = 0; i < count; ++i) {
            const bool root = r.section();
            Bone bone;
            bone.name = r.token(true);
            if (!root) {
                const auto parent = names.find(r.token(true));
                // 00420a80 searches only already loaded bones. A forward or
                // absent parent would leave the original object uninitialized.
                if (parent == names.end()) r.fail("bone parent must precede child");
                bone.parent = parent->second;
            }
            if (!names.emplace(bone.name, static_cast<int>(i)).second) r.fail("duplicate bone name");
            for (auto& value : bone.bind_matrix.values) value = r.f32();
            const auto influences = r.count(2);
            bone.vertices.resize(influences);
            for (auto& index : bone.vertices) {
                index = r.u16();
                if (index >= mesh.vertices.size()) r.fail("bone vertex index outside mesh");
                mesh.vertex_bones[index] = static_cast<int>(i);
            }
            mesh.bones.push_back(std::move(bone));
        }
        validate_hierarchy(r, mesh);
        if (!mesh.bones.empty() && std::find(mesh.vertex_bones.begin(), mesh.vertex_bones.end(), -1) != mesh.vertex_bones.end())
            r.fail("skeletal mesh contains unowned vertices");
    }
    if (r.section()) {
        const auto count = r.count(2);
        mesh.animation_names.reserve(count);
        // 00420c40 appends every slot; 00410350 selects by action ordinal.
        // Repeated filenames intentionally alias actions, not duplicate keys.
        for (std::size_t i = 0; i < count; ++i) {
            auto name = r.token();
            mesh.animation_names.push_back(std::move(name));
        }
    }
    r.finish(true);
    return mesh;
}

Animation read_animation(const std::filesystem::path& path) {
    Reader r(path);
    Animation animation;
    const auto count = r.count(2);
    if (!count) r.fail("animation has no bones");
    animation.bone_names.reserve(count);
    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < count; ++i) {
        auto name = r.token(true);
        if (!names.emplace(name).second) r.fail("duplicate animation bone name");
        animation.bone_names.push_back(std::move(name));
    }
    animation.frame_count = r.u32();
    if (!animation.frame_count || animation.frame_count > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
        r.fail("invalid animation frame count");
    r.require(animation.frame_count, 28);
    const auto per_frame_limit = r.remaining() / 28 / animation.frame_count;
    if (count > per_frame_limit) r.fail("truncated animation frame records");
    animation.frames.resize(count * animation.frame_count);
    // 00401900..0040192a and 00426190 prove disk order x,y,z,w,tx,ty,tz.
    for (auto& frame : animation.frames) {
        frame.rotation = {r.f32(), r.f32(), r.f32(), r.f32()};
        frame.translation = vector3(r);
    }
    r.finish();
    return animation;
}
Animation read_animation(const std::filesystem::path& path, const Mesh& mesh) {
    auto animation = read_animation(path);
    animation.bone_indices.reserve(animation.bone_names.size());
    for (const auto& name : animation.bone_names) {
        const auto it = std::find_if(mesh.bones.begin(), mesh.bones.end(),
                                   [&](const Bone& bone) { return bone.name == name; });
        // 00401670 returns zero on a failed lookup. 004015c0 samples by
        // ordinal, so this metadata is never used to remap animation frames.
        animation.bone_indices.push_back(it == mesh.bones.end() ? 0 : std::size_t(it - mesh.bones.begin()));
    }
    return animation;
}

Material read_material(const std::filesystem::path& path) {
    Reader r(path);
    Material material;
    const auto* header = r.take(material.exported_state.size());
    std::copy_n(header, material.exported_state.size(), material.exported_state.begin());
    material.cull = state_flag(r, header, 0);
    material.cull_face = little_u32(header + 4);
    material.polygon_offset = state_flag(r, header, 8);
    material.polygon_offset_factor = state_float(r, header, 12);
    material.polygon_offset_units = state_float(r, header, 16);
    const auto count = r.count(62);
    material.passes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        MaterialPass pass;
        const auto type = r.u32();
        if (type > 1) r.fail("invalid material texture type");
        pass.video = type == 1;
        // All 2523 shipped material exports have a newline-delimited filename
        // followed by exactly 56 bytes. Intentional compatibility correction:
        // original fscanf("%s")+getc corrupts ten paths containing spaces,
        // including reachable level1/DefaultLib_Material196 (canak anten.tga).
        // Keep the physical filename unchanged, including stale exporter paths.
        pass.texture_name = r.filename_line();
        const auto* state = r.take(pass.exported_state.size());
        std::copy_n(state, pass.exported_state.size(), pass.exported_state.begin());
        // 0042df40 establishes all offsets; padding contains exporter heap
        // garbage, including pointer-looking bytes, and must not be executed.
        pass.alpha_test = state_flag(r, state, 0);
        pass.alpha_function = little_u32(state + 4);
        pass.alpha_reference = state_float(r, state, 8);
        pass.blend = state_flag(r, state, 12);
        pass.blend_source = little_u32(state + 16);
        pass.blend_destination = little_u32(state + 20);
        for (std::size_t channel = 0; channel < 4; ++channel)
            pass.color_write[channel] = state_flag(r, state, 24 + channel);
        pass.depth_test = state_flag(r, state, 28);
        pass.depth_function = little_u32(state + 32);
        pass.depth_write = state_flag(r, state, 36);
        material.passes.push_back(std::move(pass));
    }
    r.finish();
    return material;
}

std::filesystem::path resolve_asset(const std::filesystem::path& root, const std::string& name) {
    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty() || normalized.find('\0') != std::string::npos || normalized.find(':') != std::string::npos)
        throw std::runtime_error("invalid resource path: " + name);
    const std::filesystem::path relative(normalized);
    if (relative.is_absolute()) throw std::runtime_error("absolute resource path: " + name);
    auto result = root;
    for (const auto& component : relative) {
        const auto part = component.string();
        if (part.empty() || part == ".") continue;
        if (part == "..") throw std::runtime_error("resource path escapes root: " + name);
        const auto candidate = result / component;
        if (std::filesystem::exists(candidate)) {
            if (std::filesystem::is_symlink(candidate)) throw std::runtime_error("symlink resource path: " + name);
            result = candidate;
            continue;
        }
        bool found = false;
        if (std::filesystem::is_directory(result)) {
            for (const auto& entry : std::filesystem::directory_iterator(result)) {
                if (folded(entry.path().filename().string()) != folded(part)) continue;
                if (entry.is_symlink()) throw std::runtime_error("symlink resource path: " + name);
                if (found) throw std::runtime_error("ambiguous case-insensitive resource: " + name);
                result = entry.path(); found = true;
            }
        }
        if (!found) throw std::filesystem::filesystem_error(
            "missing resource", candidate, std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (!std::filesystem::is_regular_file(result)) throw std::filesystem::filesystem_error(
        "resource is not a file", result, std::make_error_code(
            std::filesystem::is_directory(result) ? std::errc::is_a_directory : std::errc::invalid_argument));
    return result;
}
std::filesystem::path mesh_path(const std::filesystem::path& model_file, const ModelPart& part) {
    return resolve_asset(model_file.parent_path(), part.mesh_name + ".dat");
}
std::filesystem::path animation_path(const std::filesystem::path& mesh_file, const std::string& name) {
    return resolve_asset(mesh_file.parent_path(), "animations/" + name + ".dat");
}

void prepare_skinning(const Mesh& mesh, SkinningState& state) {
    state.local.resize(mesh.bones.size()); state.final.resize(mesh.bones.size());
    state.vertices = mesh.vertices;
}
std::size_t animation_frame(const Animation& animation, std::uint32_t elapsed) {
    if (!animation.frame_count) throw std::runtime_error("cannot sample empty animation");
    // 004015c4 FILD integer, FMUL .001f, FMUL 30f, FSTP double: no
    // intermediate binary32 store. CRT 0044adc8 selects x87 53-bit precision.
    const double seconds = static_cast<double>(elapsed) * static_cast<double>(0.0010000000474974513f);
    const double frames = seconds * 30.0;
    return static_cast<std::size_t>(std::floor(frames)) % animation.frame_count;
}
void skin_mesh(const Mesh& mesh, const Animation& animation, std::uint32_t elapsed,
               float scale, Vec3 offset, SkinningState& state) {
    if (state.vertices.size() != mesh.vertices.size() || state.local.size() != mesh.bones.size() ||
        state.final.size() != mesh.bones.size() || mesh.vertex_bones.size() != mesh.vertices.size())
        throw std::runtime_error("skinning buffers must be prepared for this mesh");
    if (mesh.bones.empty()) return; // Original static meshes use exported vertices unchanged.
    if (!std::isfinite(scale) || !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
        throw std::runtime_error("nonfinite model skinning transform");
    const auto frame = animation_frame(animation, elapsed);
    const auto count = animation.bone_names.size();
    if (!count || count > std::numeric_limits<std::size_t>::max() / animation.frame_count ||
        animation.frames.size() != count * animation.frame_count)
        throw std::runtime_error("invalid animation frame storage");
    for (std::size_t i = 0; i < mesh.bones.size() && i < count; ++i) {
        const auto& pose = animation.frames[frame * count + i];
        state.local[i] = pose_matrix(pose.rotation, pose.translation);
    }
    static const Mat4 conversion = [] {
        auto result = identity_matrix();
        // QWORD 004599c0 = 3.1415927410125732, confirmed at 0040fe28/0040fe9c.
        const float c = static_cast<float>(std::cos(3.1415927410125732));
        const float s = static_cast<float>(std::sin(3.1415927410125732));
        result.values[0] = c; result.values[2] = -s;
        result.values[8] = s; result.values[10] = c;
        return result;
    }();
    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
        // Original kursun_yaman_lazer has an extra uninfluential bone but only
        // one animation record. Don't reproduce its out-of-bounds read; skip
        // unused matrices and reject any missing record that affects a vertex.
        if (mesh.bones[i].vertices.empty()) continue;
        if (i >= count) throw std::runtime_error("animation omits an influencing bone ordinal");
        auto accumulated = state.local[i];
        int parent = mesh.bones[i].parent;
        std::size_t depth = 0;
        while (parent >= 0) {
            if (std::size_t(parent) >= mesh.bones.size() || std::size_t(parent) >= count || ++depth > mesh.bones.size())
                throw std::runtime_error("invalid animation parent ordinal");
            accumulated = multiply(accumulated, state.local[std::size_t(parent)]);
            parent = mesh.bones[std::size_t(parent)].parent;
        }
        state.final[i] = multiply(conversion, multiply(accumulated, mesh.bones[i].bind_matrix));
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const int bone = mesh.vertex_bones[i];
        if (bone < 0 || std::size_t(bone) >= mesh.bones.size()) throw std::runtime_error("invalid vertex bone owner");
        auto position = scaled(transform_point(state.final[std::size_t(bone)], mesh.vertices[i].position), scale);
        position.x += offset.x; position.y += offset.y; position.z += offset.z;
        state.vertices[i].position = position;
        state.vertices[i].normal = transform_direction(state.final[std::size_t(bone)], mesh.vertices[i].normal);
    }
}
} // namespace yami
