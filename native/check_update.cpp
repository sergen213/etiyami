#include "update.hpp"
#include "update_install.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Member { std::string name, bytes; bool link = false; int permissions = 0755; };
void zip(const fs::path& path, const std::vector<Member>& members) {
    auto* out = archive_write_new();
    assert(out && archive_write_set_format_zip(out) == ARCHIVE_OK);
#if defined(_WIN32)
    assert(archive_write_open_filename_w(out, path.c_str()) == ARCHIVE_OK);
#else
    assert(archive_write_open_filename(out, path.c_str()) == ARCHIVE_OK);
#endif
    for (const auto& member : members) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, member.name.c_str());
        archive_entry_set_filetype(entry, member.link ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry, member.permissions);
        if (member.link) archive_entry_set_symlink(entry, "../outside");
        archive_entry_set_size(entry, member.link ? 0 : static_cast<la_int64_t>(member.bytes.size()));
        assert(archive_write_header(out, entry) == ARCHIVE_OK);
        if (!member.link) assert(archive_write_data(out, member.bytes.data(), member.bytes.size()) == static_cast<la_ssize_t>(member.bytes.size()));
        archive_entry_free(entry);
    }
    assert(archive_write_close(out) == ARCHIVE_OK);
    assert(archive_write_free(out) == ARCHIVE_OK);
}
template<class F> void rejected(F&& action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
std::vector<Member> binaries() {
#if defined(_WIN32)
    return {{"yami-native.exe", "native"}, {"yami-updater.exe", "updater"}, {"yami-launcher.exe", "launcher"}};
#elif defined(__APPLE__)
    return {{"yami-native", "native"}, {"yami-updater", "updater"}, {"yami-launcher.app/Contents/MacOS/yami-launcher", "launcher"}};
#else
    return {{"yami-native", "native"}, {"yami-updater", "updater"}, {"yami-launcher", "launcher"}};
#endif
}
}

int main() {
    using namespace yami::updates;
    assert(newer_version("v1.10.0", "1.9.99"));
    assert(newer_version("v2.0.0", "1.999.999"));
    assert(newer_version("v18446744073709551615.0.0", "18446744073709551614.9.9"));
    for (const auto candidate : {"v1.0.0", "v0.9.99", "v01.2.3", "v2.0", "v2.0.0-beta", "v2.0.0+build", "v2.0.0/extra", "v18446744073709551616.0.0"})
        assert(!newer_version(candidate, "1.0.0"));
    assert(!newer_version("v2.0.0", "invalid"));
    assert(detail::github_api_origin("https://api.github.com/repos/owner/repo"));
    assert(detail::github_api_origin("https://api.github.com:443/repos/owner/repo"));
    assert(!detail::github_api_origin("https://release-assets.githubusercontent.com/file.zip?signature=example"));
    assert(!detail::github_api_origin("https://api.github.com.attacker.invalid/file"));
    for (const auto unsafe : {"http://api.github.com/file", "https://token@api.github.com/file", "https://api.github.com:444/file", "https://api.github.com/file#fragment", "https://api.github.com/line\nbreak"})
        rejected([&] { detail::github_api_origin(unsafe); });

    const auto assetName = "yami-" + std::string(platform_id()) + ".zip";
    const auto assetUrl = "https://api.github.com/repos/" + std::string(repository()) + "/releases/assets/123";
    const std::string digest = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto metadata = [&](const std::string& fields) {
        return "{\"tag_name\":\"v2.0.0\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"" + assetName + "\",\"url\":\"" + assetUrl + "\"," + fields + "}]}";
    };
    const auto release = detail::read_release(metadata("\"id\":123,\"size\":3,\"digest\":\"" + digest + "\""), "1.0.0");
    assert(release.tag == "v2.0.0" && release.assetId == 123 && release.size == 3 && release.digest == digest);
    rejected([&] { detail::read_release(metadata("\"id\":\"123\",\"size\":3,\"digest\":\"" + digest + "\""), "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":124,\"size\":3,\"digest\":\"" + digest + "\""), "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":123,\"size\":0,\"digest\":null"), "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":123,\"size\":3"), "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":123,\"size\":3,\"digest\":\"" + digest + "\"") + " trailing", "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":123,\"size\":1073741825,\"digest\":\"" + digest + "\""), "1.0.0"); });
    rejected([&] { detail::read_release(metadata("\"id\":123,\"size\":3.5,\"digest\":\"" + digest + "\""), "1.0.0"); });
    assert(detail::read_release("{\"tag_name\":\"v1.0.0\",\"draft\":false,\"prerelease\":false,\"assets\":[]}", "1.0.0").assetId == 0);

    const auto root = fs::canonical(fs::temp_directory_path()) / ("yami-update-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    { std::ofstream out(root / "bytes", std::ios::binary); out << "abc"; }
    detail::verify_digest(root / "bytes", digest);
    { std::ofstream out(root / "bytes", std::ios::binary | std::ios::app); out << "d"; }
    rejected([&] { detail::verify_digest(root / "bytes", digest); });
    rejected([&] { detail::verify_digest(root / "bytes", "sha256:bad"); });

    const auto archive = root / "release.zip";
    const auto payload = root / "payload";
    zip(archive, binaries());
    detail::extract_archive(archive, payload);
    detail::validate_payload(payload);
    assert(fs::is_regular_file(payload / binaries().front().name));
    rejected([&] { detail::extract_archive(archive, payload); }); // never overwrite a preexisting stage
    fs::remove_all(payload);
#if !defined(_WIN32)
    fs::create_directory(root / "outside-dir");
    fs::create_directory_symlink(root / "outside-dir", payload);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(fs::is_empty(root / "outside-dir") && fs::is_symlink(payload));
    fs::remove(payload);
#endif
    for (const auto badPath : {"../outside", "/outside", "C:/outside", "lib/../yami-native", "game/data/file", "Resources/game/file", "lib/game.ini", ".git/config", "lib/file:stream", "lib/bad\\file", "lib/bad\nfile"}) {
        auto members = binaries(); members.push_back({badPath, "bad"}); zip(archive, members);
        rejected([&] { detail::extract_archive(archive, payload); });
        assert(!fs::exists(payload) && !fs::exists(root / "outside"));
    }
    auto missing = binaries(); missing.pop_back(); zip(archive, missing);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
    auto linked = binaries(); linked.front().link = true; zip(archive, linked);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
#if !defined(_WIN32)
    auto nonExecutable = binaries(); nonExecutable.front().permissions = 0644; zip(archive, nonExecutable);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
#endif
    auto duplicate = binaries(); duplicate.push_back(duplicate.front()); zip(archive, duplicate);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
#if defined(_WIN32)
    const std::string library = "example.dll", otherCase = "Example.dll";
#elif defined(__APPLE__)
    const std::string library = "libexample.dylib", otherCase = "libExample.dylib";
#else
    const std::string library = "libexample.so", otherCase = "libExample.so";
#endif
    auto collision = binaries(); collision.push_back({library, "a"}); collision.push_back({otherCase, "b"}); zip(archive, collision);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
    zip(archive, binaries());
    fs::resize_file(archive, fs::file_size(archive) / 2);
    rejected([&] { detail::extract_archive(archive, payload); });
    assert(!fs::exists(payload));
    std::atomic_bool cancelled{true};
    zip(archive, binaries());
    rejected([&] { detail::extract_archive(archive, payload, &cancelled); });
    assert(!fs::exists(payload));
    std::cout << "update versions, release schema, SHA256 integrity and bounded safe ZIP extraction verified\n";
}
