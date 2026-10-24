#include "parallax.hpp"
#include <algorithm>
#include <cmath>
#include <future>
#include <stdexcept>

namespace yami {
namespace {
std::vector<std::uint8_t> downsample(std::span<const std::uint8_t> source,
                                     unsigned width, unsigned height, unsigned& out_width, unsigned& out_height) {
    out_width = std::max(1u, width/2); out_height = std::max(1u, height/2);
    std::vector<std::uint8_t> result(std::size_t(out_width)*out_height*4);
    for (unsigned y = 0; y < out_height; ++y) for (unsigned x = 0; x < out_width; ++x) for (unsigned c = 0; c < 4; ++c) {
        unsigned sum = 0;
        for (unsigned k = 0; k < 4; ++k) {
            const auto sx = std::min(width-1, x*2+k%2), sy = std::min(height-1, y*2+k/2);
            sum += source[(std::size_t(sy)*width+sx)*4+c];
        }
        result[(std::size_t(y)*out_width+x)*4+c] = std::uint8_t((sum+2)/4);
    }
    return result;
}
}

ReliefImage derive_relief(std::span<const std::uint8_t> rgba, unsigned width, unsigned height) {
    if (!width || !height || rgba.size() != std::size_t(width)*height*4)
        throw std::invalid_argument("Parallax source dimensions do not match RGBA bytes");
    std::vector<std::uint8_t> source(rgba.begin(), rgba.end());
    while (std::max(width, height) > 512) {
        unsigned next_width, next_height;
        source = downsample(source, width, height, next_width, next_height);
        width = next_width; height = next_height;
    }
    if (std::min(width, height) < 64) return {};
    const std::size_t count = std::size_t(width)*height;
    std::vector<float> detail(count), broad;
    for (std::size_t i = 0; i < count; ++i)
        detail[i] = (.2126f*source[i*4]+.7152f*source[i*4+1]+.0722f*source[i*4+2])/255;
    const auto blur = [&](std::vector<float>& values, int radius) {
        std::vector<float> temporary(count);
        for (int pass = 0; pass < 2; ++pass) for (int axis = 0; axis < 2; ++axis) {
            const int length = axis ? int(height) : int(width), lines = axis ? int(width) : int(height);
            const std::size_t stride = axis ? width : 1, step = axis ? 1 : width;
            for (int line = 0; line < lines; ++line) {
                float* p = values.data()+line*step;
                float sum = 0;
                for (int k = -radius; k <= radius; ++k) sum += p[std::size_t((k+length)%length)*stride];
                for (int k = 0, add = radius+1, sub = length-radius; k < length; ++k) {
                    temporary[k] = sum/float(2*radius+1);
                    if (add >= length) add -= length;
                    if (sub >= length) sub -= length;
                    sum += p[std::size_t(add++)*stride]-p[std::size_t(sub++)*stride];
                }
                for (int k = 0; k < length; ++k) p[k*stride] = temporary[k];
            }
        }
    };
    broad = detail;
    if (count >= 65536) {
        auto broad_work = std::async(std::launch::async,[&] { blur(broad,std::max(2,int(std::min(width,height)/32))); });
        blur(detail,1);
        broad_work.get();
    } else {
        blur(broad,std::max(2,int(std::min(width,height)/32)));
        blur(detail,1);
    }
    double variance = 0;
    for (std::size_t i = 0; i < count; ++i) {
        detail[i] -= broad[i];
        variance += double(detail[i])*detail[i];
    }
    const float sigma = float(std::sqrt(variance/double(count)));
    if (sigma < .02f) return {};
    for (auto& value : detail) value = std::clamp(.5f+value/(4*std::max(sigma,.1f)),0.f,1.f);
    ReliefImage result{width,height,std::vector<std::uint8_t>(count*4)};
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto at = [&](unsigned xx, unsigned yy) { return detail[std::size_t(yy%height)*width+xx%width]; };
        const auto offset = (std::size_t(y)*width+x)*4;
        result.rgba[offset] = std::uint8_t(std::lround(at(x,y)*255));
        result.rgba[offset+1] = std::uint8_t(std::lround(std::clamp(.5f+at(x+1,y)-at(x+width-1,y),0.f,1.f)*255));
        result.rgba[offset+2] = std::uint8_t(std::lround(std::clamp(.5f+at(x,y+1)-at(x,y+height-1),0.f,1.f)*255));
        result.rgba[offset+3] = 255;
    }
    return result;
}
}
