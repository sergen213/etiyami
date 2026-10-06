#include "update_install.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace fs = std::filesystem;
struct Options {
    fs::path stage, root, assets, saves;
    std::uint64_t parent = 0;
};
fs::path path(std::string_view value) {
    if (value.empty()) throw std::runtime_error("Empty updater path argument");
    return fs::absolute(fs::path(std::u8string(value.begin(), value.end()))).lexically_normal();
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (++i >= argc) throw std::runtime_error("Missing updater argument value");
        const std::string_view value = argv[i];
        const auto set = [&](fs::path& destination) {
            if (!destination.empty()) throw std::runtime_error("Duplicate updater path argument");
            destination = path(value);
        };
        if (name == "--stage") set(result.stage);
        else if (name == "--install-root") set(result.root);
        else if (name == "--asset-root") set(result.assets);
        else if (name == "--save-dir") set(result.saves);
        else if (name == "--parent-pid") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.parent);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result.parent)
                throw std::runtime_error("Invalid updater parent process ID");
        } else throw std::runtime_error("Unknown updater argument");
    }
    if (result.stage.empty() || result.root.empty() || result.assets.empty() || result.saves.empty() || !result.parent)
        throw std::runtime_error("Updater requires stage, installation, asset/save paths, and parent process ID");
    return result;
}
}

int main(int argc, char** argv) {
    bool committed = false;
    fs::path logStage;
    try {
        const auto opts = options(argc, argv);
        if (!fs::equivalent(yami::updates::installation_root(), opts.stage/"helper"))
            throw std::runtime_error("Updater must run from the detached staged helper directory");
        logStage = opts.stage;
        yami::updates::detail::append_installer_log(logStage, "Trusted installed helper started; waiting for launcher exit.");
        yami::updates::detail::wait_for_parent_exit(opts.parent);
        yami::updates::install_prepared(opts.stage, opts.root);
        committed = true;
        yami::updates::detail::restart_launcher(opts.root, opts.assets, opts.saves);
        yami::updates::detail::mark_completed(opts.stage);
        constexpr auto success = "Update committed; launcher restarted with the original asset and save directories.";
        yami::updates::detail::append_installer_log(logStage, success);
        std::cout << success << '\n' << std::flush;
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        const std::string message = std::string(committed ?
            "Update committed, but launcher restart or stage finalization failed: " : "Update installation failed: ") +
            error.what() + "\nThe staged updater.log is retained for diagnosis.";
        std::cerr << message << '\n' << std::flush;
        if (!logStage.empty()) yami::updates::detail::append_installer_log(logStage, message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ETI Yami update", message.c_str(), nullptr);
        SDL_Quit();
        return 1;
    }
}
