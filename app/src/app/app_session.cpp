/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_session.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Connexion (badge, nom et PIN, fenetre de la douchette) et session de l'utilisateur.

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

void App::draw_douchette_modal() {
  static const char *kTitle = "Euh...";
  const std::string &phrase = douchette_phrases()[douchette_index_];
  if (douchette_pending_) {
    douchette_pending_ = false;
    // une autre fenetre modale (connexion, PIN...) ne doit pas etre fermee par la blague
    if (!ImGui::IsPopupOpen(kTitle) && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
      notify(phrase);
    else
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
  ImGui::TextUnformatted(phrase.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::Spacing();
  if (button("Pardon", ImVec2(-FLT_MIN, 0)) || enter_pressed() || escape_pressed())
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::draw_login_modal() {
  if (login_prompt_ && !login_prompt_opened_) {
    ImGui::OpenPopup("Connexion requise");
    login_prompt_opened_ = true;
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Connexion requise", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::TextUnformatted("Scannez votre badge");
    ImGui::PopFont();
    ImGui::Text("pour %s.", login_reason_.c_str());
    ImGui::Spacing();
    // saisie clavier possible (la fenetre modale bloque le champ de la pile de scans)
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::IsWindowAppearing())
      ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##badge", "ou saisissez le code du badge", &login_manual_,
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_Password)) {
      handle_scan(login_manual_, ScanSource::Manual);
      login_manual_.clear();
    }
    ImGui::PopStyleColor();
    // sans badge, pour un secouriste qui a un code PIN : discret, replie par defaut
    ImGui::Spacing();
    if (name_login_.open)
      draw_name_login();
    else if (ImGui::TextLink("Sans badge : nom, prénom et code PIN")) {
      name_login_       = NameLogin{};
      name_login_.open  = true;
      name_login_.focus = true;
    }
    if (declared_identity && login_declared_) {
      // reglage du serveur : nom declare, non verifie, garde seulement pour cette action
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Ou, sans badge (nom non vérifié) :");
      ImGui::SetNextItemWidth(360.0f);
      const bool enter = ImGui::InputTextWithHint("##declared", "prénom et nom", &declared_name_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
      const bool empty = declared_name_.find_first_not_of(" \t") == std::string::npos;
      ImGui::BeginDisabled(empty);
      if ((button("Continuer sans badge", ImVec2(360, 0)) || enter) && !empty) {
        auto action     = std::move(pending_action_);
        pending_action_ = nullptr;
        login_prompt_   = false;
        declared_action_ = declared_name_.substr(0, 30);
        if (action)
          action();
        declared_action_.clear();
      }
      ImGui::EndDisabled();
    }
    if (button("Annuler", ImVec2(160, 0)) || escape_pressed()) {
      login_prompt_   = false;
      pending_action_ = nullptr;
    }
    if (!login_prompt_ || logged_in() || pin_.active) // le PIN prend le relais (l'action en attente est gardee)
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  } else if (login_prompt_opened_) {
    login_prompt_opened_ = false;
    login_prompt_        = false;
    name_login_          = NameLogin{};
  }
}

void App::draw_name_login() {
  ImGui::SeparatorText("Sans badge");
  ImGui::TextDisabled("Si vous avez un code PIN :");
  const auto filled = [](const std::string &text) { return text.find_first_not_of(" \t") != std::string::npos; };
  // nom et prenom deja remplis (session d'une machine non partagee) : le curseur va directement au PIN
  const bool focus_pin = name_login_.focus && filled(name_login_.prenom) && filled(name_login_.nom);
  ImGui::SetNextItemWidth(360.0f);
  if (name_login_.focus && !focus_pin)
    ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##name_prenom", "prénom", &name_login_.prenom);
  ImGui::SetNextItemWidth(360.0f);
  ImGui::InputTextWithHint("##name_nom", "nom", &name_login_.nom);
  ImGui::SetNextItemWidth(360.0f);
  if (focus_pin)
    ImGui::SetKeyboardFocusHere();
  name_login_.focus = false;
  const bool enter = ImGui::InputTextWithHint("##name_pin", "code PIN", &name_login_.pin,
                                              ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal
                                                  | ImGuiInputTextFlags_EnterReturnsTrue);
  const bool ready  = filled(name_login_.prenom) && filled(name_login_.nom) && name_login_.pin.size() >= 4
                     && name_login_.pin.size() <= 8;
  if (!name_login_.error.empty())
    ImGui::TextColored(colors::red, "%s", name_login_.error.c_str());
  ImGui::BeginDisabled(!ready || name_login_.busy);
  if ((button("Se connecter", ImVec2(360, 0)) || enter) && ready && !name_login_.busy)
    send_name_login();
  ImGui::EndDisabled();
}

// Le serveur ne dit pas si le nom existe : un seul message d'erreur, et les essais sont limites
void App::send_name_login() {
  Json body;
  body["prenom"]   = name_login_.prenom;
  body["nom"]      = name_login_.nom;
  body["pin"]      = name_login_.pin;
  name_login_.busy = true;
  api.post("/api/auth/name/", body, [this](const ApiResult &result) {
    name_login_.busy = false;
    if (result.ok) {
      StoredLogin login;
      login.nom    = name_login_.nom;
      login.prenom = name_login_.prenom;
      name_login_  = NameLogin{};
      remember_login(login);
      complete_login(result.data);
      return;
    }
    feedback.error(ScanSource::Manual, settings);
    name_login_.pin.clear();
    name_login_.error = result.error;
    name_login_.focus = false;
  });
}

// Session

Json App::user_ref() const {
  return user ? Json(user->matricule) : Json();
}

void App::add_identity(Json &body) const {
  if (user)
    body["user"] = user_ref();
  else if (!declared_action_.empty())
    body["name"] = declared_action_;
}

bool App::identity_refused(const ApiResult &result) {
  if (result.ok || !result.data["login_required"].boolean())
    return false;
  declared_identity = result.data["declared_identity"].boolean();
  if (logged_in())
    session_expired();
  return true;
}

std::string App::user_name() const {
  return user ? user->display() : std::string();
}

void App::logout(const std::string &reason) {
  const bool was_logged         = logged_in();
  const bool privileged_session = was_logged && user->privileged;
  user.reset();
  api.set_session("");
  // prochain admin : Signalements rouverte seulement s'il reste des signalements non traites
  reports_on_login_ = false;
  if (AppWindow *reports = window("reports"))
    reports->open = false;
  if (std::exchange(reports_beside_lots_, false))
    if (AppWindow *lots = window("lots"))
      lots->place_pending = true; // fenetre Lots retrecie pour les Signalements : disposition par defaut
  if (privileged_session)
    refresh_lots(); // la liste ne garde pas les cles des lots apres la deconnexion
  pending_action_ = nullptr;
  checklist_scan  = {};
  apply_default_scanner_volume();
  if (was_logged && !reason.empty())
    notify(reason);
}

void App::manual_logout() {
  forget_login();
  logout("Déconnecté.");
  reset_windows_view();
}

void App::reset_windows_view() {
  for (auto &window : windows)
    window->reset_view(*this);
}

void App::session_expired() {
  if (settings.shared_machine || (last_login_.empty() && StoredLogin::load().empty())) {
    logout("Session expirée : scannez à nouveau votre badge.");
    return;
  }
  logout("Session expirée : reconnexion.");
  restore_login();
}

void App::remember_login(StoredLogin login) {
  last_login_ = std::move(login);
  if (settings.shared_machine)
    return;
  std::string error;
  if (!last_login_.save(error))
    notify(error, true);
}

void App::forget_login() {
  last_login_ = StoredLogin{};
  StoredLogin::clear();
}

void App::apply_shared_machine() {
  if (settings.shared_machine) {
    StoredLogin::clear();
    return;
  }
  // machine qui devient personnelle : l'utilisateur deja connecte le reste aux prochains demarrages
  std::string error;
  if (logged_in() && !last_login_.empty() && !last_login_.save(error))
    notify(error, true);
}

void App::restore_login() {
  if (settings.shared_machine || logged_in() || pin_.active || pin_.busy || name_login_.busy)
    return;
  if (last_login_.empty())
    last_login_ = StoredLogin::load();
  if (last_login_.badge()) {
    // le serveur redemande le PIN si l'utilisateur en a un (fenetre « Code PIN »)
    send_auth(last_login_.matricule, last_login_.key, "", "", ScanSource::Manual);
  } else if (last_login_.by_name()) {
    // sans badge : nom et prenom deja remplis, seul le PIN est demande
    login_reason_      = "reprendre votre session";
    login_prompt_      = true;
    name_login_        = NameLogin{};
    name_login_.open   = true;
    name_login_.focus  = true;
    name_login_.nom    = last_login_.nom;
    name_login_.prenom = last_login_.prenom;
  }
}

void App::open_login() {
  if (logged_in() || login_prompt_)
    return;
  login_declared_   = false; // le nom declare ne vaut que pour une action
  pending_action_   = nullptr;
  login_reason_     = "vous connecter";
  login_prompt_     = true;
  name_login_       = NameLogin{};
  name_login_.open  = true;
  name_login_.focus = true;
  if (last_login_.by_name()) {
    name_login_.nom    = last_login_.nom;
    name_login_.prenom = last_login_.prenom;
  }
}

void App::require_login(const std::string &what, std::function< void() > action, bool allow_declared) {
  if (logged_in()) {
    action();
    return;
  }
  login_declared_ = allow_declared;
  pending_action_ = std::move(action);
  login_reason_   = what;
  login_prompt_   = true;
  refresh_server_rules(); // le nom declare n'est propose que si le serveur l'accepte
}

void App::login_with_badge(const ParsedScan &scan, ScanSource source) {
  pin_ = PinPrompt{};
  send_auth(scan.id, scan.key, "", "", source);
}

// Le serveur verifie la cle du badge puis, si besoin, le PIN : une erreur pin_required (ou
// pin_setup_required pour un admin qui n'a pas encore de PIN) ouvre la saisie du PIN.
void App::send_auth(const std::string &matricule, const std::string &key, const std::string &pin,
                    const std::string &new_pin, ScanSource source) {
  Json body;
  body["matricule"] = matricule;
  body["key"]       = key;
  if (!pin.empty())
    body["pin"] = pin;
  if (!new_pin.empty()) {
    body["new_pin"]      = new_pin;
    body["pin_at_login"] = pin_.at_login || pin_.at_login_locked;
  }
  if (pin_.guest && pin_.matricule == matricule) { // carte d'invite : premiere connexion, avec son nom
    body["guest_nom"]    = pin_.guest_nom;
    body["guest_prenom"] = pin_.guest_prenom;
    body["guest_antenne"] = pin_.guest_antenne;
  }
  pin_.busy = true;
  api.post("/api/auth/", body, [this, matricule, key, source](const ApiResult &result) {
    pin_.busy = false;
    if (result.ok) {
      pin_ = PinPrompt{};
      StoredLogin login;
      login.matricule = matricule;
      login.key       = key;
      remember_login(login);
      complete_login(result.data);
      return;
    }
    if (result.data["pin_blocked"].boolean()) {
      show_pin_blocked(result.data, source);
      return;
    }
    if (result.data["guest_name_required"].boolean()) { // carte d'invite jamais utilisee : nom et prenom
      const bool first = !pin_.active || !pin_.guest;
      if (first) {
        pin_           = PinPrompt{};
        pin_.active    = true;
        pin_.guest     = true;
        pin_.matricule = matricule;
        pin_.key       = key;
        pin_.source    = static_cast< int >(source);
        pin_.guest_hours = static_cast< int >(result.data["guest_hours"].integer(48));
      }
      pin_.focus = true;
      pin_.error = first ? "" : result.error;
      if (!first)
        feedback.error(source, settings);
      return;
    }
    const bool setup = result.data["pin_setup_required"].boolean();
    if (setup || result.data["pin_required"].boolean()) {
      const bool first = !pin_.active;
      if (first) {
        pin_           = PinPrompt{};
        pin_.active    = true;
        pin_.matricule = matricule;
        pin_.key       = key;
        pin_.source    = static_cast< int >(source);
      }
      if (setup && (first || !pin_.setup)) { // garder la case telle que la personne l'a laissee si le code est refuse
        pin_.at_login_locked = result.data["pin_at_login_locked"].boolean();
        pin_.at_login        = result.data["pin_at_login_default"].boolean(true) || pin_.at_login_locked;
      }
      pin_.setup = setup;
      pin_.focus = true;
      pin_.pin.clear();
      pin_.confirm.clear();
      // premiere demande : pas d'erreur a afficher, seulement la saisie
      pin_.error = first && !result.data["pin_locked"].boolean() ? "" : result.error;
      if (!first)
        feedback.error(source, settings);
      return;
    }
    pin_ = PinPrompt{};
    // badge renouvele ou desactive : la session gardee ne sert plus (serveur injoignable : on la garde)
    if (result.status != 0 && matricule == last_login_.matricule && key == last_login_.key)
      forget_login();
    feedback.error(source, settings);
    notify("Badge refusé : " + result.error, true);
  });
}

void App::complete_login(const Json &data) {
  SessionUser session;
  session.matricule   = data["matricule"].str();
  session.nom         = data["nom"].str();
  session.prenom      = data["prenom"].str();
  session.key_expires = data["key_expires"].str();
  session.privileged  = data["privileged"].boolean();
  session.role        = data["role"].str(session.privileged ? "admin" : "normal");
  session.fonction    = data["fonction"].str();
  user                = session;
  gestion_mode_       = true; // un gestionnaire arrive en mode gestion, il peut passer en mode normal
  mode_places_[0].clear();
  mode_places_[1].clear();
  api.set_session(data["session"].str());
  // volume de la douchette propre a l'utilisateur (sinon celui par defaut du serveur)
  if (const Json &volume = data["scanner_volume"]; volume.is_number())
    inateck.set_user_volume(volume.integer());
  notify("Bonjour " + session.display()
         + (session.admin() ? " : mode administrateur activé." : session.privileged ? " : mode gestion activé." : "."));
  if (data["guest"].boolean()) {
    if (!data["guest_expires"].str().empty())
      notify("Carte d'invité valable jusqu'au " + display_datetime(data["guest_expires"].str()) + ".");
  } else if (const auto expires = Date::parse(session.key_expires); expires && *expires < today().plus_days(30))
    notify("Votre badge expire le " + expires->display() + ", demandez son renouvellement.", true);
  login_prompt_ = false;
  auto action   = std::move(pending_action_);
  pending_action_ = nullptr;
  if (action)
    action();
  else if (session.admin() && !verif.active) {
    // configuration par defaut d'un admin : les signalements non traites, a cote des lots (pas pendant une verif)
    reports_on_login_ = true;
    refresh_report_counts();
  }
  if (session.privileged) {
    refresh_item_types();
    refresh_lot_types();
    refresh_lots(); // avec les cles des lots (etiquettes privees), reservees aux roles gestion et admin
  }
}

// Trop d'essais faux ou code oublie : seul un admin peut debloquer (fenetre Utilisateurs, ou lien affiche sur le
// telephone)
void App::show_pin_blocked(const Json &data, ScanSource source) {
  pin_            = PinPrompt{};
  pending_action_ = nullptr;
  feedback.error(source, settings);
  const std::string contact = data["pin_reset"]["contact"].str();
  notify(std::string(data["pin_reset"]["forgotten"].boolean() ? "Code PIN oublié : "
                                                              : "Code PIN bloqué après trop d'essais : ")
             + (contact.empty() ? std::string("le TECH") : contact)
             + " doit le réinitialiser (Utilisateurs > Réinitialiser le PIN, ou lien affiché en scannant le badge "
               "avec un téléphone)."
             + (data["pin_reset"]["notified"].boolean() ? " Le TECH a été prévenu." : ""),
         true);
}

// Code oublie : le serveur bloque le PIN jusqu'a sa reinitialisation par un admin (et previent les admins)
void App::forgot_pin() {
  Json body;
  body["matricule"] = pin_.matricule;
  body["key"]       = pin_.key;
  pin_.busy         = true;
  api.post("/api/pin-forgot/", body, [this, source = static_cast< ScanSource >(pin_.source)](const ApiResult &result) {
    pin_.busy = false;
    if (result.data["pin_blocked"].boolean()) {
      show_pin_blocked(result.data, source);
      return;
    }
    pin_.error = result.error;
  });
}

// Carte d'invite scannee pour la premiere fois : prenom et nom de l'invite (secouriste d'une autre antenne), dans
// la fenetre du PIN. La carte vaut ensuite guest_hours heures.
void App::draw_guest_name() {
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted("Carte d'invité");
  ImGui::PopFont();
  ImGui::TextDisabled("Carte %s", pin_.matricule.c_str());
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
  ImGui::TextWrapped("Bienvenue ! Indiquez votre prénom, votre nom et votre antenne : vos vérifs seront à votre nom. "
                     "La carte vaut %d h à partir de maintenant.", pin_.guest_hours);
  ImGui::PopTextWrapPos();
  ImGui::SetNextItemWidth(260.0f);
  if (pin_.focus) {
    ImGui::SetKeyboardFocusHere();
    pin_.focus = false;
  }
  input_limited("Prénom##guest", pin_.guest_prenom, 32);
  ImGui::SetNextItemWidth(260.0f);
  input_limited("Nom##guest", pin_.guest_nom, 32);
  ImGui::SetNextItemWidth(260.0f);
  input_limited("Antenne##guest", pin_.guest_antenne, 64, "ex : Lyon 3");
  if (!pin_.error.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "%s", pin_.error.c_str());
  const auto filled = [](const std::string &text) { return text.find_first_not_of(" \t") != std::string::npos; };
  const bool valid  = filled(pin_.guest_prenom) && filled(pin_.guest_nom) && filled(pin_.guest_antenne);
  ImGui::BeginDisabled(!valid || pin_.busy);
  if (button(pin_.busy ? "Vérification..." : "Se connecter", ImVec2(150, 0)) || (valid && !pin_.busy && enter_pressed()))
    send_auth(pin_.matricule, pin_.key, "", "", static_cast< ScanSource >(pin_.source));
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(150, 0)) || escape_pressed()) {
    pin_            = PinPrompt{};
    pending_action_ = nullptr;
  }
}

void App::draw_pin_modal() {
  if (!pin_.active)
    return;
  if (!ImGui::IsPopupOpen("Code PIN"))
    ImGui::OpenPopup("Code PIN");
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("Code PIN", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (pin_.guest) {
    draw_guest_name();
    if (!pin_.active)
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted(pin_.setup ? "Choisissez votre code PIN" : "Code PIN");
  ImGui::PopFont();
  ImGui::TextDisabled("Badge %s", pin_.matricule.c_str());
  if (pin_.setup)
    ImGui::TextWrapped("Nouveau PIN de 4 à 8 chiffres (obligatoire pour vous, ou après une réinitialisation). Il "
                       "permet aussi de vous connecter sans badge, avec votre nom et votre prénom.");
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal
                                  | ImGuiInputTextFlags_EnterReturnsTrue;
  ImGui::SetNextItemWidth(220.0f);
  if (pin_.focus) {
    ImGui::SetKeyboardFocusHere();
    pin_.focus = false;
  }
  bool submit = ImGui::InputTextWithHint("##pin", "4 à 8 chiffres", &pin_.pin, flags);
  if (pin_.setup) {
    ImGui::SetNextItemWidth(220.0f);
    submit = ImGui::InputTextWithHint("##pin_confirm", "confirmez le PIN", &pin_.confirm, flags) || submit;
  }
  if (pin_.setup) {
    ImGui::BeginDisabled(pin_.at_login_locked);
    ImGui::Checkbox("Demander à chaque connexion", &pin_.at_login);
    ImGui::EndDisabled();
    ImGui::SameLine();
    help_marker(pin_.at_login_locked ? "Toujours demandé pour les administrateurs."
                                     : "Cochée : le code est demandé après le badge.\nDécochée : le badge suffit, le "
                                       "code ne sert qu'à se connecter sans badge (nom, prénom et code).");
  }
  if (!pin_.error.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "%s", pin_.error.c_str());
  const bool valid = pin_.pin.size() >= 4 && pin_.pin.size() <= 8 && (!pin_.setup || pin_.confirm == pin_.pin);
  if (pin_.setup && !pin_.confirm.empty() && pin_.confirm != pin_.pin)
    ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1.0f), "Les deux PIN sont différents.");
  ImGui::BeginDisabled(!valid || pin_.busy);
  if (button(pin_.busy ? "Vérification..." : "Valider", ImVec2(150, 0)) || enter_pressed()
      || (submit && valid && !pin_.busy)) {
    const ScanSource source = static_cast< ScanSource >(pin_.source);
    if (pin_.setup)
      send_auth(pin_.matricule, pin_.key, "", pin_.pin, source);
    else
      send_auth(pin_.matricule, pin_.key, pin_.pin, "", source);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(150, 0)) || escape_pressed()) {
    pin_            = PinPrompt{};
    pending_action_ = nullptr;
  }
  // lien discret : bouton sans fond, texte grise
  if (!pin_.setup) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::BeginDisabled(pin_.busy);
    if (small_button("Code oublié ?"))
      ImGui::OpenPopup("pin_forgot");
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (ImGui::BeginPopup("pin_forgot")) {
      ImGui::TextUnformatted("Votre code PIN sera bloqué jusqu'à ce que le TECH le réinitialise.");
      if (danger_button("Confirmer") || enter_pressed()) {
        forgot_pin();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (button("Annuler"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  if (!pin_.active)
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

} // namespace qrprotec
