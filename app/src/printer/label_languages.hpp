#pragma once

#include "../render/raster.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Langages des imprimantes d'etiquettes « de bureau » : TSPL (TSC, IDPRT, Munbyn, Phomemo, Godex en
// emulation...) et ZPL (Zebra). Le poste rend deja l'etiquette en image : on l'envoie d'un bloc
// (BITMAP en TSPL, ^GFA en ZPL), sans police ni code-barres de l'imprimante.
namespace qrprotec::labels {

struct RawJob {
    int width_dots = 0;    // largeur de l'image (sens de la tete)
    int height_dots = 0;   // hauteur de l'image (sens du defilement)
    double dots_per_mm = 8.0;
    int copies = 1;
    int darkness = 8;      // TSPL DENSITY 0-15 ; ZPL ~SD 0-30 (double de la valeur)
    bool ribbon = true;    // transfert thermique (ruban) ; sinon thermique direct
    double gap_mm = 2.0;   // espace entre deux etiquettes (TSPL GAP)
    int offset_x_dots = 0; // decalage de l'image sur l'etiquette (>= 0)
    int offset_y_dots = 0;
};

// Lignes de l'image en octets, bit de poids fort a gauche. `one_is_black` : ZPL (1 = point noir) ;
// TSPL attend l'inverse (0 = point noir).
std::vector<std::uint8_t> pack_rows(const RasterImage& image, bool one_is_black);

std::vector<std::uint8_t> encode_tspl(const RasterImage& image, const RawJob& job);
std::vector<std::uint8_t> encode_zpl(const RasterImage& image, const RawJob& job);

} // namespace qrprotec::labels
