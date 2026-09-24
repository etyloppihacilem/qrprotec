#pragma once

#include "../core/template.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qrprotec {

struct RasterImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    std::uint8_t& at(int x, int y) { return pixels[static_cast<std::size_t>(y * width + x)]; }
    const std::uint8_t& at(int x, int y) const { return pixels[static_cast<std::size_t>(y * width + x)]; }
};

RasterImage render_template(const TemplateDocument& document);
bool write_png(const RasterImage& image, const std::string& path, std::string& error);

} // namespace qrprotec
