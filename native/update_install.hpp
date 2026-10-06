#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace yami::updates {

// SDL's bundle Resources directory is not the writable installation root on macOS.
std::filesystem::path installation_root();

// Throws on validation, replacement, or rollback failure. Unrelated files are never targets.
void install_prepared(const std::filesystem::path& stage,
                      const std::filesystem::path& installRoot);

// Runs a copy of the installed helper, not downloaded code. Success transfers no ownership:
// the caller must release its stage and exit normally so the helper can replace Windows binaries.
void launch_installer(const std::filesystem::path& stage,
                      const std::filesystem::path& installRoot,
                      const std::filesystem::path& assetRoot,
                      const std::filesystem::path& saveDirectory);

// Removes only marked, committed stages whose detached helper has exited.
void cleanup_completed_updates(const std::filesystem::path& installRoot);

namespace detail {
// Shared by archive extraction and the detached helper's independent pre-install validation.
bool valid_payload_path(std::string_view relative, bool directory);
void validate_payload(const std::filesystem::path& payload);
void secure_stage_directory(const std::filesystem::path& directory);

void wait_for_parent_exit(std::uint64_t parent);
void restart_launcher(const std::filesystem::path& installRoot,
                      const std::filesystem::path& assetRoot,
                      const std::filesystem::path& saveDirectory);
void mark_completed(const std::filesystem::path& stage);
bool append_installer_log(const std::filesystem::path& stage, std::string_view message) noexcept;
} // namespace detail
} // namespace yami::updates
