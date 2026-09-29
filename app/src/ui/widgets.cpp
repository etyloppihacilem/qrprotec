/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          widgets.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "widgets.hpp"

#include <algorithm>
#include <cctype>
#include <string>

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
