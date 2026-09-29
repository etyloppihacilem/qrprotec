/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          image_loader.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qrprotec {

// Image en niveaux de gris (0 = noir, 255 = blanc), transparence composee sur fond blanc.
struct GrayImage {
    int                         width  = 0;
    int                         height = 0;
    std::vector< std::uint8_t > pixels;
};

// Charge un PNG ou un JPEG. Le resultat est mis en cache tant que le fichier n'est pas modifie.
std::shared_ptr< const GrayImage > load_gray_image(const std::string &path, std::string &error);

} // namespace qrprotec
