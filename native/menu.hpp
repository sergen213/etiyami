#pragma once

#include "assets.hpp"
#include "graphics.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yami::menu {
struct Rect { float x = 0, y = 0, width = 0, height = 0; };
bool contains(Rect, Vec2) noexcept; // ALL edges inclusive (0041c2f0 FCOM/TEST/JP)

enum class Style { Normal, NoHitCheck, Video, Repeat, Needle };
enum class Alignment { Center, Left, Right };
struct Button {
    Style style = Style::Normal;
    Rect rect; // Original bottom-up 1024x768 pixels, NOT the painted text bounds.
    std::string material, overMaterial, textMaterial, text, font;
    Alignment alignment = Alignment::Center;
    float angle = 0;
    bool over = false, activated = false;
};
struct Label { Vec2 position; std::string material, text, font; };
struct Combo {
    Rect previous, next;
    float textX = 0, labelWidth = 0;
    std::string material, overMaterial, textMaterial, font;
    Alignment alignment = Alignment::Center;
    std::vector<std::string> labels;
    std::size_t selected = 0;
    bool overPrevious = false, overNext = false;
};
struct Page {
    int id = 0;
    Rect rect;
    std::string material;
    std::vector<Button> buttons;
    std::vector<Combo> combos;
    std::vector<Label> labels;
};
struct Glyph {
    bool present = false;
    float width = 1, height = 1;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};
struct BitmapFont {
    std::string name, texture;
    int height = 0, atlasWidth = 0, atlasHeight = 0;
    float spacing = 0;
    std::array<Glyph, 256> glyphs{}; // original signed-char XML codes index bytes
};
std::vector<BitmapFont> read_fonts(const std::filesystem::path& gameRoot);
float text_width(const BitmapFont&, std::string_view utf8);

struct Settings {
    float brightness = 0; // original game.ini value, not display gamma exponent
    float sensitivity = 50, musicVolume = 50, effectVolume = 50; // original 0..100
    bool automaticAim = true;
    GraphicsSettings graphics;
};
Settings read_settings(std::istream&);
void write_settings(std::ostream&, const Settings&);
void save_settings(const std::filesystem::path&, const Settings&); // Shared atomic native-file replacement.
struct SettingsEffect {
    Settings values;
    std::uint16_t brightnessRamp = 127; // 0042e290's original 0..255 parameter
    float mouseSensitivity = 0.5f, musicGain = 0.5f, effectGain = 0.5f;
};

// Parent executes these real engine/audio/VM actions in order. No callback layer.
enum class CommandKind {
    InitializeLevel, ResetLevel, ResumeGame, ShowMainMenu, LoadCheckpoint,
    RestartCheckpoint, RestoreCamera, StopMusic, PlayMusic, SetMusicVolume,
    ResetVideo, ApplySettings, SetAimingMode, RunScript, PersistSettings, Quit
};
struct Command {
    CommandKind kind;
    int level = 0;
    // ResumeGame=004152e0(first,second); InitializeLevel=00415210(level,first,second);
    // ShowMainMenu=00415250(first,level,second); SetAimingMode=00415360(first).
    bool first = false, second = false;
    std::string_view name; // original material/music/script/save name; stable storage
    SettingsEffect settings;
};
struct Context {
    int level = 0, previousLevel = -1;
    bool playerExists = false, playerDead = false;
    float health = 1, energy = 0, loadingProgress = 0;
    // Contiguous readable save files per level; refresh when parent writes a save.
    std::array<std::uint8_t,4> saveCounts{};
};
std::array<std::uint8_t,4> checkpoint_counts(const std::filesystem::path& saveDirectory);
struct Input {
    // Cursor quad bottom-left in original pixels. Hit point is (x, y+45).
    // Parent maps letterboxed pointer coordinates; update() tests BEFORE motion,
    // then advances/clamps the cursor for drawing, as 0041e1a0/00420430 do.
    Vec2 motion; // positive Y upwards; no settings sensitivity applied to menu
    bool leftDown = false;
};
struct Objective { std::string_view text; bool failed = false; };
struct Hud {
    int score = 0, highJumps = 0;
    bool objectivesVisible = false;
    std::span<const Objective> objectives;
};
struct Draw {
    // Quad TL,TR,BR,BL; bottom-up coordinates divided by 1024 and 768.
    // Widescreen HUD X can exceed 1; glyph and sprite sizes stay in original pixels.
    std::array<Vec2, 4> positions, uv;
    Rect rect; // same normalized coordinates, before a needle's rotation
    std::array<float, 4> color{{1,1,1,1}};
    std::string_view material, texture, text, font;
    Style style = Style::Normal;
    int page = 0, button = -1;
    bool visible = true, hovered = false, glyph = false;
};
// Shared original bitmap-text geometry, also usable by the ending dialog.
void append_text(std::vector<Draw>&, const BitmapFont&, std::string_view utf8,
                 Vec2 originalPixelBottomLeft, float scale, std::array<float,4> color);

class Menu {
public:
    explicit Menu(const std::filesystem::path& gameRoot);
    int page() const noexcept { return current_; }
    std::span<const Page> pages() const noexcept { return pages_; }
    std::span<const BitmapFont> fonts() const noexcept { return fonts_; }
    Vec2 cursor() const noexcept { return cursor_; }
    void show(int page); // 0041be80; page 0 is gameplay HUD, 7 is loading
    void set_cursor(Vec2); // 0041be00, including original right-clamp behavior
    void set_button_state(std::size_t button, bool over, bool activated = false);
    void show_notification(std::size_t button); // HUD button 2/4, 45 rendered frames
    void set_game_started(bool value) noexcept;
    void set_settings(const Settings&); // Emits real ApplySettings + menu music volume
    Settings settings() const noexcept;
    void set_graphics_limits(int maxSamples, float maxAnisotropy);
    void set_fullscreen(bool); // F11 already applies SDL/renderer; persist actual state only.
    void set_launcher_backend(GraphicsBackend); // Desired backend only; launcher owns selection.
    void set_graphics_device(bool vulkan, bool rayTracing) noexcept;
    SettingsEffect settings_effect() const noexcept;
    void update(const Input&, const Context&);
    // Reuses capacity; returned views live until subsequent mutation. Material
    // quads and every bitmap glyph are supplied, no separate render backend.
    std::span<const Draw> draw(const Context&, const Hud& = {}, float logicalWidth = 1024);
    std::span<const Command> commands() const noexcept { return commands_; }
    void clear_commands() noexcept { commands_.clear(); }
private:
    std::vector<Page> pages_;
    std::vector<BitmapFont> fonts_;
    std::vector<Draw> draws_;
    std::vector<Command> commands_;
    std::string mouseMaterial_;
    Vec2 cursor_;
    float cursorWidth_ = 0, cursorHeight_ = 0;
    int current_ = 1;
    unsigned saveFrames_ = 0;
    std::array<std::size_t,24> activeCounts_{};
    bool previousLeft_ = false, gameStarted_ = false;
    bool advancedGraphics_ = false;
    bool activeVulkan_ = false, rayTracingAvailable_ = false;
    bool originalSettingsDirty_ = false;
    std::array<char, 32> scoreText_{};
    std::array<char, 64> jumpText_{};
    GraphicsSettings graphics_;
    int maxSamples_ = 0;
    float maxAnisotropy_ = 1;
    std::array<std::array<char,64>,8> graphicsText_{};
    void dispatch(const Context&, bool released);
    void emit(CommandKind, int level = 0, bool first = false, bool second = false,
              std::string_view name = {});
    void video_reset(int page, std::size_t button);
    void quad(Rect, std::string_view material, Style, int button,
              bool over, float left = 0, float right = 1, float inset = 0);
    void text(std::string_view, std::string_view font, std::string_view material,
              int x, int y, float scale, std::array<float,4> color, int button = -1);
};
} // namespace yami::menu
