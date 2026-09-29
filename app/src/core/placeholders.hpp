/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          placeholders.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "template.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace qrprotec {

struct PlaceholderInfo {
    std::string name;
    std::string description;
    std::string example;
};

struct CategoryInfo {
    TemplateCategory               category;
    std::string                    id;    // identifiant stocke dans les fichiers .qr
    std::string                    label; // libelle affiche
    std::string                    description;
    std::vector< PlaceholderInfo > placeholders;
};

using Parameters = std::unordered_map< std::string, std::string >;

const std::vector< CategoryInfo > &template_categories();
const CategoryInfo                &category_info(TemplateCategory category);
const char                        *category_id(TemplateCategory category);
TemplateCategory                   category_from_id(const std::string &id);

// Placeholders communs a toutes les etiquettes ({{today}}, {{printed_by}}...)
const std::vector< PlaceholderInfo > &common_placeholders();

// Valeurs d'exemple pour l'apercu de l'editeur.
Parameters example_parameters(TemplateCategory category);

} // namespace qrprotec
