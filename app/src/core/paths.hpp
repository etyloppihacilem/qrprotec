/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          paths.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace qrprotec {

// Dossier des modeles d'etiquettes (*.qr) et de leurs images (logo...). Par defaut app/templates du
// depot : les modeles crees dans l'editeur y sont enregistres et peuvent etre commites.
// Ordre : reglage de l'application, $QRPROTEC_TEMPLATES_DIR, dossier des sources, dossier courant.
std::string                  default_templates_dir();
void                         set_templates_dir(const std::string &directory); // vide = defaut
const std::filesystem::path &templates_dir();

// Chemin relatif -> dans le dossier des modeles ; chemin absolu -> inchange.
std::filesystem::path resolve_template_path(const std::string &path);

// Fichiers du dossier des modeles correspondant a un motif glob (ex: "*.qr"), tries par nom.
// subdirectory : sous-dossier du dossier des modeles (ex: "images").
std::vector< std::filesystem::path > glob_templates(const std::string &pattern, const std::string &subdirectory = "");

// Sous-dossier des images a mettre sur les etiquettes (logo...) : <dossier des modeles>/images.
constexpr const char *LABEL_IMAGES_DIR = "images";

// Images PNG / JPEG utilisables sur les etiquettes, en chemins relatifs au dossier des modeles :
// celles de images/ ("images/protec.png") puis celles posees a la racine (anciens modeles).
std::vector< std::string > label_images();

} // namespace qrprotec
