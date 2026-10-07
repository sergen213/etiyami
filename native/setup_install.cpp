#include "setup.hpp"
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
constexpr std::string_view markerName = ".eti-yami-install";
constexpr std::string_view marker = "ETI Yami native Linux installation\nformat=1\n";
constexpr std::string_view shortcutName = "eti-yami.desktop";
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
std::string read_small(const fs::path& path) {
    owned_file(path);
    require(fs::file_size(path) <= 65536, "Ownership record is too large: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot read ownership record: " + path.string());
    std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    require(!stream.bad(), "Cannot read ownership record: " + path.string());
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
    explicit TemporaryDirectory(const fs::path& parent) {
        auto pattern = (parent/".yami-setup-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "Cannot stage installation in " + parent.string() + ": " + std::strerror(errno));
        path = pattern;
    }
    ~TemporaryDirectory() { if (!retain) { std::error_code error; fs::remove_all(path, error); } }
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
    require(files.size() <= 8192, "Engine payload has too many members");
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
    ShortcutPublication(const fs::path& folder, const fs::path& installation, std::string_view content)
        : destination(folder/shortcutName), root(installation), staging(folder) {
        write_file(staging.path/"new", content);
        fs::permissions(staging.path/"new", fs::perms::owner_exec, fs::perm_options::add);
        require(::lstat((staging.path/"new").c_str(), &publishedIdentity) == 0, "Cannot inspect staged shortcut");
    }
    void publish() {
        if (shortcut_exists(destination, root)) {
            rename_new(destination, staging.path/"backup"); backedUp = true;
            require(shortcut_exists(staging.path/"backup", root), "Shortcut ownership changed while publishing");
        }
        rename_new(staging.path/"new", destination); published = true;
    }
    void rollback() {
        if (published) {
            struct stat current{};
            require(::lstat(destination.c_str(), &current) == 0 &&
                    current.st_dev == publishedIdentity.st_dev && current.st_ino == publishedIdentity.st_ino,
                    "Shortcut changed concurrently; refusing to remove it during rollback: " + destination.string());
            fs::remove(destination); published = false;
        }
        if (backedUp) { rename_new(staging.path/"backup", destination); backedUp = false; }
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
    InstallResult result{{absolute_path(request.paths.root), absolute_path(request.paths.applications),
                          absolute_path(request.paths.desktop)}, false};
    const auto& paths = result.paths;
    require(paths.root != paths.root.root_path() && !within(paths.root, paths.applications) && !within(paths.root, paths.desktop),
            "Shortcut folders must be outside the installation root");
    directory_chain(paths.root.parent_path());
    result.reused = fs::exists(checked_status(paths.root));
    if (result.reused) validate_owned_installation(paths.root);
    writable_parent(paths.root.parent_path());
    for (const auto& folder : {paths.applications, paths.desktop}) {
        const auto ancestor = writable_parent(folder);
        require(fs::space(ancestor).available >= 65536, "Not enough space to publish shortcuts in " + folder.string());
        shortcut_exists(folder/shortcutName, paths.root);
    }
    report(progress, result.reused ? "Repairing owned shortcuts" : "Preparing installation");
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
    TemporaryDirectory stage(paths.root.parent_path());
    if (!result.reused) {
        fs::create_directory(stage.path/"install");
        fs::permissions(stage.path/"install", fs::perms::owner_all, fs::perm_options::replace);
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
    const auto content = desktop_entry(paths.root);
    std::vector<std::unique_ptr<ShortcutPublication>> shortcuts;
    shortcuts.push_back(std::make_unique<ShortcutPublication>(paths.applications, paths.root, content));
    if (paths.desktop != paths.applications)
        shortcuts.push_back(std::make_unique<ShortcutPublication>(paths.desktop, paths.root, content));
    bool rootPublished = false;
    struct stat rootIdentity{};
    if (!result.reused)
        require(::lstat((stage.path/"install").c_str(), &rootIdentity) == 0, "Cannot inspect staged installation");
    try {
        report(progress, "Publishing installation");
        if (!result.reused) { rename_new(stage.path/"install", paths.root); rootPublished = true; }
        report(progress, "Creating shortcuts");
        for (auto& shortcut : shortcuts) shortcut->publish();
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
        if (!recovery.empty()) throw std::runtime_error("Installation failed and automatic rollback needs manual recovery:" + recovery);
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
