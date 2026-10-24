#include "setup.hpp"
#include "install_ownership.hpp"

#include <SDL3/SDL.h>
#include <shobjidl.h>
#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

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
    throw std::runtime_error("Unsafe or cancelled installation was accepted");
}
std::wstring link_target(const fs::path& path, std::wstring* arguments = nullptr) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "COM initialization failed");
    struct Com { bool active; ~Com() { if (active) CoUninitialize(); } } com{SUCCEEDED(initialized)};
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))), "Cannot create link reader");
    struct Release { IShellLinkW* link; IPersistFile*& file; ~Release() { if (file) file->Release(); if (link) link->Release(); } } release{link, file};
    require(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Load(path.c_str(), STGM_READ)), "Cannot load shortcut");
    wchar_t target[32768]{}, args[4096]{};
    require(SUCCEEDED(link->GetPath(target, std::size(target), nullptr, SLGP_RAWPATH)) &&
            SUCCEEDED(link->GetArguments(args, std::size(args))), "Cannot read shortcut target");
    if (arguments) *arguments = args;
    return target;
}
void run_remover(const fs::path& root) {
    const auto helper = root/yami::ownership::helperDirectory/yami::ownership::helperName;
    std::wstring command = L"\"" + helper.native() + L"\" --root \"" + root.native() + L"\" --yes";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                           nullptr, root.c_str(), &startup, &process) != 0, "Cannot start installed remover");
    CloseHandle(process.hThread);
    require(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0, "Remover parent did not exit");
    DWORD code = 1; require(GetExitCodeProcess(process.hProcess, &code) != 0 && code == 0, "Remover parent failed");
    CloseHandle(process.hProcess);
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
        if (!fs::exists(helper) && !fs::exists(root/".yami-update-lock")) return;
        Sleep(100);
    }
    throw std::runtime_error("Detached remover did not finish");
}
}
int main() {
    try {
        int argc = 0;
        const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        require(argv != nullptr, "Cannot read Windows command line");
        struct Arguments { wchar_t** value; ~Arguments() { LocalFree(value); } } arguments{argv};
        require(argc == 6, "Usage: check-setup-install-windows ISO ENGINE 7ZZ REMOVER ISOLATED_CHECK_DIRECTORY");
        require(SDL_Init(0), SDL_GetError());
        struct Sdl { ~Sdl() { SDL_Quit(); } } sdl;
        const auto base = fs::absolute(fs::path(argv[5]));
        require(!fs::exists(base), "Check directory must not exist");
        fs::create_directories(base);
        InstallRequest request{{base/L"Türkçe space % dollar$ tick`"/"etiyami", base/"Start Menu", base/"Desktop"},
                               fs::path(argv[1]), fs::path(argv[2]), fs::path(argv[3]), fs::path(argv[4])};
        put(request.paths.root/"unrelated", "keep");
        rejected(request);
        require(get(request.paths.root/"unrelated") == "keep", "Unowned root changed");
        fs::remove(request.paths.root/"unrelated"); fs::remove(request.paths.root);
        put(request.paths.applications/yami::ownership::playShortcut, "unowned shortcut");
        rejected(request);
        require(get(request.paths.applications/yami::ownership::playShortcut) == "unowned shortcut", "Unowned shortcut changed");
        fs::remove(request.paths.applications/yami::ownership::playShortcut);
        rejected(request, [](std::string_view, std::uint64_t, std::uint64_t) { return false; });
        require(!fs::exists(request.paths.root), "Cancelled install published its root");
        bool extractionStarted = false;
        rejected(request, [&](std::string_view phase, std::uint64_t current, std::uint64_t) {
            if (phase == "Extracting assets" && current > 0) { extractionStarted = true; return false; }
            return true;
        });
        require(extractionStarted && !fs::exists(request.paths.root), "Mid-extraction cancellation published its root");
        const auto result = install(request, {});
        const auto& root = result.paths.root;
        require(!result.reused && fs::is_regular_file(root/"game/data/menu/menulist.xml") &&
                fs::is_regular_file(root/"yami-native.exe") && fs::is_regular_file(root/"yami-updater.exe") &&
                fs::is_regular_file(root/"yami-launcher.exe"), "Fresh Windows engine/assets missing");
        const auto menu = get(root/"game/data/menu/menulist.xml"), engine = get(root/"yami-launcher.exe");
        require(get(root/"eti-yami.ico") == get(root/"game/yami.ico"), "Shell icon is not the original ICO");
        const auto ownership = yami::ownership::parse_installed(get(root/yami::ownership::installedName));
        yami::ownership::parse_engine(get(root/yami::ownership::engineName));
        require(ownership.shortcuts.size() == 4, "Installer did not record four shortcuts");
        std::wstring linkArguments;
        require(fs::path(link_target(result.paths.applications/yami::ownership::playShortcut)) == root/"yami-launcher.exe",
                "Play shortcut target is wrong");
        require(fs::path(link_target(result.paths.applications/yami::ownership::uninstallShortcut, &linkArguments)) ==
                    root/yami::ownership::helperDirectory/yami::ownership::helperName &&
                linkArguments == L"--root \"" + root.native() + L"\"", "Uninstall shortcut target/arguments are wrong");
        put(root/"game.ini", "preserved settings");
        put(root/"checkpoints/slot1", "preserved save");
        put(root/"game/user-created.txt", "preserved unknown file");
        put(root/"user-owned.dll", "preserved unknown library");
        fs::remove(root/yami::ownership::helperDirectory/yami::ownership::helperName);
        fs::remove(result.paths.desktop/yami::ownership::uninstallShortcut);
        auto repair = request; repair.iso = base/"missing.iso"; repair.engine = base/"missing-engine";
        const auto reused = install(repair, {});
        require(reused.reused && get(root/"game/data/menu/menulist.xml") == menu &&
                get(root/"yami-launcher.exe") == engine && get(root/"game.ini") == "preserved settings" &&
                get(root/"checkpoints/slot1") == "preserved save" && get(root/"game/user-created.txt") == "preserved unknown file" &&
                get(root/"user-owned.dll") == "preserved unknown library" &&
                fs::is_regular_file(root/yami::ownership::helperDirectory/yami::ownership::helperName) &&
                fs::is_regular_file(result.paths.desktop/yami::ownership::uninstallShortcut),
                "Owned repair replaced data or failed support repair");
        const auto inventory = get(root/yami::ownership::installedName);
        rejected(repair, [](std::string_view phase, std::uint64_t, std::uint64_t) { return phase != "Publishing installation"; });
        require(get(root/yami::ownership::installedName) == inventory && get(root/"game.ini") == "preserved settings",
                "Cancelled repair changed installation");
        InstallRequest rollback = request;
        rollback.paths = {base/"rollback-install", base/"rollback-menu", base/"rollback-desktop"};
        rejected(rollback, [&](std::string_view phase, std::uint64_t, std::uint64_t) {
            if (phase == "Publishing installation") put(rollback.paths.applications/yami::ownership::uninstallShortcut, "unrelated");
            return true;
        });
        require(!fs::exists(rollback.paths.root) && !fs::exists(rollback.paths.applications/yami::ownership::playShortcut) &&
                get(rollback.paths.applications/yami::ownership::uninstallShortcut) == "unrelated" &&
                !fs::exists(rollback.paths.desktop/yami::ownership::playShortcut), "Publication failure did not roll back");
        run_remover(root);
        require(!fs::exists(root/"yami-native.exe") && !fs::exists(root/"game/data/menu/menulist.xml") &&
                !fs::exists(result.paths.applications/yami::ownership::playShortcut) &&
                !fs::exists(result.paths.desktop/yami::ownership::playShortcut) &&
                get(root/"game.ini") == "preserved settings" && get(root/"checkpoints/slot1") == "preserved save" &&
                get(root/"game/user-created.txt") == "preserved unknown file" && get(root/"user-owned.dll") == "preserved unknown library",
                "Installed remover did not preserve unowned data while removing owned files");
        fs::remove_all(base);
        std::cout << "Windows ISO install, owned repair, rollback, shortcuts and detached uninstall checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
