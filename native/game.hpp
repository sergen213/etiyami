#pragma once
#include "gameplay.hpp"
#include "scene.hpp"
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace yami {
struct GameInput {
    gameplay::PlayerInput player;
    Vec2 look_motion; // SDL relative units; positive Y downwards, before sensitivity
    Vec2 menu_motion; // Original interface pixels; positive Y upwards
    bool mouse_left = false, mouse_right = false;
    bool escape = false, interact = false, help = false; // Held physical keys
};
struct GameCallbacks {
    void* host = nullptr;
    // Original son blocks the game until its native dialog returns. The token
    // comes from the application's own score handoff, never an original asset.
    void (*ending)(void*, std::string_view scoreToken, std::uint32_t clock) = nullptr;
};
class Game {
public:
    Game(Renderer&, SceneCache&, menu::Menu&, AudioSystem&,
         std::filesystem::path saveDirectory, GameCallbacks);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    void initialize(bool skipIntro = false);
    void load_level(std::int32_t level, bool initializeFromXml = true);
    void frame(const GameInput&, std::uint32_t now);
    void render(std::uint32_t now);
    void skip_video();
    void persist_settings();
    void set_focused(bool);
    bool quit_requested() const noexcept;
    const gameplay::FrameClock& clock() const noexcept;
    std::span<const gameplay::EntityState> entities() const noexcept;
    Vec3 player_position() const noexcept;
    menu::Context menu_context() const;
    const std::filesystem::path& save_directory() const noexcept;
private:
    Renderer& renderer_;
    SceneCache& scene_;
    menu::Menu& menu_;
    AudioSystem& audio_;
    std::filesystem::path saveDirectory_;
    GameCallbacks callbacks_;
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace yami
