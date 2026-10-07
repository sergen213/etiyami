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
#if defined(__linux__)
    // Old installer markers without inventories remain update-compatible, but unowned.
    put(root/".eti-yami-install", "ETI Yami native Linux installation\nformat=1\n");
    fs::permissions(root/".eti-yami-install", fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif
    install_prepared(stage, root);
    for (const auto* path : {native, updater, launcher, library}) assert(get(root/path) == "new-engine");
    assert(!fs::exists(stage/"backup"));
    assert(!fs::exists(root/".yami-update-lock"));
#if defined(__linux__)
    // Manual engine ZIP installations must never acquire uninstall authority.
    assert(!fs::exists(root/".eti-yami-engine-files"));
    assert(!fs::exists(root/".eti-yami-installed-files"));
    const auto ownedMetadata = [&](const fs::path& path, std::string_view contents) {
        put(path, contents);
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    };
    ownedMetadata(root/".eti-yami-install", "ETI Yami native Linux installation\nformat=1\n");
    const std::string installedInventory =
        "ETI Yami installed ownership\nformat=1\n"
        "F\t.eti-yami-engine-files\nF\t.eti-yami-install\nF\t.eti-yami-installed-files\n"
        "F\t.eti-yami-uninstall/yami-remove\nF\tgame/data/original.bin\nF\tuninstall.sh\n";
    ownedMetadata(root/".eti-yami-installed-files", installedInventory);
    ownedMetadata(root/".eti-yami-engine-files",
        "ETI Yami engine ownership\nformat=1\nlib-retired-check.so.1\nyami-launcher\nyami-native\nyami-updater\n");
    put(root/"uninstall.sh", "installer-owned-script");
    put(root/".eti-yami-uninstall/yami-remove", "installer-owned-helper");
    put(root/"lib-user-check.so.1", "unknown-library");
    put(root/"lib-retired-check.so.1", "previously-owned-library");
    fs::remove_all(stage);
    payload(stage, "new-engine");
    put(stage/"payload/lib-owned-check.so.2", "trusted-added-library");
    protectStage();
    install_prepared(stage, root);
    const auto mergedInventory = get(root/".eti-yami-engine-files");
    assert(mergedInventory ==
        "ETI Yami engine ownership\nformat=1\nlib-owned-check.so.2\nlib-retired-check.so.1\n"
        "lib/libzz-check.so.1\nyami-launcher\nyami-native\nyami-updater\n");
    assert(get(root/"lib-owned-check.so.2") == "trusted-added-library");
    assert(get(root/"lib-retired-check.so.1") == "previously-owned-library");
    assert(get(root/".eti-yami-install") == "ETI Yami native Linux installation\nformat=1\n");
    assert(get(root/"lib-user-check.so.1") == "unknown-library");
    assert(get(root/".eti-yami-installed-files") == installedInventory);
    assert(get(root/"uninstall.sh") == "installer-owned-script");
    assert(get(root/".eti-yami-uninstall/yami-remove") == "installer-owned-helper");
    assert(fs::status(root/".eti-yami-engine-files").permissions() ==
           (fs::perms::owner_read | fs::perms::owner_write));
#endif
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
#if defined(__linux__)
    put(stage/"payload/.eti-yami-engine-files", "wire-ownership-is-forbidden");
    fails(stage, root);
    fs::remove(stage/"payload/.eti-yami-engine-files");
    put(stage/"payload/yami-remove", "fourth-executable-is-forbidden");
    fails(stage, root);
    fs::remove(stage/"payload/yami-remove");
    ownedMetadata(root/".eti-yami-engine-files",
                  "ETI Yami engine ownership\nformat=1\n../game.ini\n");
    fails(stage, root);
    ownedMetadata(root/".eti-yami-engine-files", mergedInventory);
    fs::create_hard_link(root/".eti-yami-engine-files", root/"ownership-hardlink");
    fails(stage, root);
    fs::remove(root/"ownership-hardlink");
#endif
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
#if defined(__linux__)
    assert(get(root/".eti-yami-engine-files") == mergedInventory);
    assert(mergedInventory.find(addedLibrary) == std::string::npos);
    assert(get(root/".eti-yami-installed-files") == installedInventory);
    assert(get(root/"uninstall.sh") == "installer-owned-script");
    assert(get(root/".eti-yami-uninstall/yami-remove") == "installer-owned-helper");
    assert(get(root/"lib-user-check.so.1") == "unknown-library");
    assert(get(root/"lib-owned-check.so.2") == "trusted-added-library");
#endif
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
    std::cout << "Installer replacement, local ownership provenance, late-failure rollback, unsafe-file rejection, and user/assets preservation checked\n";
}
