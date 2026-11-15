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

// Planche (TemplateCategory::ItemSheet) : nombre d'items par etiquette, d'apres le plus grand numero des
// placeholders d'item du modele ({{code5}} → 5, au moins 1). `item` : parametres d'un item (noms reconnus).
int sheet_slots(const TemplateDocument &document, const Parameters &item);
// Parametres d'une planche de `slots` items : chaque parametre d'item suivi du numero de l'item ({{code1}}...),
// vides pour les items manquants de la derniere planche ; sans numero, ceux du 1er item.
Parameters sheet_parameters(const TemplateDocument &document, const std::vector< Parameters > &items, int slots);

// {{tasks}} : une tache par ligne, precedee de "□ ". L'editeur fait saisir les lignes
// sans le carre (task_lines) et le rajoute a chaque ligne non vide (tasks_value).
std::string task_lines(const std::string &tasks);
std::string tasks_value(const std::string &lines);

} // namespace qrprotec
