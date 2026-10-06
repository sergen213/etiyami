#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifndef YAMI_VERSION
#define YAMI_VERSION "1.0.0"
#endif
#ifndef YAMI_UPDATE_REPO
#define YAMI_UPDATE_REPO "sergen213/etiyami"
#endif

namespace yami::updates {
enum class State { Checking, Downloading, Current, Ready, Unavailable, Failed, Cancelled };
struct Snapshot {
    State state = State::Checking;
    std::uint64_t downloaded = 0, total = 0;
    std::string message;
};
struct Prepared { std::filesystem::path stage; std::string version; };

class Job {
public:
    explicit Job(const std::filesystem::path& installRoot, std::string currentVersion = YAMI_VERSION);
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    Snapshot snapshot() const;
    // Throws unless Ready. Job retains stage ownership until release_stage().
    Prepared prepared() const;
    // Call only after launch_installer succeeds; the helper then owns cleanup.
    void release_stage();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool newer_version(std::string_view candidate, std::string_view current);
std::string_view platform_id();
std::string_view version();
std::string_view repository();

// Internal trust-boundary functions shared with the permanent offline check.
namespace detail {
struct Release { std::string tag; std::uint64_t assetId = 0, size = 0; std::string digest; };
// Validates HTTPS redirect URLs; true only for the exact GitHub API origin.
bool github_api_origin(std::string_view url);
Release read_release(std::string_view json, std::string_view currentVersion);
void verify_digest(const std::filesystem::path& file, std::string_view digest,
                   const std::atomic_bool* cancelled = nullptr);
// Requires an absent destination; removes its own partial payload on failure.
void extract_archive(const std::filesystem::path& archive, const std::filesystem::path& payload,
                     const std::atomic_bool* cancelled = nullptr);
}
} // namespace yami::updates
