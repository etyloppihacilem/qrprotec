/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_setup.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Premiere configuration du poste.

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

// Premiere configuration

void App::check_setup() {
  next_setup_check_ = ImGui::GetTime() + 5.0; // nouvel essai tant que le serveur ne repond pas
  setup_known_      = true;
  api.get("/api/setup/", [this](const ApiResult &result) {
    if (!result.ok) {
      setup_known_ = false;
      return;
    }
    needs_admin_ = result.data["needs_admin"].boolean();
    if (restore_pending_) {
      restore_pending_ = false;
      restore_login();
    }
  });
  refresh_server_rules();
}

void App::refresh_server_rules() {
  api.get("/api/health/", [this](const ApiResult &result) {
    if (!result.ok)
      return;
    declared_identity = result.data["declared_identity"].boolean();
    web_barcodes      = result.data["web_barcodes"].boolean();
    // ancien serveur sans le reglage : verif recommandee (comportement d'avant)
    const Json &restock = result.data["restock_verif_recommended"];
    restock_verif_recommended = !restock.is_bool() || restock.boolean();
    poste_key         = result.data["poste"].boolean();
    const Json &volume = result.data["scanner_volume_default"];
    scanner_volume_default = volume.is_number() ? volume.integer() : -1;
    if (!logged_in())
      apply_default_scanner_volume();
  });
}

void App::apply_default_scanner_volume() {
  inateck.set_user_volume(scanner_volume_default);
}

void App::create_first_admin() {
  Json body;
  body["matricule"]  = setup_matricule_;
  body["nom"]        = setup_nom_;
  body["prenom"]     = setup_prenom_;
  body["pin"]        = setup_pin_;
  body["role"]       = "admin";
  setup_busy_        = true;
  api.post("/api/users/", body, [this](const ApiResult &result) {
    setup_busy_ = false;
    if (!result.ok) {
      notify("Création impossible : " + result.error, true);
      return;
    }
    setup_created_ = result.data;
    needs_admin_   = false;
    // premier administrateur : le serveur renvoie son jeton de session, il est connecte directement
    SessionUser session;
    session.matricule   = result.data["matricule"].str();
    session.nom         = result.data["nom"].str();
    session.prenom      = result.data["prenom"].str();
    session.key_expires = result.data["key_expires"].str();
    session.privileged  = true;
    session.role        = "admin";
    session.fonction    = result.data["fonction"].str();
    user                = session;
    api.set_session(result.data["session"].str());
    refresh_item_types();
    refresh_lot_types();
    notify("Administrateur créé : mode privilégié activé.");
  });
}

void App::draw_server_modal() {
  if (server_modal_requested_) {
    server_modal_requested_ = false;
    ImGui::OpenPopup("Connexion au serveur");
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Connexion au serveur", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  // Une fois l'API joignable, la connexion ne se modifie plus que depuis les Reglages (administrateur).
  // Sur une borne (QRPROTEC_KIOSK, qrprotec-kiosk-session), jamais sans badge : un passant ne doit pas
  // pouvoir rediriger le poste vers un autre serveur pendant une panne.
  const bool kiosk    = AppSettings::api_env("QRPROTEC_KIOSK") != nullptr;
  const bool editable = !api.online() && !kiosk;
  if (api.online()) {
    ImGui::TextWrapped("Connecté au serveur.");
  } else {
    ImGui::TextWrapped("Le serveur est injoignable : %s", api.last_error().c_str());
    if (kiosk)
      ImGui::TextWrapped("Borne : la connexion se règle sur la machine avec sudo qrprotec-setup "
                         "--front-config FICHIER.qrprotec (fichier créé sur le serveur), ou --api-url et "
                         "--api-key, ou --local-api.");
    else
      draw_server_settings(*this, true);
  }
  ImGui::Separator();
  if (editable && (primary_button("Enregistrer") || enter_pressed())) {
    apply_settings();
    save_settings();
    refresh_item_types();
    refresh_lots();
  }
  if (editable)
    ImGui::SameLine();
  if (button("Fermer") || (!editable && enter_pressed()) || escape_pressed())
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::draw_setup_modal() {
  const bool show = (needs_admin_ || !setup_created_.is_null()) && !ImGui::IsPopupOpen("Connexion requise");
  if (show && !ImGui::IsPopupOpen("Première configuration"))
    ImGui::OpenPopup("Première configuration");
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal("Première configuration", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (setup_created_.is_null()) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Aucun administrateur n'a de badge valide");
    ImGui::PopFont();
    ImGui::TextWrapped("Créez le compte de l'administrateur (responsable technique). Il sera connecté tout de suite en mode "
                       "privilégié pour configurer le logiciel et imprimer son badge.");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
    ImGui::InputText("Matricule", &setup_matricule_, ImGuiInputTextFlags_CharsNoBlank);
    ImGui::InputText("Prénom", &setup_prenom_);
    ImGui::InputText("Nom", &setup_nom_);
    const ImGuiInputTextFlags pin_flags = ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal;
    ImGui::InputTextWithHint("Code PIN", "4 à 8 chiffres", &setup_pin_, pin_flags);
    ImGui::InputTextWithHint("Confirmation", "le même PIN", &setup_pin_confirm_, pin_flags);
    const bool pin_ok = setup_pin_.size() >= 4 && setup_pin_.size() <= 8 && setup_pin_ == setup_pin_confirm_;
    ImGui::TextDisabled("Le PIN sera demandé à chaque connexion, après le badge.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(setup_busy_ || setup_matricule_.empty() || setup_nom_.empty() || setup_prenom_.empty() || !pin_ok);
    if (button("Créer l'administrateur", ImVec2(-1, 0)) || enter_pressed())
      create_first_admin();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Alternative : python manage.py createadmin MATRICULE NOM PRENOM sur le serveur.");
    if (!needs_admin_)
      ImGui::CloseCurrentPopup();
  } else {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::Text("Bienvenue %s %s", setup_created_["prenom"].str().c_str(), setup_created_["nom"].str().c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("Imprimez maintenant votre badge : il permet de repasser en mode privilégié. Il faut un "
                       "modèle « Badge utilisateur » choisi dans Administration > Réglages (badge.qr est fourni).");
    std::string url = setup_created_["badge_url"].str();
    ImGui::TextUnformatted("URL du badge :");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##badge_url", &url, ImGuiInputTextFlags_ReadOnly);
    if (button("Imprimer mon badge"))
      preview_labels(TemplateCategory::User, { user_parameters(setup_created_) }, "Badge");
    ImGui::SameLine();
    if (button("Ouvrir les réglages")) {
      open_window("settings");
      notify("Choisissez le modèle de badge, puis imprimez votre badge depuis Administration > Utilisateurs.");
      setup_created_ = Json();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Terminer") || enter_pressed()) {
      setup_created_ = Json();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

void App::notify(const std::string &message, bool error) {
  if (error)
    error_log(message);
  // mauvais scan venant du telephone : le message accompagne le signal d'erreur envoye au telephone
  if (error && remote.feedback_pending && remote.feedback_message.empty())
    remote.feedback_message = message;
  toasts_.push_back({ message, error, ImGui::GetTime() });
  if (toasts_.size() > 6)
    toasts_.erase(toasts_.begin());
}

void App::draw_toasts() {
  const double now = ImGui::GetTime();
  toasts_.erase(
    std::remove_if(toasts_.begin(), toasts_.end(), [now](const Toast &toast) {
      return now - toast.time > (toast.error ? 10.0 : 5.0);
    }),
    toasts_.end()
  );
  if (toasts_.empty())
    return;
  // dessinees au premier plan pour ne jamais passer derriere une fenetre qui prend le focus
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImDrawList          *draw     = ImGui::GetForegroundDrawList();
  const float          padding  = 8.0f;
  const float          line     = ImGui::GetTextLineHeightWithSpacing();
  float                width    = 0.0f;
  for (const Toast &toast : toasts_)
    width = std::max(width, ImGui::CalcTextSize(toast.message.c_str()).x);
  const float  height = line * static_cast< float >(toasts_.size()) + padding * 2.0f;
  const ImVec2 min(viewport->Pos.x + 12.0f, viewport->Pos.y + viewport->Size.y - 12.0f - height);
  const ImVec2 max(min.x + width + padding * 2.0f, min.y + height);
  draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 0.97f, 0.96f)), 4.0f);
  draw->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.4f, 0.4f, 0.4f, 1.0f)), 4.0f);
  float y = min.y + padding;
  for (const Toast &toast : toasts_) {
    const ImVec4 color = toast.error ? ImVec4(0.8f, 0.05f, 0.05f, 1.0f) : ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    draw->AddText(ImVec2(min.x + padding, y), ImGui::GetColorU32(color), toast.message.c_str());
    y += line;
  }
}

} // namespace qrprotec
