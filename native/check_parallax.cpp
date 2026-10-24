#include "parallax.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace yami;

int main() {
    constexpr unsigned width = 64, height = 64;
    std::vector<std::uint8_t> tiled(width*height*4);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const bool grout = x%8 == 0 || y%8 == 0;
        const std::uint8_t value = grout ? 32 : 208;
        const auto at = (std::size_t(y)*width+x)*4;
        tiled[at] = tiled[at+1] = tiled[at+2] = value;
        tiled[at+3] = 255;
    }
    const auto original = tiled;
    const auto relief = derive_relief(tiled, width, height);
    assert(tiled == original);
    assert(relief.rgba.size() == original.size());
    const auto at = [&](unsigned x, unsigned y, unsigned channel) {
        return relief.rgba[(std::size_t(y)*width+x)*4+channel];
    };
    assert(at(1, 1, 0) < at(4, 4, 0)); // Dark grout is deeper than the tile face.
    assert(at(4, 4, 3) == 255);

    std::vector<std::uint8_t> flat(width*height*4, 128);
    assert(derive_relief(flat, width, height).rgba.empty());
    assert(derive_relief(tiled, 32, 128).rgba.empty());

    std::vector<std::uint8_t> large(1024*1024*4);
    for (unsigned y = 0; y < 1024; ++y) for (unsigned x = 0; x < 1024; ++x) {
        const auto at = (std::size_t(y)*1024+x)*4;
        large[at] = large[at+1] = large[at+2] = (x%16 == 0 || y%16 == 0) ? 24 : 220;
        large[at+3] = 255;
    }
    const auto scaled = derive_relief(large, 1024, 1024);
    assert(scaled.width == 512 && scaled.height == 512);
    assert(scaled.rgba.size() == 512*512*4);
    std::cout << "parallax relief: dark grout, flat rejection, size limit, source preservation PASS\n";
}
