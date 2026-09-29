/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          widgets.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/json.hpp"
#include "imgui.h"

#include <string>

namespace qrprotec {

namespace colors {
const ImVec4 green(0.18f, 0.62f, 0.25f, 1.0f);
const ImVec4 orange(0.93f, 0.55f, 0.10f, 1.0f);
const ImVec4 red(0.82f, 0.12f, 0.12f, 1.0f);
const ImVec4 grey(0.5f, 0.5f, 0.5f, 1.0f);
const ImVec4 yellow(0.95f, 0.80f, 0.20f, 1.0f);
} // namespace colors

bool danger_button(const char *label, const ImVec2 &size = ImVec2(0, 0));
bool primary_button(const char *label, const ImVec2 &size = ImVec2(0, 0));

// Barre de stock : verte si quantite >= minimum, orange si en dessous, rouge si vide.
// Le texte "quantite/minimum" est affiche dans la barre.
void stock_bar(int quantity, int minimum, const ImVec2 &size = ImVec2(-1.0f, 0.0f));

// Colore la ligne courante d'une table
void row_color(const ImVec4 &color, float alpha = 0.35f);

// Combo de selection d'un element d'une liste JSON (ex: types d'items). Retourne true si change.
bool json_combo(
  const char *label, const Json &list, const char *key_field, const char *name_field, std::string &selected,
  const char *filter = nullptr
);

// Champ de recherche avec completion : resultats en direct sous le champ (sans accents ni casse,
// par debut de mots ou code), fleches pour choisir, Tab ou Entree pour valider, Echap pour annuler.
// `list` : objets JSON ; `key_field` est la valeur retournee dans `selected`. `empty_label` (optionnel)
// ajoute un premier choix qui vide la selection (ex: "Tous les types"). Retourne true si change.
bool search_select(const char *id, const Json &list, const char *key_field, const char *name_field,
                   std::string &selected, const char *hint = "Rechercher…", const char *empty_label = nullptr,
                   float width = -FLT_MIN);

// Filtre de liste : meme correspondance que search_select (accents, debut de mots)
bool search_matches(const std::string &query, const std::string &text);

// Champ texte avec bouton de confirmation en deux temps (pour les actions sensibles)
bool confirm_button(const char *label, const char *question, const char *popup_id);

void help_marker(const char *text);

// Bandeau pleine largeur de couleur vive, texte blanc agrandi (etat d'un lot, resultat d'une verif)
void status_banner(const std::string &text, const ImVec4 &color, float scale = 1.25f);

// Etat d'un lot (objet JSON de l'API) : jamais verifie, incomplet (ou perimes), verifie et complet
enum class LotStatus { Never, Incomplete, Verified };
LotStatus   lot_status(const Json &lot);
ImVec4      lot_status_color(LotStatus status);
const char *lot_status_label(LotStatus status);

} // namespace qrprotec
