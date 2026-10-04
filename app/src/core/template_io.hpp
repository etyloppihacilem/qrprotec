#pragma once

#include "template.hpp"

#include <string>

namespace qrprotec {

// Contenu JSON du fichier .qr (parametres tries : sert aussi a comparer deux documents).
std::string serialize_template(const TemplateDocument& document);
bool save_template(const TemplateDocument& document, const std::string& path, std::string& error);
bool load_template(TemplateDocument& document, const std::string& path, std::string& error);

} // namespace qrprotec