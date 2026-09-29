/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          fonts.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>

namespace qrprotec {

// Chemin d'une police TrueType avec les accents francais (interface et etiquettes), ou vide.
// Ordre : $QRPROTEC_FONT, fonts/ a cote de l'executable, police fournie avec les sources
// (third_party/fonts), polices systeme courantes, puis fontconfig (fc-match).
const std::string &ui_font_path();

} // namespace qrprotec
