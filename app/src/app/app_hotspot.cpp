/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_hotspot.cpp
        -\-    _|__
         |\___/  . \        Created on 23 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Linux seulement (kiosk Fedora, CMakeLists.txt). Fenetre « Wifi de secours » : la borne diffuse son propre wifi quand celui du local tombe. Demarrage et arret par le
// TECH (gestion, admins) ; une fois lance, tout le monde peut afficher les QR codes a scanner avec les telephones.

#include "app.hpp"
#include "../ui/widgets.hpp"

#include "imgui.h"

#include <ctime>

namespace qrprotec {

namespace {

std::string since_text(long long since) {
  if (since <= 0)
    return {};
  const std::time_t time = static_cast< std::time_t >(since);
  std::tm           local{};
  localtime_r(&time, &local);
  char text[32];
  std::strftime(text, sizeof text, "%d/%m à %H:%M", &local);
  return text;
}

// QR code et sa legende, en colonne
void qr_column(const char *title, const std::string &payload, float side, const std::string &caption) {
  ImGui::BeginGroup();
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  draw_qr(payload, side);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
  ImGui::TextUnformatted(caption.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
}

} // namespace

void App::draw_hotspot_modal() {
  static const char *kTitle = "Wifi de secours###hotspot";
  if (hotspot_open_pending_) {
    hotspot_open_pending_ = false;
    hotspot.poll(ImGui::GetTime(), true);
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const HotspotState wifi  = hotspot.state();
  const std::string  error = hotspot.error();
  const bool         busy  = hotspot.busy();
  const bool         can   = manager(); // TECH : gestion ou admin, quel que soit le mode
  const float        font  = ImGui::GetFontSize();
  const float        width = font * 40.0f;
  ImGui::PushTextWrapPos(width);

  if (!wifi.active) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("Le wifi du local ne marche plus ?");
    ImGui::PopFont();
    ImGui::TextUnformatted("La borne peut diffuser son propre wifi. Les téléphones qui le rejoignent continuent "
                           "d'utiliser QRprotec normalement (même adresse, même appli), sans internet.");
    ImGui::TextDisabled("La carte wifi de la borne quitte son réseau habituel tant que le wifi de secours tourne. "
                        "Il s'arrête au redémarrage de la borne.");
    ImGui::Spacing();
    if (!error.empty())
      ImGui::TextColored(colors::red, "%s", error.c_str());
    if (can) {
      ImGui::BeginDisabled(busy);
      if (primary_button(busy ? "Démarrage... (jusqu'à une minute)" : "Démarrer le wifi de secours", ImVec2(width * 0.6f, 0)))
        hotspot.start();
      ImGui::EndDisabled();
      ImGui::SameLine();
    } else {
      ImGui::TextColored(colors::orange, "Demandez au TECH de le démarrer (menu TECH, mode gestion).");
    }
  } else {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::Text("Wifi « %s » en marche", wifi.ssid.c_str());
    ImGui::PopFont();
    if (const std::string since = since_text(wifi.since); !since.empty())
      ImGui::TextDisabled("Depuis le %s · QRprotec répond sur ce wifi à %s", since.c_str(), wifi.domain.c_str());
    ImGui::Separator();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextColored(colors::orange, "1. Coupez les données mobiles du téléphone.");
    ImGui::PopFont();
    ImGui::TextUnformatted("Ce wifi n'a pas internet : avec les données mobiles, le téléphone cherche QRprotec par la 4G "
                           "et ne le trouve pas. Si le téléphone propose de quitter ce wifi « sans internet », "
                           "répondre de rester connecté.");
    ImGui::Spacing();
    const float side = font * 13.0f;
    qr_column("2. Rejoindre le wifi", wifi_qr_payload(wifi.ssid, wifi.password), side,
              "Scanner avec l'appareil photo.\nNom : " + wifi.ssid + "\nMot de passe : " + wifi.password);
    ImGui::SameLine(0.0f, font * 3.0f);
    qr_column("3. Ouvrir QRprotec", "https://" + wifi.domain + "/", side,
              "Ou l'appli QRprotec installée, comme d'habitude.\nhttps://" + wifi.domain + "/");
    ImGui::Spacing();
    ImGui::Separator();
    if (!error.empty())
      ImGui::TextColored(colors::red, "%s", error.c_str());
    if (can) {
      ImGui::BeginDisabled(busy);
      if (button(busy ? "Arrêt..." : "Arrêter le wifi de secours", ImVec2(width * 0.6f, 0)))
        hotspot.stop();
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Quand le wifi du local remarche : la borne reprend son réseau habituel.");
      ImGui::SameLine();
    }
  }
  ImGui::PopTextWrapPos();
  if (button("Fermer", ImVec2(-1, 0)) || escape_pressed())
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

} // namespace qrprotec
