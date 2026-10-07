#pragma once

#include <filesystem>
#include <string_view>

namespace yami {
class Renderer;
class SceneCache;
namespace menu { class Menu; }

// Reuses the native window and original settings UI without starting game resources.
// true starts gameplay; false closes successfully. Persistence errors propagate.
bool run_launcher(Renderer&, SceneCache&, menu::Menu&,
                  const std::filesystem::path& saveDirectory, bool checkUpdates = true,
                  std::string_view recoveryMessage = std::string_view());
} // namespace yami
