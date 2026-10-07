#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>

namespace yami::setup {
// Return false to cancel; exceptions abort before publishing a fresh installation.
using Progress = std::function<bool(std::string_view, std::uint64_t, std::uint64_t)>;
// Caller supplies fresh private game/scratch directories; never executes ISO programs.
void extract_iso_assets(const std::filesystem::path& iso,
                        const std::filesystem::path& game,
                        const std::filesystem::path& scratch,
                        const std::filesystem::path& archiver,
                        const Progress& progress);
// Read only the requested native artwork from the user's ISO into a private preview.
void extract_iso_artwork(const std::filesystem::path& iso,
                         const std::filesystem::path& game,
                         const std::filesystem::path& scratch,
                         const std::filesystem::path& archiver,
                         std::span<const std::string_view> paths,
                         const Progress& progress);
struct InstallPaths {
    std::filesystem::path root, applications, desktop;
};
InstallPaths default_install_paths();
struct InstallRequest {
    InstallPaths paths;
    std::filesystem::path iso, engine, archiver;
};
struct InstallResult {
    InstallPaths paths;
    bool reused = false;
};
// Fresh install is staged on the destination volume; owned reinstalls keep data.
InstallResult install(const InstallRequest& request, const Progress& progress);
void launch_game(const InstallResult& installation);
} // namespace yami::setup
