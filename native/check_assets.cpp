#include "assets.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}
bool near(float a, float b) { return std::abs(a - b) < 0.00001f; }
void check_math_and_skinning() {
    yami::Model collider;
    check(yami::model_half_extents(collider).x == 0, "absent collider sections must remain zero");
    collider.has_bounds = true;
    collider.bounds_min = {-2, -3, -4}; collider.bounds_max = {2, 3, 4};
    check(yami::model_half_extents(collider).y == 3, "exported bounds half-extents");
    collider.bounds_min = {999999, 999999, 999999};
    collider.bounds_max = {-999999, -999999, -999999};
    const auto empty_bounds = yami::model_half_extents(collider);
    check(empty_bounds.x == -999999 && empty_bounds.y == -999999 && empty_bounds.z == -999999,
          "0040ecc0 preserves signed exporter empty-bounds extents");
    collider.has_collision_box = true;
    collider.collision_box = {10, 20, 30};
    const auto extents = yami::model_half_extents(collider);
    check(extents.x == 5 && extents.y == 10 && extents.z == 15, "scaled collision dimensions override bounds");
    const auto identity = yami::identity_matrix();
    const auto quarter = yami::pose_matrix({0, 0, std::sqrt(0.5f), std::sqrt(0.5f)}, {0, 0, 0});
    const auto rotated = yami::transform_point(quarter, {1, 0, 0});
    check(near(rotated.x, 0) && near(rotated.y, 1), "quaternion component/storage convention");
    auto translation = identity;
    translation.values[12] = 1;
    const auto child_parent = yami::transform_point(yami::multiply(quarter, translation), {0, 0, 0});
    check(near(child_parent.x, 0) && near(child_parent.y, 1), "column-major multiplication order");
    check(near(yami::transform_direction(translation, {1, 2, 3}).x, 1), "normal includes translation");

    yami::Mesh mesh;
    mesh.vertices.push_back({{0, 0, 0}, {0.25f, 0.75f}, {1, 0, 0}});
    mesh.bones.push_back({"parent", -1, identity, {}});
    mesh.bones.push_back({"child", 0, identity, {0}});
    mesh.vertex_bones = {1};
    yami::Animation animation;
    animation.bone_names = {"parent", "child"};
    animation.frame_count = 2;
    animation.frames = {{{0, 0, 0, 1}, {1, 0, 0}},
                        {{0, 0, std::sqrt(0.5f), std::sqrt(0.5f)}, {0, 0, 0}},
                        {{0, 0, 0, 1}, {2, 0, 0}},
                        {{0, 0, 0, 1}, {0, 0, 0}}};
    yami::SkinningState state;
    yami::prepare_skinning(mesh, state);
    const auto* allocation = state.vertices.data();
    yami::skin_mesh(mesh, animation, 0, 2, {3, 4, 5}, state);
    check(near(state.vertices[0].position.x, 3) && near(state.vertices[0].position.y, 6) &&
          near(state.vertices[0].position.z, 5), "child*parent, Y conversion, model scale/offset");
    check(near(state.vertices[0].uv.x, 0.25f) && near(state.vertices[0].uv.y, 0.75f), "skinning mutates UV");
    check(yami::animation_frame(animation, 33) == 0 && yami::animation_frame(animation, 34) == 1 &&
          yami::animation_frame(animation, 67) == 0, "original 30fps discrete frame sampling");
    yami::skin_mesh(mesh, animation, 34, 1, {}, state);
    check(near(state.vertices[0].position.x, -2), "animation must sample frame-major ordinals");
    check(state.vertices.data() == allocation, "skinning allocated a new vertex buffer");
}
std::string lower_path(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c + 'a' - 'A');
    return value;
}
bool has_animation_directory(const std::filesystem::path& path) {
    for (const auto& component : path) if (component == "animations") return true;
    return false;
}
struct AnimationUse {
    std::filesystem::path mesh, animation;
    std::vector<std::string> bones;
    std::vector<std::size_t> required_ordinals;
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::runtime_error("usage: check_assets [game-directory]");
        const std::filesystem::path game = argc == 2 ? argv[1] : "game";
        check_math_and_skinning();
        std::vector<std::filesystem::path> models, meshes, animations, materials;
        for (const auto& directory : {game / "data/models", game / "data/levels"}) {
            check(std::filesystem::is_directory(directory), "missing asset directory: " + directory.string());
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (!entry.is_regular_file() || lower_path(entry.path().extension().string()) != ".dat") continue;
                if (entry.path().filename() == "model.dat") models.push_back(entry.path());
                else if (has_animation_directory(entry.path())) animations.push_back(entry.path());
                else meshes.push_back(entry.path());
            }
        }
        const auto material_root = game / "data/materials";
        check(std::filesystem::is_directory(material_root), "missing material directory");
        for (const auto& entry : std::filesystem::recursive_directory_iterator(material_root))
            if (entry.is_regular_file() && lower_path(entry.path().extension().string()) == ".dat")
                materials.push_back(entry.path());
        for (auto* files : {&models, &meshes, &animations, &materials}) std::sort(files->begin(), files->end());

        std::size_t failures = 0, missing_images = 0, missing_videos = 0, name_mismatches = 0;
        std::size_t vertex_count = 0, index_count = 0, bone_count = 0, pose_count = 0;
        std::size_t valid_models = 0, valid_meshes = 0, valid_animations = 0, valid_materials = 0;
        std::set<std::string> model_materials;
        std::vector<AnimationUse> uses;
        std::map<std::filesystem::path, yami::Animation> animation_data;
        auto report_error = [&](const std::exception& e) { ++failures; std::cerr << "ERROR " << e.what() << '\n'; };
        for (const auto& path : models) {
            try {
                const auto model = yami::read_model(path);
                if (lower_path(path.lexically_relative(game).generic_string()) == "data/levels/level1/model/model.dat") {
                    const auto extents = yami::model_half_extents(model);
                    check(model.has_bounds && !model.has_collision_box && model.parts.empty() &&
                          model.bounds_min.x == 999999 && model.bounds_max.x == -999999 &&
                          extents.x == -999999 && extents.y == -999999 && extents.z == -999999,
                          "shipped empty descriptor bounds must remain signed, not sorted or absolutized");
                }
                ++valid_models;
                for (const auto& part : model.parts) {
                    yami::mesh_path(path, part);
                    const auto material = yami::resolve_asset(material_root, part.material_name + ".dat");
                    model_materials.insert(lower_path(material.lexically_relative(material_root).generic_string()));
                }
            } catch (const std::exception& e) { report_error(e); }
        }
        for (const auto& path : animations) {
            try {
                auto animation = yami::read_animation(path);
                ++valid_animations; pose_count += animation.frames.size();
                animation_data.emplace(path, std::move(animation));
            } catch (const std::exception& e) { report_error(e); }
        }
        for (const auto& path : meshes) {
            try {
                const auto mesh = yami::read_mesh(path);
                if (lower_path(path.lexically_relative(game).generic_string()) == "data/models/yaman/yaman.dat") {
                    check(mesh.animation_names.size() == 23 &&
                          mesh.animation_names[15] == "Yaman_anim_tekme_doner" &&
                          mesh.animation_names[19] == mesh.animation_names[15] &&
                          mesh.animation_names[16] == "Yaman_anim_tekme" &&
                          mesh.animation_names[17] == mesh.animation_names[16] &&
                          mesh.animation_names[21] == mesh.animation_names[16] &&
                          mesh.animation_names[18] == "Yaman_anim_tekme_kombo" &&
                          mesh.animation_names[20] == mesh.animation_names[18],
                          "00410350 action ordinals must retain shipped animation aliases");
                }
                ++valid_meshes;
                vertex_count += mesh.vertices.size(); index_count += mesh.indices.size(); bone_count += mesh.bones.size();
                for (const auto& name : mesh.animation_names) {
                    AnimationUse use;
                    use.mesh = path; use.animation = yami::animation_path(path, name);
                    for (const auto& bone : mesh.bones) use.bones.push_back(bone.name);
                    std::set<std::size_t> required;
                    for (std::size_t i = 0; i < mesh.bones.size(); ++i) {
                        if (mesh.bones[i].vertices.empty()) continue;
                        int bone = static_cast<int>(i);
                        while (bone >= 0) {
                            required.insert(static_cast<std::size_t>(bone));
                            bone = mesh.bones[static_cast<std::size_t>(bone)].parent;
                        }
                    }
                    use.required_ordinals.assign(required.begin(), required.end());
                    uses.push_back(std::move(use));
                }
                if (!mesh.bones.empty() && !mesh.animation_names.empty()) {
                    const auto it = animation_data.find(yami::animation_path(path, mesh.animation_names.front()));
                    check(it != animation_data.end(), path.string() + ": animation failed validation");
                    const auto& animation = it->second;
                    yami::SkinningState state;
                    yami::prepare_skinning(mesh, state);
                    yami::skin_mesh(mesh, animation, 0, 1, {}, state);
                    for (const auto& vertex : state.vertices)
                        check(std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) &&
                              std::isfinite(vertex.position.z) && std::isfinite(vertex.normal.x) &&
                              std::isfinite(vertex.normal.y) && std::isfinite(vertex.normal.z),
                              path.string() + ": nonfinite skinned vertex");
                }
            } catch (const std::exception& e) { report_error(e); }
        }
        for (const auto& use : uses) {
            const auto it = animation_data.find(use.animation);
            if (it == animation_data.end()) {
                ++failures; std::cerr << "ERROR animation reference not validated: " << use.animation << '\n';
                continue;
            }
            const auto& animation = it->second;
            for (const auto ordinal : use.required_ordinals) {
                if (ordinal >= animation.bone_names.size()) {
                    ++failures; std::cerr << "ERROR " << use.mesh << ": influencing bone " << ordinal
                                         << " absent in " << use.animation << '\n';
                }
            }
            if (animation.bone_names != use.bones) {
                ++name_mismatches;
                std::cout << "ORDINAL_ANIMATION " << use.mesh << " -> " << use.animation
                          << " (original sampler does not remap names; only influencing ordinals required)\n";
            }
        }
        for (const auto& path : materials) {
            try {
                const auto material = yami::read_material(path);
                if (lower_path(path.lexically_relative(material_root).generic_string()) == "level1/defaultlib_material196.dat") {
                    check(material.passes.size() == 1, "space-containing texture changed pass count");
                    const auto& pass = material.passes.front();
                    check(pass.texture_name == "data\\images\\canak anten.tga" &&
                          !pass.alpha_test && !pass.blend && pass.depth_test &&
                          pass.depth_function == 0x203 && pass.depth_write,
                          "newline filename/binary-state/EOF regression");
                    yami::resolve_asset(game, pass.texture_name);
                }
                ++valid_materials;
                for (const auto& pass : material.passes) {
                    try { yami::resolve_asset(game, pass.texture_name); }
                    catch (const std::exception& e) {
                        if (pass.video) ++missing_videos; else ++missing_images;
                        std::cout << (pass.video ? "MISSING_VIDEO " : "ORIGINAL_WHITE_IMAGE ")
                                  << path << ": " << e.what() << '\n';
                        // Corpus format validation is separate from unavailable dependencies.
                        // This does not give the runtime AVI loader an image-style white fallback.
                    }
                }
            } catch (const std::exception& e) {
                const auto key = lower_path(path.lexically_relative(material_root).generic_string());
                std::cerr << "MATERIAL_REACHABILITY " << path << ": "
                          << (model_materials.count(key) ? "referenced by model descriptor" : "not referenced by model descriptors")
                          << '\n';
                report_error(e);
            }
        }
        check(!models.empty() && !meshes.empty() && !animations.empty() && !materials.empty(), "asset inventory unexpectedly empty");
        std::cout << "Exact-consumption readers: models " << valid_models << '/' << models.size()
                  << ", meshes " << valid_meshes << '/' << meshes.size()
                  << ", animations " << valid_animations << '/' << animations.size()
                  << ", materials " << valid_materials << '/' << materials.size() << '\n'
                  << "Geometry: " << vertex_count << " vertices, " << index_count << " indices, "
                  << bone_count << " bones, " << pose_count << " animation poses\n"
                  << "Diagnostics: " << failures << " errors, " << missing_images << " missing image paths, "
                  << missing_videos << " missing video paths, "
                  << name_mismatches << " ordinal/name mismatches\n";
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
