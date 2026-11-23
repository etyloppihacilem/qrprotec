/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hotspot_state.cpp
        -\-    _|__
         |\___/  . \        Created on 23 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "hotspot_state.hpp"

#include <cstdlib>
#include <sstream>

namespace qrprotec {

HotspotState parse_hotspot_state(const std::string &text) {
  HotspotState       state;
  std::istringstream lines(text);
  std::string        line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::size_t equal = line.find('=');
    if (equal == std::string::npos)
      continue;
    const std::string key   = line.substr(0, equal);
    const std::string value = line.substr(equal + 1);
    if (key == "SSID")
      state.ssid = value;
    else if (key == "PASSWORD")
      state.password = value;
    else if (key == "ADDRESS")
      state.address = value;
    else if (key == "DOMAIN")
      state.domain = value;
    else if (key == "IFACE")
      state.iface = value;
    else if (key == "SINCE")
      state.since = std::atoll(value.c_str());
  }
  state.active = !state.ssid.empty() && !state.password.empty();
  return state;
}

namespace {

std::string escape_wifi(const std::string &value) {
  std::string out;
  for (const char c : value) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"')
      out += '\\';
    out += c;
  }
  return out;
}

} // namespace

std::string wifi_qr_payload(const std::string &ssid, const std::string &password) {
  return "WIFI:T:WPA;S:" + escape_wifi(ssid) + ";P:" + escape_wifi(password) + ";;";
}

} // namespace qrprotec
