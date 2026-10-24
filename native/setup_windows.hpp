#pragma once
#ifndef _WIN32
#error "This header is for the Windows installer"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <accctrl.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace yami::windows_install {
namespace fs = std::filesystem;

[[noreturn]] inline void fail(std::string_view action, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), std::string(action));
}
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
inline std::string text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
inline fs::path utf8_path(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
inline fs::path absolute_path(const fs::path& path) {
    require(!path.empty(), "Installation path is empty");
    auto result = fs::absolute(path).lexically_normal();
    while (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    const auto value = text(result);
    require(value.find_first_of("\r\n") == std::string::npos && value.find('\0') == std::string::npos,
            "Installation paths cannot contain line breaks or NUL characters");
    return result;
}
inline bool path_equal(const fs::path& a, const fs::path& b) {
    const auto equal = [](const fs::path& left, const fs::path& right) {
        return left.native().size() == right.native().size() &&
               CompareStringOrdinal(left.c_str(), static_cast<int>(left.native().size()),
                                    right.c_str(), static_cast<int>(right.native().size()), TRUE) == CSTR_EQUAL;
    };
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    if (equal(a, b)) return true;
    const auto normalized = [](const fs::path& path) {
        const auto& native = path.native();
        const int count = FoldStringW(MAP_PRECOMPOSED, native.data(), static_cast<int>(native.size()), nullptr, 0);
        require(count > 0, "Cannot normalize an installation path");
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        require(FoldStringW(MAP_PRECOMPOSED, native.data(), static_cast<int>(native.size()),
                            result.data(), count) == count, "Cannot normalize an installation path");
        return result;
    };
    const auto left = normalized(a), right = normalized(b);
    return left.size() == right.size() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
inline bool within(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin(), c = child.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == child.end() || !path_equal(*p, *c)) return false;
    }
    return true;
}
inline fs::file_status checked_status(const fs::path& path) {
    std::error_code error;
    auto result = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return fs::file_status(fs::file_type::not_found);
    if (error) throw fs::filesystem_error("Cannot inspect installation path", path, error);
    if (fs::exists(result)) {
        const auto attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Refusing a reparse-point installation path: " + text(path));
    }
    require(!fs::is_symlink(result), "Refusing a symbolic-link installation path: " + text(path));
    return result;
}
inline void directory_chain(const fs::path& path) {
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        require(fs::is_directory(checked_status(current)), "Installation parent is not a safe directory: " + text(current));
    }
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    static bool valid(HANDLE handle) { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (valid(value)) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (valid(value)) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    explicit operator bool() const { return valid(value); }
};
struct Identity {
    DWORD volume = 0, links = 0, attributes = 0;
    ULONGLONG index = 0, size = 0, lastWrite = 0;
    DWORD type = FILE_TYPE_UNKNOWN;
};
inline Identity identify(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(handle, &info) != 0, "Cannot inspect an open installation member");
    Identity result;
    result.volume = info.dwVolumeSerialNumber;
    result.links = info.nNumberOfLinks;
    result.attributes = info.dwFileAttributes;
    result.index = (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.lastWrite = (static_cast<ULONGLONG>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    result.type = GetFileType(handle);
    require(result.type != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR, "Cannot identify an installation member");
    return result;
}
inline bool same(const Identity& a, const Identity& b) {
    return a.volume == b.volume && a.index == b.index && a.type == b.type;
}
inline Handle open_directory(const fs::path& path, DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL,
                             DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE) {
    Handle result(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!result) fail("Cannot open installation directory", GetLastError());
    const auto id = identify(result.value);
    require(id.type == FILE_TYPE_DISK && (id.attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(id.attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Installation directory is unsafe: " + text(path));
    return result;
}
inline std::wstring current_user_sid() {
    auto token = GetCurrentProcessToken();
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    require(needed != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Cannot inspect the current user");
    std::vector<std::uint8_t> bytes(needed);
    require(GetTokenInformation(token, TokenUser, bytes.data(), needed, &needed) != 0,
            "Cannot inspect the current user");
    wchar_t* sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid) != 0,
            "Cannot encode the current user SID");
    std::unique_ptr<wchar_t, decltype(&LocalFree)> guard(sid, LocalFree);
    return sid;
}

inline bool current_owner(HANDLE handle) {
    auto token = GetCurrentProcessToken();
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    require(needed != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Cannot inspect the current user");
    std::vector<std::uint8_t> bytes(needed);
    require(GetTokenInformation(token, TokenUser, bytes.data(), needed, &needed) != 0,
            "Cannot inspect the current user");
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                        &owner, nullptr, nullptr, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) fail("Cannot inspect installation ownership", result);
    struct Free { PSECURITY_DESCRIPTOR value; ~Free() { if (value) LocalFree(value); } } free{descriptor};
    return owner && EqualSid(owner, reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid) != 0;
}
inline Handle open_regular(const fs::path& path, DWORD access = GENERIC_READ,
                           DWORD share = FILE_SHARE_READ, bool owned = false) {
    Handle result(CreateFileW(path.c_str(), access | (owned ? READ_CONTROL : 0), share, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!result) fail("Cannot open installation file", GetLastError());
    const auto id = identify(result.value);
    require(id.type == FILE_TYPE_DISK && !(id.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            id.links == 1, "Installation member is not a single-link regular file: " + text(path));
    require(!owned || current_owner(result.value), "Installation member belongs to another user: " + text(path));
    return result;
}
inline Identity path_identity(const fs::path& path, bool owned = false) {
    const auto found = checked_status(path);
    require(fs::exists(found), "Installation member is missing: " + text(path));
    Handle handle;
    if (fs::is_directory(found)) handle = open_directory(path);
    else handle = open_regular(path, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, owned);
    if (owned && fs::is_directory(found)) require(current_owner(handle.value), "Installation directory belongs to another user");
    return identify(handle.value);
}
inline void secure(const fs::path& path, bool directory) {
    const auto description = (directory ? std::wstring(L"D:P(A;OICI;FA;;;") : std::wstring(L"D:P(A;;FA;;;")) +
                             current_user_sid() + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(description.c_str(), SDDL_REVISION_1,
            &descriptor, nullptr) != 0, "Cannot construct private installation permissions");
    const bool success = SetFileSecurityW(path.c_str(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor);
    require(success, "Cannot protect private installation member: " + text(path));
}
inline std::string read_small(const fs::path& path, std::size_t maximum, bool owned = true) {
    auto file = open_regular(path, GENERIC_READ, 0, owned);
    const auto expected = identify(file.value);
    require(expected.size <= maximum, "Ownership record is too large: " + text(path));
    std::string result(static_cast<std::size_t>(expected.size), '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(result.size() - offset,
                                                                   std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        require(ReadFile(file.value, result.data() + offset, chunk, &count, nullptr) != 0 && count != 0,
                "Cannot read ownership record: " + text(path));
        offset += count;
    }
    char extra = 0;
    DWORD count = 0;
    require(ReadFile(file.value, &extra, 1, &count, nullptr) != 0 && count == 0 &&
            same(identify(file.value), expected), "Ownership record changed while reading: " + text(path));
    return result;
}
inline void write_new(const fs::path& path, std::string_view contents, bool privateFile = true) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) fail("Cannot create installer file", GetLastError());
    try {
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset,
                                                                       std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            require(WriteFile(file.value, contents.data() + offset, chunk, &written, nullptr) != 0 && written != 0,
                    "Cannot write installer file: " + text(path));
            offset += written;
        }
        require(FlushFileBuffers(file.value) != 0, "Cannot synchronize installer file: " + text(path));
        file = Handle();
        if (privateFile) secure(path, false);
    } catch (...) {
        file = Handle();
        DeleteFileW(path.c_str());
        throw;
    }
}
inline void copy_file(const fs::path& source, const fs::path& destination, std::uint64_t maximum) {
    directory_chain(source.parent_path());
    auto input = open_regular(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, false);
    const auto expected = identify(input.value);
    require(expected.size > 0 && expected.size <= maximum, "Trusted payload file has an invalid size: " + text(source));
    Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!output) fail("Cannot create installed payload file", GetLastError());
    try {
        std::array<char, 65536> buffer{};
        std::uint64_t copied = 0;
        while (copied < expected.size) {
            DWORD count = 0;
            const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), expected.size - copied));
            require(ReadFile(input.value, buffer.data(), wanted, &count, nullptr) != 0 && count != 0,
                    "Cannot read trusted payload: " + text(source));
            DWORD offset = 0;
            while (offset < count) {
                DWORD written = 0;
                require(WriteFile(output.value, buffer.data() + offset, count - offset, &written, nullptr) != 0 && written != 0,
                        "Cannot write installed payload: " + text(destination));
                offset += written;
            }
            copied += count;
        }
        const auto current = identify(input.value);
        require(same(current, expected) && current.size == expected.size && current.lastWrite == expected.lastWrite,
                "Trusted payload changed while copying: " + text(source));
        require(FlushFileBuffers(output.value) != 0, "Cannot synchronize installed payload");
    } catch (...) {
        output = Handle();
        DeleteFileW(destination.c_str());
        throw;
    }
}
inline void rename_new(const fs::path& from, const fs::path& to) {
    require(!fs::exists(checked_status(to)), "Installation target appeared concurrently: " + text(to));
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) fail("Cannot publish installation member");
}
inline void remove_file(const fs::path& path, const Identity* expected = nullptr) {
    auto file = open_regular(path, DELETE | FILE_READ_ATTRIBUTES, 0, false);
    if (expected) require(same(identify(file.value), *expected), "Owned file was replaced before removal: " + text(path));
    FILE_DISPOSITION_INFO disposition{TRUE};
    require(SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0,
            "Cannot remove owned file: " + text(path));
}
inline void remove_directory(const fs::path& path, const Identity* expected = nullptr) {
    auto directory = open_directory(path, DELETE | FILE_READ_ATTRIBUTES, 0);
    if (expected) require(same(identify(directory.value), *expected), "Owned directory was replaced before removal: " + text(path));
    FILE_DISPOSITION_INFO disposition{TRUE};
    require(SetFileInformationByHandle(directory.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0,
            "Cannot prune owned directory (unrecorded files will be kept): " + text(path));
}
struct CreatedDirectories {
    std::vector<fs::path> paths;
    ~CreatedDirectories() {
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
            std::error_code error;
            fs::remove(*it, error);
        }
    }
};
inline void create_chain(const fs::path& path, CreatedDirectories* created = nullptr, bool privateNew = false) {
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        auto found = checked_status(current);
        if (!fs::exists(found)) {
            if (!created) return;
            require(CreateDirectoryW(current.c_str(), nullptr) != 0, "Cannot create installation directory: " + text(current));
            created->paths.push_back(current);
            if (privateNew) secure(current, true);
            found = checked_status(current);
        }
        require(fs::is_directory(found), "Installation parent is not a directory: " + text(current));
    }
}
struct TemporaryDirectory {
    fs::path path;
    Identity identity{};
    bool retain = false;
    explicit TemporaryDirectory(const fs::path& parent) {
        directory_chain(parent);
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto name = ".yami-setup-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()) + "-" + std::to_string(attempt);
            const auto candidate = parent/name;
            if (CreateDirectoryW(candidate.c_str(), nullptr) == 0) {
                require(GetLastError() == ERROR_ALREADY_EXISTS, "Cannot create installer staging directory");
                continue;
            }
            path = candidate;
            try {
                secure(path, true);
                identity = path_identity(path, true);
                return;
            } catch (...) {
                RemoveDirectoryW(path.c_str());
                throw;
            }
        }
        throw std::runtime_error("Cannot allocate a unique installer staging directory");
    }
    ~TemporaryDirectory() {
        if (retain || path.empty()) return;
        try {
            if (same(path_identity(path), identity)) {
                std::error_code error;
                fs::remove_all(path, error);
            }
        } catch (...) {}
    }
};
inline fs::path temporary_root() {
    DWORD needed = GetTempPathW(0, nullptr);
    require(needed != 0, "Cannot locate the Windows temporary directory");
    std::wstring value(needed, L'\0');
    const auto length = GetTempPathW(needed, value.data());
    require(length != 0 && length < needed, "Cannot locate the Windows temporary directory");
    value.resize(length);
    return absolute_path(fs::path(value));
}
} // namespace yami::windows_install
