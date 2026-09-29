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

// Etiquette chargee dans l'imprimante : largeur dans le sens de la tete d'impression.
struct PhysicalLabel {
    double width_mm = 40.0;
    double height_mm = 30.0;
    double pixels_per_mm = 8.0;
    bool rotate_counterclockwise = false; // sens du quart de tour quand le modele est dans l'autre sens
    bool flip = false;                    // retournement a 180 degres
};

RasterImage render_template(const TemplateDocument& document);

// Adapte le rendu d'un modele a l'etiquette physique : quart de tour si le modele est dans l'autre
// sens, retournement eventuel et centrage. `media` decrit l'image obtenue pour l'imprimante.
bool fit_to_label(const RasterImage& image, const PhysicalLabel& label, RasterImage& out, MediaSettings& media,
                  std::string& error);
bool write_png(const RasterImage& image, const std::string& path, std::string& error);

} // namespace qrprotec
