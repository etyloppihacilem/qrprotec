#include "editor.hpp"
#include "../core/glob_utils.hpp"
#include "../core/template_io.hpp"
#include "../core/placeholders.hpp"
#include "core/template.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <GL/gl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace qrprotec {

namespace {
void copy_to_buffer(char *buffer, std::size_t size, const std::string &value) {
  std::strncpy(buffer, value.c_str(), size - 1);
  buffer[size - 1] = '\0';
}
} // namespace

Editor::Editor() {
  reload_templates();
  new_template();
  placeholder_names_ = find_placeholders(document_);
}

Editor::~Editor() {
  if (print_task_.valid())
    print_task_.wait();
  if (texture_ != 0)
    glDeleteTextures(1, &texture_);
}

void Editor::new_template() {
  document_.parameters["code"] = "QRProtec default";
  document_.elements.clear();
  document_.elements.push_back(
    { "title", ElementKind::Text, TextElement{ "{{code}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f } }
  );
  document_.elements.push_back({ "qr", ElementKind::QrCode, QrElement{ "{{code}}", 12.0f, 12.0f, 16.0f } });
  template_path_ = "";
  placeholder_names_ = find_placeholders(document_);
}

void Editor::reload_templates() {
  image_files_.clear();
  for (const char *pattern : { "*.png", "*.jpg", "*.jpeg", "*.PNG", "*.JPG", "*.JPEG" })
    for (const auto &path : glob_current_dir(pattern))
      image_files_.push_back(path);
  qr_files_ = glob_current_dir("*.qr");
  document_list_.clear();
  for (auto path : qr_files_) {
    TemplateDocument new_template;
    std::string      error;
    if (load_template(new_template, path, error)) {
      new_template.path = path;
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

void Editor::draw_document_panel() {
  ImGui::BeginChild("DocumentPanel", ImVec2(0, 0), true);
  ImGui::TextUnformatted("Template");
  ImGui::Separator();
  char name[128];
  copy_to_buffer(name, sizeof(name), document_.name);
  if (ImGui::InputText("Nom", name, sizeof(name))) {
    document_.name = name;
    preview_dirty_ = true;
  }
  char template_path[256];
  copy_to_buffer(template_path, sizeof(template_path), template_path_);
  if (ImGui::InputText("Fichier", template_path, sizeof(template_path)))
    template_path_ = template_path;
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
  float width  = static_cast< float >(document_.media.width_mm);
  float height = static_cast< float >(document_.media.height_mm);
  float ppmm   = static_cast< float >(document_.media.pixels_per_mm);
  if (ImGui::DragFloat("Largeur (mm)", &width, 0.1f, 1.0f, 300.0f)) {
    document_.media.width_mm = width;
    preview_dirty_           = true;
  }
  if (ImGui::DragFloat("Hauteur (mm)", &height, 0.1f, 1.0f, 300.0f)) {
    document_.media.height_mm = height;
    preview_dirty_            = true;
  }
  if (ImGui::DragFloat("Pixels / mm", &ppmm, 0.1f, 1.0f, 20.0f)) {
    document_.media.pixels_per_mm = ppmm;
    preview_dirty_                = true;
  }
  int orientation = document_.media.orientation == Orientation::Portrait ? 0 : 1;
  if (ImGui::Combo("Orientation", &orientation, "Portrait\0Paysage\0")) {
    document_.media.orientation = orientation == 0 ? Orientation::Portrait : Orientation::Landscape;
    preview_dirty_              = true;
  }
  ImGui::Text("Resolution: %d x %d px", document_.media.width_pixels(), document_.media.height_pixels());
  ImGui::Separator();
  ImGui::TextUnformatted("Parametres");
  for (std::string &placeholder : placeholder_names_) {
    char code[256];
    if (document_.parameters.find(placeholder) !=  document_.parameters.end())
      copy_to_buffer(code, 256, document_.parameters[placeholder]);
    else
      copy_to_buffer(code, 256, "default");
    if (ImGui::InputText(placeholder.c_str(), code, sizeof(code))) {
      document_.parameters[placeholder] = code;
      preview_dirty_                    = true;
    }
  }
  // char code[256];
  // copy_to_buffer(code, sizeof(code), document_.parameters["code"]);
  // if (ImGui::InputText("code", code, sizeof(code))) {
  //   document_.parameters["code"] = code;
  //   preview_dirty_               = true;
  // }
  if (ImGui::Button("Ajouter texte")) {
    document_.elements.push_back(
      { "texte-" + std::to_string(document_.elements.size()),
        ElementKind::Text,
        TextElement{ "{{code}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f } }
    );
    selected_element_ = static_cast< int >(document_.elements.size()) - 1;
    preview_dirty_    = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Ajouter QR")) {
    document_.elements.push_back(
      { "qr-" + std::to_string(document_.elements.size()),
        ElementKind::QrCode,
        QrElement{ "{{code}}", 12.0f, 12.0f, 16.0f } }
    );
    selected_element_ = static_cast< int >(document_.elements.size()) - 1;
    preview_dirty_    = true;
  }
  if (ImGui::Button("Ajouter image (logo)")) {
    document_.elements.push_back(
      { "image-" + std::to_string(document_.elements.size()),
        ElementKind::Image,
        ImageElement{ image_files_.empty() ? "logo.png" : image_files_.front().filename().string(), 2.0f, 2.0f, 10.0f, 10.0f, true, 128 } }
    );
    selected_element_ = static_cast< int >(document_.elements.size()) - 1;
    preview_dirty_    = true;
  }
  ImGui::Separator();
  for (int index = 0; index < static_cast< int >(document_.elements.size()); ++index) {
    const bool        selected = selected_element_ == index;
    const std::string label    = document_.elements[static_cast< std::size_t >(index)].id;
    if (ImGui::Selectable(label.c_str(), selected))
      selected_element_ = index;
  }
  ImGui::EndChild();
}

void Editor::draw_element_panel() {
  ImGui::BeginChild("PropertiesPanel", ImVec2(0, 0), true);
  if (ImGui::BeginTabBar("element_tabs")) {
    if (ImGui::BeginTabItem("Proprietes")) {
      if (selected_element_ < 0 || selected_element_ >= static_cast< int >(document_.elements.size()))
        ImGui::TextUnformatted("Selectionnez un element.");
      else
        draw_properties(document_.elements[static_cast< std::size_t >(selected_element_)]);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Placeholders")) {
      draw_placeholders_panel();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
}

void Editor::draw_properties(TemplateElement &element) {
  char             id[128];
  copy_to_buffer(id, sizeof(id), element.id);
  if (ImGui::InputText("Identifiant", id, sizeof(id)))
    element.id = id;
  if (element.kind == ElementKind::Text) {
    TextElement &text = std::get< TextElement >(element.content);
    char         value[4096];
    copy_to_buffer(value, sizeof(value), text.text);
    if (ImGui::InputTextMultiline("Texte", value, sizeof(value), ImVec2(-1, 140))) {
      text.text          = value;
      preview_dirty_     = true;
      placeholder_names_ = find_placeholders(document_);
      // std::cerr << "found" << std::endl;
      // for (auto pl: placeholder_names_)
      //   std::cerr << pl << std::endl;
    }
    if (ImGui::DragFloat("X (mm)", &text.x_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Y (mm)", &text.y_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::InputFloat("Largeur texte (mm)", &text.width_mm, 0.1f, 1.0f, "%.2f")) {
      text.width_mm  = std::max(0.1f, text.width_mm);
      preview_dirty_ = true;
    }
    if (ImGui::InputFloat("Hauteur texte (mm)", &text.height_mm, 0.1f, 1.0f, "%.2f")) {
      text.height_mm = std::max(0.1f, text.height_mm);
      preview_dirty_ = true;
    }
    if (ImGui::DragFloat("Taille police (mm)", &text.font_size_mm, 0.1f, 0.5f, 30.0f))
      preview_dirty_ = true;
  } else if (element.kind == ElementKind::QrCode) {
    QrElement &qr = std::get< QrElement >(element.content);
    char       payload[512];
    copy_to_buffer(payload, sizeof(payload), qr.payload);
    if (ImGui::InputTextMultiline("Payload", payload, sizeof(payload), ImVec2(-1, 70))) {
      qr.payload     = payload;
      preview_dirty_ = true;
      placeholder_names_ = find_placeholders(document_);
    }
    if (ImGui::DragFloat("X (mm)", &qr.x_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Y (mm)", &qr.y_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Taille (mm)", &qr.size_mm, 0.1f, 5.0f, 100.0f))
      preview_dirty_ = true;
    ImGui::TextWrapped("QR code reel (correction M), centre dans la zone. Prevoir une marge blanche autour.");
  } else {
    ImageElement &image = std::get< ImageElement >(element.content);
    if (ImGui::InputText("Fichier image", &image.path))
      preview_dirty_ = true;
    if (!image_files_.empty() && ImGui::BeginCombo("##images", "Choisir une image du dossier")) {
      for (const auto &path : image_files_)
        if (ImGui::Selectable(path.filename().string().c_str())) {
          image.path     = path.filename().string();
          preview_dirty_ = true;
        }
      ImGui::EndCombo();
    }
    ImGui::TextDisabled("PNG ou JPEG (ex: logo de la protection civile), chemin relatif au dossier courant.");
    if (ImGui::DragFloat("X (mm)", &image.x_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Y (mm)", &image.y_mm, 0.1f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Largeur (mm)", &image.width_mm, 0.1f, 1.0f, 300.0f))
      preview_dirty_ = true;
    if (ImGui::DragFloat("Hauteur (mm)", &image.height_mm, 0.1f, 1.0f, 300.0f))
      preview_dirty_ = true;
    if (ImGui::Checkbox("Tramage (niveaux de gris)", &image.dither))
      preview_dirty_ = true;
    if (ImGui::SliderInt("Seuil noir/blanc", &image.threshold, 0, 255))
      preview_dirty_ = true;
  }
  if (ImGui::Button("Supprimer")) {
    document_.elements.erase(document_.elements.begin() + selected_element_);
    selected_element_ = -1;
    preview_dirty_    = true;
  }
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
  placeholder_names_ = find_placeholders(document_);
  preview_dirty_     = true;
}

void Editor::draw_placeholders_panel() {
  const CategoryInfo &info = category_info(document_.category);
  ImGui::TextWrapped("Usage du modele : %s. %s", info.label.c_str(), info.description.c_str());
  ImGui::TextDisabled("Cliquez sur un nom pour l'inserer dans l'element selectionne (texte : ajoute, QR : remplace).");
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
    ImGui::SeparatorText("Specifiques");
    table("specific", info.placeholders);
  }
  ImGui::SeparatorText("Communs a toutes les etiquettes");
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

void Editor::draw_preview_panel() {
  ImGui::BeginChild("PreviewPanel", ImVec2(0, 0), true);
  ImGui::Text("Apercu %d x %d px", preview_.width, preview_.height);
  if (preview_dirty_)
    rebuild_preview();
  const std::vector< ValidationIssue > issues = validate(document_);
  if (!issues.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.1f, 1.0f), "%s", issues.front().message.c_str());
  else
    ImGui::TextUnformatted("Template valide");
  char path[256];
  copy_to_buffer(path, sizeof(path), export_path_);
  if (ImGui::InputText("PNG", path, sizeof(path)))
    export_path_ = path;
  ImGui::SameLine();
  if (ImGui::Button("Exporter PNG")) {
    std::string error;
    message_ = write_png(preview_, export_path_, error) ? "PNG exporte." : error;
  }
  ImGui::SameLine();
  if (ImGui::Button("Imprimer test")) {
    open_print_test();
    message_ = "Test d'impression pret.";
  }
  // ImGui::SameLine();
  // ImGui::SameLine();
  // if (ImGui::Button("Charger")) {
  //   std::string error;
  //   if (load_template(document_, template_path_, error)) {
  //     selected_element_ = -1;
  //     preview_dirty_    = true;
  //     message_          = "Template charge.";
  //   } else
  //     message_ = error;
  // }
  if (!message_.empty())
    ImGui::TextUnformatted(message_.c_str());
  const float available_width = std::max(100.0f, ImGui::GetContentRegionAvail().x - 20.0f);
  const float scale           = std::min(available_width / std::max(1, preview_.width), 2.0f);
  ImGui::Image(static_cast< ImTextureID >(texture_), ImVec2(preview_.width * scale, preview_.height * scale));
  ImGui::EndChild();
}

void Editor::open_print_test() {
  placeholder_names_ = find_placeholders(document_);
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
  message_                 = result.success ? "Test imprime." : "Impression: " + result.error;
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
      std::string &value = print_values_[name];
      char         buffer[512];
      copy_to_buffer(buffer, sizeof(buffer), value);
      if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer)))
        value = buffer;
    }
    char device[256];
    copy_to_buffer(device, sizeof(device), print_settings_.serial.device);
    if (ImGui::InputText("Port serie", device, sizeof(device)))
      print_settings_.serial.device = device;
    int baud = print_settings_.serial.baud_rate;
    if (ImGui::InputInt("Debit", &baud))
      print_settings_.serial.baud_rate = baud;
    int density = print_settings_.density;
    if (ImGui::SliderInt("Densite", &density, 1, 5))
      print_settings_.density = density;
    if (print_task_.valid()) {
      ImGui::TextUnformatted("Impression en cours...");
    } else if (ImGui::Button("Imprimer")) {
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
    // ImGui::SameLine();
    // if (!print_task_.valid() && ImGui::Button("Envoyer #20012")) {
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
    if (!print_task_.valid() && ImGui::Button("Annuler")) {
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
  ImGui::Begin("QRProtec - Editeur de templates", nullptr, ImGuiWindowFlags_MenuBar);
  draw_contents();
  ImGui::End();
}

void Editor::draw_contents() {
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::BeginMenu("Open")) {
        for (TemplateDocument doc : document_list_)
          if (ImGui::MenuItem(doc.name.c_str())) {
            document_      = doc;
            template_path_ = doc.path;
            preview_dirty_ = true;
            placeholder_names_ = find_placeholders(document_);
            std::cerr << "Loaded template " << doc.name << std::endl;
          }
        ImGui::Separator();
        if (ImGui::MenuItem("Reload templates")) {
          reload_templates();
          std::cerr << "Template reloaded" << std::endl;
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Save", "Ctrl+S")) {
        std::string error;
        if (template_path_ != "")
          message_ = save_template(document_, template_path_, error) ? "Template enregistre." : error;
        if (template_path_ != "")
          reload_templates();
        else
          message_ = "Entrez un nom de fichier terminant par .qr.";
      }
      if (ImGui::MenuItem("New", "Ctrl+N")) {
        new_template();
        preview_dirty_ = true;
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }
  if (
    ImGui::BeginTable(
      "EditorColumns",
      3,
      ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
    )
  ) {
    ImGui::TableSetupColumn("Template", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Proprietes", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Apercu", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableNextColumn();
    draw_document_panel();
    ImGui::TableNextColumn();
    draw_element_panel();
    ImGui::TableNextColumn();
    draw_preview_panel();
    ImGui::EndTable();
  }
  draw_print_test_popup();
}

} // namespace qrprotec
