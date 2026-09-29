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

// Champ texte avec bouton de confirmation en deux temps (pour les actions sensibles)
bool confirm_button(const char *label, const char *question, const char *popup_id);

void help_marker(const char *text);

} // namespace qrprotec
