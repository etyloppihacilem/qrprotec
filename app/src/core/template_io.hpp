#pragma once

#include "template.hpp"

#include <string>

namespace qrprotec {

class Json;

// Contenu JSON du fichier .qr (parametres tries : sert aussi a comparer deux documents).
std::string serialize_template(const TemplateDocument& document);
bool save_template(const TemplateDocument& document, const std::string& path, std::string& error);
bool load_template(TemplateDocument& document, const std::string& path, std::string& error);

// Marges d'un type d'etiquette dans un objet JSON (.qr, impressions envoyees au poste) : « margin_mm » pour les
// quatre cotes (1 mm si absente), « margin_top_mm », « margin_bottom_mm », « margin_left_mm », « margin_right_mm »
// par-dessus ; chaque marge bornee a 0..10 mm.
LabelMargins read_label_margins(const Json& label_type);
// Toujours « margin_mm » (le haut si elles different : lu par les postes d'avant), plus une cle par cote si les
// quatre ne sont pas egales
void write_label_margins(Json& label_type, const LabelMargins& margins);

} // namespace qrprotec