/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          widgets.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "widgets.hpp"

#include "../core/search.hpp"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace qrprotec {

namespace {
bool colored_button(const char *label, const ImVec4 &color, const ImVec2 &size) {
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.1f, color.y * 1.1f, color.z * 1.1f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast< char >(std::tolower(character));
  });
  return value;
}
} // namespace

bool danger_button(const char *label, const ImVec2 &size) {
  return colored_button(label, colors::red, size);
}

bool primary_button(const char *label, const ImVec2 &size) {
  return colored_button(label, colors::green, size);
}

void stock_bar(int quantity, int minimum, const ImVec2 &size) {
  const ImVec4 color = quantity <= 0 ? colors::red : quantity < minimum ? colors::orange : colors::green;
  float        fraction = minimum > 0 ? static_cast< float >(quantity) / static_cast< float >(minimum) : 1.0f;
  if (quantity <= 0)
    fraction = 1.0f; // barre entierement rouge : bien visible
  fraction = std::clamp(fraction, 0.0f, 1.0f);
  const std::string overlay = std::to_string(quantity) + "/" + std::to_string(minimum);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
  ImGui::PushStyleColor(ImGuiCol_Text, quantity <= 0 ? ImVec4(1, 1, 1, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
  ImGui::ProgressBar(fraction, size, overlay.c_str());
  ImGui::PopStyleColor(2);
}

void row_color(const ImVec4 &color, float alpha) {
  ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, alpha)));
}

bool json_combo(
  const char *label, const Json &list, const char *key_field, const char *name_field, std::string &selected,
  const char *filter
) {
  std::string preview = selected.empty() ? "(choisir)" : selected;
  for (const Json &entry : list.items())
    if (entry[key_field].str() == selected)
      preview = entry[name_field].str() + " (" + selected + ")";
  bool changed = false;
  if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    const std::string needle = filter ? lower(filter) : std::string();
    for (const Json &entry : list.items()) {
      const std::string key  = entry[key_field].str();
      const std::string name = entry[name_field].str();
      if (!needle.empty() && lower(key + " " + name).find(needle) == std::string::npos)
        continue;
      const std::string text = name + " (" + key + ")";
      if (ImGui::Selectable(text.c_str(), key == selected)) {
        selected = key;
        changed  = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool search_matches(const std::string &query, const std::string &text) {
  return search_score(query, text) >= 0;
}

namespace {
struct SearchState {
    std::string query;
    int         highlighted   = 0;
    bool        popup_hovered = false;
    bool        accept        = false; // Tab presse (callback de completion)
};
std::unordered_map< ImGuiID, SearchState > search_states;

int search_callback(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    static_cast< SearchState * >(data->UserData)->accept = true;
  return 0;
}
} // namespace

bool search_select(const char *id, const Json &list, const char *key_field, const char *name_field,
                   std::string &selected, const char *hint, const char *empty_label, float width) {
  ImGui::PushID(id);
  SearchState &state = search_states[ImGui::GetID("state")];
  // libelle de la selection courante
  std::string selected_label = empty_label && selected.empty() ? empty_label : std::string();
  for (const Json &entry : list.items())
    if (entry[key_field].str() == selected && !selected.empty())
      selected_label = entry[name_field].str() + " (" + selected + ")";

  const ImGuiID input_id = ImGui::GetID("##input");
  const bool    active   = ImGui::GetActiveID() == input_id;
  if (!active)
    state.query = selected_label; // hors saisie : le champ montre la selection
  ImGui::SetNextItemWidth(width);
  const bool entered = ImGui::InputTextWithHint(
    "##input", hint, &state.query,
    ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion,
    search_callback, &state
  );
  const bool   now_active = ImGui::IsItemActive();
  const ImVec2 min        = ImGui::GetItemRectMin();
  const ImVec2 max        = ImGui::GetItemRectMax();
  if (ImGui::IsItemActivated())
    state.highlighted = 0;

  // resultats : texte tape (ou tout si le champ montre encore la selection), tries par pertinence
  const std::string query = state.query == selected_label ? std::string() : state.query;
  struct Match {
      int         score;
      std::string key;
      std::string label;
  };
  std::vector< Match > matches;
  if (empty_label && query.empty())
    matches.push_back({ 1000, "", empty_label });
  for (const Json &entry : list.items()) {
    const std::string key   = entry[key_field].str();
    const std::string label = entry[name_field].str() + " (" + key + ")";
    const int         score = search_score(query, label);
    if (score >= 0)
      matches.push_back({ score, key, label });
  }
  std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) { return a.score > b.score; });
  if (matches.size() > 15)
    matches.resize(15);
  state.highlighted = matches.empty() ? 0 : std::clamp(state.highlighted, 0, static_cast< int >(matches.size()) - 1);

  bool changed = false;
  const auto accept = [&](const Match &match) {
    changed     = match.key != selected;
    selected    = match.key;
    state.query = match.label;
    ImGui::ClearActiveID(); // referme la saisie : le champ affiche la selection
  };
  if (now_active) {
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
      ++state.highlighted;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
      --state.highlighted;
    state.highlighted = matches.empty() ? 0 : std::clamp(state.highlighted, 0, static_cast< int >(matches.size()) - 1);
  }
  if ((entered || state.accept) && !matches.empty())
    accept(matches[static_cast< std::size_t >(state.highlighted)]);
  state.accept = false;

  // liste deroulante des resultats, au premier plan sous le champ
  if (!changed && (now_active || state.popup_hovered)) {
    ImGui::SetNextWindowPos(ImVec2(min.x, max.y + 2.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(max.x - min.x, 240.0f), 0.0f),
                                        ImVec2(std::max(max.x - min.x, 420.0f), ImGui::GetTextLineHeightWithSpacing() * 12.0f));
    const std::string popup = std::string("##search_results_") + std::to_string(input_id);
    ImGui::Begin(popup.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                   | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    if (matches.empty())
      ImGui::TextDisabled("Aucun résultat");
    for (std::size_t index = 0; index < matches.size(); ++index) {
      const bool highlighted = static_cast< int >(index) == state.highlighted;
      if (ImGui::Selectable(matches[index].label.c_str(), highlighted))
        accept(matches[index]);
      if (highlighted && now_active)
        ImGui::SetScrollHereY();
    }
    ImGui::TextDisabled("↑↓ choisir · Tab/Entrée valider · Échap annuler");
    state.popup_hovered = ImGui::IsWindowHovered() && !changed;
    ImGui::End();
  } else {
    state.popup_hovered = false;
  }
  ImGui::PopID();
  return changed;
}

bool confirm_button(const char *label, const char *question, const char *popup_id) {
  if (danger_button(label))
    ImGui::OpenPopup(popup_id);
  bool confirmed = false;
  if (ImGui::BeginPopup(popup_id)) {
    ImGui::TextUnformatted(question);
    if (danger_button("Confirmer")) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Annuler"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return confirmed;
}

void status_banner(const std::string &text, const ImVec4 &color, float scale) {
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
  const ImVec2 padding(ImGui::GetStyle().FramePadding.x * 2.0f, ImGui::GetStyle().FramePadding.y * 2.0f);
  const float  width  = ImGui::GetContentRegionAvail().x;
  const ImVec2 size   = ImGui::CalcTextSize(text.c_str(), nullptr, false, width - padding.x * 2.0f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 end(origin.x + width, origin.y + size.y + padding.y * 2.0f);
  ImDrawList  *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, end, ImGui::GetColorU32(color), 6.0f);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + padding.x, origin.y + padding.y));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::PushTextWrapPos(origin.x + width - padding.x - ImGui::GetWindowPos().x);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::SetCursorScreenPos(ImVec2(origin.x, end.y + ImGui::GetStyle().ItemSpacing.y));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopFont();
}

LotStatus lot_status(const Json &lot) {
  if (lot["last_verif"].is_null())
    return LotStatus::Never;
  return lot["complete"].boolean() ? LotStatus::Verified : LotStatus::Incomplete;
}

ImVec4 lot_status_color(LotStatus status) {
  return status == LotStatus::Verified ? colors::green : colors::red;
}

const char *lot_status_label(LotStatus status) {
  switch (status) {
    case LotStatus::Verified: return "✔ Complet";
    case LotStatus::Incomplete: return "✘ Incomplet";
    case LotStatus::Never: return "✘ Jamais vérifié";
  }
  return "";
}

void help_marker(const char *text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

} // namespace qrprotec
