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
  // pile de scans en colonne a droite, lots publics sur le reste de l'ecran
  settings.layout["scan"] = { true, 0.70f, 0.0f, 0.30f, 1.0f };
  settings.layout["lots"] = { true, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["verif"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["stock"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["inventory"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["lot_admin"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["users"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  settings.layout["settings"] = { false, 0.1f, 0.05f, 0.55f, 0.9f };
  settings.layout["editor"] = { false, 0.0f, 0.0f, 0.70f, 1.0f };
  return settings;
}

std::filesystem::path AppSettings::file_path() {
  const char *home = std::getenv("HOME");
  if (!home || *home == '\0')
    return ".qrprotec-app.conf";
  return std::filesystem::path(home) / ".config" / "qrprotec" / "app.conf";
}

void AppSettings::load() {
  std::ifstream input(file_path());
  std::string   line;
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
    else if (key == "font_size")
      font_size = std::clamp(to_float(value, font_size), 10.0f, 40.0f);
    else if (key == "inactivity_minutes")
      inactivity_minutes = std::clamp(to_int(value, inactivity_minutes), 0, 24 * 60);
    else if (key == "expiring_soon_days")
      expiring_soon_days = std::clamp(to_int(value, expiring_soon_days), 0, 3650);
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
    else if (key.rfind("template.", 0) == 0)
      label_templates[key.substr(9)] = value;
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
    else if (key.rfind("layout.", 0) == 0) {
      WindowLayout       layout;
      int                open = 0;
      std::istringstream stream(value);
      char               comma = 0;
      if (stream >> open >> comma >> layout.x >> comma >> layout.y >> comma >> layout.w >> comma >> layout.h) {
        layout.open             = open != 0;
        this->layout[key.substr(7)] = layout;
      }
    }
  }
}

bool AppSettings::save(std::string &error) const {
  const std::filesystem::path path = file_path();
  std::error_code             code;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), code);
  std::ofstream output(path);
  if (!output) {
    error = "Impossible d'ecrire " + path.string();
    return false;
  }
  output << "# Reglages QRProtec (edites depuis la fenetre Reglages)\n"
         << "api_url=" << api_url << '\n'
         << "api_token=" << api_token << '\n'
         << "font_size=" << font_size << '\n'
         << "inactivity_minutes=" << inactivity_minutes << '\n'
         << "expiring_soon_days=" << expiring_soon_days << '\n'
         << "require_private_label=" << (require_private_label ? 1 : 0) << '\n'
         << "printer_device=" << print.serial.device << '\n'
         << "printer_baud=" << print.serial.baud_rate << '\n'
         << "printer_density=" << print.density << '\n'
         << "printer_label_type=" << print.label_type << '\n'
         << "sound_enabled=" << (sound_enabled ? 1 : 0) << '\n'
         << "beep_command=" << beep_command << '\n'
         << "flash_enabled=" << (flash_enabled ? 1 : 0) << '\n'
         << "flash_in_sdk_mode=" << (flash_in_sdk_mode ? 1 : 0) << '\n'
         << "scanner_beep_on=" << scanner_signal.beep_on << '\n'
         << "scanner_beep_off=" << scanner_signal.beep_off << '\n'
         << "scanner_beep_count=" << scanner_signal.beep_count << '\n'
         << "scanner_led_color=" << scanner_signal.led_color << '\n'
         << "scanner_led_on=" << scanner_signal.led_on << '\n'
         << "scanner_led_off=" << scanner_signal.led_off << '\n'
         << "scanner_led_count=" << scanner_signal.led_count << '\n';
  for (const auto &[category, template_path] : label_templates)
    output << "template." << category << '=' << template_path << '\n';
  for (const auto &[id, window] : layout)
    output << "layout." << id << '=' << (window.open ? 1 : 0) << ',' << window.x << ',' << window.y << ','
           << window.w << ',' << window.h << '\n';
  if (!output) {
    error = "Erreur d'ecriture des reglages";
    return false;
  }
  return true;
}

} // namespace qrprotec
