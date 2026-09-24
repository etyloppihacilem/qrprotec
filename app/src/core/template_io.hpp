#pragma once

#include "template.hpp"

#include <string>

namespace qrprotec {

bool save_template(const TemplateDocument& document, const std::string& path, std::string& error);
bool load_template(TemplateDocument& document, const std::string& path, std::string& error);

} // namespace qrprotec