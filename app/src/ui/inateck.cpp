/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inateck.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 11:29
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "inateck.hpp"
#include "widgets.hpp"
#include "imgui.h"
#include "core/codes.hpp"
#include "inateck/inateck_worker.hpp"
#include "inateck/scanner_settings.hpp"
#include "inateck/sdk_json.hpp"
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
  inateck_worker_.set_preferred_device(saved_device_id_);
  inateck_worker_.set_model(scanner_model_);
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
      else if (key == "device_id")
        saved_device_id_ = value;
      else if (key == "model")
        scanner_model_ = std::max(0, std::stoi(value));
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
           << "minimum_length=" << hid_minimum_length_ << '\n'
           << "device_id=" << saved_device_id_ << '\n'
           << "model=" << scanner_model_ << '\n';
}

void Inateck::update() {
  // Memorise la douchette connectee pour s'y reconnecter en priorite a la prochaine recherche
  const std::string device_id = inateck_worker_.preferred_device();
  if (device_id != saved_device_id_) {
    saved_device_id_ = device_id;
    save_hid_settings();
  }
  // Tant que la douchette connue n'est pas connectee (base eteinte, hors de portee...), une
  // recherche est relancee regulierement : elle s'y connecte des qu'elle apparait.
  const InateckSnapshot state = inateck_worker_.snapshot();
  const auto now = std::chrono::steady_clock::now();
  if (state.discovering || state.connecting || state.connected)
    next_reconnect_ = now + kInateckReconnectInterval;
  else if (can_reconnect(state) && now >= next_reconnect_)
    reconnect();
}

bool Inateck::can_reconnect(const InateckSnapshot& state) const {
  return state.sdk_available && !state.preferred_id.empty() && !manual_disconnect_ && !state.connected &&
         !state.connecting && !state.discovering;
}

void Inateck::reconnect() {
  next_reconnect_ = std::chrono::steady_clock::now() + kInateckReconnectInterval;
  inateck_worker_.start_discovery(); // connexion automatique a la douchette connue en fin de recherche
}

void Inateck::start_pairing() {
  manual_disconnect_ = false;
  reconnect();
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
  if (has_last_hid_character_ && now - last_hid_character_ > std::chrono::milliseconds(hid_timeout_ms_) + poll_slack_) {
    // ecart trop long : ce qui precede etait une frappe humaine, rendue telle quelle ; le caractere
    // courant commence une nouvelle sequence (c'est peut-etre le debut d'un scan)
    replay = pending_hid_characters_;
    pending_hid_characters_.clear();
    has_last_hid_character_ = false;
    hid_classifier_.reset();
  }
  hid_classifier_.feed_character(character, now, poll_slack_);
  pending_hid_characters_.push_back(character);
  last_hid_character_ = now;
  has_last_hid_character_ = true;
  return replay;
}

void Inateck::begin_poll() {
  const HidScanClassifier::TimePoint now = HidScanClassifier::Clock::now();
  poll_slack_ = has_last_poll_ ? now - last_poll_ : HidScanClassifier::Clock::duration::zero();
  last_poll_ = now;
  has_last_poll_ = true;
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
    const char* status = state.authenticated ? "connectée et authentifiée"
                       : state.connected     ? "connectée"
                       : state.connecting    ? "connexion..."
                       : state.discovering   ? "recherche"
                                             : "non connectée";
    ImGui::Text("Statut : %s", status);
    if (button(state.discovering ? "Arrêter la recherche" : "Appairer une douchette")) {
      if (state.discovering)
        inateck_worker_.stop_discovery();
      else
        start_pairing();
    }
    ImGui::SetItemTooltip("Recherche les douchettes à proximité (choix dans « Connecter » ci-dessous).");
    ImGui::SameLine();
    ImGui::BeginDisabled(state.preferred_id.empty() || state.connected || state.connecting || state.discovering);
    if (button("Reconnecter"))
      start_pairing();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Se reconnecter à la dernière douchette utilisée.");
    static int selected_device = 0;
    std::vector<const char*> device_names;
    for (const InateckDevice& device : state.devices)
      device_names.push_back(device.name.empty() ? device.id.c_str() : device.name.c_str());
    if (!device_names.empty()) {
      selected_device = std::min(selected_device, static_cast<int>(device_names.size()) - 1);
      ImGui::Combo("Appareil", &selected_device, device_names.data(), static_cast<int>(device_names.size()));
      const InateckDevice& device = state.devices[static_cast<std::size_t>(selected_device)];
      ImGui::TextWrapped("ID : %s", device.id.c_str());
      if (!state.connected && !state.connecting && button("Connecter")) {
        manual_disconnect_ = false;
        inateck_worker_.connect(device.id, device.name);
      }
    } else {
      ImGui::TextUnformatted("Aucun appareil découvert.");
    }
    if (state.connected && settings_unlocked_) {
      ImGui::SameLine();
      if (button("Déconnecter")) {
        manual_disconnect_ = true;
        inateck_worker_.disconnect();
      }
    }
  }

  // Seules la recherche et la connexion sont accessibles sans utilisateur connecte
  const bool settings_open = ImGui::CollapsingHeader("Paramétrage", ImGuiTreeNodeFlags_DefaultOpen);
  if (settings_open && !settings_unlocked_) {
    ImGui::TextColored(ImVec4(0.8f, 0.45f, 0.0f, 1.0f), "Connectez-vous (scannez votre badge) pour modifier");
    ImGui::TextColored(ImVec4(0.8f, 0.45f, 0.0f, 1.0f), "les paramètres de la douchette.");
  }
  if (settings_open && settings_unlocked_) {
    if (ImGui::Checkbox("Mode HID clavier", &hid_enabled_)) {
      apply_hid_settings();
      save_hid_settings();
    }
    ImGui::SetItemTooltip("Traiter les saisies clavier très rapides comme des scans.");
    // La valeur est appliquee au relachement du curseur (le curseur ne renvoie plus true a ce moment-la)
    ImGui::SliderInt("Délai HID (ms)", &hid_timeout_ms_, 5, 500);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      apply_hid_settings();
      save_hid_settings();
    }
    ImGui::SliderInt("Longueur minimale HID", &hid_minimum_length_, 1, 64);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      apply_hid_settings();
      save_hid_settings();
    }
    draw_scanner_settings(state);
  }

  if (ImGui::CollapsingHeader("Dernier scan", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (state.last_scan.empty())
      ImGui::TextUnformatted("Aucun code reçu.");
    else
      ImGui::TextWrapped("%s", describe_scan(parse_scan(state.last_scan)).c_str());
    ImGui::Text("Source : %s", state.selected_name.empty() ? "-" : state.selected_name.c_str());
    ImGui::TextUnformatted("La sortie SDK doit être activée pour recevoir les scans ici.");
  }
  ImGui::End();
  inateck_window_open_ = open;
}

void Inateck::draw_scanner_settings(const InateckSnapshot& state) {
  ImGui::SeparatorText("Douchette");
  // Modele : le SDK lit et ecrit les parametres a des adresses propres a chaque modele
  const int detected = detect_scanner_model(state.selected_name);
  std::string automatic = "Automatique";
  automatic += detected ? std::string(" (") + scanner_model_label(detected) + ")" : " (non reconnu)";
  const char* preview = scanner_model_ ? scanner_model_label(scanner_model_) : automatic.c_str();
  if (ImGui::BeginCombo("Modèle", preview)) {
    if (ImGui::Selectable(automatic.c_str(), scanner_model_ == 0)) {
      scanner_model_ = 0;
      inateck_worker_.set_model(0);
      save_hid_settings();
    }
    for (const ScannerModel& model : scanner_models()) {
      if (ImGui::Selectable(model.label, scanner_model_ == model.type)) {
        scanner_model_ = model.type;
        inateck_worker_.set_model(model.type);
        save_hid_settings();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Modèle inscrit sous la douchette. Les versions AI se choisissent à la main.");

  ImGui::BeginDisabled(!state.authenticated);
  if (state.authenticated) {
    if (!state.battery.empty())
      ImGui::Text("Batterie : %s %%", state.battery.c_str());
    if (!state.software_version.empty() || !state.hardware_version.empty())
      ImGui::TextDisabled("Version logicielle %s, matérielle %s", state.software_version.c_str(),
                          state.hardware_version.c_str());
    if (!state.mac.empty())
      ImGui::TextDisabled("Adresse MAC : %s", state.mac.c_str());
    if (!state.sdk_version.empty())
      ImGui::TextDisabled("SDK Inateck %s", state.sdk_version.c_str());
  }
  if (!state.message.empty())
    ImGui::TextColored(colors::green, "%s", state.message.c_str());
  if (button("Relire les paramètres"))
    inateck_worker_.refresh_settings();
  ImGui::SameLine();
  if (button("Bip de test"))
    inateck_worker_.beep();
  ImGui::SameLine();
  if (button("Synchroniser l'heure"))
    inateck_worker_.sync_time();
  ImGui::SetItemTooltip("Règle l'horloge de la douchette sur celle de l'ordinateur (date et heure ajoutées aux codes).");

  ImGui::Checkbox("Recevoir les scans dans QRProtec", &inateck_sdk_output_);
  ImGui::SameLine();
  if (button("Appliquer le mode de sortie"))
    inateck_worker_.set_sdk_output(inateck_sdk_output_);
  ImGui::InputText("Préfixe", inateck_prefix_, sizeof(inateck_prefix_));
  ImGui::SameLine();
  if (button("Appliquer##prefixe"))
    inateck_worker_.set_prefix(inateck_prefix_);
  if (!state.prefix.empty())
    ImGui::TextDisabled("Préfixe actuel : %s", state.prefix.c_str());
  ImGui::InputText("Suffixe", inateck_suffix_, sizeof(inateck_suffix_));
  ImGui::SameLine();
  if (button("Appliquer##suffixe"))
    inateck_worker_.set_suffix(inateck_suffix_);
  if (!state.suffix.empty())
    ImGui::TextDisabled("Suffixe actuel : %s", state.suffix.c_str());
  ImGui::InputText("Nom Bluetooth", inateck_name_, sizeof(inateck_name_));
  ImGui::SameLine();
  if (button("Appliquer##nom"))
    inateck_worker_.set_name(inateck_name_);

  if (state.settings_loading)
    ImGui::TextDisabled("Lecture des paramètres de la douchette…");
  else if (state.authenticated && state.settings.empty())
    ImGui::TextDisabled("Paramètres non lus : vérifiez le modèle puis « Relire les paramètres ».");

  // Un groupe par theme, avec les seuls parametres que ce modele de douchette connait
  for (const char* group : scanner_setting_groups()) {
    std::vector<const ScannerSettingValue*> settings;
    for (const ScannerSettingValue& setting : state.settings)
      if (std::string(scanner_setting_info(setting.flag, setting.name).group) == group)
        settings.push_back(&setting);
    const bool codes = std::string(group) == scanner_setting_groups()[4];
    if (settings.empty() && !(codes && !state.settings.empty()))
      continue;
    const ImGuiTreeNodeFlags flags = group == scanner_setting_groups().front() ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    if (!ImGui::TreeNodeEx(group, flags))
      continue;
    if (codes) {
      if (button("Tout activer"))
        inateck_worker_.open_all_codes();
      ImGui::SameLine();
      if (button("Tout désactiver"))
        inateck_worker_.close_all_codes();
      ImGui::SameLine();
      if (button("Types par défaut"))
        inateck_worker_.reset_all_codes();
      ImGui::SetItemTooltip("Remet les types de codes lus à leur réglage d'usine.");
    }
    for (const ScannerSettingValue* setting : settings)
      draw_scanner_setting(*setting);
    ImGui::TreePop();
  }
  draw_scanner_actions(state);
  ImGui::EndDisabled();
}

void Inateck::draw_scanner_setting(const ScannerSettingValue& setting) {
  const ScannerSettingInfo info = scanner_setting_info(setting.flag, setting.name);
  const std::string label = std::string(*info.label ? info.label : setting.name.c_str()) + "##" +
                            std::to_string(setting.flag);
  switch (info.kind) {
  case ScannerSettingKind::Toggle: {
    bool enabled = setting.value != 0;
    if (ImGui::Checkbox(label.c_str(), &enabled))
      inateck_worker_.set_setting(setting.flag, enabled ? 1 : 0);
    break;
  }
  case ScannerSettingKind::Choice: {
    std::string preview = "Valeur " + std::to_string(setting.value);
    for (const ScannerSettingOption& option : info.options)
      if (option.value == setting.value)
        preview = option.label;
    if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
      for (const ScannerSettingOption& option : info.options)
        if (ImGui::Selectable(option.label, option.value == setting.value) && option.value != setting.value)
          inateck_worker_.set_setting(setting.flag, option.value);
      ImGui::EndCombo();
    }
    break;
  }
  case ScannerSettingKind::Number:
  case ScannerSettingKind::Character: {
    // La valeur part a la douchette au relachement du curseur, pas a chaque deplacement
    const auto pending = pending_values_.find(setting.flag);
    int value = pending != pending_values_.end() ? pending->second : setting.value;
    const int minimum = std::min(info.minimum, value);
    const int maximum = std::max(info.maximum, value);
    ImGui::SliderInt(label.c_str(), &value, minimum, maximum);
    if (ImGui::IsItemActive())
      pending_values_[setting.flag] = value;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      pending_values_.erase(setting.flag);
      if (value != setting.value)
        inateck_worker_.set_setting(setting.flag, value);
    } else if (!ImGui::IsItemActive() && pending != pending_values_.end() && !ImGui::IsItemDeactivated()) {
      pending_values_.erase(setting.flag);
    }
    if (info.kind == ScannerSettingKind::Character && value > 32 && value < 127) {
      ImGui::SameLine();
      ImGui::TextDisabled("« %c »", static_cast<char>(value));
    }
    break;
  }
  }
  if (*info.help)
    ImGui::SetItemTooltip("%s", info.help);
}

void Inateck::draw_scanner_actions(const InateckSnapshot& state) {
  if (!ImGui::TreeNode("Mémoire et entretien"))
    return;
  ImGui::Text("Codes en mémoire : %s", state.cache_count.empty() ? "?" : state.cache_count.c_str());
  ImGui::SameLine();
  if (button("Compter"))
    inateck_worker_.read_cache_count();
  if (button("Envoyer les codes en mémoire"))
    inateck_worker_.upload_cache();
  ImGui::SetItemTooltip("Mode inventaire : la douchette envoie les codes lus hors de portée.");
  ImGui::SameLine();
  if (button("Vider la mémoire"))
    inateck_worker_.clear_cache();
  if (button("Redémarrer la douchette"))
    inateck_worker_.restart();
  ImGui::SameLine();
  if (danger_button("Réglages d'usine…"))
    ImGui::OpenPopup("scanner_factory_reset");
  if (ImGui::BeginPopup("scanner_factory_reset")) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextWrapped("Remettre la douchette à ses réglages d'usine ? Volume, types de codes, préfixe, suffixe, "
                       "clavier… sont effacés.");
    ImGui::PopTextWrapPos();
    if (danger_button("Réinitialiser")) {
      inateck_worker_.factory_reset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Annuler"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::TreePop();
}

void Inateck::draw_menu() {
  if (ImGui::MenuItem("Ouvrir les paramètres", nullptr, inateck_window_open_))
    inateck_window_open_ = true;
  const InateckSnapshot state = inateck_worker_.snapshot();
  ImGui::Separator();
  ImGui::Text("Statut : %s", state.authenticated ? "connectée"
                             : state.connecting  ? "connexion..."
                             : state.discovering ? "recherche"
                                                 : "hors ligne");
  if (can_reconnect(state)) {
    const auto remaining = std::chrono::ceil<std::chrono::seconds>(next_reconnect_ - std::chrono::steady_clock::now());
    ImGui::TextDisabled("Nouvel essai de connexion dans %d s", static_cast<int>(std::max<long long>(0, remaining.count())));
  }
  ImGui::Text("Mode HID clavier : %s", hid_enabled_ ? "actif" : "inactif");
  const bool idle = !state.connected && !state.connecting && !state.discovering;
  if (ImGui::MenuItem("Reconnecter la douchette", nullptr, false, idle && !state.preferred_id.empty()))
    start_pairing();
  ImGui::SetItemTooltip("Se reconnecter tout de suite à la dernière douchette utilisée.");
  if (ImGui::MenuItem("Appairer une douchette", nullptr, false, !state.discovering))
    start_pairing();
  ImGui::SetItemTooltip("Rechercher les douchettes à proximité, puis la choisir dans « Connecter à ».");
  if (ImGui::MenuItem("Arrêter la recherche", nullptr, false, state.discovering))
    inateck_worker_.stop_discovery();
  // Choix de la douchette accessible sans utilisateur connecte
  if (ImGui::BeginMenu("Connecter à", !state.connected && !state.connecting)) {
    const std::vector<InateckDevice> devices = ordered_devices(state.devices, state.preferred_id, {});
    if (devices.empty())
      ImGui::TextDisabled("Aucun appareil : utilisez « Appairer une douchette »");
    for (const InateckDevice& device : devices) {
      std::string label = device.name.empty() ? device.id : device.name + " (" + device.id + ")";
      if (device.id == state.preferred_id)
        label += " - dernière douchette";
      if (ImGui::MenuItem(label.c_str())) {
        manual_disconnect_ = false;
        inateck_worker_.connect(device.id, device.name);
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Déconnecter", nullptr, false, state.connected && settings_unlocked_)) {
    manual_disconnect_ = true;
    inateck_worker_.disconnect();
  }
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
