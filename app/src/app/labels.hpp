/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          labels.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/json.hpp"
#include "../core/placeholders.hpp"
#include "../core/template.hpp"

#include <string>
#include <vector>

namespace qrprotec {

// Conversion des objets de l'API en valeurs de placeholders (voir core/placeholders.cpp).
Parameters item_parameters(const Json &item, int index = 1, int count = 1);
Parameters sealed_pack_parameters(const Json &pack);
Parameters lot_parameters(const Json &lot);
Parameters user_parameters(const Json &user);

// Charge le modele et remplit ses placeholders (valeurs par defaut du modele < valeurs fournies).
bool build_label(
  const std::string &template_path, const Parameters &values, TemplateDocument &out, std::string &error
);

} // namespace qrprotec
