#include "update_install.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace yami::updates {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
std::uint64_t process_id();
constexpr std::uint64_t maxFileSize = 512ULL * 1024 * 1024;
constexpr std::uint64_t maxPayloadSize = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t maxEntries = 8192;
#ifdef _WIN32
constexpr std::string_view nativeName = "yami-native.exe", updaterName = "yami-updater.exe";
constexpr std::string_view launcherName = "yami-launcher.exe";
#elif defined(__APPLE__)
constexpr std::string_view nativeName = "yami-native", updaterName = "yami-updater";
constexpr std::string_view launcherName = "yami-launcher.app/Contents/MacOS/yami-launcher";
#else
constexpr std::string_view nativeName = "yami-native", updaterName = "yami-updater";
constexpr std::string_view launcherName = "yami-launcher";
#endif
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path utf8_path(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string folded(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
bool safe_component(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    for (const auto c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    const auto lower = folded(name);
    if (lower == "game" || lower == "game.ini" || lower == "checkpoints" || lower == ".git" ||
        lower == "checkpoint" || lower.starts_with("checkpoint.")) return false;
    const auto stem = lower.substr(0, lower.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
         stem[3] >= '1' && stem[3] <= '9')) return false;
    return true;
}
bool runtime_name(std::string_view name) {
    if (!safe_component(name)) return false;
#ifdef _WIN32
    return name.size() > 4 && folded(name).ends_with(".dll");
#elif defined(__APPLE__)
    return name.starts_with("lib") && name.size() > 9 && name.ends_with(".dylib");
#else
    if (!name.starts_with("lib")) return false;
    const auto suffix = name.find(".so");
    if (suffix == std::string_view::npos || suffix <= 3) return false;
    const auto tail = name.substr(suffix + 3);
    if (tail.empty()) return true;
    if (tail.front() != '.' || tail.back() == '.') return false;
    bool digit = false;
    for (const auto c : tail.substr(1)) {
        if (c == '.') { if (!digit) return false; digit = false; }
        else if (c >= '0' && c <= '9') digit = true;
        else return false;
    }
    return digit;
#endif
}
bool binary(std::string_view relative) {
    return relative == nativeName || relative == updaterName || relative == launcherName;
}
fs::path absolute_path(const fs::path& path) {
    require(!path.empty(), "Empty update path");
    auto result = fs::absolute(path).lexically_normal();
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    return result;
}
fs::file_status checked_status(const fs::path& path) {
    std::error_code error;
    auto result = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return fs::file_status(fs::file_type::not_found);
    if (error) throw fs::filesystem_error("Cannot inspect update path", path, error);
#ifdef _WIN32
    if (fs::exists(result)) {
        const auto attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Update path is a reparse point or cannot be inspected: " + text(path));
    }
#endif
    require(!fs::is_symlink(result), "Update path is a symlink: " + text(path));
    return result;
}
void directory_chain(const fs::path& path) {
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        const auto found = checked_status(current);
        require(fs::is_directory(found), "Update parent is not a directory: " + text(current));
    }
}
void regular_file(const fs::path& path) {
    const auto found = checked_status(path);
    require(fs::is_regular_file(found) && fs::hard_link_count(path) == 1,
            "Update member is not a single-link regular file: " + text(path));
}
void private_directory(const fs::path& path) {
#ifdef _WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)",
            SDDL_REVISION_1, &descriptor, nullptr) != 0, "Cannot construct private update directory permissions");
    const bool success = SetFileSecurityW(path.c_str(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor);
    require(success, "Cannot protect update directory: " + text(path));
#else
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
}
void verify_stage(const fs::path& stage, const fs::path& root) {
    directory_chain(root);
    directory_chain(stage);
    const auto name = text(stage.filename());
    require(stage.parent_path() == root && name.starts_with(".yami-update-") && name.size() > 13 &&
            safe_component(name), "Update staging directory is not an owned child of the installation");
#ifndef _WIN32
    struct stat rootInfo{}, stageInfo{};
    require(::stat(root.c_str(), &rootInfo) == 0 && ::stat(stage.c_str(), &stageInfo) == 0 &&
            rootInfo.st_dev == stageInfo.st_dev && stageInfo.st_uid == geteuid() &&
            !(stageInfo.st_mode & (S_IWGRP | S_IWOTH)),
            "Update staging directory must be private, owned, and on the installation filesystem");
#else
    const auto volume = [](const fs::path& path) {
        HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot inspect update filesystem volume");
        BY_HANDLE_FILE_INFORMATION information{};
        const bool success = GetFileInformationByHandle(handle, &information) != 0;
        CloseHandle(handle);
        require(success, "Cannot inspect update filesystem volume");
        return information.dwVolumeSerialNumber;
    };
    require(volume(root) == volume(stage), "Update staging must be on the installation filesystem");
#endif
    require(!fs::exists(checked_status(stage/"backup")), "Unfinished update backup exists; recover it before retrying");
}
void check_case(const fs::path& parent, const fs::path& requested) {
    if (!fs::exists(checked_status(parent))) return;
    const auto expected = text(requested);
    for (const auto& entry : fs::directory_iterator(parent)) {
        const auto existing = text(entry.path().filename());
        require(existing == expected || folded(existing) != folded(expected),
                "Installed update target has a case-colliding sibling: " + text(entry.path()));
    }
}
void destination(const fs::path& root, const fs::path& relative) {
    auto parent = root;
    for (const auto& component : relative) {
        check_case(parent, component);
        parent /= component;
        const auto found = checked_status(parent);
        if (!fs::exists(found)) continue;
        if (parent == root/relative) regular_file(parent);
        else require(fs::is_directory(found), "Installed update parent is not a directory: " + text(parent));
    }
}
void rename_file(const fs::path& from, const fs::path& to, Clock::time_point& lockDeadline) {
    for (;;) {
        std::error_code error;
        fs::rename(from, to, error);
        if (!error) return;
#ifdef _WIN32
        const bool locked = error == std::errc::permission_denied || error.value() == ERROR_SHARING_VIOLATION ||
                            error.value() == ERROR_LOCK_VIOLATION || error.value() == ERROR_ACCESS_DENIED;
        if (locked) {
            if (lockDeadline == Clock::time_point{}) lockDeadline = Clock::now() + std::chrono::seconds(15);
            if (Clock::now() < lockDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
#else
        (void)lockDeadline;
#endif
        throw fs::filesystem_error("Cannot atomically move update member", from, to, error);
    }
}
struct InstallLock {
    fs::path path;
    bool retain = false;
    InstallLock(const fs::path& root, const fs::path& stage) : path(root/".yami-update-lock") {
        require(fs::create_directory(path), "Another updater or interrupted recovery owns .yami-update-lock; inspect its owner record");
        try {
            private_directory(path);
            std::ofstream owner(path/"owner", std::ios::binary);
            owner << "pid=" << process_id() << "\nstage=" << text(stage) << '\n';
            owner.close();
            require(bool(owner), "Cannot record update lock ownership");
            fs::permissions(path/"owner", fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        } catch (...) {
            std::error_code error;
            fs::remove(path/"owner", error);
            fs::remove(path, error);
            throw;
        }
    }
    ~InstallLock() {
        if (!retain) {
            std::error_code error;
            fs::remove(path/"owner", error);
            fs::remove(path, error);
        }
    }
};
void create_parents(const fs::path& root, const fs::path& relative, std::vector<fs::path>* created = nullptr) {
    auto parent = root;
    for (const auto& component : relative.parent_path()) {
        parent /= component;
        const auto found = checked_status(parent);
        if (!fs::exists(found)) {
            require(fs::create_directory(parent), "Cannot create update parent: " + text(parent));
            if (created) created->push_back(parent);
            fs::permissions(parent, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec, fs::perm_options::replace);
        } else require(fs::is_directory(found), "Unsafe update parent: " + text(parent));
    }
}
void clean_created(const std::vector<fs::path>& created) {
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
        std::error_code error;
        fs::remove(*it, error); // Never remove existing directories or unrelated files.
    }
}
struct SdlProperties {
    SDL_PropertiesID value = SDL_CreateProperties();
    SdlProperties() { require(value != 0, SDL_GetError()); }
    ~SdlProperties() { SDL_DestroyProperties(value); }
};
using Environment = std::unique_ptr<SDL_Environment, decltype(&SDL_DestroyEnvironment)>;
void start_process(const std::vector<std::string>& arguments, const fs::path& working) {
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    Environment environment(SDL_CreateEnvironment(true), SDL_DestroyEnvironment);
    require(bool(environment), SDL_GetError());
    SdlProperties properties;
    require(SDL_SetPointerProperty(properties.value, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data()) &&
            SDL_SetPointerProperty(properties.value, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment.get()) &&
            SDL_SetStringProperty(properties.value, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, text(working).c_str()) &&
            SDL_SetBooleanProperty(properties.value, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true) &&
            SDL_SetNumberProperty(properties.value, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL) &&
            SDL_SetNumberProperty(properties.value, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL) &&
            SDL_SetNumberProperty(properties.value, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL), SDL_GetError());
    auto* process = SDL_CreateProcessWithProperties(properties.value);
    require(process != nullptr, "Cannot start update process: " + std::string(SDL_GetError()));
    SDL_DestroyProcess(process);
}
std::uint64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
bool process_alive(std::uint64_t id) {
    if (!id) return true;
#ifdef _WIN32
    if (id > MAXDWORD) return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(id));
    if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
    const auto result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result != WAIT_OBJECT_0;
#else
    if (id > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return true;
    return ::kill(static_cast<pid_t>(id), 0) == 0 || errno != ESRCH;
#endif
}
constexpr std::string_view completedMagic = "YAMI_UPDATE_COMMITTED";
} // namespace

namespace detail {
void secure_stage_directory(const fs::path& directory) {
    const auto path = absolute_path(directory);
    directory_chain(path);
    private_directory(path);
}
bool valid_payload_path(std::string_view relative, bool directory) {
    if (relative.empty() || relative.size() > 512 || relative.front() == '/' || relative.back() == '/') return false;
    std::size_t offset = 0;
    while (offset < relative.size()) {
        const auto slash = relative.find('/', offset);
        const auto end = slash == std::string_view::npos ? relative.size() : slash;
        if (!safe_component(relative.substr(offset, end - offset))) return false;
        offset = end + 1;
    }
    if (directory) {
        if (relative == "lib") return true;
#ifdef __APPLE__
        return relative == "yami-launcher.app" || relative == "yami-launcher.app/Contents" ||
               relative == "yami-launcher.app/Contents/MacOS" || relative == "yami-launcher.app/Contents/Resources" ||
               relative == "yami-launcher.app/Contents/Frameworks" || relative == "yami-launcher.app/Contents/_CodeSignature";
#else
        return false;
#endif
    }
    if (binary(relative)) return true;
    if (relative.find('/') == std::string_view::npos) return runtime_name(relative);
    if (relative.starts_with("lib/") && relative.find('/', 4) == std::string_view::npos)
        return runtime_name(relative.substr(4));
#ifdef __APPLE__
    if (relative == "yami-launcher.app/Contents/Info.plist" || relative == "yami-launcher.app/Contents/PkgInfo" ||
        relative == "yami-launcher.app/Contents/_CodeSignature/CodeResources") return true;
    constexpr std::string_view resources = "yami-launcher.app/Contents/Resources/";
    constexpr std::string_view frameworks = "yami-launcher.app/Contents/Frameworks/";
    if (relative.starts_with(resources)) {
        const auto file = relative.substr(resources.size());
        return file.find('/') == std::string_view::npos &&
               (file == "Assets.car" || file == "native-release.txt" || file.ends_with(".icns"));
    }
    if (relative.starts_with(frameworks)) {
        const auto file = relative.substr(frameworks.size());
        return file.find('/') == std::string_view::npos && runtime_name(file);
    }
#endif
    return false;
}
void validate_payload(const fs::path& payload) {
    directory_chain(absolute_path(payload));
    std::set<std::string> names;
    std::array<bool, 3> binaries{};
    std::uint64_t total = 0;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(payload)) {
        require(++count <= maxEntries, "Update payload has too many members");
        const auto relative = text(entry.path().lexically_relative(payload));
        const auto found = checked_status(entry.path());
        const bool directory = fs::is_directory(found);
        require(valid_payload_path(relative, directory), "Unowned or unsafe update member: " + relative);
        require(names.insert(folded(relative)).second, "Case-colliding update members: " + relative);
        if (directory) continue;
        regular_file(entry.path());
#ifndef _WIN32
        require((found.permissions() & (fs::perms::set_uid | fs::perms::set_gid | fs::perms::sticky_bit)) == fs::perms::none,
                "Update member has elevated permissions: " + relative);
        if (binary(relative))
            require((found.permissions() & fs::perms::owner_exec) != fs::perms::none,
                    "Required update binary is not executable: " + relative);
#endif
        const auto size = fs::file_size(entry.path());
        require(size > 0 && size <= maxFileSize && total <= maxPayloadSize - size, "Update payload size is invalid");
        total += size;
        if (relative == nativeName) binaries[0] = true;
        if (relative == updaterName) binaries[1] = true;
        if (relative == launcherName) binaries[2] = true;
    }
    require(std::all_of(binaries.begin(), binaries.end(), [](bool present) { return present; }),
            "Update payload is missing a required engine, launcher, or updater executable");
}
void wait_for_parent_exit(std::uint64_t parent) {
    require(parent != 0 && parent != process_id(), "Invalid updater parent process");
#ifdef _WIN32
    require(parent <= MAXDWORD, "Invalid updater parent process ID");
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent));
    if (!process) {
        require(GetLastError() == ERROR_INVALID_PARAMETER, "Cannot wait for launcher exit");
        return;
    }
    const auto result = WaitForSingleObject(process, 30000);
    CloseHandle(process);
    require(result == WAIT_OBJECT_0, "Launcher did not exit within 30 seconds; no update was installed");
#else
    require(parent <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()), "Invalid updater parent process ID");
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        if (::kill(static_cast<pid_t>(parent), 0) != 0) {
            if (errno == ESRCH) return;
            throw std::runtime_error("Cannot observe launcher exit");
        }
        require(Clock::now() < deadline, "Launcher did not exit within 30 seconds; no update was installed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}
void restart_launcher(const fs::path& installRoot, const fs::path& assetRoot, const fs::path& saveDirectory) {
    const auto root = absolute_path(installRoot);
    const auto launcher = root/utf8_path(launcherName);
    directory_chain(launcher.parent_path());
    regular_file(launcher);
    start_process({text(launcher), "--launcher", "--asset-root", text(absolute_path(assetRoot)),
                   "--save-dir", text(absolute_path(saveDirectory))}, root);
}
void mark_completed(const fs::path& stagePath) {
    const auto stage = absolute_path(stagePath);
    directory_chain(stage);
    const auto marker = stage/"completed";
    require(!fs::exists(checked_status(marker)), "Update completion marker unexpectedly exists");
    std::ofstream output(marker, std::ios::binary);
    output << completedMagic << '\n' << process_id() << '\n';
    output.close();
    require(bool(output), "Cannot record committed update completion");
    fs::permissions(marker, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
}
bool append_installer_log(const fs::path& stagePath, std::string_view message) noexcept {
    try {
        const auto stage = absolute_path(stagePath);
        directory_chain(stage);
        regular_file(stage/"updater.log");
        std::ofstream output(stage/"updater.log", std::ios::binary | std::ios::app);
        output << message << '\n';
        output.close();
        return bool(output);
    } catch (...) { return false; } // Error reporting must not interrupt install/recovery.
}
} // namespace detail

fs::path installation_root() {
    const char* base = SDL_GetBasePath();
    require(base != nullptr, SDL_GetError());
    auto root = absolute_path(utf8_path(base));
#ifdef __APPLE__
    if ((root.filename() == "Resources" || root.filename() == "MacOS") &&
        root.parent_path().filename() == "Contents" && root.parent_path().parent_path().extension() == ".app")
        root = root.parent_path().parent_path().parent_path();
#endif
    return root;
}

void install_prepared(const fs::path& stagePath, const fs::path& rootPath) {
    const auto stage = absolute_path(stagePath), root = absolute_path(rootPath);
    verify_stage(stage, root);
    const auto payload = stage/"payload", backup = stage/"backup";
    detail::validate_payload(payload);
    struct Member { fs::path relative; std::string name; bool backed = false, installed = false; };
    std::vector<Member> members;
    for (const auto& entry : fs::recursive_directory_iterator(payload)) {
        if (!fs::is_regular_file(entry.symlink_status())) continue;
        auto relative = entry.path().lexically_relative(payload);
        members.push_back({relative, text(relative)});
    }
    std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
        if (binary(a.name) != binary(b.name)) return binary(a.name);
        return a.name < b.name;
    });
    for (const auto& member : members) destination(root, member.relative);
    InstallLock lock(root, stage);
    require(fs::create_directory(backup), "Cannot create transaction backup");
    private_directory(backup);
    std::vector<fs::path> created;
    created.reserve(16);
    Clock::time_point lockDeadline{};
    try {
        for (const auto& member : members) {
            create_parents(root, member.relative, &created);
            create_parents(backup, member.relative);
            const auto mode = fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read | fs::perms::others_read;
            fs::permissions(payload/member.relative, binary(member.name) ?
                            mode | fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec : mode,
                            fs::perm_options::replace);
        }
        for (auto& member : members) {
            const auto source = payload/member.relative, target = root/member.relative, saved = backup/member.relative;
            directory_chain(target.parent_path());
            directory_chain(source.parent_path());
            regular_file(source);
            destination(root, member.relative);
            if (fs::exists(checked_status(target))) {
                rename_file(target, saved, lockDeadline);
                member.backed = true;
            }
            rename_file(source, target, lockDeadline);
            member.installed = true;
        }
    } catch (const std::exception& failure) {
        const std::string reason = failure.what();
        std::string rollbackError;
        Clock::time_point rollbackDeadline{};
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            try {
                if (it->installed) {
                    directory_chain((root/it->relative).parent_path());
                    regular_file(root/it->relative);
                    require(!fs::exists(checked_status(payload/it->relative)), "Staged rollback member unexpectedly exists");
                    rename_file(root/it->relative, payload/it->relative, rollbackDeadline);
                }
                if (it->backed) {
                    directory_chain((backup/it->relative).parent_path());
                    regular_file(backup/it->relative);
                    require(!fs::exists(checked_status(root/it->relative)), "Installed rollback target unexpectedly exists");
                    rename_file(backup/it->relative, root/it->relative, rollbackDeadline);
                }
            } catch (const std::exception& error) {
                if (!rollbackError.empty()) rollbackError += "; ";
                rollbackError += error.what();
            }
        }
        clean_created(created);
        if (!rollbackError.empty()) {
            lock.retain = true;
            throw std::runtime_error(reason + ". Rollback incomplete: " + rollbackError +
                                     ". Recovery backup retained at " + text(backup));
        }
        fs::remove_all(backup);
        throw std::runtime_error("Update failed; previous installation restored: " + reason);
    }
    // Commit is complete. Cleanup failures must not misreport a successful replacement as rollback.
    std::error_code cleanup;
    fs::remove_all(backup, cleanup);
    if (cleanup) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Update committed; backup cleanup failed: %s", cleanup.message().c_str());
}

void launch_installer(const fs::path& stagePath, const fs::path& rootPath,
                      const fs::path& assetRoot, const fs::path& saveDirectory) {
    const auto stage = absolute_path(stagePath), root = absolute_path(rootPath);
    verify_stage(stage, root);
    detail::validate_payload(stage/"payload");
    private_directory(stage);
    const auto helper = stage/"helper";
    require(fs::create_directory(helper), "Detached update helper already exists");
    private_directory(helper);
    const auto copy = [&](const fs::path& relative) {
        directory_chain((root/relative).parent_path());
        regular_file(root/relative);
        fs::create_directories((helper/relative).parent_path());
        require(fs::copy_file(root/relative, helper/relative), "Cannot copy installed updater runtime");
        fs::permissions(helper/relative, relative == utf8_path(updaterName) ? fs::perms::owner_all :
                        fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    };
    copy(utf8_path(updaterName));
    for (const auto& entry : fs::directory_iterator(root))
        if (runtime_name(text(entry.path().filename()))) copy(entry.path().filename());
    const auto libraryRoot = root/"lib";
    if (fs::exists(checked_status(libraryRoot))) {
        directory_chain(libraryRoot);
        for (const auto& entry : fs::directory_iterator(libraryRoot))
            if (runtime_name(text(entry.path().filename()))) copy(fs::path("lib")/entry.path().filename());
    }
    const auto log = stage/"updater.log";
    require(!fs::exists(checked_status(log)), "Updater log already exists");
    {
        std::ofstream output(log, std::ios::binary);
        output << "Starting the trusted installed helper. If no helper-started record follows, the helper could not load.\n";
        output.close();
        require(bool(output), "Cannot create updater diagnostic log");
        fs::permissions(log, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    }
    start_process({text(helper/utf8_path(updaterName)), "--stage", text(stage), "--install-root", text(root),
                   "--asset-root", text(absolute_path(assetRoot)), "--save-dir", text(absolute_path(saveDirectory)),
                   "--parent-pid", std::to_string(process_id())}, helper);
}

void cleanup_completed_updates(const fs::path& installRoot) {
    try {
        const auto root = absolute_path(installRoot);
        directory_chain(root);
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = text(entry.path().filename());
            if (!name.starts_with(".yami-update-") || !safe_component(name)) continue;
            try {
                const auto stage = entry.path();
                directory_chain(stage);
#ifndef _WIN32
                struct stat information{};
                if (::stat(stage.c_str(), &information) != 0 || information.st_uid != geteuid() ||
                    (information.st_mode & (S_IWGRP | S_IWOTH))) continue;
#endif
                const auto marker = stage/"completed";
                if (!fs::exists(checked_status(marker))) continue;
                regular_file(marker);
                if (fs::file_size(marker) > 100) continue;
                std::ifstream input(marker, std::ios::binary);
                std::string magic, pidText, extra;
                if (!std::getline(input, magic) || !std::getline(input, pidText) ||
                    magic != completedMagic || std::getline(input, extra)) continue;
                std::uint64_t pid = 0;
                const auto parsed = std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
                if (parsed.ec != std::errc{} || parsed.ptr != pidText.data() + pidText.size() ||
                    process_alive(pid)) continue;
                input.close();
                bool safe = true;
                std::size_t count = 0;
                for (const auto& member : fs::recursive_directory_iterator(stage)) {
                    if (++count > maxEntries * 3) { safe = false; break; }
                    const auto found = checked_status(member.path());
                    if (fs::is_directory(found)) continue;
                    regular_file(member.path());
                }
                if (safe) fs::remove_all(stage);
            } catch (const std::exception& error) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot clean completed update stage: %s", error.what());
            }
        }
    } catch (const std::exception& error) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot inspect completed updates: %s", error.what());
    }
}
} // namespace yami::updates
