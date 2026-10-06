#include "update.hpp"
#include "update_install.hpp"

#include <SDL3/SDL.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <json-c/json.h>
extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace yami::updates {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t maxArchive = 1024ull * 1024 * 1024;
constexpr std::uint64_t maxPayload = 2 * maxArchive;
constexpr std::uint64_t maxMember = maxArchive / 2;
constexpr std::size_t maxEntries = 8192;
constexpr std::size_t maxMetadata = 4 * 1024 * 1024;
using Clock = std::chrono::steady_clock;

struct Cancelled : std::exception {};
struct Failure { State state; const char* message; };
void check_cancel(const std::atomic_bool* cancelled) {
    if (cancelled && cancelled->load(std::memory_order_relaxed)) throw Cancelled{};
}
void invalid_metadata() { throw std::runtime_error("GitHub release metadata is invalid or incomplete."); }
void invalid_archive() { throw std::runtime_error("Update ZIP is unsafe, incomplete or too large."); }
void wipe(std::string& value) {
    volatile char* data = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
    value.clear();
}
struct Secret { std::string value; ~Secret() { wipe(value); } };

bool parse_version(std::string_view text, std::array<std::uint64_t, 3>& parts) {
    if (!text.empty() && text.front() == 'v') text.remove_prefix(1);
    if (text.empty() || text.size() > 64) return false;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto end = text.find('.');
        const auto part = text.substr(0, end);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) return false;
        for (char ch : part) if (ch < '0' || ch > '9') return false;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), parts[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) return false;
        if (i + 1 == parts.size()) return end == std::string_view::npos;
        if (end == std::string_view::npos) return false;
        text.remove_prefix(end + 1);
    }
    return false;
}
std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& ch : result) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return result;
}
bool digest_valid(std::string_view digest) {
    if (digest.size() != 71 || !digest.starts_with("sha256:")) return false;
    for (char ch : digest.substr(7))
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
    return true;
}
std::string asset_url(std::uint64_t id) {
    return "https://api.github.com/repos/" + std::string(repository()) + "/releases/assets/" + std::to_string(id);
}
json_object* field(json_object* object, const char* name, json_type type) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) || !json_object_is_type(value, type)) invalid_metadata();
    return value;
}
std::string_view text_field(json_object* object, const char* name, std::size_t limit) {
    auto* value = field(object, name, json_type_string);
    const auto size = static_cast<std::size_t>(json_object_get_string_len(value));
    if (size > limit) invalid_metadata();
    const std::string_view result(json_object_get_string(value), size);
    if (result.find('\0') != std::string_view::npos) invalid_metadata();
    return result;
}

bool valid_token(std::string_view token) {
    if (token.empty() || token.size() > 4096) return false;
    for (unsigned char ch : token)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
    return true;
}
struct StopProcess {
    void operator()(SDL_Process* process) const {
        if (!process) return;
        int code = 0;
        if (!SDL_WaitProcess(process, false, &code)) {
            SDL_KillProcess(process, true);
            const auto deadline = Clock::now() + std::chrono::milliseconds(250);
            while (!SDL_WaitProcess(process, false, &code) && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SDL_DestroyProcess(process);
    }
};
void get_token(Secret& token, const std::atomic_bool& cancelled) {
    for (const auto* name : {"YAMI_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"}) {
        const auto* value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), name);
        if (!value || !*value) continue;
        const auto size = SDL_strnlen(value, 4097);
        if (size > 4096 || !valid_token(std::string_view(value, size)))
            throw Failure{State::Unavailable, "GitHub token is invalid. Check YAMI_GITHUB_TOKEN or gh auth login."};
        token.value.assign(value, size);
        return;
    }
    check_cancel(&cancelled);
    const char* args[] = {"gh", "auth", "token", "--hostname", "github.com", nullptr};
    const auto props = SDL_CreateProperties();
    if (!props) return;
    const bool configured = SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    std::unique_ptr<SDL_Process, StopProcess> process(configured ? SDL_CreateProcessWithProperties(props) : nullptr);
    SDL_DestroyProperties(props);
    if (!process) return; // gh is optional; public repositories still work.
    auto* output = SDL_GetProcessOutput(process.get());
    Secret captured;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool exited = false;
    int exitCode = -1;
    std::array<char, 256> buffer{};
    while (output && Clock::now() < deadline && !cancelled.load(std::memory_order_relaxed)) {
        const auto count = SDL_ReadIO(output, buffer.data(), buffer.size());
        if (count) {
            if (captured.value.size() + count > 4096) break;
            captured.value.append(buffer.data(), count);
            std::fill(buffer.begin(), buffer.end(), 0);
            continue;
        }
        if (SDL_WaitProcess(process.get(), false, &exitCode)) { exited = true; break; }
        if (SDL_GetIOStatus(output) != SDL_IO_STATUS_NOT_READY && SDL_GetIOStatus(output) != SDL_IO_STATUS_EOF) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::fill(buffer.begin(), buffer.end(), 0);
    check_cancel(&cancelled);
    if (!exited || exitCode != 0) return;
    while (!captured.value.empty() && (captured.value.back() == '\n' || captured.value.back() == '\r')) captured.value.pop_back();
    if (valid_token(captured.value)) token.value.swap(captured.value);
}

struct CurlRuntime {
    CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("TLS client initialization failed.");
        const auto* info = curl_version_info(CURLVERSION_NOW);
        if (!info || !(info->features & CURL_VERSION_SSL) || !(info->features & CURL_VERSION_ASYNCHDNS)) {
            curl_global_cleanup();
            throw Failure{State::Unavailable, "Updates require libcurl with TLS and asynchronous DNS for bounded cancellation."};
        }
    }
    ~CurlRuntime() { curl_global_cleanup(); }
};
// Global curl initialization is thread-safe, once per process and before handles.
void init_curl() { static const CurlRuntime runtime; (void)runtime; }
std::string url_part(CURLU* url, CURLUPart part) {
    char* raw = nullptr;
    if (curl_url_get(url, part, &raw, 0) != CURLUE_OK) return {};
    std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
    return value.get();
}
bool has_url_part(CURLU* url, CURLUPart part) {
    char* raw = nullptr;
    const bool present = curl_url_get(url, part, &raw, 0) == CURLUE_OK;
    curl_free(raw);
    return present;
}
bool api_origin(std::string_view text) {
    if (text.empty() || text.size() > 8192) throw std::runtime_error("Unsafe update redirect.");
    for (unsigned char ch : text) if (ch <= 32 || ch == 127 || ch == '\\') throw std::runtime_error("Unsafe update redirect.");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    const std::string owned(text);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK ||
        lower_ascii(url_part(url.get(), CURLUPART_SCHEME)) != "https" ||
        has_url_part(url.get(), CURLUPART_USER) || has_url_part(url.get(), CURLUPART_PASSWORD) ||
        has_url_part(url.get(), CURLUPART_FRAGMENT)) throw std::runtime_error("Unsafe update redirect.");
    const auto host = lower_ascii(url_part(url.get(), CURLUPART_HOST));
    const auto port = url_part(url.get(), CURLUPART_PORT);
    if (host.empty() || (!port.empty() && port != "443")) throw std::runtime_error("Unsafe update redirect.");
    return host == "api.github.com";
}

struct Transfer {
    CURL* easy = nullptr;
    const std::atomic_bool* cancelled = nullptr;
    std::ofstream* file = nullptr;
    std::string body;
    std::uint64_t expected = 0, received = 0, discarded = 0;
    void* progressContext = nullptr;
    void (*progress)(void*, std::uint64_t, std::uint64_t) = nullptr;
    bool failed = false;
};
std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto bytes = size * count;
    if (transfer.cancelled->load(std::memory_order_relaxed)) return 0;
    try {
        long status = 0;
        if (curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) return 0;
        if (status != 200) {
            if (bytes > 65536 - transfer.discarded) { transfer.failed = true; return 0; }
            transfer.discarded += bytes;
            return bytes;
        }
        if (transfer.file) {
            if (bytes > transfer.expected - transfer.received) { transfer.failed = true; return 0; }
            transfer.file->write(data, static_cast<std::streamsize>(bytes));
            if (!*transfer.file) { transfer.failed = true; return 0; }
            transfer.received += bytes;
            if (transfer.progress) transfer.progress(transfer.progressContext, transfer.received, transfer.expected);
        } else {
            if (bytes > maxMetadata - transfer.body.size()) { transfer.failed = true; return 0; }
            transfer.body.append(data, bytes);
        }
        return bytes;
    } catch (...) { transfer.failed = true; return 0; }
}
int transfer_progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<Transfer*>(context)->cancelled->load(std::memory_order_relaxed) ? 1 : 0;
}
struct Response { long status = 0; std::string body; };
Response request(std::string url, const Secret& token, Transfer& transfer) {
    init_curl();
    const auto deadline = Clock::now() + (transfer.file ? std::chrono::minutes(10) : std::chrono::seconds(30));
    for (int redirects = 0; redirects <= 5; ++redirects) {
        check_cancel(transfer.cancelled);
        const bool api = api_origin(url);
        if (!transfer.file && !api) throw std::runtime_error("Unsafe GitHub API redirect.");
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
        if (!easy) throw std::runtime_error("TLS client initialization failed.");
        transfer.easy = easy.get();
        transfer.body.clear(); transfer.discarded = 0;
        auto set = [&](CURLoption option, auto value) {
            if (curl_easy_setopt(easy.get(), option, value) != CURLE_OK) throw std::runtime_error("TLS client configuration failed.");
        };
        set(CURLOPT_URL, url.c_str());
        set(CURLOPT_PROTOCOLS_STR, "https");
        set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
        set(CURLOPT_FOLLOWLOCATION, 0L); // Rebuild headers for every origin; never forward a token.
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
        set(CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
#if (defined(_WIN32) || defined(__APPLE__)) && defined(CURLSSLOPT_NATIVE_CA)
        set(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) throw Failure{State::Unavailable, "Update connection timed out. Current game remains available."};
        set(CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
        set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
        set(CURLOPT_LOW_SPEED_TIME, 20L);
        set(CURLOPT_USERAGENT, "Yami-Updater/1");
        set(CURLOPT_WRITEFUNCTION, &receive);
        set(CURLOPT_WRITEDATA, &transfer);
        set(CURLOPT_NOPROGRESS, 0L);
        set(CURLOPT_XFERINFOFUNCTION, &transfer_progress);
        set(CURLOPT_XFERINFODATA, &transfer);
        curl_slist* rawHeaders = nullptr;
        auto append = [&](const char* header) {
            auto* next = curl_slist_append(rawHeaders, header);
            if (!next) { curl_slist_free_all(rawHeaders); rawHeaders = nullptr; throw std::bad_alloc(); }
            rawHeaders = next;
        };
        try {
            if (api) {
                append(transfer.file ? "Accept: application/octet-stream" : "Accept: application/vnd.github+json");
                append("X-GitHub-Api-Version: 2022-11-28");
                if (!token.value.empty()) {
                    Secret header; header.value = "Authorization: Bearer " + token.value;
                    append(header.value.c_str());
                }
            }
        } catch (...) { curl_slist_free_all(rawHeaders); throw; }
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
        set(CURLOPT_HTTPHEADER, headers.get());
        std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
        if (!multi || curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) throw std::runtime_error("TLS client initialization failed.");
        int running = 1;
        CURLMcode multiCode = CURLM_OK;
        while (running && multiCode == CURLM_OK && !transfer.cancelled->load(std::memory_order_relaxed)) {
            multiCode = curl_multi_perform(multi.get(), &running);
            if (running && multiCode == CURLM_OK) multiCode = curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr);
        }
        CURLcode result = CURLE_FAILED_INIT;
        int remainingMessages = 0;
        while (auto* message = curl_multi_info_read(multi.get(), &remainingMessages))
            if (message->msg == CURLMSG_DONE) result = message->data.result;
        curl_multi_remove_handle(multi.get(), easy.get());
        check_cancel(transfer.cancelled);
        if (transfer.failed) throw std::runtime_error("Update data is too large or could not be saved.");
        if (multiCode != CURLM_OK || result != CURLE_OK)
            throw Failure{State::Unavailable, "Secure update connection failed. Check network and system certificates."};
        long status = 0;
        if (curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) throw std::runtime_error("Invalid update response.");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char* redirect = nullptr;
            if (redirects == 5 || curl_easy_getinfo(easy.get(), CURLINFO_REDIRECT_URL, &redirect) != CURLE_OK || !redirect)
                throw std::runtime_error("Unsafe update redirect.");
            url.assign(redirect);
            continue;
        }
        return {status, std::move(transfer.body)};
    }
    throw std::runtime_error("Too many update redirects.");
}
void require_success(long status) {
    if (status == 200) return;
    if (status == 401) throw Failure{State::Unavailable, "GitHub authorization failed. Renew the token or run gh auth login."};
    if (status == 403 || status == 429)
        throw Failure{State::Unavailable, "GitHub denied access or rate-limited updates. Check token access and try later."};
    if (status == 404)
        throw Failure{State::Unavailable, "GitHub release asset is unavailable. Check repository access and try later."};
    throw Failure{State::Unavailable, "GitHub updates are temporarily unavailable. Current game remains available."};
}

fs::path make_stage(const fs::path& installRoot) {
    const auto root = fs::absolute(installRoot).lexically_normal();
    for (auto parent = root; !parent.empty();) {
        const auto status = fs::symlink_status(parent);
        if (fs::is_symlink(status) || !fs::is_directory(status)) throw std::runtime_error("Installation directory is unsafe or unavailable.");
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    std::random_device random;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto name = ".yami-update-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(random());
        const auto stage = root / name;
        if (!fs::create_directory(stage)) continue;
        try { detail::secure_stage_directory(stage); }
        catch (...) { std::error_code ec; fs::remove(stage, ec); throw; }
        return stage;
    }
    throw std::runtime_error("Could not create a private update staging directory.");
}
} // namespace

bool newer_version(std::string_view candidate, std::string_view current) {
    std::array<std::uint64_t, 3> proposed{}, installed{};
    return parse_version(candidate, proposed) && parse_version(current, installed) && proposed > installed;
}
std::string_view platform_id() {
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(_WIN32)
    return "windows-arm64";
#elif defined(__APPLE__)
    return "macos-arm64";
#elif defined(__linux__)
    return "linux-arm64";
#else
#error Unsupported update platform
#endif
#elif defined(__x86_64__) || defined(_M_X64)
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__)
    return "macos-x86_64";
#elif defined(__linux__)
    return "linux-x86_64";
#else
#error Unsupported update platform
#endif
#else
#error Unsupported update architecture
#endif
}
std::string_view version() { return YAMI_VERSION; }
std::string_view repository() { return YAMI_UPDATE_REPO; }

namespace detail {
bool github_api_origin(std::string_view url) {
    init_curl();
    return api_origin(url);
}
Release read_release(std::string_view json, std::string_view currentVersion) {
    if (json.empty() || json.size() > maxMetadata) invalid_metadata();
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(json_tokener_new_ex(32), json_tokener_free);
    if (!parser) throw std::bad_alloc();
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    std::unique_ptr<json_object, decltype(&json_object_put)> object(
        json_tokener_parse_ex(parser.get(), json.data(), static_cast<int>(json.size())), json_object_put);
    if (json_tokener_get_error(parser.get()) != json_tokener_success || !json_object_is_type(object.get(), json_type_object)) invalid_metadata();
    for (auto ch : json.substr(json_tokener_get_parse_end(parser.get())))
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') invalid_metadata();
    const auto tag = text_field(object.get(), "tag_name", 65);
    std::array<std::uint64_t, 3> parsed{};
    if (tag.empty() || tag.front() != 'v' || !parse_version(tag, parsed) || !parse_version(currentVersion, parsed) ||
        json_object_get_boolean(field(object.get(), "draft", json_type_boolean)) ||
        json_object_get_boolean(field(object.get(), "prerelease", json_type_boolean))) invalid_metadata();
    Release release; release.tag = tag;
    if (!newer_version(tag, currentVersion)) return release;
    auto* assets = field(object.get(), "assets", json_type_array);
    const auto count = json_object_array_length(assets);
    if (count > maxEntries) invalid_metadata();
    const auto expectedName = "yami-" + std::string(platform_id()) + ".zip";
    for (std::size_t i = 0; i < count; ++i) {
        auto* asset = json_object_array_get_idx(assets, i);
        if (!json_object_is_type(asset, json_type_object)) invalid_metadata();
        if (text_field(asset, "name", 256) != expectedName) continue;
        if (release.assetId) invalid_metadata();
        const auto id = json_object_get_int64(field(asset, "id", json_type_int));
        const auto size = json_object_get_int64(field(asset, "size", json_type_int));
        if (id <= 0 || id > 9007199254740991ll || size <= 0 || static_cast<std::uint64_t>(size) > maxArchive) invalid_metadata();
        release.assetId = static_cast<std::uint64_t>(id);
        release.size = static_cast<std::uint64_t>(size);
        if (text_field(asset, "url", 1024) != asset_url(release.assetId)) invalid_metadata();
        const auto digest = text_field(asset, "digest", 71);
        if (!digest_valid(digest)) invalid_metadata();
        release.digest = digest;
    }
    if (!release.assetId) throw std::runtime_error("No complete update package exists for this platform.");
    return release;
}

void verify_digest(const fs::path& file, std::string_view digest, const std::atomic_bool* cancelled) {
    if (!digest_valid(digest)) throw std::runtime_error("Update SHA256 digest is missing or invalid.");
    if (!fs::is_regular_file(fs::symlink_status(file)) || fs::file_size(file) > maxArchive)
        throw std::runtime_error("Update archive is not a bounded regular file.");
    std::ifstream input(file, std::ios::binary);
    std::unique_ptr<AVSHA, decltype(&av_free)> hash(av_sha_alloc(), av_free);
    if (!input || !hash || av_sha_init(hash.get(), 256) != 0) throw std::runtime_error("Update integrity check could not start.");
    std::array<unsigned char, 65536> bytes{};
    std::uint64_t total = 0;
    while (input) {
        check_cancel(cancelled);
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > maxArchive - total) throw std::runtime_error("Update archive is too large.");
        total += count;
        av_sha_update(hash.get(), bytes.data(), count);
    }
    if (!input.eof()) throw std::runtime_error("Update archive could not be read.");
    std::array<unsigned char, 32> result{};
    av_sha_final(hash.get(), result.data());
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, 64> encoded{};
    for (std::size_t i = 0; i < result.size(); ++i) { encoded[2 * i] = hex[result[i] >> 4]; encoded[2 * i + 1] = hex[result[i] & 15]; }
    if (lower_ascii(digest.substr(7)) != std::string_view(encoded.data(), encoded.size()))
        throw std::runtime_error("Update SHA256 verification failed. No installed files were changed.");
}

void extract_archive(const fs::path& source, const fs::path& payload, const std::atomic_bool* cancelled) {
    check_cancel(cancelled);
    if (!fs::is_regular_file(fs::symlink_status(source)) || fs::file_size(source) == 0 || fs::file_size(source) > maxArchive) invalid_archive();
    if (!fs::is_directory(fs::symlink_status(payload.parent_path())) || !fs::create_directory(payload))
        throw std::runtime_error("Update payload directory already exists or is unsafe.");
    struct Partial {
        fs::path path; bool complete = false;
        ~Partial() { if (!complete) { std::error_code ec; fs::remove_all(path, ec); } }
    } partial{payload};
    secure_stage_directory(payload);
    std::unique_ptr<struct archive, decltype(&archive_read_free)> input(archive_read_new(), archive_read_free);
    if (!input || archive_read_support_format_zip_seekable(input.get()) != ARCHIVE_OK) invalid_archive();
#if defined(_WIN32)
    const auto opened = archive_read_open_filename_w(input.get(), source.c_str(), 65536);
#else
    const auto opened = archive_read_open_filename(input.get(), source.c_str(), 65536);
#endif
    if (opened != ARCHIVE_OK) invalid_archive();
    std::set<std::string> entries;
    // Includes implicit parents, so lib/one and LIB/two collide even without directory entries.
    std::map<std::string, std::pair<std::string, bool>> components;
    std::uint64_t total = 0;
    std::array<char, 65536> bytes{};
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
        check_cancel(cancelled);
        const auto type = archive_entry_filetype(entry);
        const bool directory = type == AE_IFDIR;
        const char* raw = archive_entry_pathname_utf8(entry);
        if (!raw) raw = archive_entry_pathname(entry);
        if (!raw || (type != AE_IFREG && !directory) || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry) || archive_entry_sparse_count(entry) || !archive_entry_size_is_set(entry)) invalid_archive();
        std::string name(raw);
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        if (!valid_payload_path(name, directory) || entries.size() >= maxEntries || !entries.insert(lower_ascii(name)).second) invalid_archive();
        const auto declared = archive_entry_size(entry);
        if (declared < 0 || static_cast<std::uint64_t>(declared) > maxMember || (directory && declared != 0) ||
            static_cast<std::uint64_t>(declared) > maxPayload - total) invalid_archive();
        total += static_cast<std::uint64_t>(declared);
        std::size_t position = 0;
        do {
            position = name.find('/', position);
            const auto part = name.substr(0, position);
            const bool isDirectory = position != std::string::npos || directory;
            const auto key = lower_ascii(part);
            const auto [found, inserted] = components.emplace(key, std::make_pair(part, isDirectory));
            if (!inserted && (found->second.first != part || found->second.second != isDirectory)) invalid_archive();
            if (position != std::string::npos) ++position;
        } while (position != std::string::npos);
        const auto target = payload / fs::path(name);
        fs::create_directories(target.parent_path());
        if (directory) { fs::create_directories(target); continue; }
        if (fs::exists(fs::symlink_status(target))) invalid_archive();
        std::ofstream output(target, std::ios::binary | std::ios::out);
        if (!output) throw std::runtime_error("Update payload could not be saved.");
        std::uint64_t written = 0;
        la_ssize_t count = 0;
        while ((count = archive_read_data(input.get(), bytes.data(), bytes.size())) > 0) {
            check_cancel(cancelled);
            if (static_cast<std::uint64_t>(count) > static_cast<std::uint64_t>(declared) - written) invalid_archive();
            output.write(bytes.data(), count);
            if (!output) throw std::runtime_error("Update payload could not be saved.");
            written += static_cast<std::uint64_t>(count);
        }
        if (count < 0 || written != static_cast<std::uint64_t>(declared)) invalid_archive();
        output.close();
        if (!output) throw std::runtime_error("Update payload could not be saved.");
        const auto executable = (archive_entry_perm(entry) & 0111) != 0;
        const auto permissions = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read |
            (executable ? fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec : fs::perms::none);
        fs::permissions(target, permissions, fs::perm_options::replace);
    }
    if (status != ARCHIVE_EOF || archive_read_has_encrypted_entries(input.get()) > 0 || archive_read_close(input.get()) != ARCHIVE_OK) invalid_archive();
    check_cancel(cancelled);
    validate_payload(payload);
    partial.complete = true;
}
} // namespace detail

struct Job::Impl {
    fs::path installRoot, ownedStage;
    std::string currentVersion;
    mutable std::mutex mutex;
    Snapshot status{State::Checking, 0, 0, "Checking GitHub updates..."};
    Prepared ready;
    std::atomic_bool cancelled{false};
    bool transferred = false;
    std::thread worker;

    Impl(fs::path root, std::string current) : installRoot(std::move(root)), currentVersion(std::move(current)) {
        status.message.reserve(192); // All displayed messages are fixed ASCII and bounded below this capacity.
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        cancelled.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
        if (!transferred && !ownedStage.empty()) { std::error_code ec; fs::remove_all(ownedStage, ec); }
    }
    void publish(State state, const char* message) {
        std::lock_guard lock(mutex);
        status.state = state; status.message = message;
    }
    static void progress(void* context, std::uint64_t downloaded, std::uint64_t total) {
        auto& self = *static_cast<Impl*>(context);
        std::lock_guard lock(self.mutex);
        self.status.downloaded = std::min(downloaded, total); self.status.total = total;
    }
    void run() noexcept {
        try {
            std::array<std::uint64_t, 3> parsed{};
            if (!parse_version(currentVersion, parsed)) throw std::runtime_error("Installed update version is invalid.");
            Secret token;
            get_token(token, cancelled);
            Transfer metadata; metadata.cancelled = &cancelled;
            const auto base = "https://api.github.com/repos/" + std::string(repository());
            auto response = request(base + "/releases/latest", token, metadata);
            if (response.status == 404) {
                auto repo = request(base, token, metadata);
                if (repo.status == 200) throw Failure{State::Unavailable, "No published GitHub release is available yet. Current game remains available."};
                if (repo.status == 404) {
                    if (token.value.empty()) throw Failure{State::Unavailable, "Repository is private or has no release. Set YAMI_GITHUB_TOKEN or run gh auth login."};
                    throw Failure{State::Unavailable, "Token cannot access the repository, or no release exists. Check private repository read access."};
                }
                require_success(repo.status);
            }
            require_success(response.status);
            const auto release = detail::read_release(response.body, currentVersion);
            if (!newer_version(release.tag, currentVersion)) { publish(State::Current, "Engine and launcher are up to date."); return; }
            check_cancel(&cancelled);
            ownedStage = make_stage(installRoot);
            const auto archive = ownedStage / "release.zip";
            std::ofstream file(archive, std::ios::binary | std::ios::out);
            if (!file) throw std::runtime_error("Update archive could not be saved.");
            Transfer download; download.cancelled = &cancelled; download.file = &file;
            download.expected = release.size; download.progressContext = this; download.progress = &Impl::progress;
            {
                std::lock_guard lock(mutex);
                status.state = State::Downloading; status.downloaded = 0; status.total = release.size;
                status.message = "Downloading engine and launcher update...";
            }
            require_success(request(asset_url(release.assetId), token, download).status);
            file.close();
            if (!file || download.received != release.size) throw std::runtime_error("Update download is incomplete.");
            wipe(token.value);
            publish(State::Downloading, "Verifying update integrity and package...");
            detail::verify_digest(archive, release.digest, &cancelled);
            detail::extract_archive(archive, ownedStage / "payload", &cancelled);
            fs::remove(archive);
            check_cancel(&cancelled);
            { std::lock_guard lock(mutex); ready = {ownedStage, release.tag.substr(1)}; status.state = State::Ready; status.message = "Update verified and ready to install."; }
            return;
        } catch (const Cancelled&) { publish(State::Cancelled, "Update cancelled."); }
        catch (const Failure& error) { publish(error.state, error.message); }
        catch (const fs::filesystem_error&) { publish(State::Failed, "Update staging failed. Check install permissions and free disk space."); }
        catch (const std::runtime_error&) { publish(State::Failed, "Update metadata, integrity or package validation failed. No installed files were changed."); }
        catch (...) { publish(State::Failed, "Update could not be prepared. Current game remains available."); }
        if (!ownedStage.empty()) { std::error_code ec; fs::remove_all(ownedStage, ec); ownedStage.clear(); }
    }
};

Job::Job(const fs::path& installRoot, std::string currentVersion) : impl_(std::make_unique<Impl>(installRoot, std::move(currentVersion))) {}
Job::~Job() = default;
Snapshot Job::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
Prepared Job::prepared() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->status.state != State::Ready) throw std::logic_error("Update is not ready.");
    return impl_->ready;
}
void Job::release_stage() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->status.state != State::Ready) throw std::logic_error("Update is not ready.");
    impl_->transferred = true;
}
} // namespace yami::updates
