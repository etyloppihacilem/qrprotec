/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hotspot.cpp
        -\-    _|__
         |\___/  . \        Created on 23 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "hotspot.hpp"

#include "../core/system.hpp"
#include "../printer/logging.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace qrprotec {

namespace {

const char *const kRunDir  = "/run/qrprotec-hotspot";
const char *const kHelper  = "/usr/libexec/qrprotec/qrprotec-hotspot";
const char *const kService = "qrprotec-hotspot.service";

std::string trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  return text;
}

} // namespace

bool Hotspot::supported() {
#if defined(__linux__)
  static const bool supported = []() {
    if (std::getenv("QRPROTEC_KIOSK") == nullptr)
      return false;
    // serveur sur une autre machine : le wifi de la borne ne le rendrait pas joignable
    if (const char *url = std::getenv("QRPROTEC_API_URL"); url != nullptr && *url != '\0')
      return false;
    std::error_code error;
    return std::filesystem::exists(kHelper, error);
  }();
  return supported;
#else
  return false;
#endif
}

void Hotspot::poll(double now, bool force) {
  if (busy_ || (!force && now < next_poll_))
    return;
  next_poll_ = now + 2.0;
  read_files();
}

void Hotspot::read_files() {
  const std::filesystem::path dir(kRunDir);
  std::string                 text, failure;
  const bool                  has_state = read_file(dir / "state", text, "wifi de secours");
  read_file(dir / "error", failure, "wifi de secours");
  std::lock_guard< std::mutex > lock(mutex_);
  state_ = has_state ? parse_hotspot_state(text) : HotspotState{};
  if (!trim(failure).empty())
    error_ = trim(failure);
}

HotspotState Hotspot::state() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return state_;
}

std::string Hotspot::error() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return error_;
}

void Hotspot::start() {
  run("start");
}

void Hotspot::stop() {
  run("stop");
}

// systemctl attend la fin du script (jusqu'a 90 s pour le demarrage) : dans un fil a part, l'interface reste fluide
void Hotspot::run(const std::string &verb) {
#ifndef _WIN32
  if (busy_.exchange(true))
    return;
  {
    std::lock_guard< std::mutex > lock(mutex_);
    error_.clear();
  }
  std::thread([this, verb]() {
    const std::string command = "systemctl --no-ask-password " + verb + " " + kService + " 2>&1";
    std::string       output;
    int               status  = -1;
    if (FILE *pipe = popen(command.c_str(), "r")) {
      char buffer[512];
      while (std::fgets(buffer, sizeof buffer, pipe) != nullptr)
        output += buffer;
      status = pclose(pipe);
    }
    const bool ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    read_files();
    if (!ok) {
      std::lock_guard< std::mutex > lock(mutex_);
      // message du script (/run/qrprotec-hotspot/error) en priorite, sinon celui de systemctl
      if (error_.empty()) {
        output = trim(output);
        error_ = output.find("ccess denied") != std::string::npos || output.find("nteractive authentication") != std::string::npos
                   ? "Le poste n'a pas le droit de lancer le wifi de secours (règle polkit du paquet qrprotec-kiosk absente ?)."
                   : "Le wifi de secours n'a pas pu " + std::string(verb == "start" ? "démarrer" : "s'arrêter")
                         + (output.empty() ? "." : " : " + output);
      }
      error_log("systemctl " + verb + " " + kService + " : " + (output.empty() ? error_ : output), "wifi de secours");
    }
    busy_ = false;
  }).detach();
#else
  (void)verb;
#endif
}

} // namespace qrprotec
