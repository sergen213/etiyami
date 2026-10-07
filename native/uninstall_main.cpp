#include "install_ownership.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace removal {
namespace fs = std::filesystem;
namespace own = yami::ownership;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string failure(const std::string& action) { return action + ": " + std::strerror(errno); }
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(other.value) { other.value = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (value >= 0) ::close(value);
        value = other.value; other.value = -1; return *this;
    }
};
struct Identity {
    dev_t device;
    ino_t inode;
    std::uint64_t mount;
    uid_t uid;
    mode_t mode;
    nlink_t links;
};
Identity identify(int fd) {
    struct stat st{}; struct statx sx{};
    require(::fstat(fd, &st) == 0, failure("Cannot inspect open file"));
    require(::syscall(SYS_statx, fd, "", AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,
                      STATX_MNT_ID, &sx) == 0 && (sx.stx_mask & STATX_MNT_ID),
            "Linux mount identity unavailable; refusing unsafe removal");
    return {st.st_dev, st.st_ino, sx.stx_mnt_id, st.st_uid, st.st_mode, st.st_nlink};
}
bool same(const Identity& a, const Identity& b) {
    return a.device == b.device && a.inode == b.inode && a.mount == b.mount &&
           a.uid == b.uid && (a.mode & S_IFMT) == (b.mode & S_IFMT);
}
void safe_directory(const Identity& id, bool ancestor) {
    require(S_ISDIR(id.mode) && !(id.mode & (S_IWGRP|S_IWOTH)) &&
            (id.uid == ::geteuid() || (ancestor && id.uid == 0)),
            "Unowned or shared writable directory; refusing removal");
}
void safe_file(const Identity& id, bool metadata) {
    require(S_ISREG(id.mode) && id.uid == ::geteuid() && id.links == 1 &&
            !(id.mode & (S_ISUID|S_ISGID)) &&
            !(id.mode & (metadata ? 0077 : (S_IWGRP|S_IWOTH))),
            "Unowned, linked, shared or unsafe file; refusing removal");
}
fs::path checked_absolute(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path &&
            own::valid_text(path.string()), "Expected an exact absolute installation path");
    return path;
}
// Directory identities, not live filenames, are the traversal authority.
struct Tree {
    std::map<std::string, Identity> directories;
    fs::path rootAnchor;
    Fd open_directory(const fs::path& path, bool snapshot = false, bool optional = false) {
        if (!rootAnchor.empty() && path != rootAnchor && !path.string().starts_with(rootAnchor.string() + "/")) {
            auto anchor = open_directory(rootAnchor);
        }
        Fd current(::open("/", O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(current.value >= 0, failure("Cannot open filesystem root"));
        fs::path prefix = "/";
        for (const auto& part : path.relative_path()) {
            Fd next(::openat(current.value, part.c_str(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
            if (next.value < 0 && errno == ENOENT && optional) return Fd();
            require(next.value >= 0, failure("Cannot safely open directory " + (prefix/part).string()));
            prefix /= part;
            const auto id = identify(next.value);
            safe_directory(id, true);
            const auto key = prefix.string();
            auto found = directories.find(key);
            if (found != directories.end()) require(same(id, found->second), "Directory replaced: " + key);
            else {
                require(snapshot, "Unrecorded directory appeared: " + key);
                directories.emplace(key, id);
            }
            current = std::move(next);
        }
        return current;
    }
};
struct Target {
    fs::path path;
    Identity id{};
    bool present = false;
    bool metadata = false;
};
Target snapshot(Tree& tree, const fs::path& path, bool metadata = false) {
    auto parent = tree.open_directory(path.parent_path(), true, !metadata);
    Target target{path, {}, false, metadata};
    if (parent.value < 0) return target;
    Fd file(::openat(parent.value, path.filename().c_str(), O_PATH|O_NOFOLLOW|O_CLOEXEC));
    if (file.value < 0 && errno == ENOENT && !metadata) return target;
    require(file.value >= 0, failure("Cannot inspect " + path.string()));
    target.id = identify(file.value); target.present = true;
    safe_file(target.id, metadata);
    const auto parentId = identify(parent.value);
    require(target.id.device == parentId.device && target.id.mount == parentId.mount,
            "Mounted file is not an owned installation member: " + path.string());
    return target;
}
std::string read_file(Tree& tree, const Target& target, std::size_t limit) {
    auto parent = tree.open_directory(target.path.parent_path());
    Fd file(::openat(parent.value, target.path.filename().c_str(), O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    require(file.value >= 0 && same(identify(file.value), target.id), "File changed while reading: " + target.path.string());
    safe_file(identify(file.value), target.metadata);
    std::string result; std::array<char, 8192> buffer{};
    for (;;) {
        const auto n = ::read(file.value, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        require(n >= 0, failure("Cannot read " + target.path.string()));
        if (!n) break;
        require(result.size() + static_cast<std::size_t>(n) <= limit, "Ownership file is too large");
        result.append(buffer.data(), static_cast<std::size_t>(n));
    }
    require(same(identify(file.value), target.id), "File changed during read");
    return result;
}
std::string desktop_value(std::string_view value) {
    std::string result;
    for (const char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        case ' ': result += "\\s"; break;
        default: result += c;
        }
    }
    return result;
}
bool asset_file(std::string_view value) {
    if (!value.starts_with("game/")) return false;
    for (const auto& part : fs::path(value)) {
        auto name = part.string();
        for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (name == "game.ini" || name == "save" || name == "saves" || name == "checkpoints") return false;
    }
    return true;
}
bool installed_file(std::string_view value) {
    return asset_file(value) || value == "eti-yami.png" || value == own::scriptName ||
           value == own::markerName || value == own::installedName || value == own::engineName ||
           value == std::string(own::helperDirectory) + "/" + std::string(own::helperName);
}
bool late_file(const fs::path& path) {
    const auto name = path.filename().string();
    return name == own::markerName || name == own::installedName || name == own::engineName ||
           name == own::scriptName || (name == own::helperName && path.parent_path().filename() == own::helperDirectory);
}
struct Plan {
    Tree tree;
    fs::path root;
    Identity rootId{};
    std::vector<Target> early, late, shortcuts;
    std::set<fs::path> prune;
};
Plan preflight(const fs::path& root) {
    Plan plan; plan.root = checked_absolute(root);
    require(std::distance(root.begin(), root.end()) >= 4, "Refusing a top-level or system installation root");
    for (const auto forbidden : {"/", "/home", "/usr", "/usr/local", "/etc", "/var", "/opt", "/mnt", "/media"})
        require(root != forbidden, "Refusing a system directory");
    if (const char* home = ::getenv("HOME")) require(root != fs::path(home).lexically_normal(), "Refusing the home directory");
    auto rootFd = plan.tree.open_directory(root, true);
    plan.rootId = identify(rootFd.value); safe_directory(plan.rootId, false);
    plan.tree.rootAnchor = root;
    const auto marker = snapshot(plan.tree, root/std::string(own::markerName), true);
    const auto installed = snapshot(plan.tree, root/std::string(own::installedName), true);
    const auto engine = snapshot(plan.tree, root/std::string(own::engineName), true);
    require(read_file(plan.tree, marker, 256) == own::marker, "Invalid installation marker");
    const auto assets = own::parse_installed(read_file(plan.tree, installed, own::maxInstalledBytes));
    const auto engines = own::parse_engine(read_file(plan.tree, engine, own::maxEngineBytes));
    std::set<std::string> paths;
    for (const auto& path : assets.files) {
        require(installed_file(path), "Inventory claims a non-installed or saved file: " + path);
        paths.insert(path);
    }
    paths.insert(engines.begin(), engines.end());
    for (const auto& relative : paths) {
        const auto path = root/relative;
        const bool metadata = relative == own::markerName || relative == own::installedName || relative == own::engineName;
        auto target = snapshot(plan.tree, path, metadata);
        if (target.present) require(target.id.device == plan.rootId.device && target.id.mount == plan.rootId.mount,
                                    "Foreign mount inside installation: " + path.string());
        for (auto parent = path.parent_path(); parent != root; parent = parent.parent_path()) {
            const auto found = plan.tree.directories.find(parent.string());
            if (found != plan.tree.directories.end()) {
                require(found->second.device == plan.rootId.device && found->second.mount == plan.rootId.mount &&
                        found->second.uid == ::geteuid(), "Foreign or unowned asset directory");
                plan.prune.insert(parent);
            }
        }
        (late_file(path) ? plan.late : plan.early).push_back(std::move(target));
    }
    for (const auto& path : assets.shortcuts) {
        auto target = snapshot(plan.tree, checked_absolute(path));
        auto shortcutParent = plan.tree.open_directory(fs::path(path).parent_path(), false, true);
        if (shortcutParent.value >= 0)
            require(identify(shortcutParent.value).uid == ::geteuid(), "Shortcut directory is not user-owned");
        if (target.present) {
            const auto content = read_file(plan.tree, target, 65536);
            std::size_t ownerCount = 0, setupCount = 0;
            for (std::size_t start = 0; start < content.size();) {
                const auto end = content.find('\n', start);
                const auto line = content.substr(start, end == std::string::npos ? end : end-start);
                if (line.starts_with("X-ETIYami-InstallRoot=")) {
                    ++ownerCount;
                    require(line == "X-ETIYami-InstallRoot=" + desktop_value(root.string()), "Shortcut belongs to another installation");
                }
                if (line == "X-ETIYami-Setup=true") ++setupCount;
                if (end == std::string::npos) break;
                start = end + 1;
            }
            require(content.starts_with("[Desktop Entry]\n") && ownerCount == 1 && setupCount == 1,
                    "Refusing an unowned shortcut: " + path);
        }
        plan.shortcuts.push_back(std::move(target));
    }
    return plan;
}
std::string process_text(const std::string& path) {
    Fd file(::open(path.c_str(), O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    if (file.value < 0) return {};
    std::array<char, 8192> text{};
    ssize_t n;
    do { n = ::read(file.value, text.data(), text.size()); } while (n < 0 && errno == EINTR);
    return n > 0 ? std::string(text.data(), static_cast<std::size_t>(n)) : std::string{};
}
std::string_view process_field(std::string_view status, std::string_view key) {
    const auto start = status.starts_with(key) ? 0 : status.find("\n" + std::string(key));
    if (start == std::string_view::npos) return {};
    status.remove_prefix(start + (start == 0 ? 0 : 1) + key.size());
    status = status.substr(0, status.find('\n'));
    while (!status.empty() && (status.front() == ' ' || status.front() == '\t')) status.remove_prefix(1);
    return status;
}
bool process_user(std::string_view status) {
    auto ids = process_field(status, "Uid:");
    for (int i = 0; i < 2 && !ids.empty(); ++i) {
        unsigned id = 0;
        const auto parsed = std::from_chars(ids.data(), ids.data() + ids.size(), id);
        if (parsed.ec != std::errc{}) return false;
        if (id == ::geteuid()) return true;
        ids.remove_prefix(static_cast<std::size_t>(parsed.ptr - ids.data()));
        while (!ids.empty() && (ids.front() == ' ' || ids.front() == '\t')) ids.remove_prefix(1);
    }
    return false;
}
bool product_process(std::string_view name) {
    return name == "yami-native" || name == "yami-launcher" || name == "yami-updater";
}
void running_guard(const fs::path& root) {
    DIR* raw = ::opendir("/proc"); require(raw, failure("Cannot inspect running applications"));
    struct Close { DIR* value; ~Close() { ::closedir(value); } } close{raw};
    while (auto* entry = ::readdir(raw)) {
        const std::string pid(entry->d_name);
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos || pid == std::to_string(::getpid())) continue;
        const auto directory = "/proc/" + pid;
        struct stat st{};
        if (::stat(directory.c_str(), &st) < 0) continue;
        std::string status;
        // Nondumpable user processes can have root-owned /proc inodes. Their
        // bounded public status identifies the real caller without ptrace access.
        if (st.st_uid != ::geteuid()) {
            if (st.st_uid != 0) continue;
            status = process_text(directory + "/status");
            if (!process_user(status)) continue;
        }
        std::array<char, 8192> buffer{};
        const auto n = ::readlink((directory + "/exe").c_str(), buffer.data(), buffer.size());
        if (n < 0 && (errno == ENOENT || errno == ESRCH)) continue;
        if (n >= 0 && static_cast<std::size_t>(n) < buffer.size()) {
            const std::string_view executable(buffer.data(), static_cast<std::size_t>(n));
            require(!executable.starts_with(root.string() + "/"),
                    "Close the game, launcher and updater before uninstalling (process " + pid + ")");
            continue;
        }
        if (status.empty()) status = process_text(directory + "/status");
        const auto state = process_field(status, "State:");
        if (state.starts_with("Z") || state.starts_with("X")) continue;
        auto comm = process_text(directory + "/comm");
        if (!comm.empty() && comm.back() == '\n') comm.pop_back();
        const auto name = comm.empty() ? process_field(status, "Name:") : std::string_view(comm);
        // Access denial on an unrelated sandbox is not evidence of a running
        // game. A product-named inaccessible process remains an unsafe candidate.
        if (!product_process(name)) continue;
        const auto cwdBytes = ::readlink((directory + "/cwd").c_str(), buffer.data(), buffer.size());
        const bool inRoot = cwdBytes >= 0 && static_cast<std::size_t>(cwdBytes) < buffer.size() &&
            (std::string_view(buffer.data(), static_cast<std::size_t>(cwdBytes)) == root.string() ||
             std::string_view(buffer.data(), static_cast<std::size_t>(cwdBytes)).starts_with(root.string() + "/"));
        require(false, "Cannot inspect an active ETI Yami candidate safely; close " + std::string(name) +
                " (process " + pid + (inRoot ? ", installation working directory" : "") + ") before uninstalling");
    }
}
std::string random_name() {
    std::array<unsigned char, 12> bytes{};
    require(::getrandom(bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()), failure("Cannot allocate private recovery name"));
    std::string name = ".eti-yami-remove-recovery-";
    constexpr char hex[] = "0123456789abcdef";
    for (auto b : bytes) { name += hex[b >> 4]; name += hex[b & 15]; }
    return name;
}
fs::path descriptor_path(int fd, const fs::path& fallback) {
    std::array<char, 8192> text{};
    const auto n = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), text.data(), text.size());
    return n > 0 && static_cast<std::size_t>(n) < text.size()
        ? fs::path(std::string(text.data(), static_cast<std::size_t>(n))) : fallback;
}
int rename_new(int from, const std::string& name, int to, const std::string& dest);
bool remove_created_directory(int parent, const std::string& name, const Identity& expected) {
    const auto detached = random_name();
    if (rename_new(parent, name, parent, detached) != 0) return false;
    try {
        Fd moved(::openat(parent, detached.c_str(), O_PATH|O_NOFOLLOW|O_CLOEXEC));
        if (moved.value >= 0 && same(identify(moved.value), expected) &&
            ::unlinkat(parent, detached.c_str(), AT_REMOVEDIR) == 0) return true;
    } catch (...) {
        if (rename_new(parent, detached, parent, name) != 0)
            std::cerr << "Detached directory retained at " << descriptor_path(parent, "/")/detached << '\n';
        throw;
    }
    if (rename_new(parent, detached, parent, name) != 0)
        std::cerr << "Detached directory retained at " << descriptor_path(parent, "/")/detached << '\n';
    return false;
}
struct Recovery {
    fs::path path;
    Fd parent, directory;
    Identity id{}, parentId{};
    bool retained = false;
    Recovery(Tree& tree, const fs::path& parentPath) {
        parent = tree.open_directory(parentPath);
        parentId = identify(parent.value);
        path = parentPath/random_name();
        require(::mkdirat(parent.value, path.filename().c_str(), 0700) == 0, failure("Cannot create private recovery directory"));
        directory = Fd(::openat(parent.value, path.filename().c_str(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(directory.value >= 0, failure("Cannot open private recovery directory"));
        id = identify(directory.value); safe_directory(id, false);
        require(id.mount == parentId.mount && id.device == parentId.device, "Foreign recovery mount");
        tree.directories.emplace(path.string(), id);
    }
    fs::path location() const { return descriptor_path(directory.value, path); }
    ~Recovery() {
        try {
            if (!retained && directory.value >= 0 &&
                !remove_created_directory(parent.value, path.filename().string(), id))
                std::cerr << "Recovery directory retained: " << location() << '\n';
        } catch (...) { std::cerr << "Recovery directory retained: " << location() << '\n'; }
    }
    void verify(Tree& tree) {
        auto p = tree.open_directory(path.parent_path());
        require(same(identify(p.value), parentId), "Recovery parent changed");
        Fd named(::openat(parent.value, path.filename().c_str(), O_PATH|O_NOFOLLOW|O_CLOEXEC));
        require(named.value >= 0 && same(identify(named.value), id), "Recovery directory changed: " + path.string());
    }
};
int rename_new(int from, const std::string& name, int to, const std::string& dest) {
    return static_cast<int>(::syscall(SYS_renameat2, from, name.c_str(), to, dest.c_str(), RENAME_NOREPLACE));
}
struct Detached {
    Target target;
    Recovery* recovery;
    std::string name;
    Fd originalParent;
    bool active = true;
    Detached(Target t, Recovery& r, std::string n, Fd parent)
        : target(std::move(t)), recovery(&r), name(std::move(n)), originalParent(std::move(parent)) {}
    Detached(Detached&& other) noexcept
        : target(std::move(other.target)), recovery(other.recovery), name(std::move(other.name)),
          originalParent(std::move(other.originalParent)), active(other.active) { other.active = false; }
    Detached(const Detached&) = delete;
    ~Detached() { if (active) restore(); }
    void restore() noexcept {
        if (rename_new(recovery->directory.value, name, originalParent.value, target.path.filename().string()) == 0) {
            active = false;
            const auto actualParent = descriptor_path(originalParent.value, target.path.parent_path());
            if (actualParent != target.path.parent_path())
                std::cerr << "Parent moved; captured file restored at " << actualParent/target.path.filename() << ".\n";
        } else {
            recovery->retained = true;
            std::cerr << "Recovery retained at " << recovery->location()/name << "; original path " << target.path << " was not overwritten.\n";
        }
    }
    void verify(Tree& tree) {
        recovery->verify(tree);
        auto parent = tree.open_directory(target.path.parent_path());
        require(same(identify(parent.value), identify(originalParent.value)), "Original parent replaced");
        Fd file(::openat(recovery->directory.value, name.c_str(), O_PATH|O_NOFOLLOW|O_CLOEXEC));
        require(file.value >= 0 && same(identify(file.value), target.id), "Detached target was substituted: " + target.path.string());
        const auto id = identify(file.value);
        if (S_ISREG(target.id.mode)) safe_file(id, target.metadata);
        else safe_directory(id, false);
    }
    void erase(Tree& tree) {
        verify(tree);
        // Linux unlinkat is name-based, not inode-conditional. The current UID is
        // trusted: this private 0700 recovery namespace has no legitimate competing
        // writer under the product lock. A malicious same-UID writer can still race
        // this final unlink (and already controls the script and ownership ledgers).
        require(::unlinkat(recovery->directory.value, name.c_str(), S_ISDIR(target.id.mode) ? AT_REMOVEDIR : 0) == 0,
                failure("Cannot delete owned detached member " + target.path.string()));
        active = false;
    }
};
Detached detach(Tree& tree, const Target& target, Recovery& recovery, const std::string& name) {
    recovery.verify(tree);
    auto parent = tree.open_directory(target.path.parent_path());
    require(rename_new(parent.value, target.path.filename().string(), recovery.directory.value, name) == 0,
            failure("Cannot detach " + target.path.string()));
    Detached result(target, recovery, name, std::move(parent));
    try { result.verify(tree); }
    catch (...) {
        // Never delete the unexpected inode captured by a rename race.
        result.restore();
        throw;
    }
    return result;
}
struct MetadataBackup { Target original, copy; Recovery* storage = nullptr; };
void write_private(int parent, const std::string& name, std::string_view bytes, mode_t mode = 0600) {
    Fd file(::openat(parent, name.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, mode));
    require(file.value >= 0, failure("Cannot retain ownership metadata"));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = ::write(file.value, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, failure("Cannot write retained ownership metadata"));
        offset += static_cast<std::size_t>(n);
    }
    require(::fsync(file.value) == 0, failure("Cannot synchronize retained ownership metadata"));
}
void restore_metadata(Tree& tree, Recovery& recovery, const std::vector<MetadataBackup>& backups, std::size_t& serial) noexcept {
    for (const auto& backup : backups) {
        auto& storage = backup.storage ? *backup.storage : recovery;
        try {
            storage.verify(tree);
            auto parent = tree.open_directory(backup.original.path.parent_path());
            Fd named(::openat(parent.value, backup.original.path.filename().c_str(), O_PATH|O_NOFOLLOW|O_CLOEXEC));
            if (named.value < 0 && errno == ENOENT) {
                auto detached = detach(tree, backup.copy, storage, std::to_string(serial++));
                require(rename_new(storage.directory.value, detached.name, parent.value,
                                   backup.original.path.filename().string()) == 0,
                        "Cannot restore ownership metadata without overwriting a replacement");
                detached.active = false;
            } else {
                require(named.value >= 0 && same(identify(named.value), backup.original.id),
                        "Ownership metadata was replaced; backup must be retained");
                auto detached = detach(tree, backup.copy, storage, std::to_string(serial++));
                detached.erase(tree);
            }
        } catch (const std::exception& error) {
            storage.retained = true;
            std::cerr << error.what() << ". Retained uninstall support: " << storage.location()/backup.copy.path.filename() << '\n';
        }
    }
}
struct UpdateLock {
    fs::path root;
    Fd rootFd, lockFd;
    Identity id{};
    UpdateLock(Tree& t, const fs::path& r) : root(r), rootFd(t.open_directory(r)) {
        require(::mkdirat(rootFd.value, ".yami-update-lock", 0700) == 0,
                "An updater/remover lock exists or cannot be acquired; close active applications and resolve interrupted updates first");
        lockFd = Fd(::openat(rootFd.value, ".yami-update-lock", O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(lockFd.value >= 0, failure("Cannot open exclusive update lock"));
        id = identify(lockFd.value); safe_directory(id, false);
    }
    ~UpdateLock() {
        try {
            if (lockFd.value < 0) return;
            if (!remove_created_directory(rootFd.value, ".yami-update-lock", id))
                std::cerr << "Exclusive lock retained; inspect " << descriptor_path(rootFd.value, root) << "/.yami-update-lock\n";
        } catch (...) { std::cerr << "Exclusive lock retained; inspect " << root << "/.yami-update-lock\n"; }
    }
};
void prune_directory(Plan& plan, const fs::path& path, Recovery& recovery, std::size_t& serial) {
    const auto found = plan.tree.directories.find(path.string());
    if (found == plan.tree.directories.end()) return;
    auto directory = plan.tree.open_directory(path, false, true);
    if (directory.value < 0) return;
    // rmdir after detachment, rather than recursively deleting a directory.
    Target target{path, found->second, true, false};
    auto detached = detach(plan.tree, target, recovery, std::to_string(serial++));
    detached.verify(plan.tree);
    if (::unlinkat(recovery.directory.value, detached.name.c_str(), AT_REMOVEDIR) == 0) {
        detached.active = false;
        return;
    }
    const int error = errno;
    if (error == ENOTEMPTY || error == EEXIST) { detached.restore(); return; }
    errno = error;
    throw std::runtime_error(failure("Cannot prune detached directory " + path.string()));
}
int uninstall(const fs::path& root, bool yes) {
    require(::geteuid() != 0 && ::getuid() == ::geteuid() && ::getgid() == ::getegid() &&
            !::getenv("SUDO_USER") && !::getenv("SUDO_UID") && !::getenv("SUDO_COMMAND"),
            "Run uninstall as the installing user, never with root, sudo or setuid");
    auto plan = preflight(root);
    running_guard(root);
    std::cout << "Installation: " << root << "\nRemoves only recorded game/engine files and owned shortcuts.\n"
                 "Saved games, settings and unrecorded files will be kept.\n";
    if (!yes) {
        require(::isatty(STDIN_FILENO), "Interactive terminal required; no removal was authorized");
        std::cout << "Uninstall ETI Yami? [yes/No] " << std::flush;
        std::string answer; std::getline(std::cin, answer);
        if (answer != "yes" && answer != "y" && answer != "YES" && answer != "Y") {
            std::cout << "Cancelled. Nothing was removed.\n"; return 0;
        }
    }
    UpdateLock lock(plan.tree, root);
    // Confirmation may take arbitrarily long; snapshot again under the updater lock.
    auto current = preflight(root);
    require(same(current.rootId, plan.rootId), "Installation root changed during confirmation");
    for (const auto* before : {&plan.early, &plan.late, &plan.shortcuts}) {
        const auto* after = before == &plan.early ? &current.early : before == &plan.late ? &current.late : &current.shortcuts;
        require(before->size() == after->size(), "Ownership changed during confirmation");
        for (std::size_t i = 0; i < before->size(); ++i)
            require((*before)[i].path == (*after)[i].path && (*before)[i].present == (*after)[i].present &&
                    (!(*before)[i].present || same((*before)[i].id, (*after)[i].id)),
                    "Owned member changed during confirmation; no files removed");
    }
    plan = std::move(current);
    running_guard(root);
    Recovery recovery(plan.tree, root);
    std::size_t serial = 0;
    std::vector<MetadataBackup> backups;
    std::map<fs::path, Recovery> shortcutRecoveries;
    try {
        for (const auto& target : plan.early) if (target.present) {
            auto detached = detach(plan.tree, target, recovery, std::to_string(serial++));
            detached.erase(plan.tree);
        }
        std::vector<fs::path> directories(plan.prune.begin(), plan.prune.end());
        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
        });
        for (const auto& path : directories) if (path != root/std::string(own::helperDirectory))
            prune_directory(plan, path, recovery, serial);
        // All late support, metadata and shortcut files are recoverable until the
        // removal commit succeeds. Copies stay on each original filesystem.
        auto backup_file = [&](const Target& target, Recovery& storage) {
            const auto name = "backup-" + target.path.filename().string();
            const auto bytes = read_file(plan.tree, target, 64 * 1024 * 1024);
            write_private(storage.directory.value, name, bytes, target.id.mode & 0700);
            backups.push_back({target, snapshot(plan.tree, storage.path/name, true), &storage});
        };
        for (const auto& target : plan.late) if (target.present) backup_file(target, recovery);
        for (const auto& target : plan.shortcuts) if (target.present) {
            auto entry = shortcutRecoveries.try_emplace(target.path.parent_path(), plan.tree, target.path.parent_path());
            backup_file(target, entry.first->second);
        }
        // Keep every authoritative metadata inode until all late members are detached
        // and checked. Failed detachment restores them without overwriting replacements.
        std::stable_sort(plan.late.begin(), plan.late.end(), [](const auto& a, const auto& b) {
            return !a.metadata && b.metadata;
        });
        std::vector<Detached> late;
        for (const auto& target : plan.late) if (target.present)
            late.push_back(detach(plan.tree, target, recovery, std::to_string(serial++)));
        for (const auto& target : plan.shortcuts) if (target.present)
            late.push_back(detach(plan.tree, target, shortcutRecoveries.at(target.path.parent_path()), std::to_string(serial++)));
        for (auto& detached : late) detached.verify(plan.tree);
        for (auto& detached : late) detached.erase(plan.tree);
        prune_directory(plan, root/std::string(own::helperDirectory), recovery, serial);
    } catch (...) {
        restore_metadata(plan.tree, recovery, backups, serial);
        std::cerr << "Removal stopped. Remaining ownership metadata stays in " << descriptor_path(lock.rootFd.value, root)
                  << ". Recovery storage (if retained): " << recovery.location()
                  << ". Resolve the reported conflict and rerun uninstall.\n";
        throw;
    }
    // The installation is fully removed now. A private backup-cleanup failure
    // retains bytes and reports them, rather than pretending removal can roll back.
    for (const auto& backup : backups) {
        auto& storage = *backup.storage;
        try {
            auto detached = detach(plan.tree, backup.copy, storage, std::to_string(serial++));
            detached.erase(plan.tree);
        } catch (const std::exception& error) {
            storage.retained = true;
            std::cerr << "Uninstall completed, but support backup cleanup stopped: " << error.what()
                      << ". Inspect retained recovery directory " << storage.location() << ".\n";
        }
    }
    std::cout << "ETI Yami uninstalled. Saved/settings/unrecorded files, if any, remain at " << root << ".\n";
    return 0;
}
} // namespace removal

#ifndef YAMI_REMOVE_NO_MAIN
int main(int argc, char** argv) {
    try {
        std::filesystem::path root; bool yes = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--help") { std::cout << "Usage: yami-remove --root ABSROOT [--yes]\n"; return 0; }
            if (arg == "--root" && i + 1 < argc && root.empty()) root = argv[++i];
            else if (arg == "--yes" && !yes) yes = true;
            else throw std::runtime_error("Usage: yami-remove --root ABSROOT [--yes]");
        }
        removal::require(!root.empty(), "Usage: yami-remove --root ABSROOT [--yes]");
        return removal::uninstall(root, yes);
    } catch (const std::exception& error) {
        std::cerr << "Cannot uninstall ETI Yami: " << error.what() << '\n'; return 1;
    }
}
#endif
