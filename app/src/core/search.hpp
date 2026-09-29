/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          search.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>

namespace qrprotec {

// Minuscules sans accents (é -> e, ç -> c, œ -> oe...) pour comparer des textes saisis au clavier.
std::string normalize_search(const std::string &text);

// Score de correspondance d'une recherche (plusieurs mots possibles) avec un texte, -1 si aucun :
// chaque mot recherche doit commencer un mot du texte (meilleur score) ou y apparaitre. Bonus si le
// texte commence par la recherche ou lui est egal. "ser ph" trouve "Sérum physiologique".
int search_score(const std::string &query, const std::string &text);

} // namespace qrprotec
