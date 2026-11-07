/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          label_prints.cpp
        -\-    _|__
         |\___/  . \        Created on 07 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Fenetre « Impressions en attente » : etiquettes d'items demandees depuis le front web par un admin (Gestion de la
// TECH, bouton +), voir database/inventory/label_prints.py. Elle s'ouvre d'elle-meme a chaque nouvelle demande, meme
// sans personne de connecte : un secouriste qui rentre de poste avec du reassort lance l'impression et colle lui-meme
// les etiquettes. Les items sont crees par le serveur au moment ou l'on lance l'impression, au nom de l'admin.

#include "../core/plural.hpp"
#include "../ui/widgets.hpp"
#include "app.hpp"
#include "labels.hpp"

#include "imgui.h"

#include <algorithm>
#include <memory>

namespace qrprotec {

namespace {

constexpr double kPollSeconds = 20.0;

std::string request_what(const Json &request) {
  std::string text = std::to_string(request["count"].integer()) + " × " + request["type_name"].str();
  if (!request["peremption"].is_null())
    text += ", péremption " + display_date(request["peremption"]);
  return text;
}

} // namespace

void App::refresh_label_prints() {
  label_prints_.loading   = true;
  label_prints_.next_poll = ImGui::GetTime() + kPollSeconds;
  api.get("/api/label-prints/pending/", [this](const ApiResult &result) {
    label_prints_.loading = false;
    if (!result.ok)
      return; // serveur injoignable ou ancien serveur : rien a montrer, on reessaie au prochain tour
    label_prints_.data = result.data;
    bool fresh         = false;
    for (const Json &request : result.data["pending"].items())
      fresh |= label_prints_.seen.insert(request["id"].integer()).second;
    if (fresh)
      open_label_prints();
  });
}

void App::print_label_request(const Json &request) {
  const Json               &items = request["items"];
  std::vector< Parameters > labels;
  const int                 count = static_cast< int >(items.size());
  for (int index = 0; index < count; ++index) {
    Parameters parameters = item_parameters(items[index], index + 1, count);
    parameters["printed_by"] = request["by"].str(); // l'admin qui les a demandees (personne n'est forcement connecte)
    labels.push_back(std::move(parameters));
  }
  if (!labels.empty())
    print_labels(TemplateCategory::Item, labels, request["type_name"].str());
}

// Demandes envoyees une par une : le serveur cree les items de chacune, puis leurs etiquettes partent a l'imprimante
void App::launch_label_prints(std::vector< int > ids) {
  if (ids.empty()) {
    label_prints_.busy = false;
    refresh_label_prints();
    return;
  }
  label_prints_.busy = true;
  const int id       = ids.front();
  ids.erase(ids.begin());
  api.post("/api/label-prints/" + std::to_string(id) + "/print/", Json::object(),
           [this, ids](const ApiResult &result) {
             if (result.ok)
               print_label_request(result.data);
             else
               notify("Impression refusée : " + result.error, true);
             launch_label_prints(ids);
           });
}

void App::draw_label_prints() {
  if (!label_prints_.open)
    return;
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, viewport->WorkSize.x * 0.9f), std::min(520.0f, viewport->WorkSize.y * 0.8f)),
                           ImGuiCond_Appearing);
  if (label_prints_.focus) {
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowCollapsed(false);
    label_prints_.focus = false;
  }
  if (!ImGui::Begin("Impressions en attente###label_prints", &label_prints_.open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  const Json  &pending = label_prints_.data["pending"];
  const Json  &recent  = label_prints_.data["recent"];
  const float  base    = ImGui::GetStyle().FontSizeBase;
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted("Étiquettes d'items demandées par le TECH depuis le web. Lancez l'impression, puis collez "
                         "chaque étiquette sur un item de ce type : les items sont ajoutés au stock.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  int labels = 0;
  std::vector< int > ids;
  for (const Json &request : pending.items()) {
    labels += request["count"].integer();
    ids.push_back(request["id"].integer());
  }
  if (pending.size() == 0) {
    ImGui::TextColored(colors::green, "Aucune impression en attente.");
  } else if (ImGui::BeginTable("label_prints_pending", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn("Étiquettes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Demandé par", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Quand", ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableHeadersRow();
    for (const Json &request : pending.items()) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushFont(nullptr, base * 1.2f);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::Text("%d × %s", request["count"].integer(), request["type_name"].str().c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(request["peremption"].is_null() ? "-" : display_date(request["peremption"]).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(request["by"].str().c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(display_datetime(request["created"]).c_str());
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  if (ids.empty() && !label_prints_.busy) {
    if (button("Fermer"))
      label_prints_.open = false;
  } else {
    ImGui::BeginDisabled(label_prints_.busy);
    const std::string launch = label_prints_.busy ? std::string("Impression en cours...")
                                                  : "Lancer l'impression (" + nb(labels, "étiquette") + ")";
    ImGui::PushFont(nullptr, base * 1.3f);
    if (primary_button(launch.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.6f)))
      launch_label_prints(ids);
    ImGui::PopFont();
    ImGui::EndDisabled();
    if (button("Plus tard"))
      label_prints_.open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("La demande reste en attente (bouton bleu de la barre de menu).");
  }

  // imprimees dans les dernieres 24 h : reimpression si une etiquette a ete abimee ou le rouleau a coince
  if (recent.size() > 0) {
    ImGui::SeparatorText("Imprimées dans les dernières 24 h");
    for (const Json &request : recent.items()) {
      ImGui::PushID(request["id"].integer());
      if (small_button("Réimprimer"))
        print_label_request(request);
      ImGui::SameLine();
      ImGui::Text("%s", request_what(request).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("imprimées %s", display_on(request["printed"]).c_str());
      ImGui::PopID();
    }
  }
  ImGui::End();
}

} // namespace qrprotec
