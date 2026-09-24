#include "editor.hpp"
#include "../core/template_io.hpp"

#include "imgui.h"

#include <GL/gl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>

namespace qrprotec {

namespace {
void copy_to_buffer(char* buffer, std::size_t size, const std::string& value)
{
    std::strncpy(buffer, value.c_str(), size - 1);
    buffer[size - 1] = '\0';
}
}

Editor::Editor()
{
    document_.parameters["code"] = "QRProtec";
    document_.elements.push_back({"title", ElementKind::Text, TextElement{"{{code}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f}});
    document_.elements.push_back({"qr", ElementKind::QrCode, QrElement{"{{code}}", 12.0f, 12.0f, 16.0f}});
}

Editor::~Editor()
{
    if (print_task_.valid())
        print_task_.wait();
    if (texture_ != 0)
        glDeleteTextures(1, &texture_);
}

void Editor::rebuild_preview()
{
    preview_ = render_template(document_);
    if (texture_ == 0)
        glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::vector<unsigned char> rgba(static_cast<std::size_t>(preview_.width * preview_.height * 4));
    for (std::size_t index = 0; index < preview_.pixels.size(); ++index) {
        const unsigned char value = preview_.pixels[index];
        rgba[index * 4] = value;
        rgba[index * 4 + 1] = value;
        rgba[index * 4 + 2] = value;
        rgba[index * 4 + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_.width, preview_.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    texture_width_ = preview_.width;
    texture_height_ = preview_.height;
    preview_dirty_ = false;
}

void Editor::draw_document_panel()
{
    ImGui::BeginChild("DocumentPanel", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Template");
    ImGui::Separator();
    char name[128];
    copy_to_buffer(name, sizeof(name), document_.name);
    if (ImGui::InputText("Nom", name, sizeof(name))) {
        document_.name = name;
        preview_dirty_ = true;
    }
    float width = static_cast<float>(document_.media.width_mm);
    float height = static_cast<float>(document_.media.height_mm);
    float ppmm = static_cast<float>(document_.media.pixels_per_mm);
    if (ImGui::DragFloat("Largeur (mm)", &width, 0.1f, 1.0f, 300.0f)) { document_.media.width_mm = width; preview_dirty_ = true; }
    if (ImGui::DragFloat("Hauteur (mm)", &height, 0.1f, 1.0f, 300.0f)) { document_.media.height_mm = height; preview_dirty_ = true; }
    if (ImGui::DragFloat("Pixels / mm", &ppmm, 0.1f, 1.0f, 20.0f)) { document_.media.pixels_per_mm = ppmm; preview_dirty_ = true; }
    int orientation = document_.media.orientation == Orientation::Portrait ? 0 : 1;
    if (ImGui::Combo("Orientation", &orientation, "Portrait\0Paysage\0")) {
        document_.media.orientation = orientation == 0 ? Orientation::Portrait : Orientation::Landscape;
        preview_dirty_ = true;
    }
    ImGui::Text("Resolution: %d x %d px", document_.media.width_pixels(), document_.media.height_pixels());
    ImGui::Separator();
    ImGui::TextUnformatted("Parametres");
    char code[256];
    copy_to_buffer(code, sizeof(code), document_.parameters["code"]);
    if (ImGui::InputText("code", code, sizeof(code))) {
        document_.parameters["code"] = code;
        preview_dirty_ = true;
    }
    if (ImGui::Button("Ajouter texte")) {
        document_.elements.push_back({"texte-" + std::to_string(document_.elements.size()), ElementKind::Text,
                          TextElement{"{{code}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f}});
        selected_element_ = static_cast<int>(document_.elements.size()) - 1;
        preview_dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Ajouter QR")) {
        document_.elements.push_back({"qr-" + std::to_string(document_.elements.size()), ElementKind::QrCode,
                                      QrElement{"{{code}}", 12.0f, 12.0f, 16.0f}});
        selected_element_ = static_cast<int>(document_.elements.size()) - 1;
        preview_dirty_ = true;
    }
    ImGui::Separator();
    for (int index = 0; index < static_cast<int>(document_.elements.size()); ++index) {
        const bool selected = selected_element_ == index;
        const std::string label = document_.elements[static_cast<std::size_t>(index)].id;
        if (ImGui::Selectable(label.c_str(), selected))
            selected_element_ = index;
    }
    ImGui::EndChild();
}

void Editor::draw_element_panel()
{
    ImGui::BeginChild("PropertiesPanel", ImVec2(0, 0), true);
    ImGui::TextUnformatted("Proprietes");
    ImGui::Separator();
    if (selected_element_ < 0 || selected_element_ >= static_cast<int>(document_.elements.size())) {
        ImGui::TextUnformatted("Selectionnez un element.");
        ImGui::EndChild();
        return;
    }
    TemplateElement& element = document_.elements[static_cast<std::size_t>(selected_element_)];
    char id[128];
    copy_to_buffer(id, sizeof(id), element.id);
    if (ImGui::InputText("Identifiant", id, sizeof(id)))
        element.id = id;
    if (element.kind == ElementKind::Text) {
        TextElement& text = std::get<TextElement>(element.content);
        char value[4096];
        copy_to_buffer(value, sizeof(value), text.text);
        if (ImGui::InputTextMultiline("Texte", value, sizeof(value), ImVec2(-1, 140))) { text.text = value; preview_dirty_ = true; }
        if (ImGui::DragFloat("X (mm)", &text.x_mm, 0.1f)) preview_dirty_ = true;
        if (ImGui::DragFloat("Y (mm)", &text.y_mm, 0.1f)) preview_dirty_ = true;
        if (ImGui::InputFloat("Largeur texte (mm)", &text.width_mm, 0.1f, 1.0f, "%.2f")) { text.width_mm = std::max(0.1f, text.width_mm); preview_dirty_ = true; }
        if (ImGui::InputFloat("Hauteur texte (mm)", &text.height_mm, 0.1f, 1.0f, "%.2f")) { text.height_mm = std::max(0.1f, text.height_mm); preview_dirty_ = true; }
        if (ImGui::DragFloat("Taille police (mm)", &text.font_size_mm, 0.1f, 0.5f, 30.0f)) preview_dirty_ = true;
    } else {
        QrElement& qr = std::get<QrElement>(element.content);
        char payload[512];
        copy_to_buffer(payload, sizeof(payload), qr.payload);
        if (ImGui::InputTextMultiline("Payload", payload, sizeof(payload), ImVec2(-1, 70))) { qr.payload = payload; preview_dirty_ = true; }
        if (ImGui::DragFloat("X (mm)", &qr.x_mm, 0.1f)) preview_dirty_ = true;
        if (ImGui::DragFloat("Y (mm)", &qr.y_mm, 0.1f)) preview_dirty_ = true;
        if (ImGui::DragFloat("Taille (mm)", &qr.size_mm, 0.1f, 5.0f, 100.0f)) preview_dirty_ = true;
        ImGui::TextWrapped("Apercu QR diagnostique : l'encodeur QR reel sera branche dans le module QR.");
    }
    if (ImGui::Button("Supprimer")) {
        document_.elements.erase(document_.elements.begin() + selected_element_);
        selected_element_ = -1;
        preview_dirty_ = true;
    }
    ImGui::EndChild();
}

void Editor::draw_preview_panel()
{
    ImGui::BeginChild("PreviewPanel", ImVec2(0, 0), true);
    ImGui::Text("Apercu %d x %d px", preview_.width, preview_.height);
    if (preview_dirty_)
        rebuild_preview();
    const std::vector<ValidationIssue> issues = validate(document_);
    if (!issues.empty())
        ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.1f, 1.0f), "%s", issues.front().message.c_str());
    else
        ImGui::TextUnformatted("Template valide");
    char path[256];
    copy_to_buffer(path, sizeof(path), export_path_);
    if (ImGui::InputText("PNG", path, sizeof(path))) export_path_ = path;
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
    char template_path[256];
    copy_to_buffer(template_path, sizeof(template_path), template_path_);
    if (ImGui::InputText("Template", template_path, sizeof(template_path))) template_path_ = template_path;
    ImGui::SameLine();
    if (ImGui::Button("Enregistrer")) {
        std::string error;
        message_ = save_template(document_, template_path_, error) ? "Template enregistre." : error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Charger")) {
        std::string error;
        if (load_template(document_, template_path_, error)) { selected_element_ = -1; preview_dirty_ = true; message_ = "Template charge."; }
        else message_ = error;
    }
    if (!message_.empty())
        ImGui::TextUnformatted(message_.c_str());
    const float available_width = std::max(100.0f, ImGui::GetContentRegionAvail().x - 20.0f);
    const float scale = std::min(available_width / std::max(1, preview_.width), 2.0f);
    ImGui::Image(static_cast<ImTextureID>(texture_),
                 ImVec2(preview_.width * scale, preview_.height * scale));
    ImGui::EndChild();
}

void Editor::open_print_test()
{
    placeholder_names_ = find_placeholders(document_);
    print_values_.clear();
    for (const std::string& name : placeholder_names_)
        print_values_[name] = document_.parameters[name];
    print_test_open_ = true;
    print_test_popup_pending_ = true;
}

void Editor::poll_print_task()
{
    if (!print_task_.valid() || print_task_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;

    const PrintResult result = print_task_.get();
    message_ = result.success ? "Test imprime." : "Impression: " + result.error;
    if (result.success)
        print_test_open_ = false;
}

void Editor::draw_print_test_popup()
{
    if (!print_test_open_) return;
    if (print_test_popup_pending_) {
        ImGui::OpenPopup("Valeurs d'impression");
        print_test_popup_pending_ = false;
    }
    poll_print_task();
    bool open = true;
    if (ImGui::BeginPopupModal("Valeurs d'impression", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Renseignez les valeurs du test avant impression.");
        for (const std::string& name : placeholder_names_) {
            std::string& value = print_values_[name];
            char buffer[512];
            copy_to_buffer(buffer, sizeof(buffer), value);
            if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer))) value = buffer;
        }
        char device[256];
        copy_to_buffer(device, sizeof(device), print_settings_.serial.device);
        if (ImGui::InputText("Port serie", device, sizeof(device))) print_settings_.serial.device = device;
        int baud = print_settings_.serial.baud_rate;
        if (ImGui::InputInt("Debit", &baud)) print_settings_.serial.baud_rate = baud;
        int density = print_settings_.density;
        if (ImGui::SliderInt("Densite", &density, 1, 5)) print_settings_.density = density;
        if (print_task_.valid()) {
            ImGui::TextUnformatted("Impression en cours...");
        } else if (ImGui::Button("Imprimer")) {
            TemplateDocument test_document = document_;
            test_document.parameters = print_values_;
            const RasterImage test_image = render_template(test_document);
            const PrintSettings settings = print_settings_;
            print_task_ = std::async(std::launch::async, [this, image = test_image, media = test_document.media, settings]() {
                std::string error;
                if (!printer_.connect(settings, error))
                    return PrintResult{false, "Connexion imprimante: " + error};

                const PrintRequest request{image, media, settings};
                const bool printed = printer_.print(request, {}, error);
                printer_.disconnect();
                return PrintResult{printed, error};
            });
            message_ = "Impression en cours...";
        }
        if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
        ImGui::SameLine();
        if (!print_task_.valid() && ImGui::Button("Annuler")) {
            print_test_open_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (!print_test_open_ && !print_task_.valid())
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!open) print_test_open_ = false;
}

void Editor::draw()
{
    ImGui::Begin("QRProtec - Editeur de templates");
    if (ImGui::BeginTable("EditorColumns", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
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
    ImGui::End();
}

} // namespace qrprotec
