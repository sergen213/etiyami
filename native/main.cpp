#include "game.hpp"
#include "ending.hpp"
#include "launcher.hpp"
#include "update.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
using namespace yami;
namespace fs = std::filesystem;
fs::path utf8_path(std::string_view value) {
#ifdef _WIN32
    return fs::path(std::u8string(value.begin(),value.end()));
#else
    return fs::path(value);
#endif
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
void sdl_check(bool success) {
    if (!success) throw std::runtime_error(SDL_GetError());
}
using SdlText = std::unique_ptr<char, decltype(&SDL_free)>;
struct SdlLifetime {
    SdlLifetime() {
#if defined(__linux__)
        if (std::getenv("WAYLAND_DISPLAY"))
            SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "wayland,x11", SDL_HINT_DEFAULT);
#endif
        sdl_check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    }
    ~SdlLifetime() { SDL_Quit(); }
};
enum GraphicsOption { Msaa, Filtering, Occlusion, Reflections, Bloom, Sharpen,
                      EnhancedLighting, Fullscreen, GraphicsOptionCount };
struct Options {
    DisplayOptions display;
    fs::path assets, saves, capture;
    int level = 0;
    bool skipIntro = false, smoke = false, help = false;
#ifdef YAMI_LAUNCHER_DEFAULT
    bool launcher = true;
#else
    bool launcher = false;
#endif
    bool explicitAssets = false;
    bool checkUpdates = true, printVersion = false;
    std::array<bool,GraphicsOptionCount> graphicsOverrides{};
};
template<class T> T number(std::string_view value, std::string_view option) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data()+value.size(),
            std::string("Invalid value for ")+std::string(option));
    return result;
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto split = argument.find('=');
        const auto name = argument.substr(0, split);
        auto value = [&]() -> std::string_view {
            if (split != std::string_view::npos) return argument.substr(split+1);
            require(i+1 < argc, std::string("Missing value for ")+std::string(name));
            return argv[++i];
        };
        if (name == "--asset-root") { result.assets = utf8_path(value()); result.explicitAssets = true; }
        else if (name == "--save-dir") result.saves = utf8_path(value());
        else if (name == "--capture") result.capture = utf8_path(value());
        else if (name == "--width") result.display.width = number<int>(value(), name);
        else if (name == "--height") result.display.height = number<int>(value(), name);
        else if (name == "--samples") { result.display.samples = number<int>(value(), name); result.graphicsOverrides[Msaa] = true; }
        else if (name == "--anisotropy") { result.display.anisotropy = number<float>(value(), name); result.graphicsOverrides[Filtering] = true; }
        else if (name == "--ao") { result.display.ambient_occlusion = number<float>(value(), name); result.graphicsOverrides[Occlusion] = true; }
        else if (name == "--reflections") { result.display.reflections = number<float>(value(), name); result.graphicsOverrides[Reflections] = true; }
        else if (name == "--bloom") { result.display.bloom = number<float>(value(), name); result.graphicsOverrides[Bloom] = true; }
        else if (name == "--sharpen") { result.display.sharpen = number<float>(value(), name); result.graphicsOverrides[Sharpen] = true; }
        else if (name == "--level") {
            result.level = number<int>(value(), name);
            require(result.level >= 1 && result.level <= 4, "Level must be in 1..4");
        }
        else {
            require(split == std::string_view::npos, "Boolean options do not take a value");
            if (name == "--fullscreen") { result.display.fullscreen = true; result.graphicsOverrides[Fullscreen] = true; }
            else if (name == "--gles") result.display.gles = true;
            else if (name == "--classic-graphics") { result.display.enhanced = false; result.graphicsOverrides[EnhancedLighting] = true; }
            else if (name == "--skip-intro") result.skipIntro = true;
            else if (name == "--launcher") result.launcher = true;
            else if (name == "--no-launcher") result.launcher = false;
            else if (name == "--no-updates") result.checkUpdates = false;
            else if (name == "--version") result.printVersion = true;
            else if (name == "--smoke") result.smoke = true;
            else if (name == "--help" || name == "-h") result.help = true;
            else throw std::runtime_error(std::string("Unknown option: ")+std::string(name));
        }
    }
    require(result.display.width > 0 && result.display.height > 0, "Window dimensions must be positive");
    require(result.display.samples >= 0, "MSAA samples must be nonnegative");
    require(std::isfinite(result.display.anisotropy) && result.display.anisotropy >= 1,
            "Anisotropy must be finite and at least 1");
    for (const auto strength : {result.display.ambient_occlusion, result.display.reflections,
                               result.display.bloom, result.display.sharpen})
        require(std::isfinite(strength) && strength >= 0 && strength <= 1,
                "Graphics effect strengths must be finite and in 0..1");
    require(result.level >= 0 && result.level <= 4, "Level must be in 1..4");
    if (result.smoke) result.launcher = false;
    return result;
}
GraphicsSettings merge_graphics(const Options& options, GraphicsSettings saved) {
    const auto& flags = options.graphicsOverrides;
    const auto& requested = options.display;
    if (flags[Msaa]) saved.samples = requested.samples;
    if (flags[Filtering]) saved.anisotropy = requested.anisotropy;
    if (flags[Occlusion]) saved.ambient_occlusion = requested.ambient_occlusion;
    if (flags[Reflections]) saved.reflections = requested.reflections;
    if (flags[Bloom]) saved.bloom = requested.bloom;
    if (flags[Sharpen]) saved.sharpen = requested.sharpen;
    if (flags[EnhancedLighting]) saved.enhanced = requested.enhanced;
    if (flags[Fullscreen]) saved.fullscreen = requested.fullscreen;
    return saved;
}

fs::path asset_root(const Options& opts) {
    const auto valid = [](const fs::path& root) {
        std::error_code error;
        return fs::is_regular_file(root/"data/menu/menulist.xml", error);
    };
    if (opts.explicitAssets) {
        require(!opts.assets.empty() && valid(opts.assets),
                "Invalid --asset-root: expected data/menu/menulist.xml");
        return fs::canonical(opts.assets);
    }
    const char* baseText = SDL_GetBasePath();
    require(baseText != nullptr, SDL_GetError());
    const auto base = utf8_path(baseText);
    for (const auto& root : {base/"game", base/".."/"game"})
        if (valid(root)) return fs::canonical(root);
    const auto local = fs::current_path()/"game";
    if (valid(local)) return fs::canonical(local);
    throw std::runtime_error("Cannot find original game assets; use --asset-root PATH");
}

void require_native_write(const fs::path& path, const fs::path& assets) {
    auto ancestor = fs::weakly_canonical(path);
    while (!ancestor.empty()) {
        if (fs::exists(ancestor))
            require(!fs::equivalent(ancestor, assets), "Native output must be outside the immutable asset root");
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
}

// A smoke run gets a newly-created directory, never somebody's live checkpoints.
struct SaveDirectory {
    fs::path path;
    bool temporary = false;
    SaveDirectory(const Options& opts, const fs::path& assets) {
        if (!opts.saves.empty()) path = fs::absolute(opts.saves);
        else if (opts.smoke) {
            const auto base = fs::temp_directory_path();
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                path = base/("yami-native-smoke-"+std::to_string(SDL_GetTicksNS())+"-"+std::to_string(attempt));
                if (fs::create_directory(path)) { temporary = true; break; }
            }
            require(temporary, "Cannot create isolated smoke save directory");
        } else {
            SdlText pref(SDL_GetPrefPath("ETI", "YamiNative"), SDL_free);
            require(bool(pref), SDL_GetError());
            path = utf8_path(pref.get());
        }
        require_native_write(path, assets);
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~SaveDirectory() {
        if (temporary) { std::error_code ignored; fs::remove_all(path, ignored); }
    }
};

struct Platform {
    Renderer& renderer;
    SceneCache& scene;
    menu::Menu& menu;
    AudioSystem& audio;
    Game* game = nullptr;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    GameInput input;
    bool quit = false, focused = false, dialog = false, captured = false;
    Vec2 pointer{};

    SDL_Window* window() const { return renderer.window(); }
    void clear_held() { keys.fill(false); input = {}; }
    Vec2 menu_delta(Vec2 logical) const {
        int width = 0, height = 0;
        sdl_check(SDL_GetWindowSize(window(), &width, &height));
        require(width > 0 && height > 0, "Invalid logical window size");
        const auto viewport = fit_original_interface(renderer.pixel_width(), renderer.pixel_height());
        return {logical.x*renderer.pixel_width()/width*1024/viewport.width,
                -logical.y*renderer.pixel_height()/height*768/viewport.height};
    }
    Vec2 menu_point(Vec2 logical) const {
        int width = 0, height = 0;
        sdl_check(SDL_GetWindowSize(window(), &width, &height));
        require(width > 0 && height > 0, "Invalid logical window size");
        const auto viewport = fit_original_interface(renderer.pixel_width(), renderer.pixel_height());
        return {(logical.x*renderer.pixel_width()/width-viewport.x)*1024/viewport.width,
                (renderer.pixel_height()-logical.y*renderer.pixel_height()/height-viewport.y)*768/viewport.height};
    }
    void capture() {
        const bool wanted = focused && !dialog && !quit;
        if (wanted == captured) return;
        sdl_check(SDL_SetWindowRelativeMouseMode(window(), wanted));
        sdl_check(wanted ? SDL_HideCursor() : SDL_ShowCursor());
        captured = wanted;
    }
    void focus(bool value) {
        focused = value;
        if (!value) clear_held();
        if (game) game->set_focused(value && !dialog);
        capture();
    }
    void consume(const SDL_Event& event) {
        const SDL_WindowID id = SDL_GetWindowID(window());
        if (event.type == SDL_EVENT_QUIT) { quit = true; clear_held(); capture(); return; }
        if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
            if (event.window.windowID != id) return;
            switch (event.type) {
            case SDL_EVENT_WINDOW_FOCUS_GAINED: focus(true); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: focus(false); break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit = true; clear_held(); capture(); break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: renderer.resize(); break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                // Wayland completes fullscreen requests asynchronously.
                menu.set_fullscreen(bool(SDL_GetWindowFlags(window()) & SDL_WINDOW_FULLSCREEN));
                renderer.resize();
                break;
            default: break;
            }
        } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            if (event.key.windowID != id || !focused) return;
            const auto code = event.key.scancode;
            if (code >= 0 && code < SDL_SCANCODE_COUNT) keys[static_cast<std::size_t>(code)] = event.key.down;
            if (event.key.down && !event.key.repeat && code == SDL_SCANCODE_F11) {
                const bool fullscreen = !menu.settings().graphics.fullscreen;
                sdl_check(SDL_SetWindowFullscreen(window(), fullscreen));
                menu.set_fullscreen(fullscreen);
                renderer.resize();
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (event.motion.windowID != id || !focused) return;
            pointer = menu_point({event.motion.x, event.motion.y});
            if (!dialog) {
                input.look_motion.x += event.motion.xrel;
                input.look_motion.y += event.motion.yrel;
                const auto motion = menu_delta({event.motion.xrel, event.motion.yrel});
                input.menu_motion.x += motion.x;
                input.menu_motion.y += motion.y;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (event.button.windowID != id || !focused) return;
            pointer = menu_point({event.button.x, event.button.y});
            if (event.button.button == SDL_BUTTON_LEFT) input.mouse_left = event.button.down;
            if (event.button.button == SDL_BUTTON_RIGHT) input.mouse_right = event.button.down;
        }
    }
    void events(bool wait = false) {
        input.look_motion = {}; input.menu_motion = {};
        SDL_Event event;
        if (wait && SDL_WaitEventTimeout(&event, 16)) consume(event);
        while (SDL_PollEvent(&event)) consume(event);
    }
    const GameInput& game_input() {
        auto held = [&](SDL_Scancode code) { return keys[static_cast<std::size_t>(code)]; };
        auto& player = input.player;
        player.forward = held(SDL_SCANCODE_W); player.back = held(SDL_SCANCODE_S);
        player.left = held(SDL_SCANCODE_A); player.right = held(SDL_SCANCODE_D);
        player.alternateForward = held(SDL_SCANCODE_UP); player.alternateBack = held(SDL_SCANCODE_DOWN);
        player.alternateLeft = held(SDL_SCANCODE_LEFT); player.alternateRight = held(SDL_SCANCODE_RIGHT);
        player.fire = held(SDL_SCANCODE_LCTRL) || held(SDL_SCANCODE_RCTRL) || input.mouse_left;
        player.secondaryFire = false; player.special = held(SDL_SCANCODE_SPACE);
        input.escape = held(SDL_SCANCODE_ESCAPE);
        input.interact = held(SDL_SCANCODE_RETURN) || held(SDL_SCANCODE_E);
        input.help = held(SDL_SCANCODE_TAB);
        return input;
    }
    void draw(std::uint32_t now) {
        renderer.clear();
        game->frame(game_input(), now);
        if (!quit && focused) game->render(now);
    }
    static void ending_callback(void* host, std::string_view token, std::uint32_t clock) {
        static_cast<Platform*>(host)->native_ending(token, clock);
    }
    void native_ending(std::string_view token, std::uint32_t clock) {
        const auto handoff = game->save_directory()/"save43.eti";
        require(fs::symlink_status(handoff).type() == fs::file_type::regular,
                "Native ending score handoff is missing or not a regular file");
        std::ifstream stream(handoff);
        require(bool(stream), "Cannot open native ending score handoff");
        const auto score = ending::read_score(stream);
        require(score == token, "Native ending score handoff does not belong to this invocation");
        stream.close();
        ending::Ending ending(score, clock);
        require(!menu.fonts().empty(), "Native ending requires the original bitmap font");
        const auto& font = menu.fonts().front();
        const std::string oldTitle = SDL_GetWindowTitle(window());
        std::vector<menu::Draw> historicalNotice;
        menu::append_text(historicalNotice, font,
            "Historical 2006 promotion. No network connection; telephone stays local.",
            {24,24}, 0.75f, {{1,1,1,1}});
        {
        dialog = true;
        clear_held();
        game->set_focused(false);
        capture();
        sdl_check(SDL_SetWindowTitle(window(), std::string(ending::Ending::title).c_str()));
        sdl_check(SDL_StartTextInput(window()));
        struct Restore {
            Platform& host;
            const std::string& title;
            ~Restore() {
                SDL_StopTextInput(host.window());
                SDL_SetWindowTitle(host.window(), title.c_str());
                host.dialog = false;
                host.clear_held();
                host.focused = bool(SDL_GetWindowFlags(host.window()) & SDL_WINDOW_INPUT_FOCUS);
                host.game->set_focused(host.focused && !host.quit);
                // The outer scope reacquires relative input after restoring focus.
            }
        } restore{*this, oldTitle};
        auto commands = [&]() {
            for (const auto& command : ending.commands()) {
                if (command.kind == ending::CommandKind::ConsumeNativeScoreHandoff) {
                    require(fs::remove(handoff), "Cannot consume native ending score handoff");
                } else if (command.kind == ending::CommandKind::CopySelection) {
                    sdl_check(SDL_SetClipboardText(command.text.c_str()));
                }
                // ReturnToHost is represented by Ending::closed(), not app quit.
            }
            ending.clear_commands();
        };
        commands();
        float mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);
        pointer = menu_point({mouseX, mouseY});
        while (!quit && !ending.closed()) {
            SDL_Event event;
            auto event_input = [&](const SDL_Event& current) {
                consume(current);
                if (quit) { ending.close(); commands(); return; }
                ending::Input edit;
                edit.pointer = pointer;
                edit.primaryDown = input.mouse_left;
                edit.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                std::string pasted;
                const auto id = SDL_GetWindowID(window());
                if (focused && current.type == SDL_EVENT_MOUSE_BUTTON_DOWN && current.button.windowID == id)
                    edit.primaryPressed = current.button.button == SDL_BUTTON_LEFT;
                if (focused && current.type == SDL_EVENT_MOUSE_BUTTON_UP && current.button.windowID == id)
                    edit.primaryReleased = current.button.button == SDL_BUTTON_LEFT;
                if (focused && current.type == SDL_EVENT_TEXT_INPUT && current.text.windowID == id)
                    edit.text = current.text.text;
                if (focused && current.type == SDL_EVENT_KEY_DOWN && current.key.windowID == id) {
                    const bool ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
                    switch (current.key.scancode) {
                    case SDL_SCANCODE_TAB: edit.key = ending::Key::Tab; break;
                    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: edit.key = ending::Key::Enter; break;
                    case SDL_SCANCODE_ESCAPE: edit.key = ending::Key::Escape; break;
                    case SDL_SCANCODE_BACKSPACE: edit.key = ending::Key::Backspace; break;
                    case SDL_SCANCODE_DELETE: edit.key = ending::Key::Delete; break;
                    case SDL_SCANCODE_LEFT: edit.key = ending::Key::Left; break;
                    case SDL_SCANCODE_RIGHT: edit.key = ending::Key::Right; break;
                    case SDL_SCANCODE_HOME: edit.key = ending::Key::Home; break;
                    case SDL_SCANCODE_END: edit.key = ending::Key::End; break;
                    case SDL_SCANCODE_A: if (ctrl) edit.key = ending::Key::SelectAll; break;
                    case SDL_SCANCODE_C: if (ctrl && !current.key.repeat) edit.key = ending::Key::Copy; break;
                    case SDL_SCANCODE_V:
                        if (ctrl && !current.key.repeat) {
                            SdlText text(SDL_GetClipboardText(), SDL_free);
                            require(bool(text), SDL_GetError());
                            pasted = text.get(); edit.text = pasted;
                        }
                        break;
                    default: break;
                    }
                }
                if (focused) ending.update(edit, font);
                commands();
            };
            if (!focused && SDL_WaitEventTimeout(&event, 16)) event_input(event);
            while (!ending.closed() && SDL_PollEvent(&event)) event_input(event);
            audio.update();
            if (focused && !quit && !ending.closed()) {
                ending::Input held;
                held.pointer = pointer; held.primaryDown = input.mouse_left;
                held.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                ending.update(held, font); commands();
                renderer.clear();
                renderer.camera(identity_matrix(), interface_projection(), true);
                scene.draw_ui(ending.draw(font), static_cast<std::uint32_t>(SDL_GetTicks()));
                scene.draw_ui(historicalNotice, static_cast<std::uint32_t>(SDL_GetTicks()));
                renderer.present();
                SDL_Delay(1);
            }
        }
        }
        capture();
    }
};

void capture_ppm(Renderer& renderer, const fs::path& destination) {
    const auto pixels = renderer.capture_rgba();
    const int width = renderer.pixel_width(), height = renderer.pixel_height();
    require(pixels.size() == static_cast<std::size_t>(width)*height*4, "Invalid captured surface");
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    require(bool(output), "Cannot open capture destination");
    output << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<char> row(static_cast<std::size_t>(width)*3);
    for (int y = height-1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const auto source = (static_cast<std::size_t>(y)*width+x)*4;
            for (int component = 0; component < 3; ++component)
                row[static_cast<std::size_t>(x)*3+component] = static_cast<char>(pixels[source+component]);
        }
        output.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    require(bool(output), "Cannot write captured surface");
}

void push(SDL_Event event) { sdl_check(SDL_PushEvent(&event)); }
void push_key(Platform& host, SDL_Scancode code, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(host.window());
    event.key.scancode = code; event.key.down = down;
    push(event);
}
void push_button(Platform& host, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = SDL_GetWindowID(host.window());
    event.button.button = SDL_BUTTON_LEFT; event.button.down = down;
    push(event);
}
void push_motion(Platform& host, Vec2 raw) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = SDL_GetWindowID(host.window());
    event.motion.xrel = raw.x; event.motion.yrel = raw.y;
    push(event);
}
void smoke(Platform& host, const Options& opts) {
    using gameplay::Phase;
    auto& game = *host.game;
    const auto focusDeadline = SDL_GetTicks()+5000;
    sdl_check(SDL_RaiseWindow(host.window()));
    while (!host.focused && !host.quit && SDL_GetTicks() < focusDeadline) {
        host.events(true); host.audio.update();
        host.focus(bool(SDL_GetWindowFlags(host.window()) & SDL_WINDOW_INPUT_FOCUS));
    }
    require(host.focused && !host.quit, "Smoke requires an actual focused SDL window");
    std::uint32_t now = static_cast<std::uint32_t>(SDL_GetTicks());
    auto step = [&](std::uint32_t milliseconds = 33, bool present = true) {
        now += milliseconds;
        host.events(); host.capture(); host.audio.update();
        require(host.focused && !host.quit, "Smoke SDL window lost focus or was closed");
        host.draw(now);
        require(!game.quit_requested() && !host.quit, "Game unexpectedly quit during smoke");
        if (present) host.renderer.present();
    };
    // Render actual intro frames before the public skip operation, not a mock intro.
    for (unsigned frame = 0; game.clock().phase == Phase::Intro && frame < 6; ++frame) step(80);
    if (game.clock().phase == Phase::Intro) game.skip_video();
    step();
    require(game.clock().phase == Phase::Menu && host.menu.page() == 1, "Startup did not reach the real main menu");
    require(!host.menu.pages()[1].buttons.empty(), "Original NewGame hit region is missing");
    const auto hit = host.menu.pages()[1].buttons[0].rect;
    const Vec2 target{hit.x+hit.width*0.5f, hit.y+hit.height*0.5f-45};
    const Vec2 beforeCursor = host.menu.cursor();
    // Invert the same high-DPI/letterbox map used by the actual event consumer.
    const auto unit = host.menu_delta({1,1});
    push_motion(host, {(target.x-beforeCursor.x)/unit.x, (target.y-beforeCursor.y)/unit.y});
    step();
    const auto afterCursor = host.menu.cursor();
    require(std::abs(afterCursor.x-target.x) <= 2 && std::abs(afterCursor.y-target.y) <= 2,
            "SDL relative event did not move the original menu cursor to NewGame");
    require(std::abs(afterCursor.x-beforeCursor.x)+std::abs(afterCursor.y-beforeCursor.y) > 1,
            "Smoke relative cursor event did not move the cursor");
    push_button(host, true); step();
    push_button(host, false); step();
    require(game.clock().level == 1 && game.clock().phase != Phase::Menu,
            "Original NewGame hit/release did not initialize the real level");
    std::cout << "smoke menu SDL-relative cursor=" << afterCursor.x << ',' << afterCursor.y
              << " NewGame phase=" << static_cast<unsigned>(game.clock().phase)
              << " level=" << game.clock().level << '\n';
    if (opts.level > 1) game.load_level(opts.level);
    for (unsigned frame = 0; game.clock().phase != Phase::Active && frame < 300; ++frame) {
        step(80);
        // At least six real video frames exercise level4's reached galata_ucus AVI.
        if (frame >= 6 && game.clock().phase == Phase::Intro) game.skip_video();
    }
    if (opts.level == 4) {
        // Restart reaches the same real movie and its unavailable original WAV.
        // Both completion and repeated registration must remain valid.
        game.load_level(4);
        for (unsigned frame = 0; frame < 7; ++frame) step(80);
        if (game.clock().phase == Phase::Intro) game.skip_video();
        step();
    }
    require(game.clock().phase == Phase::Active && !game.clock().paused && game.menu_context().playerExists,
            "Real level did not reach active native gameplay");
    const Vec3 initial = game.player_position();
    auto findPlayer = [&]() -> const gameplay::EntityState& {
        const auto entities = game.entities();
        const auto player = std::find_if(entities.begin(), entities.end(),
            [](const auto& entity) { return entity.role == 0; });
        require(player != entities.end(), "Active Game has no real player entity");
        return *player;
    };
    const auto initialAction = findPlayer().action;
    const auto firstTick = game.clock().nextTick;
    bool moved = false, acted = false;
    const auto observe = [&]() {
        const auto position = game.player_position();
        require(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z),
                "Real physics produced a nonfinite player position");
        const float dx = initial.x-position.x, dz = initial.z-position.z;
        moved = moved || dx*dx+dz*dz > 0.01f;
        const auto action = findPlayer().action;
        acted = acted || (action != initialAction && (action == 4 || action == 6 || action == 7 ||
                          action == 8 || action == 10 || action == 15 || action == 22));
    };
    // Direction groups, held fire, special action and raw camera all take the
    // ordinary SDL event path; try four directions to avoid a spawn-wall false failure.
    constexpr std::array<SDL_Scancode,4> directions{{SDL_SCANCODE_W, SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_A}};
    for (const auto direction : directions) {
        push_key(host, direction, true);
        for (unsigned frame = 0; frame < 16; ++frame) { step(); observe(); }
        push_key(host, direction, false); step(); observe();
    }
    push_key(host, SDL_SCANCODE_UP, true); step(); observe();
    push_key(host, SDL_SCANCODE_UP, false);
    push_motion(host, {24, -12});
    push_key(host, SDL_SCANCODE_SPACE, true); step(); observe();
    push_key(host, SDL_SCANCODE_SPACE, false);
    // Let the original special animation finish before testing normal fire.
    for (unsigned frame = 0; frame < 32; ++frame) { step(); observe(); }
    push_key(host, SDL_SCANCODE_LCTRL, true);
    for (unsigned frame = 0; frame < 16; ++frame) { step(); observe(); }
    push_key(host, SDL_SCANCODE_LCTRL, false);
    push_button(host, true); step(); observe();
    push_button(host, false); step(); observe();
    require(game.clock().nextTick > firstTick+33*60, "Real 33ms physics/AI ticks did not advance");
    require(moved || acted, "Held native input caused no meaningful player position/action transition");
    require(game.clock().phase == Phase::Active, "Smoke input unexpectedly left active gameplay");
    // Original pause/resume excludes menu time from gameplay and animation clocks.
    push_key(host, SDL_SCANCODE_ESCAPE, true); step();
    require(game.clock().paused && host.menu.page()==1, "Escape did not open the original pause menu");
    const auto pausedClock=game.clock().frameNow;
    push_key(host, SDL_SCANCODE_ESCAPE, false); step(120000);
    require(game.clock().frameNow==pausedClock, "Rendering a paused menu advanced the gameplay clock");
    const auto cursor=host.menu.cursor();
    push_motion(host, {(target.x-cursor.x)/unit.x, (target.y-cursor.y)/unit.y}); step();
    push_button(host, true); step();
    push_button(host, false); step();
    require(game.clock().phase==Phase::Active && !game.clock().paused,
            "Original Continue hit/release did not resume the level");
    step();
    require(game.clock().frameNow==pausedClock+33, "Menu wall time leaked into resumed gameplay");
    // Capture before present: GL backbuffer contains only the real Game render.
    step(33, false);
    if (!opts.capture.empty()) capture_ppm(host.renderer, opts.capture);
    host.renderer.present();
    const auto position = game.player_position();
    std::cout << "smoke active phase=" << static_cast<unsigned>(game.clock().phase)
              << " level=" << game.clock().level << " ticks=" << (game.clock().nextTick-firstTick)/33
              << " player=" << position.x << ',' << position.y << ',' << position.z
              << " initial=" << initial.x << ',' << initial.y << ',' << initial.z
              << " moved=" << moved << " actionChanged=" << acted
              << " SDL=" << SDL_GetCurrentVideoDriver()
              << " pixels=" << host.renderer.pixel_width() << 'x' << host.renderer.pixel_height()
              << " samples=" << host.renderer.samples() << " anisotropy=" << host.renderer.anisotropy()
              << " graphics=" << (opts.display.enhanced ? "enhanced" : "classic")
              << " ao=" << opts.display.ambient_occlusion << " reflections=" << opts.display.reflections
              << " bloom=" << opts.display.bloom << " sharpen=" << opts.display.sharpen
              << " api=" << (opts.display.gles ? "GLES" : "OpenGL") << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto opts = options(argc, argv);
        if (opts.printVersion) {
            std::cout << "ETI Yami native " << updates::version() << '\n';
            return 0;
        }
        if (opts.help) {
            std::cout << "Yami native: --asset-root PATH --save-dir PATH --width N --height N\n"
                         "  --launcher (settings before gameplay) --no-launcher (direct gameplay)\n"
                         "  --no-updates (offline launcher) --version\n"
                         "  --fullscreen --samples N --anisotropy N --gles --skip-intro\n"
                         "  --level 1..4 --smoke (bypasses launcher) --capture FILE.ppm --classic-graphics\n"
                         "  --ao 0..1 --reflections 0..1 --bloom 0..1 --sharpen 0..1\n"
#ifdef YAMI_LAUNCHER_DEFAULT
                         "  Default: launcher; assets discovered beside executable or in game/.\n";
#else
                         "  Default: direct gameplay; assets discovered beside executable or in game/.\n";
#endif
            return 0;
        }
        SdlLifetime sdl;
        const auto assets = asset_root(opts);
        if (!opts.capture.empty()) require_native_write(opts.capture, assets);
        SaveDirectory saves(opts, assets);
        menu::Settings settings;
        const auto config = saves.path/"game.ini";
        if (fs::exists(config)) {
            require(fs::symlink_status(config).type() == fs::file_type::regular,
                    "Native game.ini must be a regular file");
            std::ifstream stream(config);
            require(bool(stream), "Cannot read native game.ini");
            settings = menu::read_settings(stream);
        }
        settings.graphics = merge_graphics(opts, settings.graphics);
        static_cast<GraphicsSettings&>(opts.display) = settings.graphics;
        if (opts.launcher) opts.display.fullscreen = false;
        Renderer renderer(opts.display);
        // Wayland maps/focuses a window only after its first buffer commit.
        // Present before the focus-gated loop, otherwise neither can happen.
        renderer.clear();
        renderer.present();
        SceneCache scene(renderer, assets);
        menu::Menu menu(assets);
        menu.set_graphics_limits(renderer.max_samples(), renderer.max_anisotropy());
        settings.graphics.samples = renderer.samples();
        settings.graphics.anisotropy = renderer.anisotropy();
        menu.set_settings(settings);
        if (opts.launcher) {
            if (!run_launcher(renderer, scene, menu, saves.path, opts.checkUpdates)) return 0;
            settings = menu.settings();
            renderer.set_graphics(settings.graphics);
            settings.graphics.samples = renderer.samples();
            settings.graphics.anisotropy = renderer.anisotropy();
            menu.clear_commands();
            menu.set_settings(settings);
            menu.show(1);
            sdl_check(SDL_SetWindowTitle(renderer.window(), "ETI Yami — native"));
        }
        static_cast<GraphicsSettings&>(opts.display) = settings.graphics;
        AudioSystem audio;
        audio.open_device();
        Platform host{renderer, scene, menu, audio};
        Game game(renderer, scene, menu, audio, saves.path, {&host, Platform::ending_callback});
        host.game = &game;
        game.initialize(opts.skipIntro);
        host.focus(bool(SDL_GetWindowFlags(renderer.window()) & SDL_WINDOW_INPUT_FOCUS));
        if (opts.smoke) smoke(host, opts);
        else {
            if (opts.level) game.load_level(opts.level);
            bool captured = false;
            while (!host.quit && !game.quit_requested()) {
                host.events(!host.focused);
                host.capture();
                audio.update();
                if (!host.quit && host.focused) {
                    host.draw(static_cast<std::uint32_t>(SDL_GetTicks()));
                    if (!captured && !opts.capture.empty() && game.clock().phase == gameplay::Phase::Active) {
                        capture_ppm(renderer, opts.capture); captured = true;
                    }
                    if (!host.quit) renderer.present();
                    SDL_Delay(1); // Also bounds CPU use if a compositor ignores swap interval.
                }
            }
        }
        game.persist_settings();
        host.quit = true;
        host.capture();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Yami native: " << error.what() << '\n';
        return 1;
    }
}
