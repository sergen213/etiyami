#include "setup.hpp"

#include <SDL3/SDL.h>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace yami::setup {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t maxIso = 4ull << 30, maxMsi = 16ull << 20, maxCab = 2ull << 30;
constexpr std::uint64_t maxAsset = 1ull << 30, maxAssets = 4ull << 30;
constexpr std::size_t maxEntries = 32768, maxPath = 1024, maxDepth = 64;
using Archive = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

void require(bool okay, std::string_view message) {
    if (!okay) throw std::runtime_error(std::string(message));
}
void report(const Progress& progress, std::string_view phase, std::uint64_t done, std::uint64_t total) {
    if (progress && !progress(phase, done, total)) throw std::runtime_error("Installation cancelled.");
}
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept { std::swap(value, other.value); return *this; }
};
Fd directory_fd(const fs::path& path) {
    Fd result(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    require(result.value >= 0, "Installation directory is missing or is a symbolic link.");
    require(::fchmod(result.value, 0700) == 0, "Cannot secure the private installation directory.");
    return result;
}
Fd input_file(const fs::path& path, std::uint64_t limit, std::uint64_t& size) {
    Fd result(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat info{};
    require(result.value >= 0 && ::fstat(result.value, &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size > 0 && static_cast<std::uint64_t>(info.st_size) <= limit,
            "Input is not a bounded regular file (or is a symbolic link).");
    size = static_cast<std::uint64_t>(info.st_size);
    return result;
}
struct CaseLess {
    bool operator()(const std::string& a, const std::string& b) const { return SDL_strcasecmp(a.c_str(), b.c_str()) < 0; }
};
void validate_component(std::string_view part) {
    require(!part.empty() && part.size() <= 255 && part != "." && part != ".." &&
            part.back() != '.' && part.back() != ' ', "Unsafe asset path component.");
    require(part.find_first_of("/\\:*?\"<>|") == std::string_view::npos, "Unsafe asset path component.");
    const char* text = part.data();
    auto left = part.size();
    while (left) {
        const auto code = SDL_StepUTF8(&text, &left);
        require(code != SDL_INVALID_UNICODE_CODEPOINT && code >= 32 && !(code >= 127 && code < 160),
                "Invalid UTF-8 or control character in an asset path.");
    }
}
void validate_path(std::string_view path) {
    require(!path.empty() && path.size() <= maxPath, "Unsafe asset path.");
    std::size_t begin = 0, depth = 0;
    for (;;) {
        const auto end = path.find('/', begin);
        validate_component(path.substr(begin, end == std::string_view::npos ? end : end - begin));
        require(++depth <= maxDepth, "Asset directory nesting is too deep.");
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
}
// Includes implicit parents: data/a and DATA/b, or data/a and data/a/b, must not coexist.
void record_path(std::map<std::string, bool, CaseLess>& components, const std::string& path, bool directory) {
    std::size_t end = 0;
    do {
        end = path.find('/', end);
        const auto prefix = path.substr(0, end);
        const bool isDirectory = end != std::string::npos || directory;
        const auto [found, inserted] = components.emplace(prefix, isDirectory);
        require(inserted || (found->first == prefix && found->second == isDirectory), "Colliding asset paths or directory case.");
        if (end != std::string::npos) ++end;
    } while (end != std::string::npos);
}
Fd output_file(int root, const std::string& path) {
    validate_path(path);
    Fd parent(::dup(root));
    require(parent.value >= 0, "Cannot access installation directory.");
    std::size_t begin = 0;
    for (;;) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (end == std::string::npos) {
            Fd file(::openat(parent.value, part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            require(file.value >= 0, "Asset destination already exists or cannot be created safely.");
            return file;
        }
        require(::mkdirat(parent.value, part.c_str(), 0700) == 0 || errno == EEXIST, "Cannot create an asset directory.");
        Fd next(::openat(parent.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        require(next.value >= 0, "Asset directory is not a real directory (symbolic links are forbidden).");
        parent = std::move(next);
        begin = end + 1;
    }
}
void write_bytes(int fd, const char* bytes, std::size_t size) {
    while (size) {
        const auto written = ::write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        require(written > 0, "Cannot save an original asset (check available disk space).");
        bytes += written; size -= static_cast<std::size_t>(written);
    }
}
template<class Tick> void copy_member(struct archive* input, int output, std::uint64_t declared, Tick&& tick) {
    std::array<char, 65536> bytes{};
    std::uint64_t written = 0;
    for (;;) {
        tick(0);
        const auto count = archive_read_data(input, bytes.data(), bytes.size());
        require(count >= 0, "Corrupt or truncated ISO/CAB member.");
        if (!count) break;
        require(static_cast<std::uint64_t>(count) <= declared - written, "ISO/CAB member exceeds its declared size.");
        if (output >= 0) write_bytes(output, bytes.data(), static_cast<std::size_t>(count));
        written += static_cast<std::uint64_t>(count);
        tick(static_cast<std::uint64_t>(count));
    }
    require(written == declared, "Corrupt or truncated ISO/CAB member size.");
}
std::uint32_t little(std::string_view bytes, std::size_t offset, unsigned width) {
    require(offset <= bytes.size() && width <= bytes.size() - offset, "Truncated MSI/ISO metadata.");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}
void validate_iso_volume(int fd, std::uint64_t size) {
    bool primary = false, terminated = false;
    std::array<char, 2048> bytes{};
    for (unsigned sector = 16; sector < 16 + 64; ++sector) {
        ssize_t count;
        do { count = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(sector) * 2048); } while (count < 0 && errno == EINTR);
        require(count == static_cast<ssize_t>(bytes.size()), "Unsupported or truncated ISO9660 image.");
        const std::string_view descriptor(bytes.data(), bytes.size());
        require(descriptor.substr(1, 5) == "CD001" && bytes[6] == 1, "Unsupported ISO9660 image.");
        const auto type = static_cast<unsigned char>(bytes[0]);
        if (type == 1) {
            require(!primary, "Duplicate ISO9660 primary volume descriptor.");
            const auto blocks = little(descriptor, 80, 4), blockSize = little(descriptor, 128, 2);
            std::uint32_t bigBlocks = 0;
            for (unsigned i = 84; i < 88; ++i) bigBlocks = (bigBlocks << 8) | static_cast<unsigned char>(bytes[i]);
            const auto bigBlockSize = (std::uint32_t(static_cast<unsigned char>(bytes[130])) << 8) |
                                      static_cast<unsigned char>(bytes[131]);
            require(blocks > 0 && blocks == bigBlocks && blockSize == 2048 && blockSize == bigBlockSize &&
                    static_cast<std::uint64_t>(blocks) * blockSize <= size, "Corrupt or truncated ISO9660 volume length.");
            primary = true;
        } else if (type == 255) { terminated = true; break; }
        else require(type <= 3, "Unsupported ISO9660 descriptor.");
    }
    require(primary && terminated, "Missing ISO9660 primary volume or terminator.");
}
std::uint64_t entry_size(archive_entry* entry, std::uint64_t limit) {
    require(archive_entry_size_is_set(entry) && archive_entry_size(entry) >= 0 &&
            static_cast<std::uint64_t>(archive_entry_size(entry)) <= limit &&
            !archive_entry_symlink(entry) && !archive_entry_hardlink(entry) &&
            !archive_entry_is_encrypted(entry) && !archive_entry_sparse_count(entry), "Unsafe or oversized ISO/CAB entry.");
    return static_cast<std::uint64_t>(archive_entry_size(entry));
}
std::string entry_name(archive_entry* entry) {
    const char* name = archive_entry_pathname_utf8(entry);
    if (!name) name = archive_entry_pathname(entry);
    require(name != nullptr && SDL_strnlen(name, maxPath + 1) <= maxPath, "Missing or oversized ISO/CAB filename.");
    return name;
}
void read_iso(const fs::path& path, int scratch, const Progress& progress) {
    report(progress, "Reading ISO", 0, 1);
    std::uint64_t size = 0;
    auto file = input_file(path, maxIso, size);
    validate_iso_volume(file.value, size);
    Archive input(archive_read_new(), archive_read_free);
    require(input && archive_read_support_format_iso9660(input.get()) == ARCHIVE_OK &&
            archive_read_open_fd(input.get(), file.value, 65536) == ARCHIVE_OK, "Cannot read this ISO9660 image.");
    std::set<std::string, CaseLess> entries;
    std::map<std::string, bool, CaseLess> components;
    std::uint64_t declaredTotal = 0, read = 0;
    bool msi = false, cab = false;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
        report(progress, "Reading ISO", read, size);
        const auto type = archive_entry_filetype(entry);
        require(type == AE_IFREG || type == AE_IFDIR, "ISO links and special files are forbidden.");
        const bool directory = type == AE_IFDIR;
        const auto declared = entry_size(entry, directory ? maxMsi : maxIso);
        auto name = entry_name(entry);
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        require(entries.size() < maxEntries && entries.insert(name).second, "Duplicate ISO filename.");
        if (directory && name == ".") continue;
        validate_path(name); record_path(components, name, directory);
        if (directory) continue;
        require(declared <= size - declaredTotal, "ISO members exceed the image size.");
        declaredTotal += declared;
        const auto basename = name.substr(name.find_last_of('/') == std::string::npos ? 0 : name.find_last_of('/') + 1);
        const char* selected = nullptr;
        if (SDL_strcasecmp(basename.c_str(), "Yami.msi") == 0) {
            require(!msi && declared > 0 && declared <= maxMsi, "Duplicate, empty or oversized Yami.msi.");
            msi = true; selected = "Yami.msi";
        } else if (SDL_strcasecmp(basename.c_str(), "Data1.cab") == 0) {
            require(!cab && declared > 0 && declared <= maxCab, "Duplicate, empty or oversized Data1.cab.");
            cab = true; selected = "Data1.cab";
        }
        Fd output(selected ? ::openat(scratch, selected, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1);
        require(!selected || output.value >= 0, "ISO scratch destination already exists or cannot be created safely.");
        copy_member(input.get(), output.value, declared, [&](std::uint64_t count) {
            read += count; report(progress, "Reading ISO", read, size);
        });
    }
    require(status == ARCHIVE_EOF && archive_read_close(input.get()) == ARCHIVE_OK && msi && cab,
            "Not a complete original Yami ISO (Yami.msi and Data1.cab are required).");
    report(progress, "Reading ISO", size, size);
}

struct StopProcess {
    void operator()(SDL_Process* process) const {
        if (!process) return;
        int code = 0;
        if (!SDL_WaitProcess(process, false, &code)) {
            SDL_KillProcess(process, true);
            SDL_WaitProcess(process, true, &code);
        }
        SDL_DestroyProcess(process);
    }
};
std::string msi_stream(const fs::path& archiver, const fs::path& msi, const char* name,
                       unsigned index, const Progress& progress) {
    report(progress, "Reading MSI", index, 5);
    const auto tool = archiver.string(), source = msi.string(), selected = "!" + std::string(name);
    const char* args[] = {tool.c_str(), "x", "-so", "-bd", "-bsp0", "-y", "--", source.c_str(), selected.c_str(), nullptr};
    const auto props = SDL_CreateProperties();
    require(props != 0, "Cannot configure the bundled MSI reader.");
    const bool configured = SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    std::unique_ptr<SDL_Process, StopProcess> process(configured ? SDL_CreateProcessWithProperties(props) : nullptr);
    SDL_DestroyProperties(props);
    require(process != nullptr, "Cannot start the trusted bundled MSI reader.");
    auto* output = SDL_GetProcessOutput(process.get());
    require(output != nullptr, "Cannot read the bundled MSI reader output.");
    std::string result;
    std::array<char, 65536> bytes{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    int exitCode = -1;
    bool exited = false;
    for (;;) {
        report(progress, "Reading MSI", index, 5);
        require(std::chrono::steady_clock::now() < deadline, "MSI reader timed out on an unsupported or corrupt installer.");
        const auto count = SDL_ReadIO(output, bytes.data(), bytes.size());
        if (count) {
            require(count <= maxMsi - result.size(), "MSI table stream is too large.");
            result.append(bytes.data(), count); continue;
        }
        const auto ioStatus = SDL_GetIOStatus(output);
        require(ioStatus == SDL_IO_STATUS_NOT_READY || ioStatus == SDL_IO_STATUS_EOF, "MSI reader output failed.");
        if (!exited) exited = SDL_WaitProcess(process.get(), false, &exitCode);
        if (exited && ioStatus == SDL_IO_STATUS_EOF) break;
        SDL_Delay(5);
    }
    require(exitCode == 0 && !result.empty(), "Required MSI table is missing or corrupt.");
    report(progress, "Reading MSI", index + 1, 5);
    return result;
}
std::vector<std::string> read_strings(std::string_view pool, std::string_view data, const Progress& progress) {
    require(pool.size() >= 4 && (pool.size() - 4) % 4 == 0 && little(pool, 0, 4) == 1254 &&
            (pool.size() - 4) / 4 <= 65535, "Unsupported MSI string pool; expected the Turkish Yami release.");
    struct Encoding {
        SDL_iconv_t value = SDL_iconv_open("UTF-8", "WINDOWS-1254");
        ~Encoding() { if (value != reinterpret_cast<SDL_iconv_t>(SDL_ICONV_ERROR)) SDL_iconv_close(value); }
    } encoding;
    require(encoding.value != reinterpret_cast<SDL_iconv_t>(SDL_ICONV_ERROR), "Windows-1254 character conversion is unavailable.");
    std::vector<std::string> strings;
    strings.reserve(1 + (pool.size() - 4) / 4); strings.emplace_back();
    std::size_t offset = 0;
    for (std::size_t record = 4; record < pool.size(); record += 4) {
        report(progress, "Checking MSI", record - 4, pool.size() - 4);
        const auto length = little(pool, record, 2), refs = little(pool, record + 2, 2);
        require((length || !refs) && length <= data.size() - offset, "Unsupported extended or truncated MSI string record.");
        std::string decoded(length * 3, '\0');
        const char* source = data.data() + offset;
        auto left = static_cast<std::size_t>(length), available = decoded.size();
        char* target = decoded.data();
        if (length) {
            const auto converted = SDL_iconv(encoding.value, &source, &left, &target, &available);
            require(converted != SDL_ICONV_ERROR && converted != SDL_ICONV_E2BIG && converted != SDL_ICONV_EILSEQ &&
                    converted != SDL_ICONV_EINVAL && left == 0, "Invalid Windows-1254 MSI string.");
        }
        decoded.resize(decoded.size() - available);
        require(decoded.find('\0') == std::string::npos, "NUL character in an MSI string.");
        strings.push_back(std::move(decoded)); offset += length;
    }
    require(offset == data.size(), "Corrupt MSI string-data length.");
    return strings;
}
template<std::size_t N> struct Table {
    std::string_view bytes;
    std::array<unsigned, N> widths;
    std::array<std::size_t, N> starts{};
    std::size_t count = 0;
    Table(std::string_view raw, std::array<unsigned, N> columns) : bytes(raw), widths(columns) {
        std::size_t rowSize = 0;
        for (auto width : widths) rowSize += width;
        require(!raw.empty() && raw.size() % rowSize == 0, "Corrupt fixed-schema MSI table.");
        count = raw.size() / rowSize;
        require(count <= maxEntries, "Too many rows in an MSI table.");
        for (std::size_t i = 1; i < N; ++i) starts[i] = starts[i - 1] + count * widths[i - 1];
    }
    std::uint32_t at(std::size_t row, std::size_t column) const { return little(bytes, starts[column] + row * widths[column], widths[column]); }
};
std::string target_name(std::string_view raw, bool directory) {
    if (directory) {
        const auto colon = raw.find(':');
        require(colon == std::string_view::npos || raw.find(':', colon + 1) == std::string_view::npos, "Invalid MSI directory target/source name.");
        if (colon != std::string_view::npos) {
            auto source = raw.substr(colon + 1);
            const auto sourceBar = source.find('|');
            if (sourceBar != std::string_view::npos) {
                validate_component(source.substr(0, sourceBar)); source.remove_prefix(sourceBar + 1);
            }
            if (source != ".") validate_component(source);
        }
        raw = raw.substr(0, colon);
    }
    const auto bar = raw.find('|');
    require(bar == std::string_view::npos || (bar > 0 && raw.find('|', bar + 1) == std::string_view::npos), "Invalid MSI short/long name.");
    if (bar != std::string_view::npos) { validate_component(raw.substr(0, bar)); raw.remove_prefix(bar + 1); }
    if (!(directory && raw == ".")) validate_component(raw);
    return std::string(raw);
}
struct Asset { std::string path; std::uint64_t size = 0; bool extracted = false; };
using Assets = std::map<std::string, Asset>;
struct Directory { std::string parent, name; unsigned state = 0; std::optional<std::string> path; };
const std::optional<std::string>& resolve_directory(std::map<std::string, Directory>& directories, const std::string& key,
                                                   std::size_t depth, const Progress& progress) {
    report(progress, "Checking directories", 0, directories.size());
    require(depth <= maxDepth, "MSI directory nesting is too deep.");
    const auto found = directories.find(key);
    require(found != directories.end(), "MSI directory reference is missing.");
    auto& directory = found->second;
    require(directory.state != 1, "Cyclic MSI directory mapping.");
    if (directory.state == 2) return directory.path;
    directory.state = 1;
    const std::optional<std::string>* base = nullptr;
    if (!directory.parent.empty()) base = &resolve_directory(directories, directory.parent, depth + 1, progress);
    if (key == "INSTALLDIR") directory.path = std::string();
    else if (base && *base) {
        directory.path = **base;
        if (directory.name != ".") {
            if (!directory.path->empty()) *directory.path += '/';
            *directory.path += directory.name; validate_path(*directory.path);
        }
    }
    directory.state = 2;
    return directory.path;
}
bool ascii_equal(std::string_view value, std::string_view expected) {
    if (value.size() != expected.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto c = static_cast<unsigned char>(value[i]);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != static_cast<unsigned char>(expected[i])) return false;
    }
    return true;
}
bool native_path(std::string_view path) {
    return (path.size() > 5 && ascii_equal(path.substr(0, 5), "data/")) || ascii_equal(path, "yami.ico") || ascii_equal(path, "eula.rtf");
}
Assets parse_msi(const std::array<std::string, 5>& streams, const Progress& progress) {
    for (const auto& stream : streams) require(stream.size() <= maxMsi, "Oversized MSI stream.");
    const auto strings = read_strings(streams[0], streams[1], progress);
    const auto text = [&](std::uint32_t index) -> const std::string& {
        require(index < strings.size(), "Invalid MSI string-table index."); return strings[index];
    };
    // ponytail: the original release's fixed table schemas; arbitrary MSI support needs a full MSI library.
    const Table<3> directoryTable(streams[2], {2, 2, 2});
    const Table<6> componentTable(streams[3], {2, 2, 2, 2, 2, 2});
    const Table<8> fileTable(streams[4], {2, 2, 2, 4, 2, 2, 2, 2});
    std::map<std::string, Directory> directories;
    std::set<std::string, CaseLess> directoryKeys, componentKeys, fileKeys, destinations;
    for (std::size_t row = 0; row < directoryTable.count; ++row) {
        report(progress, "Checking directories", row, directoryTable.count);
        const auto& key = text(directoryTable.at(row, 0)); validate_component(key);
        const auto& parent = text(directoryTable.at(row, 1));
        if (!parent.empty()) validate_component(parent);
        require(directoryKeys.insert(key).second, "Duplicate MSI directory key.");
        directories.emplace(key, Directory{parent, target_name(text(directoryTable.at(row, 2)), true), 0, {}});
    }
    require(directories.contains("INSTALLDIR"), "MSI does not contain the original game directory.");
    for (const auto& [key, directory] : directories) resolve_directory(directories, key, 0, progress);
    std::map<std::string, std::string> components;
    for (std::size_t row = 0; row < componentTable.count; ++row) {
        report(progress, "Checking components", row, componentTable.count);
        for (const auto column : {0u, 1u, 2u, 4u, 5u}) text(componentTable.at(row, column));
        const auto& key = text(componentTable.at(row, 0)); validate_component(key);
        const auto& directory = text(componentTable.at(row, 2));
        require(componentKeys.insert(key).second && directories.contains(directory), "Duplicate MSI component or missing directory.");
        components.emplace(key, directory);
    }
    Assets assets;
    std::map<std::string, bool, CaseLess> pathComponents;
    std::set<std::uint32_t> sequences;
    std::uint64_t total = 0;
    bool marker = false;
    for (std::size_t row = 0; row < fileTable.count; ++row) {
        report(progress, "Checking files", row, fileTable.count);
        for (const auto column : {0u, 1u, 2u, 4u, 5u}) text(fileTable.at(row, column));
        const auto& key = text(fileTable.at(row, 0)); validate_component(key);
        const auto component = components.find(text(fileTable.at(row, 1)));
        require(fileKeys.insert(key).second && component != components.end(), "Duplicate MSI file ID or missing component.");
        const auto sequence = fileTable.at(row, 7);
        require(sequence > 0x8000 && sequences.insert(sequence).second, "Invalid or duplicate MSI file sequence.");
        const auto biasedSize = fileTable.at(row, 3);
        require(biasedSize >= 0x80000000u && biasedSize - 0x80000000u <= maxAsset, "Invalid or oversized MSI file size.");
        const auto size = static_cast<std::uint64_t>(biasedSize - 0x80000000u);
        const auto name = target_name(text(fileTable.at(row, 2)), false);
        const auto& base = directories.at(component->second).path;
        if (!base) continue; // No system DLLs, codecs, installer files or other external destinations.
        const auto path = base->empty() ? name : *base + '/' + name;
        validate_path(path);
        require(destinations.insert(path).second, "Duplicate MSI destination path.");
        record_path(pathComponents, path, false);
        if (!native_path(path)) continue; // Only data/**, yami.ico and eula.rtf; never executables or user saves.
        const auto dot = path.find_last_of('.');
        if (dot != std::string::npos) for (const auto suffix : {".exe", ".dll", ".ax", ".sys", ".msi", ".cab"})
            require(!ascii_equal(std::string_view(path).substr(dot), suffix), "Windows code is not a native game asset.");
        require(size <= maxAssets - total, "Native asset payload is too large."); total += size;
        if (ascii_equal(path, "data/menu/menulist.xml")) { require(size > 0, "Original menu marker is empty."); marker = true; }
        assets.emplace(key, Asset{path, size, false});
    }
    require(marker && !assets.empty(), "MSI is missing the original data/menu/menulist.xml marker.");
    report(progress, "Checking files", fileTable.count, fileTable.count);
    return assets;
}
void read_cab(const fs::path& path, int game, Assets& assets, const Progress& progress) {
    std::uint64_t total = 0, size = 0;
    for (const auto& [key, asset] : assets) { require(asset.size <= maxAssets - total, "Native asset payload is too large."); total += asset.size; }
    report(progress, "Extracting assets", 0, total);
    auto file = input_file(path, maxCab, size);
    Archive input(archive_read_new(), archive_read_free);
    require(input && archive_read_support_format_cab(input.get()) == ARCHIVE_OK &&
            archive_read_open_fd(input.get(), file.value, 65536) == ARCHIVE_OK, "Cannot read the original Data1.cab.");
    std::set<std::string, CaseLess> entries;
    std::uint64_t declaredTotal = 0, done = 0;
    archive_entry* entry = nullptr;
    int status;
    while ((status = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
        report(progress, "Extracting assets", done, total);
        require(archive_entry_filetype(entry) == AE_IFREG, "CAB links, directories and special files are forbidden.");
        const auto declared = entry_size(entry, maxAsset);
        const auto key = entry_name(entry); validate_component(key);
        require(entries.size() < maxEntries && entries.insert(key).second, "Duplicate CAB file ID.");
        require(declared <= maxAssets - declaredTotal, "CAB decompressed payload is too large."); declaredTotal += declared;
        const auto found = assets.find(key);
        if (found != assets.end()) require(!found->second.extracted && found->second.size == declared, "CAB/MSI file size or duplicate mapping mismatch.");
        Fd output = found == assets.end() ? Fd(-1) : output_file(game, found->second.path);
        copy_member(input.get(), output.value, declared, [&](std::uint64_t count) {
            if (found != assets.end()) done += count;
            report(progress, "Extracting assets", done, total);
        });
        if (found != assets.end()) found->second.extracted = true;
    }
    require(status == ARCHIVE_EOF && archive_read_close(input.get()) == ARCHIVE_OK && done == total, "Corrupt or incomplete original CAB payload.");
    for (const auto& [key, asset] : assets) require(asset.extracted, "An expected native asset is missing from Data1.cab.");
    report(progress, "Extracting assets", total, total);
}

void extract_files(const fs::path& iso, const fs::path& game, const fs::path& scratch,
                   const fs::path& archiver, std::span<const std::string_view> paths,
                   const Progress& progress) {
    report(progress, "Reading ISO", 0, 1);
    require(fs::is_directory(fs::symlink_status(game)) && fs::is_directory(fs::symlink_status(scratch)) &&
            fs::is_empty(game) && fs::is_empty(scratch) && fs::canonical(game) != fs::canonical(scratch),
            "Extraction requires distinct fresh game and scratch directories.");
    auto gameFd = directory_fd(game), scratchFd = directory_fd(scratch);
    read_iso(iso, scratchFd.value, progress);
    const auto tool = fs::absolute(archiver), msi = fs::absolute(scratch / "Yami.msi");
    require(fs::is_regular_file(tool) && ::access(tool.c_str(), X_OK) == 0, "The trusted bundled 7zz MSI reader is missing or not executable.");
    constexpr std::array<const char*, 5> names{"_StringPool", "_StringData", "Directory", "Component", "File"};
    std::array<std::string, 5> streams;
    std::uint64_t streamBytes = 0;
    for (unsigned i = 0; i < names.size(); ++i) {
        streams[i] = msi_stream(tool, msi, names[i], i, progress);
        require(streams[i].size() <= 2 * maxMsi - streamBytes, "MSI metadata exceeds the bounded extraction limit.");
        streamBytes += streams[i].size();
    }
    auto assets = parse_msi(streams, progress);
    if (!paths.empty()) {
        require(paths.size() <= 64, "Too many installer artwork files.");
        std::set<std::string, CaseLess> wanted;
        for (const auto path : paths) {
            validate_path(path);
            require(native_path(path) && wanted.insert(std::string(path)).second,
                    "Invalid or duplicate installer artwork path.");
        }
        std::uint64_t bytes = 0;
        for (auto it = assets.begin(); it != assets.end();) {
            const auto found = wanted.find(it->second.path);
            if (found == wanted.end()) { it = assets.erase(it); continue; }
            require(it->second.size <= (32ULL << 20) && it->second.size <= (128ULL << 20) - bytes,
                    "Installer artwork exceeds safe preview bounds.");
            bytes += it->second.size;
            wanted.erase(found); ++it;
        }
        require(wanted.empty(), "This ISO is missing original installer artwork.");
    }
    read_cab(scratch / "Data1.cab", gameFd.value, assets, progress);
}
} // namespace

void extract_iso_assets(const fs::path& iso, const fs::path& game, const fs::path& scratch,
                        const fs::path& archiver, const Progress& progress) {
    extract_files(iso, game, scratch, archiver, {}, progress);
}

void extract_iso_artwork(const fs::path& iso, const fs::path& game, const fs::path& scratch,
                         const fs::path& archiver, std::span<const std::string_view> paths,
                         const Progress& progress) {
    require(!paths.empty(), "Installer artwork selection is empty.");
    extract_files(iso, game, scratch, archiver, paths, progress);
}
} // namespace yami::setup
