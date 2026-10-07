#pragma once

#include "menu.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace yami::setup {
// Original artwork is loaded only from the user's extracted ISO. All drawing
// coordinates are top-left pixels on the installer's 1024x768 logical canvas.
class Artwork {
public:
    static std::span<const std::string_view> files() noexcept;
    Artwork(SDL_Renderer*, const std::filesystem::path& game);
    void background(); // Paper content panel: {350,24,646,716}.
    void button(SDL_FRect, std::string_view label, bool enabled, bool focused,
                bool primary = false);
    void text(float x, float y, std::string_view, SDL_Color,
              float pixelHeight = 20, float maxWidth = 0);
    float width(std::string_view, float pixelHeight = 20) const;
private:
    struct DestroyTexture {
        void operator()(SDL_Texture* texture) const noexcept { SDL_DestroyTexture(texture); }
    };
    using Texture = std::unique_ptr<SDL_Texture, DestroyTexture>;
    SDL_Renderer* renderer_;
    std::array<Texture,4> textures_; // Main menu, settings paper/buttons, robot, font.
    menu::BitmapFont font_;
    std::vector<menu::Draw> glyphs_;
    void crop(std::size_t texture, SDL_FRect source, SDL_FRect destination,
              SDL_Color color = {255,255,255,255});
};
} // namespace yami::setup
