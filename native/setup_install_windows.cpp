#include "setup.hpp"
#include "install_ownership.hpp"
#include "setup_windows.hpp"

#include <SDL3/SDL.h>
#include <shobjidl.h>
#include <objbase.h>

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace yami::setup {
namespace {
namespace fs = std::filesystem;
using namespace yami::ownership;
using namespace yami::windows_install;
constexpr std::array executables{"yami-native.exe", "yami-updater.exe", "yami-launcher.exe"};
constexpr std::string_view iconName = "eti-yami.ico";

void report(const Progress& progress, std::string_view phase, std::uint64_t current = 0, std::uint64_t total = 0) {
    if (progress && !progress(phase, current, total)) throw std::runtime_error("Installation cancelled");
}
std::string folded(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
std::string quoted(const fs::path& path) { return "\"" + text(path) + "\""; }

struct ShellLink {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    bool uninitialize = false;
    ShellLink() {
        try {
            const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize Windows shortcut services");
            uninitialize = SUCCEEDED(initialized);
            const auto created = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(&link));
            require(SUCCEEDED(created), "Cannot create a Windows shell link");
            const auto persisted = link->QueryInterface(IID_PPV_ARGS(&file));
            require(SUCCEEDED(persisted), "Cannot access Windows shell-link persistence");
        } catch (...) {
            if (file) file->Release();
            if (link) link->Release();
            if (uninitialize) CoUninitialize();
            throw;
        }
    }
    ~ShellLink() {
        if (file) file->Release();
        if (link) link->Release();
        if (uninitialize) CoUninitialize();
    }
};
struct LinkShape {
    fs::path target, working, icon;
    std::string arguments;
};
LinkShape expected_link(const fs::path& root, bool uninstall) {
    return {uninstall ? root/helperDirectory/helperName : root/"yami-launcher.exe", root,
            root/iconName, uninstall ? "--root " + quoted(root) : std::string()};
}
void check_link_shape(IShellLinkW* link, const LinkShape& expected) {
    wchar_t target[32768]{}, working[32768]{}, icon[32768]{}, arguments[4096]{};
    int iconIndex = 0;
    require(SUCCEEDED(link->GetPath(target, std::size(target), nullptr, SLGP_RAWPATH)) &&
            SUCCEEDED(link->GetWorkingDirectory(working, std::size(working))) &&
            SUCCEEDED(link->GetIconLocation(icon, std::size(icon), &iconIndex)) &&
            SUCCEEDED(link->GetArguments(arguments, std::size(arguments))),
            "Cannot inspect Windows shortcut ownership");
    const auto actualTarget = absolute_path(target), actualWorking = absolute_path(working), actualIcon = absolute_path(icon);
    require(path_equal(actualTarget, expected.target) && path_equal(actualWorking, expected.working) &&
            path_equal(actualIcon, expected.icon) && iconIndex == 0 && expected.arguments == text(arguments),
            "Shortcut does not belong to this ETI Yami installation");
}
bool shortcut_exists(const fs::path& path, const fs::path& root, bool uninstall) {
    if (!fs::exists(checked_status(path))) return false;
    directory_chain(path.parent_path());
    const auto before = path_identity(path, true);
    ShellLink shell;
    require(SUCCEEDED(shell.file->Load(path.c_str(), STGM_READ)), "Cannot read existing Windows shortcut: " + text(path));
    check_link_shape(shell.link, expected_link(root, uninstall));
    require(same(path_identity(path, true), before), "Shortcut changed while its ownership was checked");
    return true;
}
void create_shortcut(const fs::path& staged, const fs::path& root, bool uninstall) {
    ShellLink shell;
    const auto expected = expected_link(root, uninstall);
    require(SUCCEEDED(shell.link->SetPath(expected.target.c_str())) &&
            SUCCEEDED(shell.link->SetWorkingDirectory(expected.working.c_str())) &&
            SUCCEEDED(shell.link->SetIconLocation(expected.icon.c_str(), 0)) &&
            SUCCEEDED(shell.link->SetArguments(utf8_path(expected.arguments).c_str())) &&
            SUCCEEDED(shell.link->SetDescription(uninstall ? L"Uninstall ETI Yami; preserve saves and settings" : L"Play ETI Yami")),
            "Cannot configure Windows shortcut");
    require(SUCCEEDED(shell.file->Save(staged.c_str(), TRUE)), "Cannot save staged Windows shortcut");
    const auto identity = path_identity(staged, true);
    (void)identity;
}
struct Publication {
    fs::path destination, staged, backup;
    Identity identity{};
    bool backedUp = false, published = false;
    Publication(fs::path target, fs::path fresh, fs::path previous, bool inspect = true)
        : destination(std::move(target)), staged(std::move(fresh)), backup(std::move(previous)),
          identity(inspect ? path_identity(staged, true) : Identity{}) {}
    std::function<void()> verify;
    void publish() {
        if (fs::exists(checked_status(destination))) {
            const auto expected = path_identity(destination, true);
            if (verify) verify();
            rename_new(destination, backup); backedUp = true;
            require(same(path_identity(backup, true), expected), "Owned file changed during publication");
        }
        rename_new(staged, destination); published = true;
    }
    void rollback() {
        if (published) {
            require(same(path_identity(destination, true), identity),
                    "Published file changed concurrently; refusing rollback deletion: " + text(destination));
            rename_new(destination, staged); published = false;
        }
        if (backedUp) { rename_new(backup, destination); backedUp = false; }
    }
};
struct ShortcutPublication {
    TemporaryDirectory staging;
    Publication publication;
    ShortcutPublication(const fs::path& folder, std::string_view name, const fs::path& root, bool uninstall)
        : staging(folder), publication(folder/name, staging.path/"new.lnk", staging.path/"backup.lnk", false) {
        create_shortcut(staging.path/"new.lnk", root, uninstall);
        publication.identity = path_identity(staging.path/"new.lnk", true);
        publication.verify = [destination = publication.destination, root, uninstall] {
            require(shortcut_exists(destination, root, uninstall), "Owned shortcut disappeared during publication");
        };
    }
};
struct RepairLock {
    fs::path root, path;
    Identity rootIdentity{}, identity{}, ownerIdentity{};
    bool retain = false;
    RepairLock(const fs::path& installation, const fs::path& stage) : root(installation), path(root/".yami-update-lock") {
        rootIdentity = path_identity(root, true);
        require(CreateDirectoryW(path.c_str(), nullptr) != 0,
                "Another update, removal or repair owns .yami-update-lock; do not break the lock");
        try {
            secure(path, true);
            identity = path_identity(path, true);
            write_new(path/"owner", "pid=" + std::to_string(GetCurrentProcessId()) + "\nstage=" + text(stage) + "\n");
            ownerIdentity = path_identity(path/"owner", true);
        } catch (...) {
            std::error_code error;
            fs::remove_all(path, error);
            throw;
        }
    }
    ~RepairLock() {
        if (retain) return;
        try {
            if (!same(path_identity(root, true), rootIdentity) || !same(path_identity(path, true), identity)) return;
            remove_file(path/"owner", &ownerIdentity);
            remove_directory(path, &identity);
        } catch (...) {}
    }
};
void validate_owned_installation(const fs::path& root) {
    directory_chain(root);
    require(fs::is_directory(checked_status(root)) && current_owner(open_directory(root).value),
            "Installation root is not owned by the current user: " + text(root));
    require(read_small(root/markerName, marker.size()) == marker,
            "Refusing to overwrite an unowned installation folder: " + text(root));
    for (const auto* name : executables) (void)path_identity(root/name, true);
    (void)path_identity(root/"game/data/menu/menulist.xml", true);
    (void)path_identity(root/iconName, true);
}
void validate_icon(const fs::path& path) {
    directory_chain(path.parent_path());
    auto file = open_regular(path);
    const auto size = identify(file.value).size;
    require(size >= 22 && size <= 1024 * 1024, "Original ICO exceeds safe size bounds");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    DWORD count = 0;
    require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) != 0 && count == bytes.size(),
            "Cannot read original ICO");
    const auto le16 = [&](std::size_t offset) { return unsigned(bytes[offset]) | unsigned(bytes[offset + 1]) << 8; };
    const auto le32 = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset + 1]) << 8 |
               std::uint32_t(bytes[offset + 2]) << 16 | std::uint32_t(bytes[offset + 3]) << 24;
    };
    const auto be32 = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) << 24 | std::uint32_t(bytes[offset + 1]) << 16 |
               std::uint32_t(bytes[offset + 2]) << 8 | std::uint32_t(bytes[offset + 3]);
    };
    const auto entries = le16(4);
    require(le16(0) == 0 && le16(2) == 1 && entries > 0 && entries <= 64 && 6 + entries * 16 <= size,
            "Original icon is not a bounded ICO image");
    constexpr std::array<std::uint8_t, 8> png{137, 80, 78, 71, 13, 10, 26, 10};
    for (unsigned i = 0; i < entries; ++i) {
        const auto length = le32(6 + i * 16 + 8), offset = le32(6 + i * 16 + 12);
        require(offset >= 6 + entries * 16 && offset <= size && length >= 40 && length <= size - offset,
                "Original icon has an invalid image extent");
        const bool isPng = std::equal(png.begin(), png.end(), bytes.begin() + offset);
        const auto width = isPng ? be32(offset + 16) : le32(offset + 4);
        const auto height = isPng ? be32(offset + 20) : le32(offset + 8);
        require(width > 0 && width <= 1024 && height > 0 && height <= (isPng ? 1024U : 2048U) &&
                (isPng || (le32(offset) >= 40 && le32(offset) <= length && height % 2 == 0)),
                "Original icon image dimensions exceed safe bounds");
    }
}
std::vector<fs::path> engine_files(const fs::path& engine) {
    directory_chain(engine);
    require(fs::is_directory(checked_status(engine)), "Engine payload is missing: " + text(engine));
    std::vector<fs::path> files;
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(engine)) {
        const auto relative = text(entry.path().filename());
        require(valid_engine_file(relative) && relative.find('/') == std::string::npos &&
                names.insert(folded(relative)).second,
                "Engine payload must contain only the three trusted executables and flat application DLLs: " + relative);
        require(fs::is_regular_file(checked_status(entry.path())), "Engine payload member is not a regular file");
        (void)path_identity(entry.path());
        files.push_back(entry.path());
    }
    for (const auto* name : executables)
        require(names.contains(name), "Trusted native engine executable is missing: " + std::string(name));
    require(files.size() <= maxEnginePaths, "Engine payload has too many members");
    return files;
}
bool asset_owned_path(const fs::path& relative) {
    for (const auto& part : relative) {
        const auto name = folded(text(part));
        if (name == "game.ini" || name == "checkpoints" || name == "save" || name == "saves") return false;
    }
    return true;
}
} // namespace

InstallPaths default_install_paths() {
    const auto valid = [](const char* value) { return value && *value && utf8_path(value).is_absolute(); };
    const auto* local = SDL_getenv("LOCALAPPDATA");
    const auto* roaming = SDL_getenv("APPDATA");
    require(valid(local) && valid(roaming), "Windows LOCALAPPDATA and APPDATA must select per-user installation folders");
    const fs::path localPath = absolute_path(utf8_path(local)), roamingPath = absolute_path(utf8_path(roaming));
    fs::path desktop;
    try {
        if (const auto* folder = SDL_GetUserFolder(SDL_FOLDER_DESKTOP); valid(folder)) desktop = absolute_path(utf8_path(folder));
    } catch (const std::runtime_error&) { desktop.clear(); }
    return {absolute_path(localPath/"Programs/etiyami"),
            absolute_path(roamingPath/"Microsoft/Windows/Start Menu/Programs/ETI Yami"), desktop};
}

InstallResult install(const InstallRequest& request, const Progress& progress) {
    require(!request.remover.empty(), "Trusted yami-remove.exe helper is required");
    InstallResult result{{absolute_path(request.paths.root), absolute_path(request.paths.applications), {}}, false};
    const auto& paths = result.paths;
    require(paths.root != paths.root.root_path() && !within(paths.root, paths.applications) &&
            !path_equal(paths.root, paths.applications), "Start Menu folder must be outside the installation root");
    directory_chain(paths.root.parent_path());
    result.reused = fs::exists(checked_status(paths.root));
    std::unique_ptr<TemporaryDirectory> repairStage, freshStage;
    std::unique_ptr<RepairLock> repairLock;
    Identity rootIdentity{};
    if (result.reused) {
        validate_owned_installation(paths.root);
        rootIdentity = path_identity(paths.root, true);
        repairStage = std::make_unique<TemporaryDirectory>(paths.root.parent_path());
        repairLock = std::make_unique<RepairLock>(paths.root, repairStage->path);
    }
    const auto checkRoot = [&] {
        directory_chain(paths.root.parent_path());
        require(same(path_identity(paths.root, true), rootIdentity),
                "Installation path changed concurrently; refusing its replacement");
    };
    InstalledFiles inventory;
    std::vector<std::string> engineInventory;
    if (result.reused) {
        inventory = parse_installed(read_small(paths.root/installedName, maxInstalledBytes));
        engineInventory = parse_engine(read_small(paths.root/engineName, maxEngineBytes));
    }
    create_chain(paths.applications, nullptr);
    shortcut_exists(paths.applications/playShortcut, paths.root, false);
    shortcut_exists(paths.applications/uninstallShortcut, paths.root, true);
    report(progress, result.reused ? "Repairing uninstaller and shortcuts" : "Preparing installation");
    std::vector<fs::path> files;
    std::uint64_t engineBytes = 0;
    if (!result.reused) {
        files = engine_files(absolute_path(request.engine));
        for (const auto& file : files) {
            const auto size = identify(open_regular(file).value).size;
            require(size <= 512ULL * 1024 * 1024 && engineBytes <= 2ULL * 1024 * 1024 * 1024 - size,
                    "Trusted engine payload exceeds installation size limits");
            engineBytes += size;
        }
        auto ancestor = paths.root.parent_path();
        while (!fs::exists(checked_status(ancestor))) ancestor = ancestor.parent_path();
        require(fs::space(ancestor).available >= engineBytes + 1536ULL * 1024 * 1024,
                "Not enough free space: original assets and extraction need 1.5 GiB plus the native engine");
    }
    CreatedDirectories created;
    create_chain(paths.root.parent_path(), &created);
    create_chain(paths.applications, &created);
    if (!result.reused) freshStage = std::make_unique<TemporaryDirectory>(paths.root.parent_path());
    auto& stage = result.reused ? *repairStage : *freshStage;
    require(CreateDirectoryW((stage.path/"install").c_str(), nullptr) != 0, "Cannot create staged installation");
    secure(stage.path/"install", true);
    require(CreateDirectoryW((stage.path/"install"/helperDirectory).c_str(), nullptr) != 0,
            "Cannot stage private removal support");
    secure(stage.path/"install"/helperDirectory, true);
    report(progress, "Copying pinned removal support");
    copy_file(absolute_path(request.remover), stage.path/"install"/helperDirectory/helperName, 512ULL * 1024 * 1024);
    if (!result.reused) {
        std::uint64_t copied = 0;
        for (const auto& file : files) {
            report(progress, "Copying native engine", copied, engineBytes);
            copy_file(file, stage.path/"install"/file.filename(), 512ULL * 1024 * 1024);
            engineInventory.push_back(text(file.filename()));
            copied += identify(open_regular(file).value).size;
        }
        require(CreateDirectoryW((stage.path/"install/game").c_str(), nullptr) != 0 &&
                CreateDirectoryW((stage.path/"scratch").c_str(), nullptr) != 0, "Cannot stage original assets");
        extract_iso_assets(absolute_path(request.iso), stage.path/"install/game", stage.path/"scratch",
                           absolute_path(request.archiver), progress);
        report(progress, "Validating original game icon");
        validate_icon(stage.path/"install/game/yami.ico");
        copy_file(stage.path/"install/game/yami.ico", stage.path/"install"/iconName, 1024 * 1024);
        write_new(stage.path/"install"/markerName, marker);
        for (const auto& entry : fs::recursive_directory_iterator(stage.path/"install/game")) {
            if (!fs::is_regular_file(checked_status(entry.path()))) continue;
            const auto relative = entry.path().lexically_relative(stage.path/"install");
            if (!asset_owned_path(relative)) continue;
            const auto name = text(relative);
            require(valid_relative_file(name), "Original asset path cannot be recorded safely");
            inventory.files.push_back(name);
        }
        for (const auto name : {markerName, installedName, engineName}) inventory.files.emplace_back(name);
        inventory.files.emplace_back(iconName);
        inventory.files.emplace_back(std::string(helperDirectory) + "/" + std::string(helperName));
    }
    std::vector<std::unique_ptr<ShortcutPublication>> shortcuts, desktop;
    shortcuts.push_back(std::make_unique<ShortcutPublication>(paths.applications, playShortcut, paths.root, false));
    shortcuts.push_back(std::make_unique<ShortcutPublication>(paths.applications, uninstallShortcut, paths.root, true));
    if (!result.reused) rootIdentity = path_identity(stage.path/"install", true);
    bool rootPublished = false;
    const auto desktopInventoryBase = inventory.shortcuts.size();
    const auto skipDesktop = [&](const std::exception& failure) {
        for (auto it = desktop.rbegin(); it != desktop.rend(); ++it) it->get()->publication.rollback();
        inventory.shortcuts.resize(desktopInventoryBase);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Desktop shortcuts skipped; use the Start Menu: %s", failure.what());
        desktop.clear();
        result.paths.desktop.clear();
    };
    std::vector<std::unique_ptr<Publication>> support;
    try {
        try {
            if (!request.paths.desktop.empty()) {
                result.paths.desktop = absolute_path(request.paths.desktop);
                require(!within(paths.root, result.paths.desktop) && !path_equal(paths.root, result.paths.desktop),
                        "Desktop folder must be outside the installation root");
                if (!path_equal(result.paths.desktop, paths.applications)) {
                    create_chain(result.paths.desktop, &created);
                    require(fs::space(result.paths.desktop).available >= 65536, "Not enough space for Desktop shortcuts");
                    for (const auto name : {playShortcut, uninstallShortcut})
                        shortcut_exists(result.paths.desktop/name, paths.root, name == uninstallShortcut);
                    desktop.push_back(std::make_unique<ShortcutPublication>(result.paths.desktop, playShortcut, paths.root, false));
                    desktop.push_back(std::make_unique<ShortcutPublication>(result.paths.desktop, uninstallShortcut, paths.root, true));
                }
            }
        } catch (const std::runtime_error& error) { skipDesktop(error); }
        report(progress, "Publishing installation");
        if (result.reused) checkRoot();
        try {
            for (auto& shortcut : desktop) {
                shortcut->publication.publish();
                inventory.shortcuts.push_back(text(shortcut->publication.destination));
            }
        } catch (const std::runtime_error& error) { skipDesktop(error); }
        for (const auto& shortcut : shortcuts) inventory.shortcuts.push_back(text(shortcut->publication.destination));
        const auto installedRecord = serialize_installed(inventory);
        write_new(stage.path/"install"/installedName, installedRecord);
        if (!result.reused) write_new(stage.path/"install"/engineName, serialize_engine(engineInventory));
        if (result.reused) {
            checkRoot();
            create_chain(paths.root/helperDirectory, &created);
            const auto helper = paths.root/helperDirectory/helperName;
            support.push_back(std::make_unique<Publication>(helper, stage.path/"install"/helperDirectory/helperName,
                                                             stage.path/"backup-helper"));
            if (read_small(paths.root/installedName, maxInstalledBytes) != installedRecord)
                support.push_back(std::make_unique<Publication>(paths.root/installedName, stage.path/"install"/installedName,
                                                                 stage.path/"backup-inventory"));
        } else {
            validate_owned_installation(stage.path/"install");
            rename_new(stage.path/"install", paths.root); rootPublished = true;
            repairLock = std::make_unique<RepairLock>(paths.root, stage.path);
        }
        checkRoot();
        for (auto& file : support) { checkRoot(); file->publish(); }
        report(progress, "Creating shortcuts");
        for (auto& shortcut : shortcuts) { checkRoot(); shortcut->publication.publish(); }
        checkRoot();
    } catch (...) {
        const auto original = std::current_exception();
        std::string recovery;
        for (auto* group : {&desktop, &shortcuts}) {
            for (auto it = group->rbegin(); it != group->rend(); ++it) {
                try { (*it)->publication.rollback(); }
                catch (const std::exception& error) {
                    (*it)->staging.retain = true;
                    recovery += "\nShortcut backup retained at " + text((*it)->staging.path) + ": " + error.what();
                }
            }
        }
        for (auto it = support.rbegin(); it != support.rend(); ++it) {
            if (!(*it)->published && !(*it)->backedUp) continue;
            try { checkRoot(); (*it)->rollback(); }
            catch (const std::exception& error) {
                stage.retain = true;
                recovery += "\nRemoval support backup retained at " + text(stage.path) + ": " + error.what();
            }
        }
        if (rootPublished) {
            try {
                require(same(path_identity(paths.root, true), rootIdentity),
                        "Installation path changed concurrently; refusing to remove its replacement");
                rename_new(paths.root, stage.path/"install");
            } catch (const std::exception& error) {
                stage.retain = true;
                recovery += "\nInstallation retained for recovery: " + std::string(error.what());
            }
        }
        if (!recovery.empty()) {
            if (repairLock) repairLock->retain = true;
            throw std::runtime_error("Installation failed and automatic rollback needs manual recovery:" + recovery);
        }
        std::rethrow_exception(original);
    }
    return result;
}

void launch_game(const InstallResult& installation) {
    const auto root = absolute_path(installation.paths.root);
    validate_owned_installation(root);
    const auto executable = text(root/"yami-launcher.exe");
    const char* args[] = {executable.c_str(), nullptr};
    const auto properties = SDL_CreateProperties();
    require(properties != 0, SDL_GetError());
    const bool configured = SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args) &&
        SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, text(root).c_str()) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    auto* process = configured ? SDL_CreateProcessWithProperties(properties) : nullptr;
    SDL_DestroyProperties(properties);
    require(process != nullptr, "Cannot launch native settings: " + std::string(SDL_GetError()));
    SDL_DestroyProcess(process);
}
} // namespace yami::setup
