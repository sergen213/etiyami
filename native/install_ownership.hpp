#pragma once
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace yami::ownership {
inline constexpr std::string_view markerName = ".eti-yami-install";
inline constexpr std::string_view marker = "ETI Yami native Linux installation\nformat=1\n";
inline constexpr std::string_view installedName = ".eti-yami-installed-files";
inline constexpr std::string_view installedMagic = "ETI Yami installed ownership\nformat=1\n";
inline constexpr std::string_view engineName = ".eti-yami-engine-files";
inline constexpr std::string_view engineMagic = "ETI Yami engine ownership\nformat=1\n";
inline constexpr std::string_view helperDirectory = ".eti-yami-uninstall";
inline constexpr std::string_view helperName = "yami-remove";
inline constexpr std::string_view scriptName = "uninstall.sh";
inline constexpr std::string_view playShortcut = "eti-yami.desktop";
inline constexpr std::string_view uninstallShortcut = "eti-yami-uninstall.desktop";
inline constexpr std::size_t maxInstalledBytes = 8 * 1024 * 1024;
inline constexpr std::size_t maxInstalledRecords = 32768;
inline constexpr std::size_t maxPathBytes = 4096;
inline constexpr std::size_t maxEngineBytes = 65536;
inline constexpr std::size_t maxEnginePaths = 2048;
inline bool valid_text(std::string_view value) {
    return !value.empty() && value.size() <= maxPathBytes &&
           value.find_first_of("\r\n") == std::string_view::npos && value.find('\0') == std::string_view::npos;
}
inline bool valid_relative_file(std::string_view value) {
    if (!valid_text(value) || value.front() == '/' || value.find('\\') != std::string_view::npos) return false;
    for (std::size_t begin = 0; begin <= value.size();) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}
inline bool valid_engine_file(std::string_view value) {
    if (!valid_relative_file(value)) return false;
    if (value == "yami-native" || value == "yami-updater" || value == "yami-launcher") return true;
    if (value.starts_with("lib/")) value.remove_prefix(4);
    if (value.find('/') != std::string_view::npos) return false;
    static const std::regex library(R"(lib[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*)");
    static const std::regex host(R"((?:lib(?:c|m|dl|pthread|rt|resolv|util|anl|nss_[^.]+|stdc\+\+|gcc_s)|lib(?:GL|GLX|GLdispatch|OpenGL|EGL|GLESv[12]|vulkan|drm(?:_[^.]+)?|gbm|cuda|nvidia[^.]*|vdpau|va(?:-x11|-drm|-wayland)?))\.so(?:\.[0-9]+)*)");
    const std::string name(value);
    return std::regex_match(name, library) && !std::regex_match(name, host);
}
inline bool valid_shortcut(std::string_view value) {
    if (!valid_text(value)) return false;
    const std::filesystem::path path(value);
    return path.is_absolute() && path.lexically_normal() == path &&
           (path.filename() == playShortcut || path.filename() == uninstallShortcut);
}
struct InstalledFiles { std::vector<std::string> files, shortcuts; };
inline std::vector<std::string_view> lines(std::string_view text, std::string_view magic, std::size_t bytes, std::size_t records) {
    if (text.size() > bytes || !text.starts_with(magic) || !text.ends_with('\n'))
        throw std::runtime_error("Invalid ETI Yami ownership inventory");
    text.remove_prefix(magic.size());
    std::vector<std::string_view> result;
    while (!text.empty()) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos || result.size() == records)
            throw std::runtime_error("Invalid ETI Yami ownership inventory");
        result.push_back(text.substr(0, end)); text.remove_prefix(end + 1);
    }
    return result;
}
inline InstalledFiles parse_installed(std::string_view text) {
    InstalledFiles result;
    bool shortcuts = false;
    for (const auto line : lines(text, installedMagic, maxInstalledBytes, maxInstalledRecords)) {
        if (line.starts_with("F\t") && !shortcuts && valid_relative_file(line.substr(2))) result.files.emplace_back(line.substr(2));
        else if (line.starts_with("S\t") && valid_shortcut(line.substr(2))) { shortcuts = true; result.shortcuts.emplace_back(line.substr(2)); }
        else throw std::runtime_error("Invalid ETI Yami installed ownership path");
    }
    for (const auto* values : {&result.files, &result.shortcuts})
        if (!std::is_sorted(values->begin(), values->end()) || std::adjacent_find(values->begin(), values->end()) != values->end())
            throw std::runtime_error("ETI Yami ownership paths must be sorted and unique");
    for (const auto required : {markerName, installedName, engineName, scriptName})
        if (!std::binary_search(result.files.begin(), result.files.end(), std::string(required)))
            throw std::runtime_error("Incomplete ETI Yami ownership inventory");
    if (!std::binary_search(result.files.begin(), result.files.end(), std::string(helperDirectory) + "/" + std::string(helperName)))
        throw std::runtime_error("Incomplete ETI Yami remover ownership inventory");
    return result;
}
inline std::vector<std::string> parse_engine(std::string_view text) {
    std::vector<std::string> result;
    for (const auto line : lines(text, engineMagic, maxEngineBytes, maxEnginePaths)) {
        if (!valid_engine_file(line)) throw std::runtime_error("Invalid ETI Yami engine ownership path");
        result.emplace_back(line);
    }
    if (!std::is_sorted(result.begin(), result.end()) || std::adjacent_find(result.begin(), result.end()) != result.end())
        throw std::runtime_error("ETI Yami engine ownership paths must be sorted and unique");
    for (const auto name : {"yami-native", "yami-updater", "yami-launcher"})
        if (!std::binary_search(result.begin(), result.end(), std::string(name)))
            throw std::runtime_error("Incomplete ETI Yami engine ownership inventory");
    return result;
}
inline std::string serialize_installed(InstalledFiles values) {
    std::string text(installedMagic);
    for (auto* paths : {&values.files, &values.shortcuts}) {
        std::sort(paths->begin(), paths->end()); paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
        for (const auto& path : *paths) text += (paths == &values.files ? "F\t" : "S\t") + path + "\n";
    }
    parse_installed(text); return text;
}
inline std::string serialize_engine(std::vector<std::string> values) {
    std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end());
    std::string text(engineMagic);
    for (const auto& value : values) text += value + "\n";
    parse_engine(text); return text;
}
} // namespace yami::ownership
