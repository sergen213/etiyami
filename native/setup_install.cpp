#include "setup.hpp"
#include "install_ownership.hpp"
#include "media.hpp"
#include <SDL3/SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace yami::setup {
namespace {
namespace fs = std::filesystem;
using namespace yami::ownership;
constexpr auto shortcutName = playShortcut;
constexpr std::array executables{"yami-native", "yami-updater", "yami-launcher"};
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void report(const Progress& progress, std::string_view phase, std::uint64_t current = 0, std::uint64_t total = 0) {
    if (progress && !progress(phase, current, total)) throw std::runtime_error("Installation cancelled");
}
fs::path absolute_path(const fs::path& path) {
    require(!path.empty(), "Installation path is empty");
    auto result = fs::absolute(path).lexically_normal();
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    const auto value = result.string();
    require(value.find_first_of("\r\n") == std::string::npos && value.find('\0') == std::string::npos,
            "Installation paths cannot contain line breaks or NUL characters");
    return result;
}
fs::file_status checked_status(const fs::path& path) {
    std::error_code error;
    const auto result = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return fs::file_status(fs::file_type::not_found);
    if (error) throw fs::filesystem_error("Cannot inspect installation path", path, error);
    require(!fs::is_symlink(result), "Refusing symbolic-link installation target: " + path.string());
    return result;
}
void owned_file(const fs::path& path) {
    struct stat info{};
    require(fs::is_regular_file(checked_status(path)) && ::lstat(path.c_str(), &info) == 0 &&
            info.st_uid == geteuid() && info.st_nlink == 1 && !(info.st_mode & (S_IWGRP | S_IWOTH)),
            "File must be a private, user-owned regular file: " + path.string());
}
std::string read_small(const fs::path& path, std::size_t maximum = 65536) {
    owned_file(path);
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "Cannot open ownership record: " + path.string());
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat info{}, current{};
    require(::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
            info.st_nlink == 1 && !(info.st_mode & (S_IWGRP | S_IWOTH)) && info.st_size >= 0 &&
            static_cast<std::uint64_t>(info.st_size) <= maximum,
            "Invalid or oversized private ownership record: " + path.string());
    std::string result(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        const auto count = ::read(fd, result.data() + offset, result.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "Cannot read ownership record: " + path.string());
        offset += static_cast<std::size_t>(count);
    }
    require(::lstat(path.c_str(), &current) == 0 && current.st_dev == info.st_dev && current.st_ino == info.st_ino,
            "Ownership record changed while reading: " + path.string());
    return result;
}
void write_file(const fs::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    require(bool(stream), "Cannot write installer file: " + path.string());
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
}
void directory_chain(const fs::path& path, std::vector<fs::path>* created = nullptr) {
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        auto found = checked_status(current);
        if (!fs::exists(found) && created) {
            require(fs::create_directory(current), "Cannot create installation directory: " + current.string());
            created->push_back(current);
            fs::permissions(current, fs::perms::owner_all, fs::perm_options::replace);
            found = checked_status(current);
        }
        if (!fs::exists(found) && !created) return;
        require(fs::is_directory(found), "Installation parent is not a directory: " + current.string());
    }
}
fs::path writable_parent(fs::path path) {
    directory_chain(path);
    while (!fs::exists(checked_status(path))) path = path.parent_path();
    require(::access(path.c_str(), W_OK | X_OK) == 0, "Choose a writable installation folder: " + path.string());
    for (auto ancestor = path; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        struct stat info{};
        require(::lstat(ancestor.c_str(), &info) == 0, "Cannot inspect installation parent: " + ancestor.string());
        const bool shared = info.st_mode & (S_IWGRP | S_IWOTH);
        const bool protectedSticky = (info.st_mode & S_ISVTX) && (info.st_uid == 0 || info.st_uid == geteuid());
        require(!shared || protectedSticky,
                "Choose a private folder inside your home; another account can replace this parent: " + ancestor.string());
        if (ancestor == ancestor.root_path()) break;
    }
    return path;
}
struct CreatedDirectories {
    std::vector<fs::path> paths;
    ~CreatedDirectories() {
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
            std::error_code error; fs::remove(*it, error); // Only empty directories created by this operation.
        }
    }
};
struct TemporaryDirectory {
    fs::path path;
    bool retain = false;
    struct stat identity{};
    explicit TemporaryDirectory(const fs::path& parent) {
        auto pattern = (parent/".yami-setup-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "Cannot stage installation in " + parent.string() + ": " + std::strerror(errno));
        path = pattern;
        require(::lstat(path.c_str(), &identity) == 0, "Cannot inspect installer staging directory");
    }
    ~TemporaryDirectory() {
        struct stat current{};
        if (!retain && ::lstat(path.c_str(), &current) == 0 && S_ISDIR(current.st_mode) &&
            current.st_dev == identity.st_dev && current.st_ino == identity.st_ino) {
            std::error_code error; fs::remove_all(path, error);
        }
    }
};
void rename_new(const fs::path& from, const fs::path& to) {
    // RENAME_NOREPLACE prevents a concurrently created unrelated target from being clobbered.
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1U) != 0)
        throw fs::filesystem_error("Cannot publish installation without replacing an existing target", from, to,
                                   std::error_code(errno, std::generic_category()));
}
bool within(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c) if (c == child.end() || *p != *c) return false;
    return true;
}
std::string desktop_value(std::string_view text) {
    std::string output;
    for (const auto c : text) {
        switch (c) {
        case '\\': output += "\\\\"; break;
        case ' ': output += "\\s"; break;
        case '\t': output += "\\t"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        default: output += c;
        }
    }
    return output;
}
std::string exec_argument(std::string_view text) {
    std::string quoted = "\"";
    for (const auto c : text) {
        if (c == '%') quoted += "%%";
        else {
            if (c == '\\' || c == '"' || c == '`' || c == '$') quoted += '\\';
            quoted += c;
        }
    }
    quoted += '"';
    return desktop_value(quoted);
}
std::string shortcut_owner(const fs::path& root) {
    return "X-ETIYami-InstallRoot=" + desktop_value(root.string()) + "\n";
}
std::string desktop_entry(const fs::path& root) {
    // GIO checks argv[0] before expanding %% in paths; env avoids that broken preflight.
    return "[Desktop Entry]\nType=Application\nVersion=1.0\nName=ETI Yami\n"
           "Comment=Yami: Mekanik İstila\nExec=/usr/bin/env -- " + exec_argument((root/"yami-launcher").string()) +
           "\nIcon=" + desktop_value((root/"eti-yami.png").string()) +
           "\nPath=" + desktop_value(root.string()) +
           "\nTerminal=false\nCategories=Game;\nX-ETIYami-Setup=true\n" + shortcut_owner(root);
}
std::string uninstall_entry(const fs::path& root) {
    return "[Desktop Entry]\nType=Application\nVersion=1.0\nName=Uninstall ETI Yami\n"
           "Comment=Remove installed ETI Yami files; preserve saves and settings\n"
           "Exec=/usr/bin/env -- /bin/sh " + exec_argument((root/scriptName).string()) +
           "\nIcon=" + desktop_value((root/"eti-yami.png").string()) +
           "\nPath=" + desktop_value(root.string()) +
           "\nTerminal=true\nCategories=Game;\nX-ETIYami-Setup=true\n" + shortcut_owner(root);
}
bool shortcut_exists(const fs::path& path, const fs::path& root) {
    if (!fs::exists(checked_status(path))) return false;
    const auto content = read_small(path);
    require(content.starts_with("[Desktop Entry]\n") &&
            content.find("\nX-ETIYami-Setup=true\n") != std::string::npos &&
            content.find("\n" + shortcut_owner(root)) != std::string::npos,
            "Refusing to overwrite an unowned shortcut: " + path.string() +
            ". Move or rename it yourself, then retry.");
    return true;
}
void validate_owned_installation(const fs::path& root) {
    struct stat info{};
    require(fs::is_directory(checked_status(root)) && ::lstat(root.c_str(), &info) == 0 &&
            info.st_uid == geteuid() && !(info.st_mode & (S_IWGRP | S_IWOTH)),
            "Installation root is not a private user-owned directory: " + root.string());
    require(fs::exists(checked_status(root/markerName)) && read_small(root/markerName) == marker,
            "Refusing to overwrite an unowned installation folder: " + root.string() +
            ". Choose a new empty location or move the existing folder yourself.");
    for (const auto* name : executables) {
        owned_file(root/name);
        require(::access((root/name).c_str(), X_OK) == 0, "Installed engine is not executable: " + (root/name).string());
    }
    directory_chain(root/"game/data/menu");
    owned_file(root/"game/data/menu/menulist.xml");
    owned_file(root/"eti-yami.png");
}
std::vector<fs::path> engine_files(const fs::path& engine) {
    directory_chain(engine);
    require(fs::is_directory(checked_status(engine)), "Engine payload is missing: " + engine.string());
    std::vector<fs::path> files;
    for (const auto* name : executables) {
        const auto path = engine/name;
        require(fs::is_regular_file(checked_status(path)) && ::access(path.c_str(), X_OK) == 0,
                "Trusted native engine executable is missing: " + path.string());
        files.push_back(path);
    }
    static const std::regex library(R"(lib[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*)");
    static const std::regex hostLibrary(
        R"((?:lib(?:c|m|dl|pthread|rt|resolv|util|anl|nss_[^.]+|stdc\+\+|gcc_s)|lib(?:GL|GLX|GLdispatch|OpenGL|EGL|GLESv[12]|vulkan|drm(?:_[^.]+)?|gbm|cuda|nvidia[^.]*|vdpau|va(?:-x11|-drm|-wayland)?))\.so(?:\.[0-9]+)*)");
    for (const auto& entry : fs::directory_iterator(engine)) {
        const auto name = entry.path().filename().string();
        if (!std::regex_match(name, library)) continue;
        require(!std::regex_match(name, hostLibrary), "Engine payload improperly bundles a host OS/graphics runtime: " + name);
        require(fs::is_regular_file(checked_status(entry.path())), "Runtime payload member is not a regular file: " + entry.path().string());
        files.push_back(entry.path());
    }
    require(files.size() <= maxEnginePaths, "Engine payload has too many members");
    return files;
}
void validate_icon(const fs::path& path) {
    const auto size = fs::file_size(path);
    require(size >= 22 && size <= 1024 * 1024, "Original ICO exceeds safe size bounds");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    require(bool(input), "Cannot read original ICO");
    const auto le16 = [&](std::size_t offset) { return unsigned(bytes[offset]) | unsigned(bytes[offset + 1]) << 8; };
    const auto le32 = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset + 1]) << 8 |
               std::uint32_t(bytes[offset + 2]) << 16 | std::uint32_t(bytes[offset + 3]) << 24;
    };
    const auto be32 = [&](std::size_t offset) {
        return std::uint32_t(bytes[offset]) << 24 | std::uint32_t(bytes[offset + 1]) << 16 |
               std::uint32_t(bytes[offset + 2]) << 8 | std::uint32_t(bytes[offset + 3]);
    };
    const auto count = le16(4);
    require(le16(0) == 0 && le16(2) == 1 && count > 0 && count <= 64 && 6 + count * 16 <= size,
            "Original icon is not a bounded ICO image");
    constexpr std::array<std::uint8_t, 8> pngSignature{137, 80, 78, 71, 13, 10, 26, 10};
    for (unsigned i = 0; i < count; ++i) {
        const auto length = le32(6 + i * 16 + 8), offset = le32(6 + i * 16 + 12);
        require(offset >= 6 + count * 16 && offset <= size && length >= 40 && length <= size - offset,
                "Original icon has an invalid image extent");
        const bool png = std::equal(pngSignature.begin(), pngSignature.end(), bytes.begin() + offset);
        const auto width = png ? be32(offset + 16) : le32(offset + 4);
        const auto height = png ? be32(offset + 20) : le32(offset + 8);
        require(width > 0 && width <= 1024 && height > 0 && height <= (png ? 1024U : 2048U) &&
                (png || (le32(offset) >= 40 && le32(offset) <= length && height % 2 == 0)),
                "Original icon image dimensions exceed safe bounds");
    }
}
void encode_icon(const fs::path& game, const fs::path& output) {
    validate_icon(game/"yami.ico");
    yami::VideoDecoder decoder(game/"yami.ico");
    yami::VideoFrame image;
    require(decoder.next(image) && image.width > 0 && image.height > 0 && image.width <= 1024 && image.height <= 1024,
            "Original game icon cannot be decoded");
    const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    require(encoder != nullptr, "Installer FFmpeg runtime is missing the PNG encoder");
    auto free_codec = [](AVCodecContext* value) { avcodec_free_context(&value); };
    auto free_frame = [](AVFrame* value) { av_frame_free(&value); };
    auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_codec)> codec(avcodec_alloc_context3(encoder), free_codec);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(av_frame_alloc(), free_frame);
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    require(codec && frame && packet, "Cannot allocate PNG encoder");
    codec->width = image.width; codec->height = image.height;
    codec->pix_fmt = AV_PIX_FMT_RGBA; codec->time_base = AVRational{1, 1};
    require(avcodec_open2(codec.get(), encoder, nullptr) >= 0, "Cannot open PNG encoder");
    frame->width = image.width; frame->height = image.height; frame->format = AV_PIX_FMT_RGBA;
    // Decoder owns these pixels until encode completes; avoid a duplicate RGBA allocation.
    frame->data[0] = const_cast<std::uint8_t*>(image.rgba.data()); frame->linesize[0] = image.stride;
    require(avcodec_send_frame(codec.get(), frame.get()) >= 0 && avcodec_receive_packet(codec.get(), packet.get()) >= 0,
            "Cannot encode original icon as PNG");
    write_file(output, {reinterpret_cast<const char*>(packet->data), static_cast<std::size_t>(packet->size)});
}
struct ShortcutPublication {
    fs::path destination, root;
    TemporaryDirectory staging;
    bool backedUp = false, published = false;
    struct stat publishedIdentity{};
    ShortcutPublication(const fs::path& folder, std::string_view name, const fs::path& installation, std::string_view content)
        : destination(folder/name), root(installation), staging(folder) {
        write_file(staging.path/"new", content);
        fs::permissions(staging.path/"new", fs::perms::owner_exec, fs::perm_options::add);
        require(::lstat((staging.path/"new").c_str(), &publishedIdentity) == 0, "Cannot inspect staged shortcut");
    }
    void publish() {
        if (shortcut_exists(destination, root)) {
            struct stat expected{}, moved{};
            require(::lstat(destination.c_str(), &expected) == 0, "Cannot inspect owned shortcut");
            rename_new(destination, staging.path/"backup"); backedUp = true;
            require(::lstat((staging.path/"backup").c_str(), &moved) == 0 &&
                    moved.st_dev == expected.st_dev && moved.st_ino == expected.st_ino &&
                    shortcut_exists(staging.path/"backup", root), "Shortcut ownership changed while publishing");
        }
        rename_new(staging.path/"new", destination); published = true;
    }
    void rollback() {
        if (published) {
            struct stat current{};
            require(::lstat(destination.c_str(), &current) == 0 &&
                    current.st_dev == publishedIdentity.st_dev && current.st_ino == publishedIdentity.st_ino,
                    "Shortcut changed concurrently; refusing to remove it during rollback: " + destination.string());
            rename_new(destination, staging.path/"withdrawn"); published = false;
            require(::lstat((staging.path/"withdrawn").c_str(), &current) == 0 &&
                    current.st_dev == publishedIdentity.st_dev && current.st_ino == publishedIdentity.st_ino,
                    "Substituted shortcut retained for recovery; refusing deletion");
        }
        if (backedUp) { rename_new(staging.path/"backup", destination); backedUp = false; }
    }
};
bool asset_owned_path(const fs::path& relative) {
    for (const auto& part : relative) {
        const auto& name = part.native();
        for (const std::string_view reserved : {"game.ini", "checkpoints", "save", "saves"})
            if (name.size() == reserved.size() && std::equal(name.begin(), name.end(), reserved.begin(),
                    [](char value, char expected) {
                        return (value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) == expected;
                    })) return false;
    }
    return true;
}
bool same_bytes(const fs::path& a, const fs::path& b) {
    owned_file(a); owned_file(b);
    if (fs::file_size(a) != fs::file_size(b)) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    require(bool(left) && bool(right), "Cannot compare original ISO provenance");
    std::array<char, 65536> x{}, y{};
    while (left) {
        left.read(x.data(), x.size()); right.read(y.data(), y.size());
        if (left.gcount() != right.gcount() || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
    }
    require(!left.bad() && !right.bad(), "Cannot compare original ISO provenance");
    return true;
}
void provision_remover(const fs::path& source, const fs::path& destination, const Progress& progress) {
    directory_chain(source.parent_path());
    struct Input {
        int fd = -1;
        struct stat identity{};
        Input(const fs::path& path, std::uint64_t maximum, bool executable) {
            fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            require(fd >= 0, "Trusted removal support is missing or cannot be opened safely: " + path.string() +
                    "; provide yami-remove and adjacent uninstall.sh");
            if (::fstat(fd, &identity) != 0 || !S_ISREG(identity.st_mode) || identity.st_nlink != 1 ||
                (identity.st_uid != geteuid() && identity.st_uid != 0) ||
                (identity.st_mode & (S_IWGRP | S_IWOTH)) || identity.st_size <= 0 ||
                static_cast<std::uint64_t>(identity.st_size) > maximum ||
                (executable && !(identity.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))) {
                ::close(fd); fd = -1;
                throw std::runtime_error("Trusted removal support must be a bounded, private regular file: " + path.string());
            }
        }
        ~Input() { if (fd >= 0) ::close(fd); }
    };
    Input helper(source, 512ULL * 1024 * 1024, true);
    Input script(source.parent_path()/scriptName, 65536, false);
    report(progress, "Copying pinned removal support");
    fs::create_directory(destination/helperDirectory);
    fs::permissions(destination/helperDirectory, fs::perms::owner_all, fs::perm_options::replace);
    const auto copy = [&](const Input& input, const fs::path& target) {
        const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
        require(fd >= 0, "Cannot create private removal support: " + target.string());
        struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
        std::array<char, 65536> buffer{};
        std::uint64_t copied = 0;
        while (copied < static_cast<std::uint64_t>(input.identity.st_size)) {
            const auto count = ::read(input.fd, buffer.data(), std::min<std::uint64_t>(buffer.size(), input.identity.st_size - copied));
            if (count < 0 && errno == EINTR) continue;
            require(count > 0, "Cannot read pinned removal support");
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(count)) {
                const auto bytes = ::write(fd, buffer.data() + written, static_cast<std::size_t>(count) - written);
                if (bytes < 0 && errno == EINTR) continue;
                require(bytes > 0, "Cannot write private removal support");
                written += static_cast<std::size_t>(bytes);
            }
            copied += static_cast<std::uint64_t>(count);
        }
        struct stat current{};
        require(::fstat(input.fd, &current) == 0 && current.st_dev == input.identity.st_dev &&
                current.st_ino == input.identity.st_ino && current.st_size == input.identity.st_size &&
                current.st_uid == input.identity.st_uid && current.st_mode == input.identity.st_mode && current.st_nlink == 1 &&
                current.st_mtim.tv_sec == input.identity.st_mtim.tv_sec && current.st_mtim.tv_nsec == input.identity.st_mtim.tv_nsec,
                "Pinned removal support changed while copying");
        require(::fchmod(fd, 0700) == 0, "Cannot secure private removal support");
    };
    copy(helper, destination/helperDirectory/helperName);
    copy(script, destination/scriptName);
}
struct FilePublication {
    fs::path destination, staged, backup;
    struct stat identity{};
    bool backedUp = false, published = false;
    FilePublication(fs::path target, fs::path fresh, fs::path previous)
        : destination(std::move(target)), staged(std::move(fresh)), backup(std::move(previous)) {
        require(::lstat(staged.c_str(), &identity) == 0, "Cannot inspect staged removal support");
    }
    void publish() {
        if (fs::exists(checked_status(destination))) {
            owned_file(destination);
            struct stat expected{}, moved{};
            require(::lstat(destination.c_str(), &expected) == 0, "Cannot inspect existing removal support");
            rename_new(destination, backup); backedUp = true;
            require(::lstat(backup.c_str(), &moved) == 0 && moved.st_dev == expected.st_dev && moved.st_ino == expected.st_ino,
                    "Removal support changed during publication");
        }
        rename_new(staged, destination); published = true;
    }
    void rollback() {
        if (published) {
            struct stat current{};
            require(::lstat(destination.c_str(), &current) == 0 && current.st_dev == identity.st_dev &&
                    current.st_ino == identity.st_ino, "Removal support changed; refusing rollback deletion");
            rename_new(destination, staged); published = false;
            require(::lstat(staged.c_str(), &current) == 0 && current.st_dev == identity.st_dev &&
                    current.st_ino == identity.st_ino, "Substituted removal support retained for recovery");
        }
        if (backedUp) { rename_new(backup, destination); backedUp = false; }
    }
};
struct RepairLock {
    fs::path root, path;
    struct stat rootIdentity{}, identity{}, ownerIdentity{};
    bool retain = false;
    RepairLock(const fs::path& installation, const fs::path& stage) : root(installation), path(root/".yami-update-lock") {
        require(::lstat(root.c_str(), &rootIdentity) == 0, "Cannot inspect root before repair lock");
        require(fs::create_directory(path), "Another update, removal or repair owns .yami-update-lock; do not break the lock");
        try {
            fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
            require(::lstat(path.c_str(), &identity) == 0, "Cannot inspect repair lock");
            write_file(path/"owner", "pid=" + std::to_string(getpid()) + "\nstage=" + stage.string() + "\n");
            require(::lstat((path/"owner").c_str(), &ownerIdentity) == 0, "Cannot inspect repair lock owner");
        } catch (...) {
            std::error_code error; fs::remove(path/"owner", error); fs::remove(path, error); throw;
        }
    }
    ~RepairLock() {
        if (retain) return;
        struct stat current{}, lock{}, owner{};
        if (::lstat(root.c_str(), &current) != 0 || current.st_dev != rootIdentity.st_dev || current.st_ino != rootIdentity.st_ino ||
            ::lstat(path.c_str(), &lock) != 0 || lock.st_dev != identity.st_dev || lock.st_ino != identity.st_ino) return;
        if (::lstat((path/"owner").c_str(), &owner) == 0 &&
            owner.st_dev == ownerIdentity.st_dev && owner.st_ino == ownerIdentity.st_ino) {
            std::error_code error; fs::remove(path/"owner", error); fs::remove(path, error);
        }
    }
};
} // namespace

InstallPaths default_install_paths() {
    const auto* home = SDL_getenv("HOME");
    const auto* configured = SDL_getenv("XDG_DATA_HOME");
    const auto valid = [](const char* value) { return value && *value && fs::path(value).is_absolute(); };
    require(valid(configured) || valid(home), "Set HOME or an absolute XDG_DATA_HOME to choose a per-user installation");
    const fs::path data = valid(configured) ? fs::path(configured) : fs::path(home)/".local/share";
    const auto* desktop = SDL_GetUserFolder(SDL_FOLDER_DESKTOP);
    require(valid(desktop) || valid(home), "Desktop folder is unavailable; set HOME or choose a Desktop folder");
    return {absolute_path(data/"etiyami"), absolute_path(data/"applications"),
            absolute_path(valid(desktop) ? fs::path(desktop) : fs::path(home)/"Desktop")};
}

InstallResult install(const InstallRequest& request, const Progress& progress) {
    require(geteuid() != 0, "Run the installer as your ordinary user, never with root or sudo");
    require(!request.remover.empty(), "Trusted removal helper is required; provide --remover FILE pointing to yami-remove beside uninstall.sh");
    InstallResult result{{absolute_path(request.paths.root), absolute_path(request.paths.applications),
                          absolute_path(request.paths.desktop)}, false};
    const auto& paths = result.paths;
    require(paths.root != paths.root.root_path() && !within(paths.root, paths.applications) && !within(paths.root, paths.desktop),
            "Shortcut folders must be outside the installation root");
    directory_chain(paths.root.parent_path());
    result.reused = fs::exists(checked_status(paths.root));
    std::unique_ptr<TemporaryDirectory> repairStage;
    std::unique_ptr<RepairLock> repairLock;
    struct stat rootIdentity{};
    if (result.reused) {
        validate_owned_installation(paths.root);
        require(::lstat(paths.root.c_str(), &rootIdentity) == 0, "Cannot inspect owned installation");
        writable_parent(paths.root.parent_path());
        repairStage = std::make_unique<TemporaryDirectory>(paths.root.parent_path());
        repairLock = std::make_unique<RepairLock>(paths.root, repairStage->path);
    }
    const auto checkRoot = [&] {
        directory_chain(paths.root.parent_path());
        struct stat current{};
        require(::lstat(paths.root.c_str(), &current) == 0 && S_ISDIR(current.st_mode) &&
                current.st_dev == rootIdentity.st_dev && current.st_ino == rootIdentity.st_ino,
                "Installation path changed concurrently; refusing its replacement");
    };
    InstalledFiles inventory;
    std::vector<std::string> engineInventory;
    bool legacy = false;
    std::string installedRecord, engineRecord;
    if (result.reused) {
        const bool installed = fs::exists(checked_status(paths.root/installedName));
        const bool engine = fs::exists(checked_status(paths.root/engineName));
        require(installed == engine, "Incomplete ownership inventories; restore both records before repairing");
        legacy = !installed;
        if (!legacy) {
            installedRecord = read_small(paths.root/installedName, maxInstalledBytes);
            inventory = parse_installed(installedRecord);
            engineRecord = read_small(paths.root/engineName, maxEngineBytes);
            engineInventory = parse_engine(engineRecord);
        } else {
            require(!request.iso.empty() && fs::is_regular_file(checked_status(absolute_path(request.iso))),
                    "Legacy installation needs the original YAMI.iso to establish exact asset ownership; unknown files will be preserved");
            for (const auto* name : executables) engineInventory.emplace_back(name);
        }
    }
    writable_parent(paths.root.parent_path());
    for (const auto& folder : {paths.applications, paths.desktop}) {
        const auto ancestor = writable_parent(folder);
        require(fs::space(ancestor).available >= 65536, "Not enough space to publish shortcuts in " + folder.string());
        shortcut_exists(folder/shortcutName, paths.root);
        shortcut_exists(folder/uninstallShortcut, paths.root);
    }
    report(progress, result.reused ? (legacy ? "Verifying legacy ownership from original ISO" : "Repairing uninstaller and shortcuts") : "Preparing installation");
    std::vector<fs::path> files;
    std::uint64_t engineBytes = 0;
    if (!result.reused) {
        files = engine_files(absolute_path(request.engine));
        for (const auto& file : files) {
            const auto size = fs::file_size(file);
            require(size <= 512ULL * 1024 * 1024 && engineBytes <= 2ULL * 1024 * 1024 * 1024 - size,
                    "Trusted engine payload exceeds installation size limits");
            engineBytes += size;
        }
        auto ancestor = paths.root.parent_path();
        while (!fs::exists(checked_status(ancestor))) ancestor = ancestor.parent_path();
        // Extraction stages the compressed CAB plus ~638 MB assets on this volume.
        require(fs::space(ancestor).available >= engineBytes + 1536ULL * 1024 * 1024,
                "Not enough free space: the original assets and extraction need 1.5 GiB plus the native engine");
    }
    CreatedDirectories created;
    directory_chain(paths.root.parent_path(), &created.paths);
    directory_chain(paths.applications, &created.paths);
    directory_chain(paths.desktop, &created.paths);
    if (legacy)
        require(fs::space(paths.root.parent_path()).available >= 1536ULL * 1024 * 1024,
                "Legacy ownership migration needs 1.5 GiB to verify the original ISO without overwriting installed assets");
    std::unique_ptr<TemporaryDirectory> freshStage;
    if (!result.reused) freshStage = std::make_unique<TemporaryDirectory>(paths.root.parent_path());
    auto& stage = result.reused ? *repairStage : *freshStage;
    fs::create_directory(stage.path/"install");
    fs::permissions(stage.path/"install", fs::perms::owner_all, fs::perm_options::replace);
    provision_remover(absolute_path(request.remover), stage.path/"install", progress);
    if (!result.reused) {
        for (const auto& file : files) engineInventory.push_back(file.filename().string());
        std::uint64_t copied = 0;
        for (const auto& file : files) {
            report(progress, "Copying native engine", copied, engineBytes);
            const auto target = stage.path/"install"/file.filename();
            fs::copy_file(file, target);
            fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace);
            copied += fs::file_size(file);
        }
        fs::create_directory(stage.path/"install/game"); fs::create_directory(stage.path/"scratch");
        extract_iso_assets(absolute_path(request.iso), stage.path/"install/game", stage.path/"scratch",
                           absolute_path(request.archiver), progress);
        report(progress, "Converting original game icon");
        encode_icon(stage.path/"install/game", stage.path/"install/eti-yami.png");
        write_file(stage.path/"install"/markerName, marker);
        validate_owned_installation(stage.path/"install");
    }
    if (legacy) {
        fs::create_directory(stage.path/"install/game"); fs::create_directory(stage.path/"scratch");
        extract_iso_assets(absolute_path(request.iso), stage.path/"install/game", stage.path/"scratch",
                           absolute_path(request.archiver), progress);
        checkRoot();
        report(progress, "Verifying original shortcut icon provenance");
        encode_icon(stage.path/"install/game", stage.path/"install/eti-yami.png");
    }
    if (!result.reused || legacy) {
        for (const auto& entry : fs::recursive_directory_iterator(stage.path/"install/game")) {
            if (!entry.is_regular_file()) continue;
            const auto relative = entry.path().lexically_relative(stage.path/"install");
            if (!asset_owned_path(relative)) continue;
            require(valid_relative_file(relative.generic_string()), "Original asset path cannot be recorded safely");
            if (legacy) {
                const auto installed = paths.root/relative;
                directory_chain(installed.parent_path());
                if (!fs::exists(checked_status(installed))) continue;
                if (!same_bytes(entry.path(), installed)) continue; // Modified/user data has no ISO provenance.
            }
            inventory.files.push_back(relative.generic_string());
        }
        for (const auto name : {markerName, installedName, engineName, scriptName})
            inventory.files.emplace_back(name);
        if (!legacy || same_bytes(stage.path/"install/eti-yami.png", paths.root/"eti-yami.png"))
            inventory.files.emplace_back("eti-yami.png");
        inventory.files.emplace_back(std::string(helperDirectory) + "/" + std::string(helperName));
    }
    for (const auto& folder : {paths.applications, paths.desktop})
        for (const auto name : {playShortcut, uninstallShortcut})
            inventory.shortcuts.push_back((folder/name).string());
    const auto newInstalledRecord = serialize_installed(inventory);
    const auto newEngineRecord = serialize_engine(engineInventory);
    write_file(stage.path/"install"/installedName, newInstalledRecord);
    write_file(stage.path/"install"/engineName, newEngineRecord);
    std::vector<std::unique_ptr<FilePublication>> support;
    if (result.reused) {
        checkRoot();
        const auto helperFolder = paths.root/helperDirectory;
        directory_chain(helperFolder);
        if (fs::exists(checked_status(helperFolder))) {
            struct stat info{};
            require(::lstat(helperFolder.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                    info.st_uid == geteuid() && !(info.st_mode & (S_IWGRP | S_IWOTH)), "Removal support directory is not private");
        }
        const std::array<fs::path, 4> names{fs::path(scriptName), fs::path(helperDirectory)/helperName,
                                          fs::path(installedName), fs::path(engineName)};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto& name = names[index];
            if (name == engineName && !legacy) continue; // Preserve the authoritative updater ledger byte-for-byte.
            if (name == installedName && newInstalledRecord == installedRecord) continue;
            if (legacy && (name == scriptName || name == fs::path(helperDirectory)/helperName) &&
                fs::exists(checked_status(paths.root/name)))
                require(same_bytes(paths.root/name, stage.path/"install"/name),
                        "Legacy removal support has unknown provenance; move it aside yourself before migration");
            support.push_back(std::make_unique<FilePublication>(paths.root/name, stage.path/"install"/name,
                              stage.path/("backup-" + std::to_string(index))));
        }
    }
    std::vector<std::unique_ptr<ShortcutPublication>> shortcuts;
    for (const auto& folder : {paths.applications, paths.desktop}) {
        if (folder == paths.desktop && paths.desktop == paths.applications && !shortcuts.empty()) continue;
        shortcuts.push_back(std::make_unique<ShortcutPublication>(folder, playShortcut, paths.root, desktop_entry(paths.root)));
        shortcuts.push_back(std::make_unique<ShortcutPublication>(folder, uninstallShortcut, paths.root, uninstall_entry(paths.root)));
    }
    bool rootPublished = false;
    if (!result.reused)
        require(::lstat((stage.path/"install").c_str(), &rootIdentity) == 0, "Cannot inspect staged installation");
    if (legacy) report(progress, "Legacy ISO provenance verified; unproven libraries, modified assets and user files will be preserved");
    try {
        report(progress, "Publishing installation");
        if (!result.reused) { rename_new(stage.path/"install", paths.root); rootPublished = true; }
        checkRoot();
        if (!result.reused) repairLock = std::make_unique<RepairLock>(paths.root, stage.path);
        if (result.reused) {
            directory_chain(paths.root/helperDirectory, &created.paths);
            for (auto& file : support) { checkRoot(); file->publish(); }
        }
        report(progress, "Creating shortcuts");
        for (auto& shortcut : shortcuts) { checkRoot(); shortcut->publish(); }
        checkRoot();
    } catch (...) {
        const auto original = std::current_exception();
        std::string recovery;
        for (auto it = shortcuts.rbegin(); it != shortcuts.rend(); ++it) {
            try { (*it)->rollback(); }
            catch (const std::exception& error) {
                (*it)->staging.retain = true;
                recovery += "\nShortcut backup retained at " + (*it)->staging.path.string() + ": " + error.what();
            }
        }
        for (auto it = support.rbegin(); it != support.rend(); ++it) {
            if (!(*it)->published && !(*it)->backedUp) continue;
            try { checkRoot(); (*it)->rollback(); }
            catch (const std::exception& error) {
                stage.retain = true;
                recovery += "\nRemoval support backup retained at " + stage.path.string() + ": " + error.what();
            }
        }
        if (rootPublished) {
            try {
                struct stat current{};
                require(::lstat(paths.root.c_str(), &current) == 0 &&
                        current.st_dev == rootIdentity.st_dev && current.st_ino == rootIdentity.st_ino,
                        "Installation path changed concurrently; refusing to remove its replacement");
                rename_new(paths.root, stage.path/"install");
                if (::lstat((stage.path/"install").c_str(), &current) != 0 ||
                    current.st_dev != rootIdentity.st_dev || current.st_ino != rootIdentity.st_ino) {
                    stage.retain = true;
                    throw std::runtime_error("Concurrent replacement retained for recovery at " + stage.path.string());
                }
            }
            catch (const std::exception& error) { recovery += "\nInstallation retained at " + paths.root.string() + ": " + error.what(); }
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
    const auto executable = (root/"yami-launcher").string();
    const char* args[] = {executable.c_str(), nullptr};
    const auto properties = SDL_CreateProperties();
    require(properties != 0, SDL_GetError());
    const bool configured = SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args) &&
        SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, root.c_str()) &&
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
