#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace yami {
struct ReliefImage {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
// IKV-compatible enhanced-only relief: Rec.709 high-pass, normalized per texture.
// Empty output means flat artwork or a source smaller than 64 texels on either axis.
ReliefImage derive_relief(std::span<const std::uint8_t> rgba, unsigned width, unsigned height);
}
