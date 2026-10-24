#include "update_install.hpp"
#include "install_ownership.hpp"
#include "setup_windows.hpp"

#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << value;
    require(bool(out), "Cannot write updater fixture");
}
std::string get(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void payload(const fs::path& stage, std::string_view value, bool added = true) {
    for (const auto* name : {"yami-native.exe", "yami-launcher.exe", "yami-updater.exe"}) put(stage/"payload"/name, value);
    if (added) put(stage/"payload/new-check.dll", "new owned DLL");
}
struct Temporary {
    fs::path path;
    Temporary() {
        path = fs::temp_directory_path()/("yami-windows-update-check-" + std::to_string(GetCurrentProcessId()) + "-" +
                                          std::to_string(GetTickCount64()));
        require(fs::create_directory(path), "Cannot create updater check directory");
    }
    ~Temporary() { std::error_code error; fs::remove_all(path, error); }
};
}
int main() {
    try {
        require(SDL_Init(0), SDL_GetError());
        struct Sdl { ~Sdl() { SDL_Quit(); } } sdl;
        Temporary temporary; const auto root = temporary.path/"install";
        for (const auto* name : {"yami-native.exe", "yami-launcher.exe", "yami-updater.exe", "old-check.dll"}) put(root/name, "old");
        put(root/"game/data/original.bin", "assets"); put(root/"game.ini", "settings"); put(root/"unknown.dll", "unknown");
        put(root/yami::ownership::helperDirectory/yami::ownership::helperName, "helper");
        put(root/yami::ownership::markerName, yami::ownership::marker);
        const auto installed = yami::ownership::serialize_installed({
            {".eti-yami-engine-files", ".eti-yami-install", ".eti-yami-installed-files",
             ".eti-yami-uninstall/yami-remove.exe", "game/data/original.bin"}, {}});
        put(root/yami::ownership::installedName, installed);
        put(root/yami::ownership::engineName, yami::ownership::serialize_engine(
            {"old-check.dll", "yami-launcher.exe", "yami-native.exe", "yami-updater.exe"}));
        const auto stage = root/".yami-update-check";
        payload(stage, "new");
        yami::updates::detail::secure_stage_directory(stage);
        yami::updates::install_prepared(stage, root);
        for (const auto* name : {"yami-native.exe", "yami-launcher.exe", "yami-updater.exe"}) require(get(root/name) == "new", "Engine not updated");
        require(get(root/"new-check.dll") == "new owned DLL" && get(root/"old-check.dll") == "old" &&
                get(root/"unknown.dll") == "unknown" && get(root/"game.ini") == "settings" &&
                get(root/"game/data/original.bin") == "assets", "Update changed unowned data");
        require(get(root/yami::ownership::engineName) == yami::ownership::serialize_engine(
            {"new-check.dll", "old-check.dll", "yami-launcher.exe", "yami-native.exe", "yami-updater.exe"}),
                "Updater did not merge new DLL into engine ownership");
        require(get(root/yami::ownership::installedName) == installed, "Updater changed installed-asset ownership");
        fs::remove_all(stage); payload(stage, "rejected"); yami::updates::detail::secure_stage_directory(stage);
        bool rejected = false;
        put(root/"collision.dll", "user collision"); put(stage/"payload/collision.dll", "update collision");
        try { yami::updates::install_prepared(stage, root); } catch (const std::exception&) { rejected = true; }
        require(rejected && get(root/"collision.dll") == "user collision" &&
                get(root/yami::ownership::engineName).find("collision.dll") == std::string::npos,
                "Updater replaced or claimed an unrecorded user DLL");
        fs::remove(stage/"payload/collision.dll");
        put(stage/"payload/lib/nested.dll", "nested payload");
        rejected = false;
        try { yami::updates::install_prepared(stage, root); } catch (const std::exception&) { rejected = true; }
        require(rejected && !fs::exists(root/"lib/nested.dll"), "Nested Windows update payload was accepted");
        fs::remove_all(stage/"payload/lib");
        require(CreateHardLinkW((root/"ownership-hardlink").c_str(), (root/yami::ownership::engineName).c_str(), nullptr) != 0,
                "Cannot create ownership hard-link fixture");
        rejected = false;
        try { yami::updates::install_prepared(stage, root); } catch (const std::exception&) { rejected = true; }
        require(rejected && get(root/"yami-native.exe") == "new" && get(root/"game.ini") == "settings",
                "Hard-linked ownership was accepted or update changed data");
        fs::remove(root/"ownership-hardlink");
        std::cout << "Windows updater engine-ownership merge and preservation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
