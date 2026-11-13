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

// Etiquette du code d'action « rouleau » (codes.hpp, kPosteActionPrefix) : QR qui declare le rouleau `roll` en place
// quand on le scanne a la douchette d'un poste, avec son nom et son format en clair. Imprimee sur le rouleau
// `print_on` (choisi a la main), mise en page selon son format (QR a gauche, ou en haut si l'etiquette est haute).
std::string      roll_action_code(const LabelType& roll);
TemplateDocument roll_code_document(const LabelType& roll, const LabelType& print_on);

// Etiquette d'etalonnage du rouleau `type` (Reglages > Maintenance de l'imprimante) : cadre plein sur le cadre des
// marges (`margin_mm` de chaque cote, LabelType::margin_mm), cadre pointille 1 mm a l'interieur, croix au centre graduee tous les millimetres (traits plus longs tous les 5 et 10 mm), format
// en clair. Tout est calcule sur la taille de l'etiquette : un bord coupe ou une croix decentree donne le
// decalage a regler. `with_type` : le document porte le type (rouleau charge), sinon format libre.
TemplateDocument calibration_document(const LabelType& type, bool with_type);
RasterImage      calibration_image(const TemplateDocument& document, double margin_mm);

} // namespace qrprotec
