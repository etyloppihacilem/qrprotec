/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          code128.hpp
        -\-    _|__
         |\___/  . \        Created on 20 Oct. 2026 at 22:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>
#include <vector>

namespace qrprotec {

// Code-barres Code 128 (ISO/IEC 15417), pour les etiquettes d'items posees sur des objets ronds ou un QR deforme
// se lit mal. Jeux B (ASCII imprimable, lettres minuscules comprises) et C (paires de chiffres, deux fois plus
// court) : un iid (6 lettres + 8 chiffres + 8 caracteres base62) tient en 220 a 255 modules.
// Meme table que database/inventory/web/code128.js (decodeur du front web).

// Largeurs des 6 barres et espaces de chaque symbole (107 : stop, 7 largeurs).
extern const char *const kCode128Patterns[107];

// Symboles (depart, donnees, controle, stop) ; vide si le texte a un caractere hors ASCII imprimable (32-126).
std::vector< int > code128_symbols(const std::string &text);

// Modules du code-barres, true = barre, sans les marges blanches ; vide si le texte n'est pas encodable.
std::vector< bool > code128_modules(const std::string &text);

} // namespace qrprotec
