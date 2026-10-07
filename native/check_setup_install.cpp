#include "setup.hpp"
#include "install_ownership.hpp"
#include "media.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace yami::setup;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << value;
    require(bool(stream), "Cannot write fixture");
}
std::string get(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot read installed file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void rejected(const InstallRequest& request, const Progress& progress = {}) {
    try { install(request, progress); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Unsafe/cancelled installation was accepted");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 6, "Usage: check-setup-install ISO ENGINE 7ZZ REMOVER ISOLATED_CHECK_DIRECTORY");
        const auto base = fs::absolute(argv[5]);
        require(!fs::exists(base), "Check directory must not exist");
        fs::create_directories(base);
        InstallRequest request{{base/"Türkçe space % quote\" slash\\ dollar$ tick`"/"etiyami",
                                base/"applications", base/"Desktop"}, argv[1], argv[2], argv[3], argv[4]};
        put(request.paths.root/"unrelated", "keep");
        rejected(request);
        require(get(request.paths.root/"unrelated") == "keep", "Unowned root changed");
        fs::remove(request.paths.root/"unrelated"); fs::remove(request.paths.root);
        put(request.paths.applications/"eti-yami.desktop", "unowned shortcut");
        rejected(request);
        require(get(request.paths.applications/"eti-yami.desktop") == "unowned shortcut", "Unowned shortcut changed");
        fs::remove(request.paths.applications/"eti-yami.desktop");
        put(request.paths.desktop/"eti-yami-uninstall.desktop", "unowned removal shortcut");
        rejected(request);
        require(get(request.paths.desktop/"eti-yami-uninstall.desktop") == "unowned removal shortcut", "Unowned removal shortcut changed");
        fs::remove(request.paths.desktop/"eti-yami-uninstall.desktop");
        auto missingRemover = request; missingRemover.remover = base/"missing-remover";
        rejected(missingRemover);
        require(!fs::exists(request.paths.root), "Missing removal helper published installation");
        missingRemover.remover.clear(); rejected(missingRemover);
        auto shared = request;
        shared.paths.root = base/"shared-parent"/"install";
        fs::create_directory(shared.paths.root.parent_path());
        fs::permissions(shared.paths.root.parent_path(), fs::perms::all);
        rejected(shared);
        require(!fs::exists(shared.paths.root), "Shared writable parent accepted");
        fs::permissions(shared.paths.root.parent_path(), fs::perms::owner_all);
        rejected(request, [](std::string_view, std::uint64_t, std::uint64_t) { return false; });
        require(!fs::exists(request.paths.root), "Cancelled install published its root");
        bool extractionStarted = false;
        rejected(request, [&](std::string_view phase, std::uint64_t current, std::uint64_t) {
            if (phase == "Extracting assets" && current > 0) { extractionStarted = true; return false; }
            return true;
        });
        require(extractionStarted && !fs::exists(request.paths.root), "Mid-extraction cancellation published its root");
        auto wrong = request; wrong.iso = base/"wrong.iso"; put(wrong.iso, "not an ISO");
        rejected(wrong);
        require(!fs::exists(request.paths.root), "Wrong ISO published its root");
        rejected(request, [](std::string_view phase, std::uint64_t, std::uint64_t) {
            return phase != "Publishing installation";
        });
        require(!fs::exists(request.paths.root), "Last-minute cancellation published its root");
        require(!fs::exists(request.paths.desktop/"eti-yami.desktop"), "Cancelled install published Desktop shortcut");
        require(!fs::exists(request.paths.desktop/"eti-yami-uninstall.desktop"), "Cancelled install published uninstall shortcut");
        const auto result = install(request, {});
        require(!result.reused, "Fresh install reported reused");
        require(fs::is_regular_file(result.paths.root/"game/data/menu/menulist.xml"), "Native assets missing");
        require(!fs::exists(result.paths.root/"game/eti.exe"), "Windows executable installed");
        require(fs::is_regular_file(result.paths.root/"yami-updater"), "Updater missing");
        const auto original = get(result.paths.root/"game/data/menu/menulist.xml");
        const auto engine = get(result.paths.root/"yami-launcher");
        const auto icon = get(result.paths.root/"eti-yami.png");
        yami::VideoDecoder ico(result.paths.root/"game/yami.ico"), png(result.paths.root/"eti-yami.png");
        yami::VideoFrame a, b;
        require(ico.next(a) && png.next(b) && a.width == b.width && a.height == b.height &&
                std::equal(a.rgba.begin(), a.rgba.end(), b.rgba.begin(), b.rgba.end()), "Shortcut icon is not the original icon");
        const auto shortcut = get(result.paths.applications/"eti-yami.desktop");
        require(shortcut == get(result.paths.desktop/"eti-yami.desktop"), "Desktop/app-menu entries differ");
        require((fs::status(result.paths.desktop/"eti-yami.desktop").permissions() & fs::perms::owner_exec) != fs::perms::none,
                "Desktop shortcut is not executable");
        const auto uninstall = get(result.paths.applications/"eti-yami-uninstall.desktop");
        require(uninstall == get(result.paths.desktop/"eti-yami-uninstall.desktop") &&
                uninstall.find("\nTerminal=true\n") != std::string::npos &&
                uninstall.find("Exec=/usr/bin/env -- /bin/sh ") != std::string::npos,
                "Uninstall shortcuts missing terminal-safe script command");
        require(get(result.paths.root/"uninstall.sh") == get(fs::path(argv[4]).parent_path()/"uninstall.sh") &&
                get(result.paths.root/".eti-yami-uninstall/yami-remove") == get(argv[4]), "Removal support differs from trusted source");
        const auto ownership = yami::ownership::parse_installed(get(result.paths.root/yami::ownership::installedName));
        require(ownership.shortcuts.size() == 4 &&
                !std::binary_search(ownership.files.begin(), ownership.files.end(), std::string("yami-launcher")),
                "Installer inventory lacks four shortcuts or mixes engine ownership");
        yami::ownership::parse_engine(get(result.paths.root/yami::ownership::engineName));
        put(result.paths.root/"game.ini", "preserved settings");
        put(result.paths.root/"checkpoints/slot1", "preserved save");
        put(result.paths.root/"game/user-created.txt", "preserved unknown file");
        put(result.paths.root/"libuser.so", "preserved unknown library");
        fs::remove(result.paths.root/"uninstall.sh");
        fs::remove(result.paths.root/".eti-yami-uninstall/yami-remove");
        fs::remove(result.paths.desktop/"eti-yami-uninstall.desktop");
        fs::remove(result.paths.desktop/"eti-yami.desktop");
        auto reuse = request; reuse.iso = base/"missing.iso"; reuse.engine = base/"missing-engine";
        const auto reused = install(reuse, {});
        require(reused.reused && get(result.paths.root/"game/data/menu/menulist.xml") == original &&
                get(result.paths.root/"game.ini") == "preserved settings" &&
                get(result.paths.root/"checkpoints/slot1") == "preserved save" &&
                get(result.paths.desktop/"eti-yami.desktop") == shortcut, "Reinstall modified data or failed shortcut repair");
        require(get(result.paths.root/"yami-launcher") == engine && get(result.paths.root/"eti-yami.png") == icon,
                "Reinstall overwrote engine or installed original icon");
        require(get(result.paths.root/"uninstall.sh") == get(fs::path(argv[4]).parent_path()/"uninstall.sh") &&
                get(result.paths.root/".eti-yami-uninstall/yami-remove") == get(argv[4]) &&
                get(result.paths.desktop/"eti-yami-uninstall.desktop") == uninstall &&
                get(result.paths.root/"game/user-created.txt") == "preserved unknown file" &&
                get(result.paths.root/"libuser.so") == "preserved unknown library",
                "Repair failed removal support or changed unknown files");
        rejected(reuse, [](std::string_view phase, std::uint64_t, std::uint64_t) { return phase != "Publishing installation"; });
        require(get(result.paths.root/"game.ini") == "preserved settings" &&
                get(result.paths.root/yami::ownership::installedName) == yami::ownership::serialize_installed(ownership),
                "Cancelled repair changed installation");
        const auto savedInventory = get(result.paths.root/yami::ownership::installedName);
        put(result.paths.root/yami::ownership::installedName, "invalid ownership");
        rejected(reuse);
        require(get(result.paths.root/"game.ini") == "preserved settings", "Malformed ownership changed settings");
        put(result.paths.root/yami::ownership::installedName, savedInventory);
        fs::remove(result.paths.root/"uninstall.sh");
        put(base/"outside-script", "do not overwrite");
        fs::create_symlink(base/"outside-script", result.paths.root/"uninstall.sh");
        rejected(reuse);
        require(get(base/"outside-script") == "do not overwrite", "Repair followed support symlink");
        fs::remove(result.paths.root/"uninstall.sh");
        install(reuse, {});
        fs::create_directory(result.paths.root/".yami-update-lock");
        put(result.paths.root/".yami-update-lock/owner", "other operation");
        rejected(reuse);
        require(get(result.paths.root/".yami-update-lock/owner") == "other operation", "Repair broke another operation's lock");
        fs::remove(result.paths.root/".yami-update-lock/owner"); fs::remove(result.paths.root/".yami-update-lock");
        // Replace both source leaves after the installer pins the real support files.
        auto pinnedReuse = reuse;
        pinnedReuse.remover = base/"trusted-source/yami-remove";
        fs::create_directory(pinnedReuse.remover.parent_path());
        fs::copy_file(argv[4], pinnedReuse.remover);
        fs::copy_file(fs::path(argv[4]).parent_path()/"uninstall.sh", pinnedReuse.remover.parent_path()/"uninstall.sh");
        fs::permissions(pinnedReuse.remover.parent_path(), fs::perms::owner_all | fs::perms::group_all);
        bool sourceReplaced = false;
        install(pinnedReuse, [&](std::string_view phase, std::uint64_t, std::uint64_t) {
            if (phase == "Copying pinned removal support" && !sourceReplaced) {
                fs::rename(pinnedReuse.remover, pinnedReuse.remover.parent_path()/"original-helper");
                fs::rename(pinnedReuse.remover.parent_path()/"uninstall.sh", pinnedReuse.remover.parent_path()/"original-script");
                put(pinnedReuse.remover, "unrelated substituted helper");
                put(pinnedReuse.remover.parent_path()/"uninstall.sh", "unrelated substituted script");
                sourceReplaced = true;
            }
            return true;
        });
        require(sourceReplaced && get(result.paths.root/".eti-yami-uninstall/yami-remove") == get(argv[4]) &&
                get(result.paths.root/"uninstall.sh") == get(fs::path(argv[4]).parent_path()/"uninstall.sh") &&
                get(pinnedReuse.remover) == "unrelated substituted helper" &&
                get(pinnedReuse.remover.parent_path()/"uninstall.sh") == "unrelated substituted script",
                "Source-leaf substitution altered installed support or erased unrelated replacements");
        // A format1 legacy root requires actual ISO provenance, never a live tree scan.
        fs::remove(result.paths.root/yami::ownership::installedName);
        fs::remove(result.paths.root/yami::ownership::engineName);
        rejected(reuse);
        require(get(result.paths.root/"game/user-created.txt") == "preserved unknown file", "Rejected migration changed data");
        rejected(request, [](std::string_view phase, std::uint64_t current, std::uint64_t) {
            return phase != "Extracting assets" || current == 0;
        });
        require(!fs::exists(result.paths.root/yami::ownership::installedName) &&
                get(result.paths.root/"yami-launcher") == engine, "Cancelled legacy migration published ownership or overwrote engine");
        const auto originalIco = get(result.paths.root/"game/yami.ico");
        put(result.paths.root/"game/yami.ico", "user modified original asset");
        put(result.paths.root/"eti-yami.png", icon + "\nuser icon customization\n");
        const auto migrated = install(request, {});
        const auto migratedOwnership = yami::ownership::parse_installed(get(result.paths.root/yami::ownership::installedName));
        require(migrated.reused && get(result.paths.root/"yami-launcher") == engine &&
                get(result.paths.root/"game.ini") == "preserved settings" &&
                !std::binary_search(migratedOwnership.files.begin(), migratedOwnership.files.end(), std::string("game/user-created.txt")) &&
                !std::binary_search(migratedOwnership.files.begin(), migratedOwnership.files.end(), std::string("libuser.so")) &&
                !std::binary_search(migratedOwnership.files.begin(), migratedOwnership.files.end(), std::string("game/yami.ico")) &&
                get(result.paths.root/"game/yami.ico") == "user modified original asset" &&
                !std::binary_search(migratedOwnership.files.begin(), migratedOwnership.files.end(), std::string("eti-yami.png")) &&
                get(result.paths.root/"eti-yami.png") == icon + "\nuser icon customization\n",
                "Legacy migration claimed unknown/modified data or overwrote engine");
        put(result.paths.root/"game/yami.ico", originalIco);
        put(result.paths.root/"eti-yami.png", icon);
        // Preserve the old app-menu entry when the second publication collides.
        fs::remove(result.paths.desktop/"eti-yami.desktop");
        rejected(reuse, [&](std::string_view phase, std::uint64_t, std::uint64_t) {
            if (phase == "Publishing installation") put(result.paths.desktop/"eti-yami.desktop", "unrelated");
            return true;
        });
        require(get(result.paths.applications/"eti-yami.desktop") == shortcut &&
                get(result.paths.desktop/"eti-yami.desktop") == "unrelated" &&
                get(result.paths.root/"yami-launcher") == engine, "Owned reinstall rollback damaged existing data");
        fs::remove(result.paths.desktop/"eti-yami.desktop");
        install(reuse, {});
        // Fail after the app-menu publication to exercise rollback of both a repaired
        // shortcut and the newly published root, without touching a real user folder.
        InstallRequest rollback = request;
        rollback.paths.root = base/"rollback-install";
        rollback.paths.applications = base/"rollback-applications";
        rollback.paths.desktop = base/"rollback-Desktop";
        bool blocked = false;
        rejected(rollback, [&](std::string_view phase, std::uint64_t, std::uint64_t) {
            if (phase == "Publishing installation" && !blocked) {
                put(rollback.paths.desktop/"eti-yami-uninstall.desktop", "concurrent unrelated shortcut"); blocked = true;
            }
            return true;
        });
        require(blocked && !fs::exists(rollback.paths.root) &&
                !fs::exists(rollback.paths.applications/"eti-yami.desktop") &&
                !fs::exists(rollback.paths.applications/"eti-yami-uninstall.desktop") &&
                !fs::exists(rollback.paths.desktop/"eti-yami.desktop") &&
                get(rollback.paths.desktop/"eti-yami-uninstall.desktop") == "concurrent unrelated shortcut", "Four-entry publication failure did not roll back");
        // A directory substituted after publication is not ours to roll back.
        auto substituted = request;
        substituted.paths.root = base/"substituted-install";
        substituted.paths.applications = base/"substituted-applications";
        substituted.paths.desktop = base/"substituted-Desktop";
        const auto displaced = base/"displaced-install";
        bool replaced = false;
        rejected(substituted, [&](std::string_view phase, std::uint64_t, std::uint64_t) {
            if (phase == "Creating shortcuts") {
                fs::rename(substituted.paths.root, displaced);
                put(substituted.paths.root/"unrelated", "keep unrelated replacement");
                put(substituted.paths.desktop/"eti-yami.desktop", "publication collision");
                replaced = true;
            }
            return true;
        });
        require(replaced && get(substituted.paths.root/"unrelated") == "keep unrelated replacement" &&
                fs::is_regular_file(displaced/"yami-launcher"), "Rollback deleted an unrelated replacement");
        for (const auto& entry : fs::recursive_directory_iterator(base))
            require(!entry.path().filename().string().starts_with(".yami-setup-"), "Installer staging leaked");
        std::cout << "Setup install checks passed; validate/launch: " << result.paths.applications/"eti-yami.desktop" << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
