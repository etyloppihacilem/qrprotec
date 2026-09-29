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
#include "GLFW/glfw3.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace qrprotec {

namespace {

std::filesystem::path hid_settings_path() {
  const char* home = std::getenv("HOME");
  if (!home || *home == '\0')
    return ".qrprotec-inateck-hid.conf";
  return std::filesystem::path(home) / ".config" / "qrprotec" / "inateck-hid.conf";
}

}

Inateck::Inateck() {
  load_hid_settings();
  apply_hid_settings();
}

Inateck::~Inateck() { save_hid_settings(); }

void Inateck::load_hid_settings() {
  std::ifstream settings(hid_settings_path());
  std::string line;
  while (std::getline(settings, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    try {
      if (key == "enabled")
        hid_enabled_ = std::stoi(value) != 0;
      else if (key == "timeout_ms")
        hid_timeout_ms_ = std::clamp(std::stoi(value), 5, 500);
      else if (key == "minimum_length")
        hid_minimum_length_ = std::clamp(std::stoi(value), 1, 64);
    } catch (const std::exception&) {
      // Keep the defaults for malformed individual settings.
    }
  }
}

void Inateck::save_hid_settings() const {
  const std::filesystem::path path = hid_settings_path();
  std::error_code error;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream settings(path);
  if (!settings)
    return;
  settings << "enabled=" << (hid_enabled_ ? 1 : 0) << '\n'
           << "timeout_ms=" << hid_timeout_ms_ << '\n'
           << "minimum_length=" << hid_minimum_length_ << '\n';
}

void Inateck::apply_hid_settings() {
  pending_hid_characters_.clear();
  has_last_hid_character_ = false;
  hid_classifier_.set_config({std::chrono::milliseconds(hid_timeout_ms_),
                              static_cast<std::size_t>(hid_minimum_length_)});
  hid_classifier_.set_enabled(hid_enabled_);
}

std::vector<unsigned int> Inateck::handle_hid_character(unsigned int character) {
  std::vector<unsigned int> replay;
  if (!hid_enabled_) {
    hid_classifier_.reset();
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
    replay.push_back(character);
    return replay;
  }
  const HidScanClassifier::TimePoint now = HidScanClassifier::Clock::now();
  if (has_last_hid_character_ && now - last_hid_character_ > std::chrono::milliseconds(hid_timeout_ms_)) {
    replay = pending_hid_characters_;
    replay.push_back(character);
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
    hid_classifier_.reset();
    return replay;
  }
  hid_classifier_.feed_character(character, now);
  pending_hid_characters_.push_back(character);
  last_hid_character_ = now;
  has_last_hid_character_ = true;
  return replay;
}

std::vector<unsigned int> Inateck::flush_hid_characters() {
  if (!hid_enabled_ || !has_last_hid_character_ ||
      HidScanClassifier::Clock::now() - last_hid_character_ <= std::chrono::milliseconds(hid_timeout_ms_))
    return {};
  std::vector<unsigned int> replay = pending_hid_characters_;
  pending_hid_characters_.clear();
  has_last_hid_character_ = false;
  hid_classifier_.reset();
  return replay;
}

Inateck::HidKeyResult Inateck::handle_hid_key(int key, int action) {
  HidKeyResult result;
  if (!hid_enabled_ || action != GLFW_PRESS)
    return result;
  if (key != GLFW_KEY_ENTER && key != GLFW_KEY_KP_ENTER)
    return result;
  const std::optional<std::string> scan = hid_classifier_.finish();
  if (scan) {
    inateck_worker_.on_scan_text(*scan, ScanSource::Hid);
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
    result.consume = true;
  } else {
    result.replay = pending_hid_characters_;
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
  }
  return result;
}

void Inateck::handle_window_focus(bool focused) {
  if (!focused) {
    hid_classifier_.reset();
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
  }
}

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
    if (ImGui::Checkbox("Mode HID clavier", &hid_enabled_)) {
      apply_hid_settings();
      save_hid_settings();
    }
    ImGui::SetItemTooltip("Traiter les saisies clavier tres rapides comme des scans.");
    if (ImGui::SliderInt("Delai HID (ms)", &hid_timeout_ms_, 5, 500) && ImGui::IsItemDeactivatedAfterEdit()) {
      apply_hid_settings();
      save_hid_settings();
    }
    if (ImGui::SliderInt("Longueur minimale HID", &hid_minimum_length_, 1, 64) && ImGui::IsItemDeactivatedAfterEdit()) {
      apply_hid_settings();
      save_hid_settings();
    }
    ImGui::Separator();
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

void Inateck::draw_menu() {
  if (ImGui::MenuItem("Ouvrir les parametres", nullptr, inateck_window_open_))
    inateck_window_open_ = true;
  const InateckSnapshot state = inateck_worker_.snapshot();
  ImGui::Separator();
  ImGui::Text("Statut : %s", state.authenticated ? "connectee" : state.discovering ? "recherche" : "hors ligne");
  ImGui::Text("Mode HID clavier : %s", hid_enabled_ ? "actif" : "inactif");
  if (ImGui::MenuItem("Rechercher", nullptr, false, !state.discovering))
    inateck_worker_.start_discovery();
  if (ImGui::MenuItem("Arreter la recherche", nullptr, false, state.discovering))
    inateck_worker_.stop_discovery();
  if (ImGui::MenuItem("Deconnecter", nullptr, false, state.connected))
    inateck_worker_.disconnect();
}

void Inateck::draw_window() {
  draw_inateck_window();
}

void Inateck::draw() {
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("Douchette")) {
      draw_menu();
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  draw_inateck_window();
}

}
