/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          phone_window.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "qrcodegen.hpp"
#include "windows.hpp"

#include <algorithm>
#include <cmath>

namespace qrprotec {

namespace {

// QR code dessine module par module (net a toute taille, sans texture).
void draw_qr(const std::string &payload, float size) {
  const qrcodegen::QrCode code  = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  const int               count = code.getSize() + 8; // marge blanche de 4 modules
  const float             cell  = std::floor(size / static_cast< float >(count));
  const float             side  = cell * static_cast< float >(count);
  const ImVec2            origin = ImGui::GetCursorScreenPos();
  ImDrawList             *draw   = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(255, 255, 255, 255));
  for (int y = 0; y < code.getSize(); ++y)
    for (int x = 0; x < code.getSize(); ++x)
      if (code.getModule(x, y)) {
        const ImVec2 corner(origin.x + (x + 4) * cell, origin.y + (y + 4) * cell);
        draw->AddRectFilled(corner, ImVec2(corner.x + cell, corner.y + cell), IM_COL32(0, 0, 0, 255));
      }
  ImGui::Dummy(ImVec2(side, side));
}

std::string countdown(double seconds) {
  const int total = std::max(0, static_cast< int >(std::ceil(seconds)));
  char      buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d:%02d", total / 60, total % 60);
  return buffer;
}

// Telephone utilise comme douchette : creation de la session et QR code de connexion.
class PhoneWindow final : public AppWindow {
  public:
    PhoneWindow() : AppWindow("phone", "Téléphone-douchette", false, true) {}

    void draw(App &app) override {
      RemoteSession &remote = app.remote;
      if (!remote.active) {
        draw_idle(app);
        return;
      }
      const double now = ImGui::GetTime();
      if (remote.phone_connected) {
        status_banner("✔ Téléphone connecté : ses scans arrivent dans la pile", colors::green, 1.1f);
      } else if (remote.phone_seen) {
        status_banner("✘ Téléphone déconnecté – session fermée dans " + countdown(remote.phone_deadline - now),
                      colors::red, 1.1f);
      } else {
        status_banner("En attente du téléphone – session fermée dans " + countdown(remote.phone_deadline - now),
                      colors::orange, 1.1f);
      }
      if (!app.remote_link.connected()) {
        const std::string error = app.remote_link.last_error();
        ImGui::TextColored(colors::red, "Poste non relié au serveur%s", error.empty() ? "…" : (" : " + error).c_str());
      }
      if (remote.phone_connected && !remote.phone_agent.empty())
        ImGui::TextDisabled("%s", remote.phone_agent.c_str());
      ImGui::Text("%d code(s) reçu(s) du téléphone.", remote.scans);

      const bool show_qr = !remote.phone_connected || show_qr_;
      if (remote.phone_connected && ImGui::Checkbox("Afficher le QR code (connecter un autre téléphone)", &show_qr_)) {
      }
      if (show_qr) {
        ImGui::TextWrapped("Scannez ce QR code avec l'appareil photo du téléphone, puis touchez « Démarrer la "
                           "caméra » sur la page qui s'ouvre.");
        const float available = std::min(ImGui::GetContentRegionAvail().x,
                                          ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f);
        const float size = std::clamp(available, 120.0f, 520.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - size) / 2.0f));
        draw_qr(remote.url, size);
        ImGui::TextDisabled("%s", remote.url.c_str());
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Le lien contient la clé de la session : ne le partagez pas.");
      }
      ImGui::Spacing();
      if (button("Nouveau QR code"))
        app.start_remote_session();
      ImGui::SameLine();
      if (danger_button("Fermer la session"))
        app.close_remote_session();
      ImGui::TextDisabled("Fermeture automatique après %d min de déconnexion (Réglages).", remote.timeout / 60);
    }

  private:
    void draw_idle(App &app) {
      ImGui::TextWrapped("Utilisez un téléphone comme douchette : chaque code scanné avec son appareil photo arrive "
                         "dans la pile de scans, comme avec la douchette. Les erreurs (produit périmé, code inconnu) "
                         "font clignoter et vibrer le téléphone.");
      ImGui::Spacing();
      if (!app.remote.ended_reason.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        ImGui::TextWrapped("Session précédente fermée : %s.", app.remote.ended_reason.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::SetNextItemWidth(140.0f);
      if (ImGui::InputInt("Fermeture après déconnexion (min)", &app.settings.remote_scanner_timeout_minutes)) {
        app.settings.remote_scanner_timeout_minutes = std::clamp(app.settings.remote_scanner_timeout_minutes, 1, 24 * 60);
        app.save_settings();
      }
      help_marker("Si le téléphone reste déconnecté plus longtemps (écran éteint, hors réseau...), la session se "
                  "ferme et il faut générer un nouveau QR code.");
      ImGui::BeginDisabled(app.remote.creating);
      if (primary_button(app.remote.creating ? "Création..." : "Créer une session et afficher le QR code",
                         ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f)))
        app.start_remote_session();
      ImGui::EndDisabled();
    }

    bool show_qr_ = false;
};

} // namespace

std::unique_ptr< AppWindow > make_phone_window() {
  return std::make_unique< PhoneWindow >();
}

} // namespace qrprotec
