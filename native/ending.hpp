#pragma once

#include "menu.hpp"
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yami::ending {
// son.eti 0040159d: one MSVCR71 rand(), seeded by the host's millisecond clock.
std::uint16_t original_random(std::uint32_t tickMilliseconds) noexcept;
bool valid_phone(std::string_view) noexcept;
// Safe original format: a decimal score in 1000..99999, preserving its token.
// Outside this domain the PE indexes beyond its permutation, not a larger code.
std::string read_score(std::istream&);
std::string generate_code(std::string_view score, std::string_view phone,
                          std::uint16_t random);

enum class Control { Accept, Close, Phone, Score, Message };
enum class Key { None, Tab, Enter, Escape, Backspace, Delete, Left, Right,
                 Home, End, SelectAll, Copy };
struct Input {
    Vec2 pointer; // bottom-up original 1024x768 pixels, same letterbox as menus
    bool primaryPressed = false, primaryDown = false, primaryReleased = false;
    bool shift = false;
    Key key = Key::None;
    std::string_view text; // SDL text-input or explicitly user-requested paste
};
enum class CommandKind {
    // Parent removes ONLY its self-created native handoff in the user save
    // directory. Never remove the immutable original game/data/save/save43.eti.
    ConsumeNativeScoreHandoff,
    ReturnToHost,
    CopySelection // explicit local user clipboard action; never log this payload
};
struct Command { CommandKind kind; std::string text; };

class Ending {
public:
    Ending(std::string_view scoreToken, std::uint32_t tickMilliseconds);
    // Missing file: original exits before opening a form. Parent handles that
    // before constructing this state; read_score throws for missing/bad tokens.
    Ending(std::istream& nativeScoreHandoff, std::uint32_t tickMilliseconds);
    std::string_view score() const noexcept { return score_; }
    std::string_view phone() const noexcept { return phone_; }
    std::string_view message() const noexcept { return message_; }
    std::uint16_t random() const noexcept { return random_; }
    unsigned attempts() const noexcept { return attempts_; }
    bool accept_enabled() const noexcept { return attempts_ < 3 && !closed_; }
    bool closed() const noexcept { return closed_; }
    Control focus() const noexcept { return focus_; }
    void set_phone(std::string_view); // local text only, no persistence/network
    void submit(); // 00401a00: invalid input does NOT consume an attempt
    void close(); // 004011b0 / ID_CANCEL: ends helper, returns blocking host call
    void update(const Input&, const menu::BitmapFont&);
    std::span<const menu::Draw> draw(const menu::BitmapFont&);
    std::span<const Command> commands() const noexcept { return commands_; }
    void clear_commands() noexcept { commands_.clear(); }
    static menu::Rect control_rect(Control) noexcept;
    static constexpr std::string_view title = "SifreVer";
    static constexpr std::string_view historical_submission_url =
        "www.etietieti.com/yami-mekanikistila";
private:
    std::string score_, phone_, message_;
    std::uint16_t random_ = 0;
    unsigned attempts_ = 0;
    bool closed_ = false, dragging_ = false, buttonPressed_ = false, closeBoxPressed_ = false;
    Control focus_ = Control::Accept, pressedControl_ = Control::Accept;
    std::size_t caret_ = 0, anchor_ = 0, firstVisible_ = 0;
    std::vector<Command> commands_;
    std::vector<menu::Draw> draws_;
    std::string_view focused_text() const noexcept;
    void focus_control(Control);
    void erase_selection();
    void move_caret(std::size_t, bool extend);
    void text(menu::Rect, std::string_view, const menu::BitmapFont&,
              std::array<float, 4>, bool centered = false);
    void rectangle(menu::Rect, std::array<float, 4>);
};
} // namespace yami::ending
