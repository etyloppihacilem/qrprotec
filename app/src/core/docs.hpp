/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          docs.hpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <cstddef>

namespace qrprotec {

// Pages Markdown de docs/ embarquees dans l'executable a la construction (CMakeLists.txt, generated/docs_data.cpp) :
// la documentation lue au poste est toujours celle de la version installee, sans fichier a deployer.
struct EmbeddedDoc {
    const char *path; // relatif a docs/, separateur / (ex : "utilisateur/notions.md")
    const unsigned char *data; // size octets (suivis d'un 0)
    std::size_t size;
};

extern const EmbeddedDoc kEmbeddedDocs[];
extern const std::size_t kEmbeddedDocsCount;

} // namespace qrprotec
