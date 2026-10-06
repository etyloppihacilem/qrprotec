/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          widgets.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "widgets.hpp"

#include "../core/codes.hpp"
#include "../core/search.hpp"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace qrprotec {

namespace {
// Raccourcis Alt+lettre des boutons : chaque bouton prend la premiere lettre libre de sa fenetre (initiales
// des mots d'abord, puis les autres lettres), soulignee dans son libelle. Les lettres sont attribuees dans
// l'ordre de dessin, a chaque frame : elles restent les memes tant que les boutons ne changent pas.
struct MnemonicFrame {
  int                                         frame = -1;
  std::unordered_map< ImGuiID, unsigned int > used; // lettres prises, par fenetre racine (bit 0 = a)
  // raccourci presse pendant une saisie : declenche a la frame suivante (voir mnemonic_button)
  int     deferred_frame  = -1;
  ImGuiID deferred_root   = 0;
  char    deferred_letter = 0;
};

MnemonicFrame &mnemonic_frame() {
  static MnemonicFrame state;
  if (state.frame != ImGui::GetFrameCount()) {
    state.frame = ImGui::GetFrameCount();
    state.used.clear();
  }
  return state;
}

bool ascii_letter(char character) {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

// Position de la lettre choisie dans le libelle affiche, -1 si aucune n'est libre
int pick_mnemonic(const char *text, const char *end, unsigned int &used) {
  const auto take = [&](const char *at) {
    const unsigned int bit = 1u << (std::tolower(static_cast< unsigned char >(*at)) - 'a');
    if (used & bit)
      return false;
    used |= bit;
    return true;
  };
  for (const char *at = text; at < end; ++at)
    if (ascii_letter(*at) && (at == text || at[-1] == ' ' || at[-1] == '(' || at[-1] == '\'') && take(at))
      return static_cast< int >(at - text);
  for (const char *at = text; at < end; ++at)
    if (ascii_letter(*at) && take(at))
      return static_cast< int >(at - text);
  return -1;
}

// Bouton ImGui avec raccourci Alt+lettre (Alt de gauche : Alt Gr sert a taper @, #, { sur un clavier francais).
// Les petits boutons et les boutons des tableaux (une ligne chacun) n'en ont pas : on y va au clavier avec
// les fleches.
bool mnemonic_button(const char *label, const ImVec2 &size, bool small, bool mnemonic = true) {
  const bool   pressed = small ? ImGui::SmallButton(label) : ImGui::Button(label, size);
  ImGuiWindow *window  = ImGui::GetCurrentWindow();
  if (small || !mnemonic || ImGui::GetCurrentTable() != nullptr || window->SkipItems)
    return pressed;
  const char *end = ImGui::FindRenderedTextEnd(label);
  if (end - label < 2)
    return pressed;
  const int index = pick_mnemonic(label, end, mnemonic_frame().used[window->RootWindow->ID]);
  if (index < 0)
    return pressed;

  // soulignement, place comme le texte du bouton (RenderTextClipped dans ButtonEx)
  const ImGuiStyle &style   = ImGui::GetStyle();
  const ImVec2      min     = ImGui::GetItemRectMin();
  const ImVec2      max     = ImGui::GetItemRectMax();
  const ImVec2      text    = ImGui::CalcTextSize(label, end);
  ImVec2            pos(min.x + style.FramePadding.x, min.y + style.FramePadding.y);
  pos.x                     = std::max(pos.x, pos.x + (max.x - min.x - style.FramePadding.x * 2 - text.x) * style.ButtonTextAlign.x);
  pos.y                     = std::max(pos.y, pos.y + (max.y - min.y - style.FramePadding.y * 2 - text.y) * style.ButtonTextAlign.y);
  const float left          = pos.x + ImGui::CalcTextSize(label, label + index).x;
  const float right         = left + ImGui::CalcTextSize(label + index, label + index + 1).x;
  const float line          = std::floor(pos.y + ImGui::GetFontSize() * 0.95f);
  if (right < max.x - style.FramePadding.x * 0.5f)
    window->DrawList->AddLine(ImVec2(left, line), ImVec2(right, line), ImGui::GetColorU32(ImGuiCol_Text),
                              std::max(1.0f, ImGui::GetFontSize() / 14.0f));

  const char     letter = static_cast< char >(std::tolower(static_cast< unsigned char >(label[index])));
  MnemonicFrame &state  = mnemonic_frame();
  if (state.deferred_frame == ImGui::GetFrameCount() && state.deferred_root == window->RootWindow->ID
      && state.deferred_letter == letter) {
    state.deferred_frame = -1;
    return !(GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled);
  }
  const ImGuiIO &io = ImGui::GetIO();
  if (pressed || !ImGui::IsKeyDown(ImGuiKey_LeftAlt) || io.KeyCtrl || io.KeySuper
      || (GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled)
      || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    return pressed;
  if (!ImGui::IsKeyPressed(static_cast< ImGuiKey >(ImGuiKey_A + letter - 'a'), false))
    return false;
  if (GImGui->ActiveId == 0)
    return true;
  // champ en cours de saisie : on le quitte d'abord, comme un clic, et le bouton agit a la frame suivante.
  // Sinon le champ reecrirait son texte a la frame suivante, apres que le bouton l'a vide.
  ImGui::ClearActiveID();
  state.deferred_frame  = ImGui::GetFrameCount() + 1;
  state.deferred_root   = window->RootWindow->ID;
  state.deferred_letter = letter;
  return false;
}

bool colored_button(const char *label, const ImVec4 &color, const ImVec2 &size, bool mnemonic = true) {
  ImGui::PushStyleColor(ImGuiCol_Button, color);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x * 1.1f, color.y * 1.1f, color.z * 1.1f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  const bool pressed = mnemonic_button(label, size, false, mnemonic);
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

bool button(const char *label, const ImVec2 &size) {
  return mnemonic_button(label, size, false);
}

bool small_button(const char *label) {
  return mnemonic_button(label, ImVec2(0, 0), true);
}

// Onglets : Ctrl+1 a Ctrl+9 choisissent le 1er au 9e onglet de la barre d'onglets de la fenetre qui a le focus.
bool tab_item(const char *label, ImGuiTabItemFlags flags) {
  static int                                 frame = -1;
  static std::unordered_map< ImGuiID, int > counts; // onglets deja dessines, par barre d'onglets
  if (frame != ImGui::GetFrameCount()) {
    frame = ImGui::GetFrameCount();
    counts.clear();
  }
  const ImGuiTabBar *bar   = GImGui->CurrentTabBar;
  const int          index = bar ? ++counts[bar->ID] : 0;
  if (index >= 1 && index <= 9 && ImGui::GetTopMostPopupModal() == nullptr
      && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
      && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | static_cast< ImGuiKey >(ImGuiKey_0 + index)))
    flags |= ImGuiTabItemFlags_SetSelected;
  const bool open = ImGui::BeginTabItem(label, nullptr, flags);
  if (index >= 1 && index <= 9)
    ImGui::SetItemTooltip("Ctrl+%d", index);
  return open;
}

bool danger_button(const char *label, const ImVec2 &size, bool mnemonic) {
  return colored_button(label, colors::red, size, mnemonic);
}

bool primary_button(const char *label, const ImVec2 &size) {
  return colored_button(label, colors::green, size);
}

bool warning_button(const char *label, const ImVec2 &size) {
  return colored_button(label, colors::orange, size);
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
struct LimitState {
    int  max_chars;
    bool code;
};

bool continuation_byte(char character) {
  return (static_cast< unsigned char >(character) & 0xC0) == 0x80;
}

int utf8_length(const char *text, int bytes) {
  int count = 0;
  for (int index = 0; index < bytes; ++index)
    count += continuation_byte(text[index]) ? 0 : 1;
  return count;
}

int limit_callback(ImGuiInputTextCallbackData *data) {
  const LimitState &limit = *static_cast< const LimitState * >(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
    return limit.code && !(data->EventChar < 128 && std::isalnum(static_cast< int >(data->EventChar)));
  if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit)
    return 0;
  int excess = utf8_length(data->Buf, data->BufTextLen) - limit.max_chars;
  if (excess <= 0)
    return 0;
  // retire les caracteres en trop juste avant le curseur : ceux qui viennent d'etre tapes ou colles
  int start = data->CursorPos;
  while (excess > 0 && start > 0) {
    --start;
    while (start > 0 && continuation_byte(data->Buf[start]))
      --start;
    --excess;
  }
  data->DeleteChars(start, data->CursorPos - start);
  // texte deja trop long (ne devrait pas arriver) : coupe la fin
  int end = data->BufTextLen;
  while (excess > 0 && end > 0) {
    --end;
    while (end > 0 && continuation_byte(data->Buf[end]))
      --end;
    --excess;
  }
  if (end < data->BufTextLen)
    data->DeleteChars(end, data->BufTextLen - end);
  return 0;
}

// Forme d'un nom pour detecter les doublons (models.name_key cote serveur)
std::string name_key(const std::string &name) {
  std::string key;
  for (const char character : normalize_search(name)) {
    if (std::isspace(static_cast< unsigned char >(character))) {
      if (!key.empty() && key.back() != ' ')
        key += ' ';
    } else {
      key += character;
    }
  }
  while (!key.empty() && key.back() == ' ')
    key.pop_back();
  return key;
}
} // namespace

bool input_limited(const char *label, std::string &value, int max_chars, const char *hint, ImGuiInputTextFlags flags,
                   bool code) {
  LimitState limit{ max_chars, code };
  flags |= ImGuiInputTextFlags_CallbackEdit;
  if (code)
    flags |= ImGuiInputTextFlags_CallbackCharFilter;
  const bool changed = hint ? ImGui::InputTextWithHint(label, hint, &value, flags, limit_callback, &limit)
                            : ImGui::InputText(label, &value, flags, limit_callback, &limit);
  if (ImGui::IsItemActive() && utf8_length(value.data(), static_cast< int >(value.size())) >= max_chars)
    ImGui::SetTooltip("%d caractères maximum", max_chars);
  return changed;
}

std::string name_taken(const Json &list, const char *key_field, const std::string &name, const std::string &skip_key,
                       bool *archived) {
  const std::string key = name_key(name);
  if (key.empty())
    return "";
  for (const Json &entry : list.items()) {
    if (!skip_key.empty() && entry[key_field].str() == skip_key)
      continue;
    if (name_key(entry["name"].str()) == key) {
      if (archived)
        *archived = entry["archived"].boolean() || !entry["active"].boolean(true);
      return entry["name"].str();
    }
  }
  return "";
}

Json without_archived(const Json &list) {
  Json result = Json::array();
  for (const Json &entry : list.items())
    if (!entry["archived"].boolean())
      result.push_back(entry);
  return result;
}

bool name_taken_warning(const Json &list, const char *key_field, const std::string &name, const std::string &skip_key,
                        const char *what) {
  bool              archived = false;
  const std::string existing = name_taken(list, key_field, name, skip_key, &archived);
  if (existing.empty())
    return false;
  ImGui::TextColored(colors::red, "%s « %s » existe déjà%s : choisissez un autre nom.", what, existing.c_str(),
                     archived ? " (archivé)" : "");
  return true;
}

namespace {
struct SearchState {
    std::string query;
    int         highlighted   = 0;
    bool        popup_hovered = false;
    bool        accept        = false; // Tab presse (callback de completion)
};
std::unordered_map< ImGuiID, SearchState > search_states;

// Premiere ligne de la description, coupee a ~40 caracteres (sans couper un caractere UTF-8)
std::string description_excerpt(const std::string &description) {
  std::string line = description.substr(0, description.find('\n'));
  std::size_t end = 0;
  for (int count = 0; end < line.size() && count < 40; ++count) {
    ++end;
    while (end < line.size() && (static_cast< unsigned char >(line[end]) & 0xC0) == 0x80)
      ++end;
  }
  return end < line.size() ? line.substr(0, end) + "…" : line;
}

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
  // hors saisie : le champ montre la selection. Pas pendant un clic dans la liste des resultats : le clic fait
  // perdre la saisie au champ, et la liste doit rester celle de la recherche jusqu'au relachement du bouton.
  if (!active && !state.popup_hovered)
    state.query = selected_label;
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
      std::string note; // extrait de la description quand c'est elle qui correspond
  };
  std::vector< Match > matches;
  if (empty_label && query.empty())
    matches.push_back({ 1000, "", empty_label, "" });
  for (const Json &entry : list.items()) {
    const std::string key   = entry[key_field].str();
    const std::string label = entry[name_field].str() + " (" + key + ")";
    const int         score = search_score(query, label);
    if (score >= 0) {
      matches.push_back({ score, key, label, "" });
      continue;
    }
    // la description compte aussi (ex: nom commercial d'un medicament generique), apres les noms
    const std::string description = entry["description"].str();
    if (!description.empty() && search_score(query, description) >= 0)
      matches.push_back({ -1, key, label, description_excerpt(description) });
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
      // choix des l'appui du bouton : la liste ne peut plus changer sous la souris avant le relachement
      const std::string text = matches[index].note.empty() ? matches[index].label
                                                           : matches[index].label + " · " + matches[index].note;
      if (ImGui::Selectable(text.c_str(), highlighted, ImGuiSelectableFlags_SelectOnClick))
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
    if (button("Annuler"))
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
  if (lot["is_sealed"].boolean())
    return lot["expired_count"].integer() > 0 ? LotStatus::SealedExpired : LotStatus::Sealed;
  if (lot["last_verif"].is_null())
    return LotStatus::Never;
  if (!lot["complete"].boolean())
    return LotStatus::Incomplete;
  return lot["verif_recommended"].boolean() ? LotStatus::Recommended : LotStatus::Verified;
}

bool lot_ok(LotStatus status) {
  return status == LotStatus::Verified || status == LotStatus::Sealed;
}

ImVec4 lot_status_color(LotStatus status) {
  if (status == LotStatus::Recommended)
    return colors::orange;
  return lot_ok(status) ? colors::green : colors::red;
}

const char *lot_status_label(LotStatus status) {
  switch (status) {
    case LotStatus::Verified: return "✔ Complet";
    case LotStatus::Incomplete: return "✘ Incomplet";
    case LotStatus::Never: return "✘ Jamais vérifié";
    case LotStatus::Sealed: return "✔ Scellé";
    case LotStatus::SealedExpired: return "✘ Scellé, périmés";
    case LotStatus::Recommended: return "! Vérif recommandée";
  }
  return "";
}

std::string lot_status_banner(const Json &lot) {
  const LotStatus status = lot_status(lot);
  std::string     banner;
  switch (status) {
    case LotStatus::Sealed: {
      banner = "✔ LOT SCELLÉ";
      if (!lot["seal_number"].str().empty())
        banner += " n°" + lot["seal_number"].str();
      if (const auto until = Date::parse(lot["valid_until"].str()))
        banner += " – valide jusqu'au " + until->display();
      return banner;
    }
    case LotStatus::SealedExpired: return "✘ LOT SCELLÉ MAIS CONTIENT DES PÉRIMÉS – à ouvrir";
    case LotStatus::Verified: return "✔ LOT VÉRIFIÉ ET COMPLET";
    case LotStatus::Recommended: {
      // sans reassort depuis la derniere verif : recommandee par l'ouverture du scelle (etiquette d'ouverture)
      if (lot["restocked_count"].integer() == 0) {
        banner = "! VÉRIF COMPLÈTE RECOMMANDÉE – scellé ouvert";
        if (const auto when = Date::parse(lot["unsealed"].str()))
          banner += " le " + when->display();
        if (!lot["unsealed_by"].str().empty())
          banner += " par " + lot["unsealed_by"].str();
        return banner;
      }
      banner = "! VÉRIF COMPLÈTE RECOMMANDÉE – réassort";
      if (lot["restocked_count"].integer() > 0)
        banner += " de " + std::to_string(lot["restocked_count"].integer()) + " item(s)";
      if (const auto when = Date::parse(lot["restocked"].str()))
        banner += " le " + when->display();
      if (!lot["restocked_by"].str().empty())
        banner += " par " + lot["restocked_by"].str();
      return banner;
    }
    case LotStatus::Never: banner = "✘ LOT JAMAIS VÉRIFIÉ"; break;
    case LotStatus::Incomplete:
      banner = "✘ LOT INCOMPLET";
      if (lot["verif_recommended"].boolean())
        banner += " – réassort depuis la dernière vérif";
      break;
  }
  if (lot["expired_count"].integer() > 0)
    banner += " – contient des périmés";
  return banner;
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

bool lot_is_group(const Json &lot) {
  for (const Json &row : lot["global"]["lots"].items())
    if (row["id"].str() == lot["id"].str())
      return !row["counted"].boolean();
  return false;
}

std::string group_banner(const Json &lot, ImVec4 &color, bool *all_ok) {
  // sous-lots : lignes qui suivent le lot avec une profondeur plus grande (ordre de l'arborescence)
  int  depth = -1, problems = 0, counted = 0;
  bool warn = false;
  for (const Json &row : lot["global"]["lots"].items()) {
    if (depth < 0) {
      if (row["id"].str() == lot["id"].str())
        depth = row["depth"].integer();
      continue;
    }
    if (row["depth"].integer() <= depth)
      break;
    if (!row["counted"].boolean())
      continue;
    ++counted;
    const std::string kind = row["effective"]["kind"].str();
    if (kind != "ok")
      ++problems;
    warn = warn || kind == "warn";
  }
  if (all_ok)
    *all_ok = problems == 0;
  if (problems == 0) {
    color = colors::green;
    return "✔ SOUS-LOTS TOUS VALIDES (" + std::to_string(counted) + ")";
  }
  color = warn && problems == 1 ? colors::orange : colors::red;
  return "✘ " + std::to_string(problems) + " SOUS-LOT(S) À TRAITER SUR " + std::to_string(counted);
}

} // namespace qrprotec
