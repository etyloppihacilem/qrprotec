/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          label_preview.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 18:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "label_preview.hpp"

#include "../app/app.hpp"
#include "../render/raster.hpp"
#include "widgets.hpp"

#include <GL/gl.h>
#include <algorithm>
#include <vector>

namespace qrprotec {

namespace {

struct PreviewTexture {
    GLuint      id     = 0;
    int         width  = 0;
    int         height = 0;
    bool        rotated = false; // tourne d'un quart de tour a l'impression
    std::string error;
};

PreviewTexture texture;

void upload(App &app) {
  const PrintJob   &job   = app.preview.jobs[static_cast< std::size_t >(app.preview.index)];
  const RasterImage image = render_template(job.document);
  std::vector< unsigned char > rgba(image.pixels.size() * 4);
  for (std::size_t index = 0; index < image.pixels.size(); ++index) {
    rgba[index * 4] = rgba[index * 4 + 1] = rgba[index * 4 + 2] = image.pixels[index];
    rgba[index * 4 + 3]                                          = 255;
  }
  if (texture.id == 0)
    glGenTextures(1, &texture.id);
  glBindTexture(GL_TEXTURE_2D, texture.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  texture.width  = image.width;
  texture.height = image.height;
  // meme adaptation qu'a l'impression, pour prevenir si l'etiquette sera tournee ou ne tient pas
  PhysicalLabel label   = app.settings.physical_label();
  label.pixels_per_mm   = job.document.media.pixels_per_mm;
  RasterImage   fitted;
  MediaSettings media;
  texture.error.clear();
  texture.rotated = fit_to_label(image, label, fitted, media, texture.error) && fitted.width != image.width;
  app.preview.dirty = false;
}

} // namespace

void draw_label_preview(App &app) {
  LabelPreviewState &preview = app.preview;
  if (!preview.open || preview.jobs.empty())
    return;
  preview.index = std::clamp(preview.index, 0, static_cast< int >(preview.jobs.size()) - 1);
  if (preview.dirty)
    upload(app);

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (preview.focus) {
    ImGui::SetNextWindowFocus();
    preview.focus = false;
  }
  bool open = true;
  if (ImGui::Begin("Aperçu d'étiquette###label_preview", &open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    const PrintJob &job = preview.jobs[static_cast< std::size_t >(preview.index)];
    ImGui::TextUnformatted(job.description.c_str());
    ImGui::TextDisabled("Modèle : %s – %.0f × %.0f mm", job.document.name.c_str(),
                        job.document.media.oriented_width_mm(), job.document.media.oriented_height_mm());

    // l'etiquette a une taille lisible a l'ecran (au plus 480 px de large)
    const float scale = std::min(3.0f, 480.0f / static_cast< float >(std::max(1, texture.width)));
    const ImVec2 size(texture.width * scale, texture.height * scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(origin.x - 1, origin.y - 1), ImVec2(origin.x + size.x + 1, origin.y + size.y + 1),
                                        ImGui::GetColorU32(ImVec4(0.4f, 0.4f, 0.4f, 1.0f)));
    ImGui::Image(static_cast< ImTextureID >(texture.id), size);

    if (!texture.error.empty())
      ImGui::TextColored(colors::red, "%s", texture.error.c_str());
    else if (texture.rotated)
      ImGui::TextDisabled("Sera tournée d'un quart de tour pour l'étiquette chargée (%.0f × %.0f mm).",
                          app.settings.label_width_mm, app.settings.label_height_mm);

    const int count = static_cast< int >(preview.jobs.size());
    if (count > 1) {
      ImGui::BeginDisabled(preview.index == 0);
      if (ImGui::ArrowButton("##previous", ImGuiDir_Left)) {
        --preview.index;
        preview.dirty = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Text("Étiquette %d / %d", preview.index + 1, count);
      ImGui::SameLine();
      ImGui::BeginDisabled(preview.index + 1 >= count);
      if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
        ++preview.index;
        preview.dirty = true;
      }
      ImGui::EndDisabled();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!texture.error.empty());
    if (count > 1) {
      const std::string all = "Imprimer les " + std::to_string(count) + " étiquettes";
      if (primary_button(all.c_str())) {
        app.print_documents(preview.jobs);
        open = false;
      }
      ImGui::SameLine();
      if (ImGui::Button("Imprimer celle-ci"))
        app.print_documents({ job });
    } else if (primary_button("Imprimer")) {
      app.print_documents(preview.jobs);
      open = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fermer"))
      open = false;
  }
  ImGui::End();
  if (!open) {
    preview.open = false;
    preview.jobs.clear();
  }
}

} // namespace qrprotec
