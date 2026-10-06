#include "update_install.hpp"
#include <SDL3/SDL.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace yami::updates;
namespace {
#ifdef _WIN32
constexpr auto native = "yami-native.exe";
constexpr auto updater = "yami-updater.exe";
constexpr auto launcher = "yami-launcher.exe";
constexpr auto library = "lib/zz-check.dll";
constexpr auto addedLibrary = "first-check.dll";
#elif defined(__APPLE__)
constexpr auto native = "yami-native";
constexpr auto updater = "yami-updater";
constexpr auto launcher = "yami-launcher.app/Contents/MacOS/yami-launcher";
constexpr auto library = "lib/libzz-check.dylib";
constexpr auto addedLibrary = "lib-check.dylib";
#else
constexpr auto native = "yami-native";
constexpr auto updater = "yami-updater";
constexpr auto launcher = "yami-launcher";
constexpr auto library = "lib/libzz-check.so.1";
constexpr auto addedLibrary = "lib-check.so.1";
#endif
void put(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    if (!stream) throw std::runtime_error("Cannot write check file");
}
std::string get(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void payload(const fs::path& stage, std::string_view text) {
    for (const auto* path : {native, updater, launcher, library}) {
        put(stage/"payload"/path, text);
#ifndef _WIN32
        if (std::string_view(path) != library)
            fs::permissions(stage/"payload"/path, fs::perms::owner_exec, fs::perm_options::add);
#endif
    }
}
std::string fails(const fs::path& stage, const fs::path& root) {
    try { install_prepared(stage, root); }
    catch (const std::exception& error) { return error.what(); }
    throw std::runtime_error("Expected installer failure did not occur");
}
struct Temporary {
    fs::path root;
    Temporary() {
        for (unsigned i = 0; i < 100; ++i) {
            root = fs::canonical(fs::temp_directory_path())/("yami-install-check-" +
                std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(i));
            if (fs::create_directory(root)) return;
        }
        throw std::runtime_error("Cannot create isolated check root");
    }
    ~Temporary() {
        std::error_code error;
        fs::permissions(root/"lib", fs::perms::owner_all, fs::perm_options::add, error);
        fs::remove_all(root, error);
    }
};
}

int main() {
    Temporary temporary;
    const auto& root = temporary.root;
    const auto stage = root/".yami-update-check";
    const auto protectStage = [&] {
        fs::permissions(stage, fs::perms::owner_all, fs::perm_options::replace);
    };
    for (const auto* path : {native, updater, launcher, library}) put(root/path, "old-engine");
    put(root/"game/data/original.bin", "original-assets");
    put(root/"game.ini", "preferences");
    put(root/"checkpoints/slot1", "checkpoint");
    put(root/".git/config", "private-repository");
#ifdef __APPLE__
    put(root/"yami-launcher.app/Contents/Resources/game/data/original.bin", "bundle-assets");
#endif
    payload(stage, "new-engine");
    protectStage();
    fs::create_directory(root/".yami-update-lock");
    const auto busy = fails(stage, root);
    assert(busy.find(".yami-update-lock") != std::string::npos);
    for (const auto* path : {native, updater, launcher, library}) assert(get(root/path) == "old-engine");
    assert(!fs::exists(stage/"backup"));
    fs::remove(root/".yami-update-lock");
    install_prepared(stage, root);
    for (const auto* path : {native, updater, launcher, library}) assert(get(root/path) == "new-engine");
    assert(!fs::exists(stage/"backup"));
    assert(!fs::exists(root/".yami-update-lock"));
    detail::mark_completed(stage);
    cleanup_completed_updates(root);
    assert(fs::exists(stage)); // A live helper's marked stage must not be deleted.

    // Validation failures must not alter a single installed member.
    fs::remove_all(stage);
    payload(stage, "rejected-engine");
    protectStage();
    put(stage/"payload/game.ini", "stolen-preferences");
    fails(stage, root);
    fs::remove(stage/"payload/game.ini");
    fs::create_hard_link(stage/"payload"/native, stage/"extra-link");
    fails(stage, root);
    fs::remove(stage/"extra-link");
#ifndef _WIN32
    fs::remove(stage/"payload"/updater);
    fs::create_symlink(root/updater, stage/"payload"/updater);
    fails(stage, root);
    fs::remove(stage/"payload"/updater);
    put(stage/"payload"/updater, "rejected-engine");
#endif
    for (const auto* path : {native, updater, launcher, library}) assert(get(root/path) == "new-engine");

    // A real late filesystem failure follows binary replacement and exercises backup rollback.
    payload(stage, "rolled-back-engine");
    put(stage/"payload"/addedLibrary, "new-file-rolled-back");
#ifdef _WIN32
    HANDLE locked = CreateFileW((root/library).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    const auto rollbackFailure = fails(stage, root);
    CloseHandle(locked);
#else
    if (geteuid() == 0) throw std::runtime_error("Run installer permission/rollback check as a non-root user");
    fs::permissions(root/"lib", fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    const auto rollbackFailure = fails(stage, root);
    fs::permissions(root/"lib", fs::perms::owner_all, fs::perm_options::replace);
#endif
    assert(rollbackFailure.starts_with("Update failed; previous installation restored:"));
    assert(!fs::exists(root/addedLibrary));
    assert(get(stage/"payload"/addedLibrary) == "new-file-rolled-back");
    for (const auto* path : {native, updater, launcher, library}) {
        assert(get(root/path) == "new-engine");
        assert(get(stage/"payload"/path) == "rolled-back-engine");
    }
    assert(!fs::exists(stage/"backup"));
    assert(!fs::exists(root/".yami-update-lock"));
    assert(get(root/"game/data/original.bin") == "original-assets");
    assert(get(root/"game.ini") == "preferences");
    assert(get(root/"checkpoints/slot1") == "checkpoint");
    assert(get(root/".git/config") == "private-repository");
#ifdef __APPLE__
    assert(get(root/"yami-launcher.app/Contents/Resources/game/data/original.bin") == "bundle-assets");
#endif
    assert(!detail::valid_payload_path("lib/../game.ini", false));
    assert(!detail::valid_payload_path("lib/test.dll:stream", false));
    assert(!detail::valid_payload_path("Resources/game/data/a", false));
    std::cout << "Installer replacement, late-failure rollback, unsafe-file rejection, and user/assets preservation checked\n";
}
