/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          settings.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace qrprotec {

namespace {

int to_int(const std::string &value, int fallback) {
  try {
    return std::stoi(value);
  } catch (const std::exception &) {
    return fallback;
  }
}

float to_float(const std::string &value, float fallback) {
  try {
    return std::stof(value);
  } catch (const std::exception &) {
    return fallback;
  }
}

} // namespace

AppSettings AppSettings::defaults() {
  AppSettings settings;
  // pile de scans en colonne a droite ; les autres fenetres s'ouvrent en taille moyenne, en cascade
  settings.layout["scan"] = { true, 0.72f, 0.0f, 0.28f, 1.0f };
  settings.layout["lots"] = { true, 0.01f, 0.02f, 0.60f, 0.70f };
  settings.layout["verif"] = { false, 0.03f, 0.05f, 0.58f, 0.80f };
  settings.layout["stock"] = { false, 0.05f, 0.06f, 0.55f, 0.65f };
  settings.layout["inventory"] = { false, 0.07f, 0.08f, 0.58f, 0.75f };
  settings.layout["pack"]      = { false, 0.30f, 0.15f, 0.40f, 0.55f };
  settings.layout["phone"]     = { false, 0.30f, 0.08f, 0.36f, 0.80f };
  settings.layout["lot_admin"] = { false, 0.09f, 0.10f, 0.58f, 0.75f };
  settings.layout["users"] = { false, 0.11f, 0.12f, 0.60f, 0.65f };
  settings.layout["journal"] = { false, 0.04f, 0.06f, 0.66f, 0.80f };
  settings.layout["settings"] = { false, 0.20f, 0.05f, 0.45f, 0.85f };
  settings.layout["editor"] = { false, 0.02f, 0.03f, 0.70f, 0.85f };
  return settings;
}

std::filesystem::path AppSettings::file_path() {
  const char *home = std::getenv("HOME");
  if (!home || *home == '\0')
    return ".qrprotec-app.conf";
  return std::filesystem::path(home) / ".config" / "qrprotec" / "app.conf";
}

namespace {
std::string escape_line(const std::string &value) {
  std::string output;
  for (const char character : value)
    output += character == '\n' ? std::string("\\n") : character == '\\' ? std::string("\\\\") : std::string(1, character);
  return output;
}

std::string unescape_line(const std::string &value) {
  std::string output;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\' && index + 1 < value.size()) {
      output += value[index + 1] == 'n' ? '\n' : value[index + 1];
      ++index;
    } else {
      output += value[index];
    }
  }
  return output;
}
} // namespace

PhysicalLabel AppSettings::physical_label() const {
  PhysicalLabel label;
  label.width_mm                = label_width_mm;
  label.height_mm               = label_height_mm;
  label.rotate_counterclockwise = rotate_counterclockwise;
  label.flip                    = flip_labels;
  return label;
}

const char *AppSettings::api_env(const char *name) {
  const char *value = std::getenv(name);
  return value && *value ? value : nullptr;
}

ApiEndpoint AppSettings::api_endpoint() const {
  const auto pick = [](const char *name, const std::string &fallback) {
    const char *value = api_env(name);
    return value ? std::string(value) : fallback;
  };
  ApiEndpoint endpoint;
  endpoint.url     = pick("QRPROTEC_API_URL", api_url);
  endpoint.token   = pick("QRPROTEC_API_TOKEN", api_token);
  endpoint.key     = pick("QRPROTEC_API_KEY", api_key);
  endpoint.ca_file = pick("QRPROTEC_API_CA_FILE", api_ca_file);
  return endpoint;
}

void AppSettings::load() {
  std::ifstream input(file_path());
  std::string   line;
  int           layout_version = 0;
  std::map< std::string, WindowLayout > saved_layout;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos || line[0] == '#')
      continue;
    const std::string key   = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "api_url")
      api_url = value;
    else if (key == "api_token")
      api_token = value;
    else if (key == "api_key")
      api_key = value;
    else if (key == "api_ca_file")
      api_ca_file = value;
    else if (key == "font_size")
      font_size = std::clamp(to_float(value, font_size), 10.0f, 40.0f);
    else if (key == "inactivity_minutes")
      inactivity_minutes = std::clamp(to_int(value, inactivity_minutes), 0, 24 * 60);
    else if (key == "expiring_soon_days")
      expiring_soon_days = std::clamp(to_int(value, expiring_soon_days), 0, 3650);
    else if (key == "order_lead_days")
      order_lead_days = std::clamp(to_int(value, order_lead_days), 0, 365);
    else if (key == "forecast_history_months")
      forecast_history_months = std::clamp(to_int(value, forecast_history_months), 1, 36);
    else if (key == "require_private_label")
      require_private_label = to_int(value, 1) != 0;
    else if (key == "printer_device")
      print.serial.device = value;
    else if (key == "printer_baud")
      print.serial.baud_rate = to_int(value, print.serial.baud_rate);
    else if (key == "printer_density")
      print.density = std::clamp(to_int(value, print.density), 1, 5);
    else if (key == "printer_label_type")
      print.label_type = std::clamp(to_int(value, print.label_type), 1, 3);
    else if (key == "templates_dir")
      templates_dir = value;
    else if (key.rfind("template.", 0) == 0)
      label_templates[key.substr(9)] = value;
    else if (key == "remote_scanner_timeout_minutes")
      remote_scanner_timeout_minutes = std::clamp(to_int(value, 5), 1, 24 * 60);
    else if (key == "sound_enabled")
      sound_enabled = to_int(value, 1) != 0;
    else if (key == "beep_command")
      beep_command = value;
    else if (key == "flash_enabled")
      flash_enabled = to_int(value, 1) != 0;
    else if (key == "flash_in_sdk_mode")
      flash_in_sdk_mode = to_int(value, 1) != 0;
    else if (key == "scanner_beep_on")
      scanner_signal.beep_on = to_int(value, scanner_signal.beep_on);
    else if (key == "scanner_beep_off")
      scanner_signal.beep_off = to_int(value, scanner_signal.beep_off);
    else if (key == "scanner_beep_count")
      scanner_signal.beep_count = to_int(value, scanner_signal.beep_count);
    else if (key == "scanner_led_color")
      scanner_signal.led_color = to_int(value, scanner_signal.led_color);
    else if (key == "scanner_led_on")
      scanner_signal.led_on = to_int(value, scanner_signal.led_on);
    else if (key == "scanner_led_off")
      scanner_signal.led_off = to_int(value, scanner_signal.led_off);
    else if (key == "scanner_led_count")
      scanner_signal.led_count = to_int(value, scanner_signal.led_count);
    else if (key == "label_width_mm")
      label_width_mm = std::clamp(to_float(value, label_width_mm), 5.0f, 200.0f);
    else if (key == "label_height_mm")
      label_height_mm = std::clamp(to_float(value, label_height_mm), 5.0f, 500.0f);
    else if (key == "rotate_counterclockwise")
      rotate_counterclockwise = to_int(value, 0) != 0;
    else if (key == "flip_labels")
      flip_labels = to_int(value, 0) != 0;
    else if (key == "label_title")
      label_title = unescape_line(value);
    else if (key == "layout_version")
      layout_version = to_int(value, 0);
    else if (key.rfind("layout.", 0) == 0) {
      WindowLayout       layout;
      int                open = 0;
      std::istringstream stream(value);
      char               comma = 0;
      if (stream >> open >> comma >> layout.x >> comma >> layout.y >> comma >> layout.w >> comma >> layout.h) {
        layout.open             = open != 0;
        saved_layout[key.substr(7)] = layout;
      }
    }
  }
  // une disposition enregistree par une version precedente (fenetres plein ecran) est ignoree
  if (layout_version >= kLayoutVersion)
    for (const auto &[id, window] : saved_layout)
      layout[id] = window;
}

bool AppSettings::save(std::string &error) const {
  const std::filesystem::path path = file_path();
  std::error_code             code;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), code);
  std::ofstream output(path);
  if (!output) {
    error = "Impossible d'écrire " + path.string();
    return false;
  }
  // le fichier peut contenir la cle API du front
  std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, code);
  output << "# Réglages QRProtec (édités depuis la fenêtre Réglages)\n"
         << "api_url=" << api_url << '\n'
         << "api_token=" << api_token << '\n'
         << "api_key=" << api_key << '\n'
         << "api_ca_file=" << api_ca_file << '\n'
         << "font_size=" << font_size << '\n'
         << "inactivity_minutes=" << inactivity_minutes << '\n'
         << "expiring_soon_days=" << expiring_soon_days << '\n'
         << "order_lead_days=" << order_lead_days << '\n'
         << "forecast_history_months=" << forecast_history_months << '\n'
         << "require_private_label=" << (require_private_label ? 1 : 0) << '\n'
         << "printer_device=" << print.serial.device << '\n'
         << "printer_baud=" << print.serial.baud_rate << '\n'
         << "printer_density=" << print.density << '\n'
         << "printer_label_type=" << print.label_type << '\n'
         << "sound_enabled=" << (sound_enabled ? 1 : 0) << '\n'
         << "remote_scanner_timeout_minutes=" << remote_scanner_timeout_minutes << '\n'
         << "beep_command=" << beep_command << '\n'
         << "flash_enabled=" << (flash_enabled ? 1 : 0) << '\n'
         << "flash_in_sdk_mode=" << (flash_in_sdk_mode ? 1 : 0) << '\n'
         << "scanner_beep_on=" << scanner_signal.beep_on << '\n'
         << "scanner_beep_off=" << scanner_signal.beep_off << '\n'
         << "scanner_beep_count=" << scanner_signal.beep_count << '\n'
         << "scanner_led_color=" << scanner_signal.led_color << '\n'
         << "scanner_led_on=" << scanner_signal.led_on << '\n'
         << "scanner_led_off=" << scanner_signal.led_off << '\n'
         << "scanner_led_count=" << scanner_signal.led_count << '\n'
         << "label_width_mm=" << label_width_mm << '\n'
         << "label_height_mm=" << label_height_mm << '\n'
         << "rotate_counterclockwise=" << (rotate_counterclockwise ? 1 : 0) << '\n'
         << "flip_labels=" << (flip_labels ? 1 : 0) << '\n'
         << "templates_dir=" << templates_dir << '\n'
         << "label_title=" << escape_line(label_title) << '\n'
         << "layout_version=" << kLayoutVersion << '\n';
  for (const auto &[category, template_path] : label_templates)
    output << "template." << category << '=' << template_path << '\n';
  for (const auto &[id, window] : layout)
    output << "layout." << id << '=' << (window.open ? 1 : 0) << ',' << window.x << ',' << window.y << ','
           << window.w << ',' << window.h << '\n';
  if (!output) {
    error = "Erreur d'écriture des réglages";
    return false;
  }
  return true;
}

} // namespace qrprotec
