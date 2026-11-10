#pragma once

#include "../core/template.hpp"
#include "raster.hpp"

#include <string>
#include <vector>

namespace qrprotec {

// Type d'etiquette d'un modele qui n'en a pas encore : le rouleau regle dans les Reglages (etiquettes chargees)
// et l'orientation qu'il recevait jusqu'ici a l'impression (quart de tour automatique, retournement).
LabelType default_label_type(const MediaSettings& media, const PhysicalLabel& label, LabelSupport support);

// Donne son type a chaque modele du dossier qui n'en a pas et enregistre le fichier. Renvoie le nombre de
// modeles mis a jour ; `types` recoit le type de chacun (pour choisir le rouleau charge), `errors` les fichiers
// qui n'ont pas pu etre enregistres (dossier en lecture seule : ils s'impriment encore en format libre).
int add_missing_label_types(const PhysicalLabel& label, LabelSupport support, std::vector<LabelType>* types = nullptr,
                            std::vector<std::string>* errors = nullptr);

} // namespace qrprotec
