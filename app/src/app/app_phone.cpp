/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_phone.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Telephone-douchette : le telephone scanne, le serveur relaie par WebSocket, le front traite le code.

#include "app.hpp"
#include "../net/front_config.hpp"
#include "../core/french.hpp"
#include "../core/plural.hpp"
#include "../printer/logging.hpp"
#include "../render/label_types.hpp"

#include "../ui/inateck.hpp"
#include "../ui/label_preview.hpp"
#include "../ui/widgets.hpp"
#include "../ui/windows/windows.hpp"
#include "labels.hpp"
#include "../core/paths.hpp"
#include "../core/template_io.hpp"

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace qrprotec {

// Telephone-douchette : le telephone scanne, le serveur relaie par WebSocket, le front traite le code
// exactement comme un scan de douchette (ScanSource::Phone).

void App::start_remote_session() {
  if (remote.creating)
    return;
  if (remote.active)
    close_remote_session("remplacée par une nouvelle session");
  remote.creating = true;
  Json body;
  body["timeout_minutes"] = settings.remote_scanner_timeout_minutes;
  api.post("/api/remote-scanner/", body, [this](const ApiResult &result) {
    remote.creating = false;
    if (!result.ok) {
      notify("Téléphone-douchette : " + result.error, true);
      return;
    }
    remote                = RemoteSession{};
    remote.active         = true;
    remote.id             = result.data["id"].str();
    remote.url            = result.data["url"].str();
    remote.timeout        = result.data["timeout"].integer(300);
    remote.phone_deadline = ImGui::GetTime() + remote.timeout;
    remote_link.start(settings.api_endpoint(), remote.id);
  });
}

void App::close_remote_session(const std::string &reason) {
  if (!remote.active)
    return;
  remote_link.send("{\"type\":\"close\"}");
  api.remove("/api/remote-scanner/" + url_encode(remote.id) + "/", [](const ApiResult &) {});
  remote_link.stop();
  remote              = RemoteSession{};
  remote.ended_reason = reason;
}

void App::poll_remote() {
  if (!remote.active)
    return;
  if (remote.feedback_pending) {
    Json message;
    message["type"]    = "feedback";
    message["result"]  = "error";
    message["message"] = remote.feedback_message;
    remote_link.send(message.dump());
    remote.feedback_pending = false;
  }
  for (const std::string &text : remote_link.take_messages()) {
    std::string error;
    const Json  message = Json::parse(text, &error);
    if (!error.empty() || !message.is_object())
      continue;
    const std::string type    = message["type"].str();
    if (type == "scan") {
      ++remote.scans;
      handle_scan(message["code"].str(), ScanSource::Phone);
    } else if (type == "undo") {
      // « Annuler le dernier » sur le telephone : seulement si le dernier scan de la pile vient de lui
      const auto &entries = stack.entries();
      if (!entries.empty() && entries.back().source == ScanSource::Phone) {
        const std::string what = entries.back().title.empty() ? describe_scan(entries.back().scan) : entries.back().title;
        stack.undo_last();
        notify("Téléphone : dernier scan annulé (" + what + ").");
      } else {
        notify("Téléphone : rien à annuler, le dernier scan de la pile ne vient pas du téléphone.", true);
      }
    } else if (type == "hello") {
      remote.phone_connected = message["phone_connected"].boolean();
      if (!message["expires_in"].is_null())
        remote.phone_deadline = ImGui::GetTime() + message["expires_in"].integer();
    } else if (type == "phone") {
      remote.phone_connected = message["connected"].boolean();
      if (remote.phone_connected) {
        remote.phone_seen  = true;
        remote.phone_agent = message["agent"].str();
        notify("Téléphone-douchette connecté.");
      } else {
        remote.phone_deadline = ImGui::GetTime() + remote.timeout;
        notify("Téléphone-douchette déconnecté.", true);
      }
    } else if (type == "closed") {
      const std::string reason = message["reason"].str("session fermée");
      remote_link.stop();
      remote              = RemoteSession{};
      remote.ended_reason = reason;
      notify("Téléphone-douchette : session fermée (" + reason + ").", true);
      return;
    }
  }
  if (remote_link.session_gone()) {
    remote_link.stop();
    remote              = RemoteSession{};
    remote.ended_reason = "session expirée";
    notify("Téléphone-douchette : session expirée, créez un nouveau QR code.", true);
  }
}

} // namespace qrprotec
