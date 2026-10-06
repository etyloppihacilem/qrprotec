#include "editor.hpp"
#include "../core/glob_utils.hpp"
#include "../core/template_io.hpp"
#include "../core/paths.hpp"
#include "../core/placeholders.hpp"
#include "core/template.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "widgets.hpp"
#include <GL/gl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <string>
#include <vector>

namespace qrprotec {

namespace {
constexpr std::size_t kHistoryLimit = 200;
constexpr float       kMinQrMm      = 5.0f;
constexpr float       kMinZoom      = 0.2f;
constexpr float       kMaxZoom      = 40.0f;
constexpr float       kHandleSize   = 4.0f; // demi-cote des poignees, en pixels ecran
constexpr float       kHandleGrab   = 7.0f; // distance de prise des poignees

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast< char >(std::tolower(c)); });
  return value;
}

// Nom de fichier propose a partir du nom du modele : minuscules, chiffres et "_"
std::string slug(const std::string &name) {
  std::string result;
  for (const unsigned char c : name) {
    if (std::isalnum(c) && c < 128)
      result += static_cast< char >(std::tolower(c));
    else if (!result.empty() && result.back() != '_')
      result += '_';
  }
  while (!result.empty() && result.back() == '_')
    result.pop_back();
  return result.empty() ? "modele" : result;
}

// "item" -> "item.qr" ; vide si le nom est vide
std::string qr_file_name(std::string name) {
  name.erase(0, name.find_first_not_of(" \t"));
  name.erase(name.find_last_not_of(" \t") + 1);
  if (!name.empty() && std::filesystem::path(name).extension() != ".qr")
    name += ".qr";
  return name;
}
} // namespace

Editor::Editor() {
  reload_templates();
  new_template();
}

Editor::~Editor() {
  if (print_task_.valid())
    print_task_.wait();
  if (texture_ != 0)
    glDeleteTextures(1, &texture_);
}

// Liste les placeholders du modele et donne une valeur d'apercu a ceux qui n'en ont pas (exemple de
// l'usage du modele, sinon d'un autre usage), pour que l'apercu ne montre jamais "{{...}}".
void Editor::refresh_placeholders() {
  placeholder_names_ = find_placeholders(document_);
  Parameters examples = example_parameters(document_.category);
  for (const CategoryInfo &info : template_categories())
    for (const PlaceholderInfo &placeholder : info.placeholders)
      examples.emplace(placeholder.name, placeholder.example);
  for (const std::string &name : placeholder_names_) {
    if (name == "titre") {
      document_.parameters[name] = label_title_; // toujours le titre des Reglages
      continue;
    }
    if (document_.parameters.find(name) != document_.parameters.end())
      continue;
    const auto example         = examples.find(name);
    document_.parameters[name] = example != examples.end() ? example->second : name;
    preview_dirty_             = true;
  }
}

void Editor::set_label_title(const std::string &title) {
  if (title == label_title_)
    return;
  label_title_                  = title;
  document_.parameters["titre"] = title;
  preview_dirty_                = true;
}

void Editor::set_default_media(double width_mm, double height_mm) {
  default_width_mm_  = width_mm;
  default_height_mm_ = height_mm;
}

void Editor::new_template() {
  document_                          = TemplateDocument{};
  document_.media.width_mm           = default_width_mm_;
  document_.media.height_mm          = default_height_mm_;
  document_.parameters["code"]       = "QRProtec default";
  document_.parameters["titre"]      = label_title_;
  const float width                  = static_cast< float >(default_width_mm_);
  const float height                 = static_cast< float >(default_height_mm_);
  const float qr                     = std::max(5.0f, std::min(width, height) * 0.5f);
  document_.elements.push_back(
    { "title", ElementKind::Text, TextElement{ "{{code}}", 2.0f, 2.0f, width - 4.0f, 8.0f, 3.0f } }
  );
  document_.elements.push_back({ "qr", ElementKind::QrCode, QrElement{ "{{code}}", (width - qr) / 2.0f, height - qr - 2.0f, qr } });
  template_path_    = "";
  selected_element_ = -1;
  preview_dirty_    = true;
  fit_pending_      = true;
  refresh_placeholders();
  reset_history();
}

void Editor::reload_templates() {
  image_files_ = label_images();
  loaded_templates_dir_ = templates_dir().string();
  qr_files_             = glob_templates("*.qr");
  document_list_.clear();
  for (auto path : qr_files_) {
    TemplateDocument new_template;
    std::string      error;
    if (load_template(new_template, path, error)) {
      new_template.path = path.filename().string(); // relatif au dossier des modeles
      document_list_.push_back(new_template);
    } else
      ; // TODO: faire un truc avec l'erreur
  }
}

void Editor::rebuild_preview() {
  preview_ = render_template(document_);
  if (texture_ == 0)
    glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  std::vector< unsigned char > rgba(static_cast< std::size_t >(preview_.width * preview_.height * 4));
  for (std::size_t index = 0; index < preview_.pixels.size(); ++index) {
    const unsigned char value = preview_.pixels[index];
    rgba[index * 4]           = value;
    rgba[index * 4 + 1]       = value;
    rgba[index * 4 + 2]       = value;
    rgba[index * 4 + 3]       = 255;
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_.width, preview_.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  texture_width_  = preview_.width;
  texture_height_ = preview_.height;
  preview_dirty_  = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Historique et etat "modifie"

void Editor::reset_history() {
  undo_.clear();
  redo_.clear();
  committed_        = document_;
  committed_serial_ = serialize_template(document_);
  saved_serial_     = committed_serial_;
  dirty_            = false;
}

// Appele a chaque image : une modification terminee (champ quitte, souris relachee) devient une etape
// d'annulation. La comparaison passe par le JSON du modele, qui sert aussi a savoir s'il est enregistre.
void Editor::track_history() {
  const std::string serial = serialize_template(document_);
  dirty_                   = serial != saved_serial_;
  if (serial == committed_serial_ || drag_ != DragMode::None || ImGui::IsAnyItemActive())
    return;
  undo_.push_back(committed_);
  if (undo_.size() > kHistoryLimit)
    undo_.erase(undo_.begin());
  redo_.clear();
  committed_        = document_;
  committed_serial_ = serial;
}

void Editor::undo() {
  track_history();
  if (undo_.empty() || drag_ != DragMode::None)
    return;
  redo_.push_back(committed_);
  document_ = undo_.back();
  undo_.pop_back();
  committed_        = document_;
  committed_serial_ = serialize_template(document_);
  placeholder_names_ = find_placeholders(document_);
  if (selected_element_ >= static_cast< int >(document_.elements.size()))
    selected_element_ = -1;
  preview_dirty_ = true;
}

void Editor::redo() {
  track_history();
  if (redo_.empty() || drag_ != DragMode::None)
    return;
  undo_.push_back(committed_);
  document_ = redo_.back();
  redo_.pop_back();
  committed_        = document_;
  committed_serial_ = serialize_template(document_);
  placeholder_names_ = find_placeholders(document_);
  if (selected_element_ >= static_cast< int >(document_.elements.size()))
    selected_element_ = -1;
  preview_dirty_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Ouverture et enregistrement des modeles

void Editor::load_document(const TemplateDocument &document) {
  document_         = document;
  template_path_    = document.path;
  selected_element_ = -1;
  preview_dirty_    = true;
  fit_pending_      = true;
  refresh_placeholders();
  reset_history();
  message_ = "Modèle « " + document_.name + " » ouvert.";
}

void Editor::request(PendingAction action, int index) {
  pending_       = action;
  pending_index_ = index;
  if (dirty_)
    unsaved_requested_ = true; // demande d'abord quoi faire des modifications
  else
    run_pending();
}

void Editor::run_pending() {
  const PendingAction action = pending_;
  pending_                   = PendingAction::None;
  if (action == PendingAction::New) {
    new_template();
    message_ = "Nouveau modèle.";
  } else if (action == PendingAction::Open && pending_index_ >= 0
             && pending_index_ < static_cast< int >(document_list_.size()))
    load_document(document_list_[static_cast< std::size_t >(pending_index_)]);
}

bool Editor::save() {
  if (template_path_.empty()) {
    save_as_requested_ = true; // jamais enregistre : demande un nom de fichier
    return false;
  }
  return save_to(template_path_);
}

bool Editor::save_to(const std::string &name) {
  const std::string file = qr_file_name(name);
  if (file.empty()) {
    message_ = "Entrez un nom de fichier (ex : item.qr).";
    return false;
  }
  std::string                 error;
  const std::filesystem::path path = resolve_template_path(file);
  if (!save_template(document_, path.string(), error)) {
    message_ = error;
    return false;
  }
  template_path_ = file;
  document_.path = file;
  saved_serial_  = serialize_template(document_);
  dirty_         = false;
  message_       = "Modèle enregistré dans " + path.string();
  reload_templates();
  return true;
}

void Editor::draw_open_popup() {
  const char *title = "Ouvrir un modèle";
  if (open_requested_) {
    reload_templates();
    open_filter_.clear();
    ImGui::OpenPopup(title);
    open_requested_ = false;
  }
  const float font = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(font * 38.0f, font * 24.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title, nullptr))
    return;
  ImGui::TextDisabled("Dossier des modèles : %s", templates_dir().string().c_str());
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##filtre", "Rechercher un modèle (nom ou fichier)", &open_filter_);
  ImGui::BeginChild("modeles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
  if (ImGui::BeginTable("liste", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Fichier", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    const std::string filter = lowercase(open_filter_);
    for (int index = 0; index < static_cast< int >(document_list_.size()); ++index) {
      const TemplateDocument &candidate = document_list_[static_cast< std::size_t >(index)];
      if (!filter.empty() && lowercase(candidate.name).find(filter) == std::string::npos
          && lowercase(candidate.path).find(filter) == std::string::npos)
        continue;
      ImGui::PushID(index);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(candidate.name.c_str(), candidate.path == template_path_, ImGuiSelectableFlags_SpanAllColumns)) {
        ImGui::CloseCurrentPopup();
        request(PendingAction::Open, index);
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(candidate.path.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(category_info(candidate.category).label.c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (document_list_.empty())
    ImGui::TextDisabled("Aucun modèle (*.qr) dans ce dossier.");
  ImGui::EndChild();
  if (button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  if (button("Actualiser la liste"))
    reload_templates();
  ImGui::EndPopup();
}

void Editor::draw_save_as_popup() {
  const char *title = "Enregistrer le modèle sous";
  if (save_as_requested_) {
    save_as_name_ = template_path_.empty() ? slug(document_.name) + ".qr" : template_path_;
    ImGui::OpenPopup(title);
    save_as_requested_ = false;
  }
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::TextUnformatted("Nom du fichier, dans le dossier des modèles :");
  ImGui::TextDisabled("%s", templates_dir().string().c_str());
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
  const bool        enter  = ImGui::InputText("##fichier", &save_as_name_, ImGuiInputTextFlags_EnterReturnsTrue);
  const std::string file   = qr_file_name(save_as_name_);
  std::error_code   error;
  const bool        exists = !file.empty() && file != template_path_
                     && std::filesystem::exists(resolve_template_path(file), error);
  if (exists)
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "%s existe déjà : il sera remplacé.", file.c_str());
  ImGui::BeginDisabled(file.empty());
  if ((button(exists ? "Remplacer" : "Enregistrer") || enter) && !file.empty() && save_to(file)) {
    ImGui::CloseCurrentPopup();
    if (pending_ != PendingAction::None)
      run_pending(); // "Enregistrer" depuis le dialogue des modifications non enregistrees
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pending_ = PendingAction::None;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void Editor::draw_unsaved_popup() {
  const char *title = "Modifications non enregistrées";
  if (unsaved_requested_) {
    ImGui::OpenPopup(title);
    unsaved_requested_ = false;
  }
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::Text("Le modèle « %s » a été modifié.", document_.name.c_str());
  ImGui::TextUnformatted("Enregistrer les modifications avant de continuer ?");
  ImGui::Spacing();
  if (button(template_path_.empty() ? "Enregistrer sous..." : "Enregistrer")) {
    ImGui::CloseCurrentPopup();
    if (template_path_.empty())
      save_as_requested_ = true; // l'action en attente suivra l'enregistrement
    else if (save_to(template_path_))
      run_pending();
    else
      pending_ = PendingAction::None;
  }
  ImGui::SameLine();
  if (button("Ne pas enregistrer")) {
    ImGui::CloseCurrentPopup();
    run_pending();
  }
  ImGui::SameLine();
  if (button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pending_ = PendingAction::None;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void Editor::draw_export_popup() {
  const char *title = "Exporter en PNG";
  if (export_requested_) {
    ImGui::OpenPopup(title);
    export_requested_ = false;
  }
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::TextUnformatted("Image de l'étiquette, telle qu'imprimée :");
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
  const bool enter = ImGui::InputText("##png", &export_path_, ImGuiInputTextFlags_EnterReturnsTrue);
  if (button("Exporter") || enter) {
    std::string error;
    message_ = write_png(preview_, export_path_, error) ? "PNG exporté : " + export_path_ : error;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------------------
// Elements

Editor::Box Editor::element_box(const TemplateElement &element) {
  if (element.kind == ElementKind::Text) {
    const TextElement &text = std::get< TextElement >(element.content);
    return { text.x_mm, text.y_mm, text.width_mm, text.height_mm };
  }
  if (element.kind == ElementKind::QrCode) {
    const QrElement &qr = std::get< QrElement >(element.content);
    return { qr.x_mm, qr.y_mm, qr.size_mm, qr.size_mm };
  }
  const ImageElement &image = std::get< ImageElement >(element.content);
  return { image.x_mm, image.y_mm, image.width_mm, image.height_mm };
}

void Editor::set_element_box(TemplateElement &element, const Box &box) {
  if (element.kind == ElementKind::Text) {
    TextElement &text = std::get< TextElement >(element.content);
    text.x_mm         = box.x;
    text.y_mm         = box.y;
    text.width_mm     = std::max(0.1f, box.w);
    text.height_mm    = std::max(0.1f, box.h);
  } else if (element.kind == ElementKind::QrCode) {
    QrElement &qr = std::get< QrElement >(element.content);
    qr.x_mm       = box.x;
    qr.y_mm       = box.y;
    qr.size_mm    = std::max(kMinQrMm, std::max(box.w, box.h));
  } else {
    ImageElement &image = std::get< ImageElement >(element.content);
    image.x_mm          = box.x;
    image.y_mm          = box.y;
    image.width_mm      = std::max(0.1f, box.w);
    image.height_mm     = std::max(0.1f, box.h);
  }
}

bool Editor::has_selection() const {
  return selected_element_ >= 0 && selected_element_ < static_cast< int >(document_.elements.size());
}

TemplateElement *Editor::selected() {
  return has_selection() ? &document_.elements[static_cast< std::size_t >(selected_element_)] : nullptr;
}

void Editor::select(int index) {
  if (index >= 0 && index != selected_element_)
    focus_element_tab_ = true; // affiche les proprietes de l'element choisi
  selected_element_ = index;
}

std::string Editor::unique_id(const std::string &base) const {
  const auto taken = [this](const std::string &id) {
    return std::any_of(document_.elements.begin(), document_.elements.end(), [&](const TemplateElement &element) {
      return element.id == id;
    });
  };
  if (!taken(base))
    return base;
  for (int suffix = 2;; ++suffix)
    if (!taken(base + "-" + std::to_string(suffix)))
      return base + "-" + std::to_string(suffix);
}

void Editor::add_element(TemplateElement element) {
  element.id = unique_id(element.id);
  document_.elements.push_back(std::move(element));
  select(static_cast< int >(document_.elements.size()) - 1);
  refresh_placeholders();
  preview_dirty_ = true;
}

void Editor::delete_selected() {
  if (!has_selection())
    return;
  document_.elements.erase(document_.elements.begin() + selected_element_);
  selected_element_  = -1;
  placeholder_names_ = find_placeholders(document_);
  preview_dirty_     = true;
}

void Editor::duplicate_selected() {
  if (!has_selection())
    return;
  TemplateElement copy = *selected();
  Box             box  = element_box(copy);
  box.x += 1.0f;
  box.y += 1.0f;
  set_element_box(copy, box);
  add_element(copy);
}

void Editor::copy_selected() {
  if (has_selection())
    clipboard_ = *selected();
}

void Editor::paste() {
  if (!clipboard_)
    return;
  Box box = element_box(*clipboard_);
  box.x += 1.0f; // decale chaque collage pour qu'il ne masque pas le precedent
  box.y += 1.0f;
  set_element_box(*clipboard_, box);
  add_element(*clipboard_);
}

void Editor::move_element(int from, int to) {
  const int count = static_cast< int >(document_.elements.size());
  if (from < 0 || from >= count || to < 0 || to >= count || from == to)
    return;
  TemplateElement element = document_.elements[static_cast< std::size_t >(from)];
  document_.elements.erase(document_.elements.begin() + from);
  document_.elements.insert(document_.elements.begin() + to, std::move(element));
  if (selected_element_ == from)
    selected_element_ = to;
  else if (from < selected_element_ && to >= selected_element_)
    --selected_element_;
  else if (from > selected_element_ && to <= selected_element_)
    ++selected_element_;
  preview_dirty_ = true;
}

void Editor::move_selected(int delta) {
  if (has_selection())
    move_element(selected_element_, selected_element_ + delta);
}

void Editor::align_selected(int horizontal, int vertical) {
  TemplateElement *element = selected();
  if (!element)
    return;
  const float width  = static_cast< float >(document_.media.oriented_width_mm());
  const float height = static_cast< float >(document_.media.oriented_height_mm());
  Box         box    = element_box(*element);
  if (horizontal != 2)
    box.x = horizontal < 0 ? 0.0f : horizontal == 0 ? (width - box.w) / 2.0f : width - box.w;
  if (vertical != 2)
    box.y = vertical < 0 ? 0.0f : vertical == 0 ? (height - box.h) / 2.0f : height - box.h;
  set_element_box(*element, box);
  preview_dirty_ = true;
}

void Editor::nudge_selected(float dx, float dy) {
  TemplateElement *element = selected();
  if (!element)
    return;
  Box box = element_box(*element);
  box.x   = std::round((box.x + dx) * 100.0f) / 100.0f;
  box.y   = std::round((box.y + dy) * 100.0f) / 100.0f;
  set_element_box(*element, box);
  preview_dirty_ = true;
}

void Editor::handle_shortcuts() {
  // seulement quand l'editeur a le focus et qu'aucun dialogue n'est ouvert
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
      || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    return;
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
    save();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S))
    save_as_requested_ = true;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
    request(PendingAction::New);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
    open_requested_ = true;
  if (io.WantTextInput || drag_ != DragMode::None)
    return; // un champ de saisie garde ses propres raccourcis (Ctrl+Z, Suppr, fleches...)
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
    undo();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
    redo();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
    duplicate_selected();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
    copy_selected();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) {
    copy_selected();
    delete_selected();
  }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
    paste();
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
    delete_selected();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    selected_element_ = -1;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Keypad0))
    fit_pending_ = true;
  const ImVec2 center(canvas_size_.x / 2.0f, canvas_size_.y / 2.0f);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd))
    zoom_at(1.25f, center);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract))
    zoom_at(0.8f, center);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
    move_selected(1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
    move_selected(-1);
  // fleches : deplacement fin de l'element (Maj : 1 mm), quand l'etiquette a le focus
  if (canvas_focused_ && has_selection() && !io.KeyCtrl) {
    const float step = io.KeyShift ? 1.0f : 0.1f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
      nudge_selected(-step, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
      nudge_selected(step, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
      nudge_selected(0.0f, -step);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
      nudge_selected(0.0f, step);
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Barre d'outils

void Editor::draw_toolbar() {
  if (button("Nouveau"))
    request(PendingAction::New);
  ImGui::SetItemTooltip("Nouveau modèle (Ctrl+N)");
  ImGui::SameLine();
  if (button("Ouvrir..."))
    open_requested_ = true;
  ImGui::SetItemTooltip("Ouvrir un modèle du dossier des modèles (Ctrl+O)");
  ImGui::SameLine();
  if (button("Enregistrer"))
    save();
  ImGui::SetItemTooltip("Enregistrer le modèle (Ctrl+S)");
  ImGui::SameLine();
  if (button("Enregistrer sous..."))
    save_as_requested_ = true;
  ImGui::SetItemTooltip("Enregistrer sous un autre nom de fichier, par ex. pour partir d'un modèle existant (Ctrl+Maj+S)");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::BeginDisabled(undo_.empty());
  if (button("Annuler"))
    undo();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Annuler la dernière modification (Ctrl+Z)");
  ImGui::SameLine();
  ImGui::BeginDisabled(redo_.empty());
  if (button("Rétablir"))
    redo();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Rétablir (Ctrl+Y)");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (button("Imprimer un test")) {
    open_print_test();
    message_.clear();
  }
  ImGui::SameLine();
  if (button("Exporter PNG..."))
    export_requested_ = true;

  // modele en cours : nom, fichier, etat
  ImGui::AlignTextToFramePadding();
  ImGui::Text("%s", document_.name.c_str());
  ImGui::SameLine();
  if (template_path_.empty())
    ImGui::TextDisabled("(pas encore enregistré)");
  else
    ImGui::TextDisabled("(%s)", template_path_.c_str());
  if (dirty_) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f), "modifié");
  }
  const std::vector< ValidationIssue > issues = validate(document_);
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (!issues.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.1f, 1.0f), "%s", issues.front().message.c_str());
  else
    ImGui::TextDisabled("Modèle valide");
  if (!message_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted(message_.c_str());
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Calques

void Editor::draw_layers_panel() {
  ImGui::BeginChild("Layers", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::SeparatorText("Ajouter");
  const float width  = static_cast< float >(document_.media.oriented_width_mm());
  const float height = static_cast< float >(document_.media.oriented_height_mm());
  const float half   = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
  if (button("Texte", ImVec2(half, 0))) {
    const float box_height = std::min(8.0f, height);
    add_element({ "texte", ElementKind::Text,
                  TextElement{ "Texte", 2.0f, (height - box_height) / 2.0f, std::max(1.0f, width - 4.0f), box_height, 3.0f } });
  }
  ImGui::SetItemTooltip("Zone de texte libre. Les {{placeholders}} sont remplacés à l'impression.");
  ImGui::SameLine();
  if (button("QR code", ImVec2(half, 0))) {
    const float size = std::max(kMinQrMm, std::min(16.0f, std::min(width, height) - 4.0f));
    add_element({ "qr", ElementKind::QrCode, QrElement{ "{{code}}", (width - size) / 2.0f, (height - size) / 2.0f, size } });
  }
  if (button("Image", ImVec2(-FLT_MIN, 0))) {
    const std::string path = image_files_.empty() ? std::string(LABEL_IMAGES_DIR) + "/protection-civile.png" : image_files_.front();
    add_element({ "image", ElementKind::Image, ImageElement{ path, (width - 10.0f) / 2.0f, (height - 10.0f) / 2.0f, 10.0f, 10.0f, true, 128 } });
  }
  ImGui::SetItemTooltip("Logo ou image PNG / JPEG du dossier images/ des modèles.");
  if (button("Nom de l'antenne", ImVec2(-FLT_MIN, 0))) {
    // {{titre}} : nom de l'antenne des Reglages, centre et en gras sur toute la largeur
    TextElement title{ "{{titre}}", 1.0f, 1.0f, std::max(1.0f, width - 2.0f), 8.0f, 3.0f };
    title.bold                    = true;
    title.align                   = TextAlign::Center;
    document_.parameters["titre"] = label_title_;
    add_element({ "nom-antenne", ElementKind::Text, title });
  }
  ImGui::SetItemTooltip("Ajoute le nom de l'antenne, centré et en gras :\n\n%s\n\n"
                        "Ce texte se change dans Gestion > Réglages et s'applique à toutes les étiquettes.",
                        label_title_.c_str());

  ImGui::SeparatorText("Calques");
  ImGui::TextDisabled("En haut de la liste = au premier plan.\nGlissez un calque pour le déplacer.");
  const float buttons_height = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("LayerList", ImVec2(0, -buttons_height));
  for (int index = static_cast< int >(document_.elements.size()) - 1; index >= 0; --index) {
    const TemplateElement &element = document_.elements[static_cast< std::size_t >(index)];
    const char            *kind    = element.kind == ElementKind::Text ? "T" : element.kind == ElementKind::QrCode ? "QR" : "IMG";
    const std::string      label   = std::string(kind) + "\t" + element.id;
    ImGui::PushID(index); // deux elements peuvent avoir le meme identifiant
    if (ImGui::Selectable(label.c_str(), selected_element_ == index))
      select(index);
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("QRPROTEC_LAYER", &index, sizeof(int));
      ImGui::TextUnformatted(label.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("QRPROTEC_LAYER"))
        move_element(*static_cast< const int * >(payload->Data), index);
      ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
  }
  if (document_.elements.empty())
    ImGui::TextDisabled("Étiquette vide.");
  ImGui::EndChild();
  const bool selection = has_selection();
  const float third    = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
  ImGui::BeginDisabled(!selection || selected_element_ + 1 >= static_cast< int >(document_.elements.size()));
  if (button("Avancer", ImVec2(third, 0)))
    move_selected(1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Passer devant l'élément du dessus (Page préc.)");
  ImGui::SameLine();
  ImGui::BeginDisabled(!selection || selected_element_ <= 0);
  if (button("Reculer", ImVec2(third, 0)))
    move_selected(-1);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Passer derrière l'élément du dessous (Page suiv.)");
  ImGui::SameLine();
  ImGui::BeginDisabled(!selection);
  if (button("Dupliquer", ImVec2(third, 0)))
    duplicate_selected();
  ImGui::SetItemTooltip("Ctrl+D");
  if (button("Supprimer", ImVec2(-FLT_MIN, 0)))
    delete_selected();
  ImGui::SetItemTooltip("Suppr");
  ImGui::EndDisabled();
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------------------------------
// Etiquette (canevas)

void Editor::fit_view() {
  if (canvas_size_.x <= 1.0f || canvas_size_.y <= 1.0f)
    return;
  const float margin = 24.0f;
  const float width  = static_cast< float >(std::max(1, preview_.width));
  const float height = static_cast< float >(std::max(1, preview_.height));
  zoom_              = std::clamp(std::min((canvas_size_.x - 2.0f * margin) / width, (canvas_size_.y - 2.0f * margin) / height),
                                  kMinZoom, kMaxZoom);
  offset_            = ImVec2((canvas_size_.x - width * zoom_) / 2.0f, (canvas_size_.y - height * zoom_) / 2.0f);
  fit_pending_       = false;
}

// zoom en gardant fixe le point "anchor" (coordonnees dans le canevas)
void Editor::zoom_at(float factor, ImVec2 anchor) {
  const float zoom = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
  const float ratio = zoom / zoom_;
  offset_           = ImVec2(anchor.x - (anchor.x - offset_.x) * ratio, anchor.y - (anchor.y - offset_.y) * ratio);
  zoom_             = zoom;
}

void Editor::draw_canvas_panel() {
  ImGui::BeginChild("CanvasPanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
  const ImVec2 center(canvas_size_.x / 2.0f, canvas_size_.y / 2.0f);
  if (button("-"))
    zoom_at(0.8f, center);
  ImGui::SetItemTooltip("Dézoomer (Ctrl+-, ou Ctrl+molette)");
  ImGui::SameLine();
  ImGui::Text("%d %%", static_cast< int >(std::round(zoom_ * 100.0f)));
  ImGui::SameLine();
  if (button("+"))
    zoom_at(1.25f, center);
  ImGui::SetItemTooltip("Zoomer (Ctrl++, ou Ctrl+molette)");
  ImGui::SameLine();
  if (button("Ajuster"))
    fit_pending_ = true;
  ImGui::SetItemTooltip("Afficher toute l'étiquette (Ctrl+0)");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::Checkbox("Grille", &show_grid_);
  ImGui::SetItemTooltip("Grille de 1 mm (traits plus marqués tous les 5 mm), visible quand le zoom le permet.");
  ImGui::SameLine();
  ImGui::Checkbox("Magnétisme", &snap_);
  ImGui::SetItemTooltip("Aligne les éléments déplacés sur une grille de 0,5 mm et sur le centre de l'étiquette.\n"
                        "Maintenez Alt pendant le déplacement pour le désactiver.");
  ImGui::SameLine();
  ImGui::Checkbox("Cadres", &show_frames_);
  ImGui::SetItemTooltip("Affiche le cadre de chaque élément (utile pour les zones de texte).");
  draw_canvas();
  // barre d'etat : position du curseur et aide
  if (cursor_valid_)
    ImGui::Text("x %.1f mm   y %.1f mm", cursor_mm_.x, cursor_mm_.y);
  else
    ImGui::TextDisabled("%.0f x %.0f mm", document_.media.oriented_width_mm(), document_.media.oriented_height_mm());
  ImGui::SameLine();
  ImGui::TextDisabled("|  Aide (?)");
  ImGui::SetItemTooltip(
    "Clic : sélectionner   Glisser : déplacer   Poignées : redimensionner\n"
    "Maj pendant le déplacement : un seul axe   Alt : sans magnétisme\n"
    "Flèches : déplacer de 0,1 mm (Maj : 1 mm)   Suppr : supprimer\n"
    "Ctrl+C / Ctrl+V / Ctrl+D : copier, coller, dupliquer\n"
    "Ctrl+Z / Ctrl+Y : annuler, rétablir   Ctrl+S : enregistrer\n"
    "Molette : défiler (Maj : horizontal)   Ctrl+molette : zoom   Ctrl+0 : ajuster\n"
    "Espace + glisser ou clic molette + glisser : déplacer la vue\n"
    "Clic droit : menu (premier plan, centrer...)"
  );
  ImGui::EndChild();
}

void Editor::draw_canvas() {
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                               | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.18f, 0.20f, 1.0f));
  ImGui::BeginChild("Canvas", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders, flags);
  ImGui::PopStyleColor();
  canvas_focused_ = ImGui::IsWindowFocused();
  if (preview_dirty_)
    rebuild_preview();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 size(std::max(50.0f, ImGui::GetContentRegionAvail().x), std::max(50.0f, ImGui::GetContentRegionAvail().y));
  canvas_size_ = size;
  if (fit_pending_)
    fit_view();
  ImGui::InvisibleButton(
    "##etiquette", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle
  );
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // la molette sert au canevas, pas au defilement de la fenetre
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
  const bool     hovered = ImGui::IsItemHovered();
  const ImGuiIO &io      = ImGui::GetIO();
  const ImVec2   mouse   = io.MousePos;

  // molette : defilement (Maj : horizontal), Ctrl : zoom autour du curseur
  if (hovered && drag_ == DragMode::None) {
    if (io.KeyCtrl && io.MouseWheel != 0.0f)
      zoom_at(std::pow(1.15f, io.MouseWheel), ImVec2(mouse.x - origin.x, mouse.y - origin.y));
    else if (!io.KeyCtrl) {
      float wheel_x = io.MouseWheelH;
      float wheel_y = io.MouseWheel;
      if (io.KeyShift) {
        wheel_x += wheel_y;
        wheel_y = 0.0f;
      }
      offset_.x += wheel_x * 40.0f;
      offset_.y += wheel_y * 40.0f;
    }
  }

  const float  ppmm   = static_cast< float >(document_.media.pixels_per_mm);
  const float  scale  = std::max(0.001f, ppmm * zoom_); // pixels ecran par mm
  const ImVec2 label(origin.x + offset_.x, origin.y + offset_.y);
  const float  width  = static_cast< float >(document_.media.oriented_width_mm());
  const float  height = static_cast< float >(document_.media.oriented_height_mm());
  const auto   screen = [&](float x, float y) { return ImVec2(label.x + x * scale, label.y + y * scale); };
  const ImVec2 mouse_mm((mouse.x - label.x) / scale, (mouse.y - label.y) / scale);
  cursor_valid_ = hovered && mouse_mm.x >= 0.0f && mouse_mm.y >= 0.0f && mouse_mm.x <= width && mouse_mm.y <= height;
  cursor_mm_    = mouse_mm;

  const auto element_at = [&](ImVec2 point) {
    for (int index = static_cast< int >(document_.elements.size()) - 1; index >= 0; --index) {
      const Box box = element_box(document_.elements[static_cast< std::size_t >(index)]);
      if (point.x >= box.x && point.x <= box.x + box.w && point.y >= box.y && point.y <= box.y + box.h)
        return index;
    }
    return -1;
  };
  // poignees : 0 haut-gauche, puis sens horaire (1 haut, 2 haut-droite, 3 droite...) ; QR : coins seulement
  const auto handle_point = [&](const Box &box, int handle) {
    const float xs[] = { box.x, box.x + box.w / 2.0f, box.x + box.w, box.x + box.w, box.x + box.w, box.x + box.w / 2.0f, box.x, box.x };
    const float ys[] = { box.y, box.y, box.y, box.y + box.h / 2.0f, box.y + box.h, box.y + box.h, box.y + box.h, box.y + box.h / 2.0f };
    return screen(xs[handle], ys[handle]);
  };
  const auto handle_enabled = [&](int handle) {
    return !(has_selection() && selected()->kind == ElementKind::QrCode && handle % 2 == 1);
  };
  const auto handle_at = [&](ImVec2 point) {
    if (!has_selection())
      return -1;
    const Box box = element_box(*selected());
    for (int handle = 0; handle < 8; ++handle) {
      const ImVec2 at = handle_point(box, handle);
      if (handle_enabled(handle) && std::fabs(point.x - at.x) <= kHandleGrab && std::fabs(point.y - at.y) <= kHandleGrab)
        return handle;
    }
    return -1;
  };

  // debut d'un geste
  const bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;
  if (hovered && drag_ == DragMode::None) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
      drag_        = DragMode::Pan;
      drag_mouse_  = mouse;
      drag_offset_ = offset_;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const int handle = handle_at(mouse);
      if (handle >= 0) {
        drag_        = DragMode::Resize;
        drag_handle_ = handle;
      } else {
        select(element_at(mouse_mm));
        drag_ = has_selection() ? DragMode::Move : DragMode::None;
      }
      if (has_selection())
        drag_box_ = element_box(*selected());
      drag_mouse_ = mouse;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      const int hit = element_at(mouse_mm);
      if (hit >= 0)
        select(hit);
      ImGui::OpenPopup("canvas_menu");
    }
  }

  // geste en cours
  guide_x_ = guide_y_ = false;
  if (drag_ != DragMode::None) {
    const bool down = drag_ == DragMode::Pan ? ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)
                                             : ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const ImVec2 delta(mouse.x - drag_mouse_.x, mouse.y - drag_mouse_.y);
    if (!down)
      drag_ = DragMode::None;
    else if (drag_ == DragMode::Pan)
      offset_ = ImVec2(drag_offset_.x + delta.x, drag_offset_.y + delta.y);
    else if (has_selection() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { // un simple clic ne deplace rien
      const bool  snapping = snap_ && !io.KeyAlt;
      const auto  snapped  = [&](float value) { return snapping ? std::round(value / grid_mm_) * grid_mm_ : value; };
      float       dx       = delta.x / scale;
      float       dy       = delta.y / scale;
      Box         box      = drag_box_;
      if (drag_ == DragMode::Move) {
        if (io.KeyShift) // deplacement sur un seul axe
          (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0.0f;
        box.x = snapped(box.x + dx);
        box.y = snapped(box.y + dy);
        if (snapping) { // centre de l'etiquette, avec un repere
          const float tolerance = 6.0f / scale;
          if (std::fabs(box.x + box.w / 2.0f - width / 2.0f) < tolerance) {
            box.x    = (width - box.w) / 2.0f;
            guide_x_ = true;
          }
          if (std::fabs(box.y + box.h / 2.0f - height / 2.0f) < tolerance) {
            box.y    = (height - box.h) / 2.0f;
            guide_y_ = true;
          }
        }
      } else {
        const int   handle  = drag_handle_;
        const bool  left    = handle == 0 || handle == 6 || handle == 7;
        const bool  right   = handle >= 2 && handle <= 4;
        const bool  top     = handle <= 2;
        const bool  bottom  = handle >= 4 && handle <= 6;
        const float minimum = selected()->kind == ElementKind::QrCode ? kMinQrMm : 1.0f;
        float       x0 = box.x, y0 = box.y, x1 = box.x + box.w, y1 = box.y + box.h;
        if (left)
          x0 = std::min(snapped(x0 + dx), x1 - minimum);
        if (right)
          x1 = std::max(snapped(x1 + dx), x0 + minimum);
        if (top)
          y0 = std::min(snapped(y0 + dy), y1 - minimum);
        if (bottom)
          y1 = std::max(snapped(y1 + dy), y0 + minimum);
        if (selected()->kind == ElementKind::QrCode) { // reste carre, ancre au coin oppose
          const float side = std::max(x1 - x0, y1 - y0);
          (left ? x0 : x1) = left ? x1 - side : x0 + side;
          (top ? y0 : y1)  = top ? y1 - side : y0 + side;
        }
        box = { x0, y0, x1 - x0, y1 - y0 };
      }
      set_element_box(*selected(), box);
      preview_dirty_ = true;
    }
  }
  if (preview_dirty_)
    rebuild_preview(); // l'element suit la souris dans la meme image

  // curseur
  const int hover_handle = drag_ == DragMode::None && hovered ? handle_at(mouse) : -1;
  const int hover_element = drag_ == DragMode::None && hovered && hover_handle < 0 ? element_at(mouse_mm) : -1;
  if (drag_ == DragMode::Pan || (hovered && space))
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  else if (drag_ == DragMode::Resize || hover_handle >= 0) {
    const int handle = drag_ == DragMode::Resize ? drag_handle_ : hover_handle;
    ImGui::SetMouseCursor(handle == 0 || handle == 4   ? ImGuiMouseCursor_ResizeNWSE
                          : handle == 2 || handle == 6 ? ImGuiMouseCursor_ResizeNESW
                          : handle == 1 || handle == 5 ? ImGuiMouseCursor_ResizeNS
                                                       : ImGuiMouseCursor_ResizeEW);
  } else if (drag_ == DragMode::Move || hover_element >= 0)
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  // dessin
  ImDrawList  *draw = ImGui::GetWindowDrawList();
  const ImVec2 label_end(label.x + preview_.width * zoom_, label.y + preview_.height * zoom_);
  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  draw->AddRectFilled(ImVec2(label.x + 4.0f, label.y + 4.0f), ImVec2(label_end.x + 4.0f, label_end.y + 4.0f), IM_COL32(0, 0, 0, 90));
  draw->AddImage(static_cast< ImTextureID >(texture_), label, label_end);
  if (show_grid_) {
    const int step = scale >= 6.0f ? 1 : scale * 5.0f >= 6.0f ? 5 : 0; // pas de grille trop serree
    for (int mm = step; step > 0 && mm < static_cast< int >(std::ceil(width)); mm += step) {
      const ImU32 color = mm % 5 == 0 ? IM_COL32(0, 110, 220, 60) : IM_COL32(0, 110, 220, 25);
      draw->AddLine(screen(static_cast< float >(mm), 0.0f), screen(static_cast< float >(mm), height), color);
    }
    for (int mm = step; step > 0 && mm < static_cast< int >(std::ceil(height)); mm += step) {
      const ImU32 color = mm % 5 == 0 ? IM_COL32(0, 110, 220, 60) : IM_COL32(0, 110, 220, 25);
      draw->AddLine(screen(0.0f, static_cast< float >(mm)), screen(width, static_cast< float >(mm)), color);
    }
  }
  draw->AddRect(label, label_end, IM_COL32(120, 120, 120, 255));
  const ImU32 accent = IM_COL32(0, 120, 215, 255);
  for (int index = 0; index < static_cast< int >(document_.elements.size()); ++index) {
    if (index == selected_element_)
      continue;
    const Box box = element_box(document_.elements[static_cast< std::size_t >(index)]);
    if (index == hover_element)
      draw->AddRect(screen(box.x, box.y), screen(box.x + box.w, box.y + box.h), IM_COL32(0, 120, 215, 200), 0.0f, 0, 2.0f);
    else if (show_frames_)
      draw->AddRect(screen(box.x, box.y), screen(box.x + box.w, box.y + box.h), IM_COL32(0, 120, 215, 70));
  }
  if (has_selection()) {
    const Box box = element_box(*selected());
    draw->AddRect(screen(box.x, box.y), screen(box.x + box.w, box.y + box.h), accent, 0.0f, 0, 2.0f);
    for (int handle = 0; handle < 8; ++handle) {
      if (!handle_enabled(handle))
        continue;
      const ImVec2 at = handle_point(box, handle);
      const float  r  = kHandleSize;
      draw->AddRectFilled(ImVec2(at.x - r, at.y - r), ImVec2(at.x + r, at.y + r), IM_COL32(255, 255, 255, 255));
      draw->AddRect(ImVec2(at.x - r, at.y - r), ImVec2(at.x + r, at.y + r), accent, 0.0f, 0, 1.5f);
    }
    if (drag_ != DragMode::None && drag_ != DragMode::Pan) {
      char info[64];
      std::snprintf(info, sizeof(info), "%.1f ; %.1f  -  %.1f x %.1f mm", box.x, box.y, box.w, box.h);
      const ImVec2 at = screen(box.x, box.y + box.h);
      const ImVec2 text_at(at.x, at.y + 6.0f);
      const ImVec2 text_size = ImGui::CalcTextSize(info);
      draw->AddRectFilled(ImVec2(text_at.x - 4.0f, text_at.y - 2.0f),
                          ImVec2(text_at.x + text_size.x + 4.0f, text_at.y + text_size.y + 2.0f), IM_COL32(20, 20, 20, 210), 3.0f);
      draw->AddText(text_at, IM_COL32(255, 255, 255, 240), info);
    }
  }
  const ImU32 guide = IM_COL32(220, 0, 160, 220);
  if (guide_x_)
    draw->AddLine(screen(width / 2.0f, 0.0f), screen(width / 2.0f, height), guide, 1.5f);
  if (guide_y_)
    draw->AddLine(screen(0.0f, height / 2.0f), screen(width, height / 2.0f), guide, 1.5f);
  draw->PopClipRect();
  draw_canvas_menu();
  ImGui::EndChild();
}

void Editor::draw_canvas_menu() {
  if (!ImGui::BeginPopup("canvas_menu"))
    return;
  const bool selection = has_selection();
  if (ImGui::MenuItem("Dupliquer", "Ctrl+D", false, selection))
    duplicate_selected();
  if (ImGui::MenuItem("Copier", "Ctrl+C", false, selection))
    copy_selected();
  if (ImGui::MenuItem("Coller", "Ctrl+V", false, clipboard_.has_value()))
    paste();
  if (ImGui::MenuItem("Supprimer", "Suppr", false, selection))
    delete_selected();
  ImGui::Separator();
  const int last = static_cast< int >(document_.elements.size()) - 1;
  if (ImGui::MenuItem("Premier plan", nullptr, false, selection && selected_element_ < last))
    move_element(selected_element_, last);
  if (ImGui::MenuItem("Avancer", "Page préc.", false, selection && selected_element_ < last))
    move_selected(1);
  if (ImGui::MenuItem("Reculer", "Page suiv.", false, selection && selected_element_ > 0))
    move_selected(-1);
  if (ImGui::MenuItem("Arrière-plan", nullptr, false, selection && selected_element_ > 0))
    move_element(selected_element_, 0);
  ImGui::Separator();
  if (ImGui::MenuItem("Centrer horizontalement", nullptr, false, selection))
    align_selected(0, 2);
  if (ImGui::MenuItem("Centrer verticalement", nullptr, false, selection))
    align_selected(2, 0);
  ImGui::Separator();
  if (ImGui::MenuItem("Ajuster la vue", "Ctrl+0"))
    fit_pending_ = true;
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------------------
// Proprietes

void Editor::draw_side_panel() {
  ImGui::BeginChild("SidePanel", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::PushItemWidth(-ImGui::GetFontSize() * 8.0f);
  if (ImGui::BeginTabBar("side_tabs")) {
    if (tab_item("Élément", focus_element_tab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
      if (TemplateElement *element = selected())
        draw_properties(*element);
      else
        ImGui::TextWrapped("Cliquez sur un élément de l'étiquette (ou dans les calques) pour le modifier.");
      ImGui::EndTabItem();
    }
    focus_element_tab_ = false;
    if (tab_item("Étiquette")) {
      draw_document_properties();
      ImGui::EndTabItem();
    }
    if (tab_item("Placeholders")) {
      draw_placeholders_panel();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::PopItemWidth();
  ImGui::EndChild();
}

void Editor::draw_document_properties() {
  if (ImGui::InputText("Nom", &document_.name))
    preview_dirty_ = true;
  ImGui::SetItemTooltip("Nom affiché dans la liste des modèles.");
  const CategoryInfo &current = category_info(document_.category);
  if (ImGui::BeginCombo("Usage", current.label.c_str())) {
    for (const CategoryInfo &info : template_categories())
      if (ImGui::Selectable(info.label.c_str(), info.category == document_.category)) {
        document_.category = info.category;
        // valeurs d'exemple pour l'apercu, sans ecraser celles deja saisies
        for (const auto &[name, value] : example_parameters(info.category))
          document_.parameters.emplace(name, value);
        preview_dirty_ = true;
      }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("%s", current.description.c_str());
  ImGui::TextDisabled("Fichier : %s", template_path_.empty() ? "pas encore enregistré" : template_path_.c_str());

  ImGui::SeparatorText("Format");
  float width  = static_cast< float >(document_.media.width_mm);
  float height = static_cast< float >(document_.media.height_mm);
  float ppmm   = static_cast< float >(document_.media.pixels_per_mm);
  if (ImGui::InputFloat("Largeur (mm)", &width, 1.0f, 5.0f, "%.1f")) {
    document_.media.width_mm = std::clamp(width, 1.0f, 300.0f);
    preview_dirty_ = fit_pending_ = true;
  }
  if (ImGui::InputFloat("Hauteur (mm)", &height, 1.0f, 5.0f, "%.1f")) {
    document_.media.height_mm = std::clamp(height, 1.0f, 300.0f);
    preview_dirty_ = fit_pending_ = true;
  }
  int orientation = document_.media.orientation == Orientation::Portrait ? 0 : 1;
  if (ImGui::Combo("Orientation", &orientation, "Portrait\0Paysage\0")) {
    document_.media.orientation = orientation == 0 ? Orientation::Portrait : Orientation::Landscape;
    preview_dirty_ = fit_pending_ = true;
  }
  if (ImGui::InputFloat("Pixels / mm", &ppmm, 1.0f, 1.0f, "%.1f")) {
    document_.media.pixels_per_mm = std::clamp(ppmm, 1.0f, 20.0f);
    preview_dirty_ = fit_pending_ = true;
  }
  ImGui::SetItemTooltip("Résolution de l'imprimante (8 px/mm = 203 dpi pour une Niimbot B1).");
  ImGui::TextDisabled("Résolution : %d x %d px", document_.media.width_pixels(), document_.media.height_pixels());

  ImGui::SeparatorText("Valeurs d'aperçu");
  ImGui::TextDisabled("Remplacent les placeholders dans l'aperçu ;\nà l'impression, ce sont les vraies valeurs.");
  ImGui::PushID("parameters"); // un parametre peut porter le meme nom qu'un element
  for (const std::string &placeholder : placeholder_names_) {
    ImGui::BeginDisabled(placeholder == "titre");
    if (ImGui::InputText(placeholder.c_str(), &document_.parameters[placeholder]))
      preview_dirty_ = true;
    ImGui::EndDisabled();
    if (placeholder == "titre")
      ImGui::SetItemTooltip("Nom de l'antenne, défini dans Gestion > Réglages.");
  }
  if (placeholder_names_.empty())
    ImGui::TextDisabled("Aucun placeholder dans ce modèle.");
  ImGui::PopID();
}

void Editor::draw_properties(TemplateElement &element) {
  ImGui::InputText("Identifiant", &element.id);
  ImGui::SetItemTooltip("Nom du calque (pour s'y retrouver).");
  if (element.kind == ElementKind::Text) {
    TextElement &text = std::get< TextElement >(element.content);
    ImGui::TextUnformatted("Texte (Entrée = nouvelle ligne)");
    if (ImGui::InputTextMultiline("##texte", &text.text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f))) {
      preview_dirty_ = true;
      refresh_placeholders();
    }
  } else if (element.kind == ElementKind::QrCode) {
    QrElement &qr = std::get< QrElement >(element.content);
    ImGui::TextUnformatted("Contenu du QR code");
    if (ImGui::InputTextMultiline("##payload", &qr.payload, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f))) {
      preview_dirty_ = true;
      refresh_placeholders();
    }
  } else {
    ImageElement &image = std::get< ImageElement >(element.content);
    if (!image_files_.empty() && ImGui::BeginCombo("Image", image.path.c_str())) {
      for (const auto &path : image_files_)
        if (ImGui::Selectable(path.c_str(), path == image.path)) {
          image.path     = path;
          preview_dirty_ = true;
        }
      ImGui::EndCombo();
    }
    if (ImGui::InputText("Fichier image", &image.path))
      preview_dirty_ = true;
    ImGui::TextDisabled("PNG ou JPEG, relatif au dossier des modèles :\ndéposez les images dans images/.");
  }

  ImGui::SeparatorText("Position et taille (mm)");
  Box        box     = element_box(element);
  bool       changed = false;
  const bool qr      = element.kind == ElementKind::QrCode;
  changed |= ImGui::InputFloat("X", &box.x, 0.1f, 1.0f, "%.2f");
  changed |= ImGui::InputFloat("Y", &box.y, 0.1f, 1.0f, "%.2f");
  if (qr) {
    if (ImGui::InputFloat("Taille", &box.w, 0.5f, 2.0f, "%.2f")) {
      box.h   = box.w = std::clamp(box.w, kMinQrMm, 100.0f);
      changed = true;
    }
  } else {
    changed |= ImGui::InputFloat("Largeur", &box.w, 0.1f, 1.0f, "%.2f");
    changed |= ImGui::InputFloat("Hauteur", &box.h, 0.1f, 1.0f, "%.2f");
  }
  if (changed) {
    set_element_box(element, box);
    preview_dirty_ = true;
  }
  ImGui::TextUnformatted("Aligner sur l'étiquette");
  ImGui::PushID("aligner"); // memes libelles que l'alignement du texte (Gauche, Droite)
  const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
  if (button("Gauche", ImVec2(third, 0)))
    align_selected(-1, 2);
  ImGui::SameLine();
  if (button("Centre", ImVec2(third, 0)))
    align_selected(0, 2);
  ImGui::SameLine();
  if (button("Droite", ImVec2(third, 0)))
    align_selected(1, 2);
  if (button("Haut", ImVec2(third, 0)))
    align_selected(2, -1);
  ImGui::SameLine();
  if (button("Milieu", ImVec2(third, 0)))
    align_selected(2, 0);
  ImGui::SameLine();
  if (button("Bas", ImVec2(third, 0)))
    align_selected(2, 1);
  ImGui::PopID();

  if (element.kind == ElementKind::Text) {
    TextElement &text = std::get< TextElement >(element.content);
    ImGui::SeparatorText("Texte");
    if (ImGui::InputFloat("Taille police (mm)", &text.font_size_mm, 0.1f, 0.5f, "%.2f")) {
      text.font_size_mm = std::clamp(text.font_size_mm, 0.5f, 30.0f);
      preview_dirty_    = true;
    }
    if (ImGui::Checkbox("Gras", &text.bold))
      preview_dirty_ = true;
    int        align = static_cast< int >(text.align);
    const char *names[] = { "Gauche", "Centré", "Droite" };
    ImGui::TextUnformatted("Alignement du texte");
    ImGui::PushID("alignement_texte");
    for (int option = 0; option < 3; ++option) {
      if (option > 0)
        ImGui::SameLine();
      if (ImGui::RadioButton(names[option], &align, option)) {
        text.align     = static_cast< TextAlign >(align);
        preview_dirty_ = true;
      }
    }
    ImGui::PopID();
  } else if (qr) {
    QrElement &code = std::get< QrElement >(element.content);
    ImGui::SeparatorText("QR code");
    if (ImGui::Combo("Correction", &code.ecc, "L (7 %)\0M (15 %)\0Q (25 %)\0H (30 %)\0"))
      preview_dirty_ = true;
    ImGui::SetItemTooltip("Plus la correction est élevée, plus le QR résiste aux salissures, mais plus il "
                          "contient de modules (donc de petits carrés) pour la même taille.");
    if (ImGui::SliderInt("Marge (modules)", &code.quiet_zone, 0, 4))
      preview_dirty_ = true;
    ImGui::SetItemTooltip("Zone blanche autour du QR, comprise dans sa taille. La norme recommande 4 modules ; "
                          "2 suffisent en général pour une douchette.");
    const int module = qr_module_pixels(code, resolve_parameters(code.payload, document_.parameters),
                                        document_.media.pixels_per_mm);
    if (module >= 3)
      ImGui::TextColored(ImVec4(0.1f, 0.55f, 0.1f, 1.0f), "%d px par module : bonne lisibilité", module);
    else if (module == 2)
      ImGui::TextColored(ImVec4(0.8f, 0.45f, 0.0f, 1.0f), "2 px par module : lisible, sans marge d'erreur");
    else
      ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "QR trop petit pour son contenu");
    ImGui::TextWrapped("Le masque est choisi automatiquement (le meilleur des 8 de la norme).");
  } else {
    ImageElement &image = std::get< ImageElement >(element.content);
    ImGui::SeparatorText("Rendu noir et blanc");
    if (ImGui::Checkbox("Tramage (niveaux de gris)", &image.dither))
      preview_dirty_ = true;
    if (ImGui::SliderInt("Seuil noir/blanc", &image.threshold, 0, 255))
      preview_dirty_ = true;
  }
  ImGui::Separator();
  if (button("Dupliquer"))
    duplicate_selected();
  ImGui::SetItemTooltip("Ctrl+D");
  ImGui::SameLine();
  if (button("Supprimer"))
    delete_selected();
  ImGui::SetItemTooltip("Suppr");
}

void Editor::insert_placeholder(const std::string &name) {
  const std::string token = "{{" + name + "}}";
  if (selected_element_ < 0 || selected_element_ >= static_cast< int >(document_.elements.size())) {
    ImGui::SetClipboardText(token.c_str());
    message_ = token + " copie dans le presse-papier.";
    return;
  }
  TemplateElement &element = document_.elements[static_cast< std::size_t >(selected_element_)];
  if (element.kind == ElementKind::Text)
    std::get< TextElement >(element.content).text += token;
  else if (element.kind == ElementKind::QrCode)
    std::get< QrElement >(element.content).payload = token;
  else {
    ImGui::SetClipboardText(token.c_str());
    message_ = token + " copie dans le presse-papier.";
    return;
  }
  if (document_.parameters.find(name) == document_.parameters.end()) {
    const Parameters examples = example_parameters(document_.category);
    const auto       example  = examples.find(name);
    document_.parameters[name] = example != examples.end() ? example->second : name;
  }
  refresh_placeholders();
  preview_dirty_     = true;
}

void Editor::draw_placeholders_panel() {
  const CategoryInfo &info = category_info(document_.category);
  ImGui::TextWrapped("Usage du modèle : %s. %s", info.label.c_str(), info.description.c_str());
  ImGui::TextDisabled("Cliquez sur un nom pour l'insérer dans l'élément sélectionné (texte : ajouté à la fin, QR : remplacé).");
  const auto table = [this](const char *id, const std::vector< PlaceholderInfo > &placeholders) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
      return;
    ImGui::TableSetupColumn("Placeholder", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    for (const PlaceholderInfo &placeholder : placeholders) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const std::string label = "{{" + placeholder.name + "}}";
      if (ImGui::Selectable(label.c_str()))
        insert_placeholder(placeholder.name);
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", placeholder.description.c_str());
      ImGui::TextDisabled("ex: %s", placeholder.example.c_str());
    }
    ImGui::EndTable();
  };
  if (!info.placeholders.empty()) {
    ImGui::SeparatorText("Spécifiques");
    table("specific", info.placeholders);
  }
  ImGui::SeparatorText("Communs à toutes les étiquettes");
  table("common", common_placeholders());
  if (ImGui::CollapsingHeader("Autres usages")) {
    for (const CategoryInfo &other : template_categories()) {
      if (other.category == document_.category || other.placeholders.empty())
        continue;
      if (ImGui::TreeNode(other.label.c_str())) {
        table(other.id.c_str(), other.placeholders);
        ImGui::TreePop();
      }
    }
  }
}

void Editor::open_print_test() {
  refresh_placeholders();
  print_values_.clear();
  for (const std::string &name : placeholder_names_)
    print_values_[name] = document_.parameters[name];
  print_test_open_          = true;
  print_test_popup_pending_ = true;
}

void Editor::poll_print_task() {
  if (!print_task_.valid() || print_task_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    return;

  const PrintResult result = print_task_.get();
  message_                 = result.success ? "Test imprimé." : "Impression: " + result.error;
  if (result.success)
    print_test_open_ = false;
}

void Editor::draw_print_test_popup() {
  if (!print_test_open_)
    return;
  if (print_test_popup_pending_) {
    ImGui::OpenPopup("Valeurs d'impression");
    print_test_popup_pending_ = false;
  }
  poll_print_task();
  bool open = true;
  if (ImGui::BeginPopupModal("Valeurs d'impression", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Renseignez les valeurs du test avant impression.");
    for (const std::string &name : placeholder_names_) {
      ImGui::InputText(name.c_str(), &print_values_[name]);
    }
    if (print_callback_) {
      // impression par la file de l'application (reglages d'imprimante et d'etiquette communs)
      ImGui::TextDisabled("Imprimante et taille d'étiquette : Gestion > Réglages.");
      if (button("Imprimer")) {
        TemplateDocument test_document = document_;
        test_document.parameters       = print_values_;
        print_callback_(test_document);
        print_test_open_ = false;
      }
    } else {
      ImGui::InputText("Port série", &print_settings_.serial.device);
      int baud = print_settings_.serial.baud_rate;
      if (ImGui::InputInt("Débit", &baud))
        print_settings_.serial.baud_rate = baud;
      int density = print_settings_.density;
      if (ImGui::SliderInt("Densité", &density, 1, 5))
        print_settings_.density = density;
      if (print_task_.valid()) {
        ImGui::TextUnformatted("Impression en cours...");
      } else if (button("Imprimer")) {
        TemplateDocument test_document = document_;
        test_document.parameters       = print_values_;
        const RasterImage   test_image = render_template(test_document);
        const PrintSettings settings   = print_settings_;
        print_task_ = std::async(std::launch::async, [this, image = test_image, media = test_document.media, settings]() {
          std::string error;
          if (!printer_.connect(settings, error))
            return PrintResult{ false, "Connexion imprimante: " + error };

          const PrintRequest request{ image, media, settings };
          const bool         printed = printer_.print(request, {}, error);
          printer_.disconnect();
          return PrintResult{ printed, error };
        });
        message_    = "Impression en cours...";
      }
    }
    // ImGui::SameLine();
    // if (!print_task_.valid() && button("Envoyer #20012")) {
    //   const PrintSettings settings = print_settings_;
    //   print_task_ = std::async(std::launch::async, [this, settings]() {
    //     std::string error;
    //     const bool sent = printer_.send_command(settings, "#20012", error);
    //     return PrintResult{ sent, sent ? "Commande #20012 envoyee." : "Envoi de #20012: " + error };
    //   });
    //   message_ = "Envoi de #20012 en cours...";
    // }
    if (!message_.empty())
      ImGui::TextWrapped("%s", message_.c_str());
    ImGui::SameLine();
    if (!print_task_.valid() && button("Annuler")) {
      print_test_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    if (!print_test_open_ && !print_task_.valid())
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (!open)
    print_test_open_ = false;
}

void Editor::draw() {
  ImGui::Begin("QRProtec - Éditeur d'étiquettes");
  draw_contents();
  ImGui::End();
}

void Editor::draw_contents() {
  if (loaded_templates_dir_ != templates_dir().string())
    reload_templates(); // dossier des modeles change dans les Reglages
  draw_toolbar();
  if (
    ImGui::BeginTable(
      "EditorColumns",
      3,
      ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
    )
  ) {
    ImGui::TableSetupColumn("Calques", ImGuiTableColumnFlags_WidthStretch, 0.85f);
    ImGui::TableSetupColumn("Étiquette", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Propriétés", ImGuiTableColumnFlags_WidthStretch, 1.35f);
    ImGui::TableNextColumn();
    draw_layers_panel();
    ImGui::TableNextColumn();
    draw_canvas_panel();
    ImGui::TableNextColumn();
    draw_side_panel();
    ImGui::EndTable();
  }
  handle_shortcuts();
  draw_open_popup();
  draw_save_as_popup();
  draw_unsaved_popup();
  draw_export_popup();
  draw_print_test_popup();
  track_history();
}

} // namespace qrprotec
