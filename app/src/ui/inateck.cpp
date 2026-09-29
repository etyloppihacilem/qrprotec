/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inateck.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 11:29
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "inateck.hpp"
#include "imgui.h"
#include "inateck/inateck_worker.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace qrprotec {

Inateck::Inateck() {}

Inateck::~Inateck() {}

void Inateck::draw_inateck_window() {
  if (!inateck_window_open_)
    return;
  bool open = true;
  if (!ImGui::Begin("Douchette Inateck", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    inateck_window_open_ = open;
    return;
  }
  const InateckSnapshot state = inateck_worker_.snapshot();
  ImGui::Text("SDK: %s", state.sdk_available ? "disponible" : "indisponible");
  if (!state.error.empty())
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.2f, 1.0f), "%s", state.error.c_str());

  if (ImGui::CollapsingHeader("Connexion", ImGuiTreeNodeFlags_DefaultOpen)) {
    const char* status = state.authenticated ? "connectee et authentifiee" : state.connected ? "connectee" : "non connectee";
    ImGui::Text("Statut : %s", status);
    if (ImGui::Button(state.discovering ? "Arreter la recherche" : "Rechercher les douchettes")) {
      if (state.discovering)
        inateck_worker_.stop_discovery();
      else
        inateck_worker_.start_discovery();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rafraichir"))
      inateck_worker_.start_discovery();
    static int selected_device = 0;
    std::vector<const char*> device_names;
    for (const InateckDevice& device : state.devices)
      device_names.push_back(device.name.empty() ? device.id.c_str() : device.name.c_str());
    if (!device_names.empty()) {
      selected_device = std::min(selected_device, static_cast<int>(device_names.size()) - 1);
      ImGui::Combo("Appareil", &selected_device, device_names.data(), static_cast<int>(device_names.size()));
      const InateckDevice& device = state.devices[static_cast<std::size_t>(selected_device)];
      ImGui::TextWrapped("ID : %s", device.id.c_str());
      if (!state.connected && ImGui::Button("Connecter"))
        inateck_worker_.connect(device.id, device.name);
    } else {
      ImGui::TextUnformatted("Aucun appareil decouvert.");
    }
    if (state.connected) {
      ImGui::SameLine();
      if (ImGui::Button("Deconnecter"))
        inateck_worker_.disconnect();
    }
  }

  if (ImGui::CollapsingHeader("Parametrage", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::BeginDisabled(!state.authenticated);
    if (ImGui::SliderInt("Volume", &inateck_volume_, 0, 3) && ImGui::IsItemDeactivatedAfterEdit())
      inateck_worker_.set_volume(inateck_volume_);
    if (ImGui::Checkbox("Vibration", &inateck_vibration_) && ImGui::IsItemDeactivatedAfterEdit())
      inateck_worker_.set_vibration(inateck_vibration_);
    ImGui::Checkbox("Recevoir les scans dans QRProtec", &inateck_sdk_output_);
    if (ImGui::Button("Appliquer le mode de sortie"))
      inateck_worker_.set_sdk_output(inateck_sdk_output_);
    ImGui::Separator();
    ImGui::InputText("Prefixe", inateck_prefix_, sizeof(inateck_prefix_));
    ImGui::SameLine();
    if (ImGui::Button("Appliquer##prefixe"))
      inateck_worker_.set_prefix(inateck_prefix_);
    ImGui::InputText("Suffixe", inateck_suffix_, sizeof(inateck_suffix_));
    ImGui::SameLine();
    if (ImGui::Button("Appliquer##suffixe"))
      inateck_worker_.set_suffix(inateck_suffix_);
    ImGui::InputText("Nom Bluetooth", inateck_name_, sizeof(inateck_name_));
    ImGui::SameLine();
    if (ImGui::Button("Appliquer##nom"))
      inateck_worker_.set_name(inateck_name_);
    if (ImGui::Button("Relire les parametres"))
      inateck_worker_.refresh_settings();
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Le volume et la vibration utilisent les flags ST23 du SDK.");
  }

  if (ImGui::CollapsingHeader("Dernier scan", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (state.last_scan.empty())
      ImGui::TextUnformatted("Aucun code recu.");
    else
      ImGui::TextWrapped("%s", state.last_scan.c_str());
    ImGui::Text("Source : %s", state.selected_name.empty() ? "-" : state.selected_name.c_str());
    ImGui::TextUnformatted("La sortie SDK doit etre activee pour recevoir les scans ici.");
  }
  ImGui::End();
  inateck_window_open_ = open;
}

void Inateck::draw() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Douchette")) {
      if (ImGui::MenuItem("Ouvrir les parametres", nullptr, inateck_window_open_))
        inateck_window_open_ = true;
      const InateckSnapshot state = inateck_worker_.snapshot();
      ImGui::Separator();
      ImGui::Text("Statut : %s", state.authenticated ? "connectee" : state.discovering ? "recherche" : "hors ligne");
      if (ImGui::MenuItem("Rechercher", nullptr, false, !state.discovering))
        inateck_worker_.start_discovery();
      if (ImGui::MenuItem("Arreter la recherche", nullptr, false, state.discovering))
        inateck_worker_.stop_discovery();
      if (ImGui::MenuItem("Deconnecter", nullptr, false, state.connected))
        inateck_worker_.disconnect();
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  draw_inateck_window();
}

}
