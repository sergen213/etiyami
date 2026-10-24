#include "install_ownership.hpp"
#include "setup_windows.hpp"

#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace removal {
namespace fs = std::filesystem;
namespace own = yami::ownership;
using namespace yami::windows_install;
constexpr std::string_view iconName = "eti-yami.ico";

std::string folded(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
std::string quoted(const fs::path& path) { return "\"" + text(path) + "\""; }
fs::path checked_absolute(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path && own::valid_text(text(path)),
            "Expected an exact absolute installation path");
    return path;
}
bool asset_file(std::string_view value) {
    if (!value.starts_with("game/")) return false;
    for (const auto& part : utf8_path(value)) {
        const auto name = folded(text(part));
        if (name == "game.ini" || name == "save" || name == "saves" || name == "checkpoints") return false;
    }
    return true;
}
bool installed_file(std::string_view value) {
    return asset_file(value) || value == iconName || value == own::markerName ||
           value == own::installedName || value == own::engineName ||
           value == std::string(own::helperDirectory) + "/" + std::string(own::helperName);
}
bool late_file(const fs::path& path) {
    const auto name = text(path.filename());
    return name == own::markerName || name == own::installedName || name == own::engineName ||
           (name == own::helperName && text(path.parent_path().filename()) == own::helperDirectory);
}
struct Target {
    fs::path path;
    Identity id{};
    bool present = false, metadata = false;
};
struct Plan {
    fs::path root;
    Identity rootId{};
    std::map<std::string, Identity> directories;
    std::vector<Target> early, late, shortcuts;
    std::set<fs::path> prune;
    std::string installedRecord, engineRecord;
};
Handle tracked_directory(Plan& plan, const fs::path& path, bool optional = false) {
    auto current = path.root_path();
    Handle result;
    for (const auto& component : path.relative_path()) {
        current /= component;
        const auto found = checked_status(current);
        if (!fs::exists(found) && optional) return Handle();
        require(fs::is_directory(found), "Removal parent is not a safe directory: " + text(current));
        result = open_directory(current);
        const auto id = identify(result.value);
        const auto key = text(current);
        const auto existing = plan.directories.find(key);
        if (existing != plan.directories.end()) require(same(id, existing->second), "Directory replaced: " + key);
        else plan.directories.emplace(key, id);
    }
    return result;
}
Target snapshot(Plan& plan, const fs::path& path, bool metadata = false) {
    auto parent = tracked_directory(plan, path.parent_path(), !metadata);
    Target target{path, {}, false, metadata};
    if (!parent) return target;
    if (!fs::exists(checked_status(path))) return target;
    auto file = open_regular(path, FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, true);
    target.id = identify(file.value); target.present = true;
    return target;
}
struct ShellLink {
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    bool uninitialize = false;
    ShellLink() {
        try {
            const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize shortcut services");
            uninitialize = SUCCEEDED(initialized);
            require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
                    "Cannot create shortcut reader");
            require(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))), "Cannot access shortcut persistence");
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
void validate_shortcut(const Target& target, const fs::path& root) {
    if (!target.present) return;
    const bool uninstall = path_equal(target.path.filename(), own::uninstallShortcut);
    const auto expectedTarget = uninstall ? root/own::helperDirectory/own::helperName : root/"yami-launcher.exe";
    const auto expectedArguments = uninstall ? "--root " + quoted(root) : std::string();
    ShellLink shell;
    require(SUCCEEDED(shell.file->Load(target.path.c_str(), STGM_READ)), "Cannot read owned shortcut: " + text(target.path));
    wchar_t path[32768]{}, working[32768]{}, icon[32768]{}, arguments[4096]{};
    int iconIndex = 0;
    require(SUCCEEDED(shell.link->GetPath(path, std::size(path), nullptr, SLGP_RAWPATH)) &&
            SUCCEEDED(shell.link->GetWorkingDirectory(working, std::size(working))) &&
            SUCCEEDED(shell.link->GetIconLocation(icon, std::size(icon), &iconIndex)) &&
            SUCCEEDED(shell.link->GetArguments(arguments, std::size(arguments))), "Cannot inspect shortcut ownership");
    const bool targetOk = path_equal(absolute_path(path), expectedTarget);
    const bool workingOk = path_equal(absolute_path(working), root);
    const bool iconOk = path_equal(absolute_path(icon), root/iconName) && iconIndex == 0;
    const bool argumentsOk = expectedArguments == text(arguments);
    if (!targetOk || !workingOk || !iconOk || !argumentsOk)
        throw std::runtime_error("Shortcut shape mismatch: target=" + text(path) + " expected=" + text(expectedTarget) +
                "; working=" + text(working) + " expected=" + text(root) + "; icon=" + text(icon) +
                " expected=" + text(root/iconName) + "; arguments=" + text(arguments) + " expected=" + expectedArguments +
                "; flags=" + std::to_string(targetOk) + std::to_string(workingOk) + std::to_string(iconOk) +
                std::to_string(argumentsOk) + "; iconIndex=" + std::to_string(iconIndex));
    require(same(path_identity(target.path, true), target.id), "Shortcut changed while its ownership was checked: " + text(target.path));
}
Plan preflight(const fs::path& input) {
    Plan plan; plan.root = checked_absolute(input);
    require(std::distance(plan.root.begin(), plan.root.end()) >= 4 && plan.root != plan.root.root_path(),
            "Refusing a top-level or system installation root");
    for (const auto* variable : {L"SystemRoot", L"USERPROFILE", L"LOCALAPPDATA", L"APPDATA", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        if (const auto* value = _wgetenv(variable); value && *value)
            require(!path_equal(plan.root, absolute_path(value)), "Refusing a user/system directory as installation root");
    }
    auto rootHandle = tracked_directory(plan, plan.root);
    plan.rootId = identify(rootHandle.value);
    require(current_owner(rootHandle.value), "Installation root belongs to another user");
    const auto marker = snapshot(plan, plan.root/own::markerName, true);
    const auto installed = snapshot(plan, plan.root/own::installedName, true);
    const auto engine = snapshot(plan, plan.root/own::engineName, true);
    require(read_small(marker.path, 256) == own::marker, "Invalid installation marker");
    plan.installedRecord = read_small(installed.path, own::maxInstalledBytes);
    plan.engineRecord = read_small(engine.path, own::maxEngineBytes);
    const auto assets = own::parse_installed(plan.installedRecord);
    const auto engines = own::parse_engine(plan.engineRecord);
    std::set<std::string> paths;
    for (const auto& path : assets.files) {
        require(installed_file(path), "Inventory claims a non-installed or saved file: " + path);
        paths.insert(path);
    }
    paths.insert(engines.begin(), engines.end());
    for (const auto& relative : paths) {
        const auto path = plan.root/utf8_path(relative);
        const bool metadata = relative == own::markerName || relative == own::installedName || relative == own::engineName;
        auto target = snapshot(plan, path, metadata);
        if (target.present) require(target.id.volume == plan.rootId.volume, "Foreign volume inside installation: " + text(path));
        for (auto parent = path.parent_path(); !path_equal(parent, plan.root); parent = parent.parent_path()) {
            require(within(plan.root, parent), "Inventory parent escaped the installation");
            plan.prune.insert(parent);
        }
        (late_file(path) ? plan.late : plan.early).push_back(std::move(target));
    }
    for (const auto& value : assets.shortcuts) {
        const auto path = checked_absolute(utf8_path(value));
        require(!within(plan.root, path), "Shortcut inventory points inside the installation");
        auto target = snapshot(plan, path);
        if (target.present) {
            require(current_owner(tracked_directory(plan, path.parent_path()).value), "Shortcut directory belongs to another user");
            validate_shortcut(target, plan.root);
        }
        plan.shortcuts.push_back(std::move(target));
    }
    return plan;
}
bool product_process(const std::wstring& name) {
    return CompareStringOrdinal(name.c_str(), -1, L"yami-native.exe", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"yami-launcher.exe", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name.c_str(), -1, L"yami-updater.exe", -1, TRUE) == CSTR_EQUAL;
}
void running_guard(const fs::path& root, DWORD ignoredParent) {
    Handle snapshotHandle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshotHandle) fail("Cannot inspect running applications");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshotHandle.value, &entry); more; more = Process32NextW(snapshotHandle.value, &entry)) {
        const auto pid = entry.th32ProcessID;
        if (!pid || pid == GetCurrentProcessId() || pid == ignoredParent) continue;
        HANDLE raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!raw) {
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue;
            require(!product_process(entry.szExeFile),
                    "Cannot inspect an active ETI Yami candidate; close the game, launcher and updater before uninstalling");
            continue;
        }
        Handle process(raw);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) continue;
        std::wstring executable(32768, L'\0');
        DWORD length = static_cast<DWORD>(executable.size());
        if (!QueryFullProcessImageNameW(process.value, 0, executable.data(), &length)) {
            require(!product_process(entry.szExeFile), "Cannot inspect an active ETI Yami executable safely");
            continue;
        }
        executable.resize(length);
        const auto path = fs::path(executable).lexically_normal();
        require(!path_equal(path, root) && !within(root, path),
                "Close the game, launcher and updater before uninstalling (process " + std::to_string(pid) + ")");
    }
    require(GetLastError() == ERROR_NO_MORE_FILES, "Cannot complete running-process inspection");
}
struct UpdateLock {
    fs::path path;
    Identity identity{}, ownerIdentity{};
    explicit UpdateLock(Plan& plan) : path(plan.root/".yami-update-lock") {
        require(CreateDirectoryW(path.c_str(), nullptr) != 0,
                "An updater/remover lock exists; close active applications and resolve interrupted updates first");
        try {
            secure(path, true);
            identity = path_identity(path, true);
            write_new(path/"owner", "pid=" + std::to_string(GetCurrentProcessId()) + "\n");
            ownerIdentity = path_identity(path/"owner", true);
        } catch (...) {
            std::error_code error; fs::remove_all(path, error); throw;
        }
    }
    ~UpdateLock() {
        try {
            remove_file(path/"owner", &ownerIdentity);
            remove_directory(path, &identity);
        } catch (...) {}
    }
};
void verify_parent(Plan& plan, const fs::path& path) {
    auto parent = tracked_directory(plan, path.parent_path());
    require(bool(parent), "Owned parent disappeared during removal");
}
void prune_directory(Plan& plan, const fs::path& path) {
    auto directory = tracked_directory(plan, path, true);
    if (!directory) return;
    std::error_code error;
    if (!fs::is_empty(path, error) || error) return; // Unrecorded files always keep their directory.
    directory = Handle();
    remove_directory(path, &plan.directories.at(text(path)));
}
struct Completion {
    HANDLE event = nullptr;
    fs::path status;
    bool sent = false;
    bool retainTemporary = false;
    void send(int code) {
        if (sent) return;
        sent = true;
        if (!status.empty()) write_new(status, std::to_string(code) + "\n");
        if (event) require(SetEvent(event) != 0, "Cannot notify uninstall caller");
    }
};
void compare_plans(const Plan& before, const Plan& after) {
    require(same(before.rootId, after.rootId) && before.installedRecord == after.installedRecord &&
            before.engineRecord == after.engineRecord, "Installation ownership changed during confirmation");
    for (const auto* group : {&before.early, &before.late, &before.shortcuts}) {
        const auto* other = group == &before.early ? &after.early : group == &before.late ? &after.late : &after.shortcuts;
        require(group->size() == other->size(), "Ownership changed during confirmation");
        for (std::size_t i = 0; i < group->size(); ++i) {
            require(path_equal((*group)[i].path, (*other)[i].path) && (*group)[i].present == (*other)[i].present &&
                    (!(*group)[i].present || same((*group)[i].id, (*other)[i].id)),
                    "Owned member changed during confirmation; no files removed");
        }
    }
}
int uninstall(const fs::path& root, bool yes, DWORD parentPid = 0, Completion* completion = nullptr) {
    auto plan = preflight(root);
    running_guard(plan.root, parentPid);
    std::cout << "Installation: " << text(plan.root) << "\nRemoves only recorded game/engine files and owned shortcuts.\n"
                 "Saved games, settings and unrecorded files will be kept.\n";
    if (!yes) {
        DWORD mode = 0;
        require(GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0,
                "Interactive console required; no removal was authorized");
        std::cout << "Uninstall ETI Yami? [yes/No] " << std::flush;
        std::string answer; std::getline(std::cin, answer);
        if (answer != "yes" && answer != "y" && answer != "YES" && answer != "Y") {
            std::cout << "Cancelled. Nothing was removed.\n";
            if (completion) completion->send(0);
            return 0;
        }
    }
    UpdateLock lock(plan);
    auto current = preflight(plan.root);
    compare_plans(plan, current);
    plan = std::move(current);
    running_guard(plan.root, parentPid);
    for (const auto& target : plan.early) if (target.present) {
        verify_parent(plan, target.path);
        remove_file(target.path, &target.id);
    }
    std::vector<fs::path> directories(plan.prune.begin(), plan.prune.end());
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    for (const auto& path : directories) if (!path_equal(path, plan.root/own::helperDirectory)) prune_directory(plan, path);
    for (const auto& target : plan.shortcuts) if (target.present) {
        verify_parent(plan, target.path);
        validate_shortcut(target, plan.root);
        remove_file(target.path, &target.id);
    }
    // Zero releases the installed caller image; the worker reports final success or failure on its console.
    if (completion) { completion->retainTemporary = true; completion->send(0); }
    if (parentPid) {
        Handle parent(OpenProcess(SYNCHRONIZE, FALSE, parentPid));
        if (!parent) require(GetLastError() == ERROR_INVALID_PARAMETER, "Cannot observe uninstall caller exit");
        if (parent) require(WaitForSingleObject(parent.value, 30000) == WAIT_OBJECT_0,
                            "Original uninstall process did not exit; helper was preserved");
    }
    running_guard(plan.root, 0);
    for (const auto& target : plan.late) if (target.present && !target.metadata) {
        verify_parent(plan, target.path);
        remove_file(target.path, &target.id);
    }
    prune_directory(plan, plan.root/own::helperDirectory);
    // Late failures retain ownership metadata so a rerun can finish safely.
    for (const auto& target : plan.late) if (target.present && target.metadata) {
        verify_parent(plan, target.path);
        remove_file(target.path, &target.id);
    }
    if (completion) completion->retainTemporary = false;
    std::cout << "ETI Yami uninstalled. Saved/settings/unrecorded files, if any, remain at " << text(plan.root) << ".\n";
    return 0;
}

fs::path module_path() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(length != 0 && length < path.size(), "Cannot locate the running uninstall helper");
    path.resize(length);
    return absolute_path(path);
}
int detached(const fs::path& root, bool yes) {
    const auto temporary = temporary_root();
    directory_chain(temporary);
    const auto serial = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    const auto helper = temporary/("eti-yami-remove-" + serial + ".exe");
    const auto status = temporary/("eti-yami-remove-" + serial + ".result");
    copy_file(module_path(), helper, 512ULL * 1024 * 1024);
    try {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle event(CreateEventW(&security, TRUE, FALSE, nullptr));
        if (!event) fail("Cannot create uninstall completion event");
        const auto handleValue = reinterpret_cast<ULONG_PTR>(event.value);
        std::wstring command = L"\"" + helper.native() + L"\" --worker --root \"" + root.native() +
            L"\" --parent-pid " + std::to_wstring(GetCurrentProcessId()) + L" --event-handle " +
            std::to_wstring(handleValue) + L" --status \"" + status.native() + L"\" --temp-self \"" + helper.native() + L"\"";
        if (yes) command += L" --yes";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                               nullptr, temporary.c_str(), &startup, &process) != 0,
                "Cannot start detached uninstall helper");
        Handle processHandle(process.hProcess), threadHandle(process.hThread);
        HANDLE waits[] = {event.value, processHandle.value};
        const auto signaled = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        require(signaled == WAIT_OBJECT_0 || signaled == WAIT_OBJECT_0 + 1, "Cannot observe detached uninstall helper");
        int result = 1;
        if (WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0 && fs::exists(checked_status(status))) {
            const auto contents = read_small(status, 32);
            try { result = std::stoi(contents); } catch (...) { result = 1; }
        }
        if (WaitForSingleObject(processHandle.value, 0) == WAIT_OBJECT_0) DeleteFileW(helper.c_str());
        DeleteFileW(status.c_str());
        return result;
    } catch (...) {
        DeleteFileW(status.c_str());
        DeleteFileW(helper.c_str());
        throw;
    }
}
void clean_temporary_helper(const fs::path& path) noexcept {
    if (path.empty()) return;
    if (!DeleteFileW(path.c_str()))
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // Windows may keep our own image mapped until reboot.
}
} // namespace removal

#ifndef YAMI_REMOVE_NO_MAIN
int main() {
    std::filesystem::path root, status, temporary;
    bool yes = false, worker = false;
    DWORD parent = 0;
    HANDLE event = nullptr;
    try {
        int argc = 0;
        const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        removal::require(argv != nullptr, "Cannot read Windows command line");
        struct Arguments { wchar_t** value; ~Arguments() { LocalFree(value); } } arguments{argv};
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument(argv[i]);
            const auto value = [&]() -> std::filesystem::path {
                if (++i == argc) throw std::runtime_error("Missing uninstall option value");
                return std::filesystem::path(argv[i]);
            };
            if (argument == L"--help") { std::cout << "Usage: yami-remove.exe --root ABSROOT [--yes]\n"; return 0; }
            if (argument == L"--root" && root.empty()) root = value();
            else if (argument == L"--yes" && !yes) yes = true;
            else if (argument == L"--worker" && !worker) worker = true;
            else if (argument == L"--parent-pid" && !parent) parent = static_cast<DWORD>(std::stoul(value().wstring()));
            else if (argument == L"--event-handle" && !event) event = reinterpret_cast<HANDLE>(std::stoull(value().wstring()));
            else if (argument == L"--status" && status.empty()) status = value();
            else if (argument == L"--temp-self" && temporary.empty()) temporary = value();
            else throw std::runtime_error("Usage: yami-remove.exe --root ABSROOT [--yes]");
        }
        removal::require(!root.empty(), "Usage: yami-remove.exe --root ABSROOT [--yes]");
        root = removal::checked_absolute(root);
        if (!worker) return removal::detached(root, yes);
        removal::require(parent && event && !status.empty() && !temporary.empty(), "Incomplete detached uninstall request");
        removal::Completion completion{event, status};
        struct TemporaryCleanup {
            bool active;
            std::filesystem::path path;
            removal::Completion* completion;
            ~TemporaryCleanup() {
                if (!active) return;
                if (completion->retainTemporary)
                    std::cerr << "Removal helper retained for recovery: " << yami::windows_install::text(path) << "\n";
                else removal::clean_temporary_helper(path);
            }
        } temporaryCleanup{worker, temporary, &completion};
        try {
            return removal::uninstall(root, yes, parent, &completion);
        } catch (...) {
            completion.send(1);
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "Cannot uninstall ETI Yami: " << error.what() << '\n';
        if (worker && event) SetEvent(event);
        return 1;
    }
}
#endif
