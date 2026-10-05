/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "app.hpp"

#include "../ui/inateck.hpp"
#include "../ui/label_preview.hpp"
#include "../ui/widgets.hpp"
#include "../ui/windows/windows.hpp"
#include "labels.hpp"
#include "../core/paths.hpp"
#include "../core/template_io.hpp"

#include "imgui_stdlib.h"

#include <algorithm>
#include <map>
#include <set>

namespace qrprotec {

namespace {
const ImVec4 kPrivilegedOrange(0.93f, 0.49f, 0.13f, 1.0f);
const ImVec4 kNormalBackground(0.12f, 0.14f, 0.16f, 1.0f);
} // namespace

std::string display_date(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : "-";
}

std::string display_datetime(const Json &value) {
  const std::string text = value.str();
  const auto        date = Date::parse(text);
  if (!date)
    return "-";
  // horodatage en heure locale (le serveur l'envoie deja converti)
  if (text.size() >= 16 && text[10] == 'T')
    return date->display() + " à " + text.substr(11, 5);
  return date->display();
}

std::string Catalog::item_type_name(const std::string &type) const {
  for (const Json &item_type : item_types.items())
    if (item_type["type"].str() == type)
      return item_type["name"].str();
  return type;
}

App::App(Inateck &inateck_ref) : feedback(inateck_ref), inateck(inateck_ref) {
  settings = AppSettings::defaults();
  settings.load();
  apply_settings();
  select_default_templates();
  windows.push_back(make_scan_window());
  windows.push_back(make_lots_window());
  windows.push_back(make_verif_window());
  windows.push_back(make_stock_window());
  windows.push_back(make_inventory_window());
  windows.push_back(make_pack_window());
  windows.push_back(make_lot_admin_window());
  windows.push_back(make_users_window());
  windows.push_back(make_editor_window());
  windows.push_back(make_settings_window());
  windows.push_back(make_phone_window());
  feedback.on_phone_error = [this]() {
    remote.feedback_pending = true;
    remote.feedback_message.clear();
  };
  apply_default_open_state();
  // session refusee par le serveur (badge renouvele, PIN bloque, 12 h ecoulees) : on redemande le badge
  api.on_login_required = [this]() {
    if (logged_in())
      logout("Session expirée : scannez à nouveau votre badge.");
  };
  refresh_item_types();
  refresh_lots();
}

App::~App() = default;

void App::apply_settings() {
  api.configure(settings.api_endpoint());
  set_templates_dir(settings.templates_dir);
}

// Premiere installation (ou nouvel usage d'etiquette) : chaque usage qui n'a encore aucun modele dans les
// reglages prend le modele fourni fait pour lui (item.qr, lot_public.qr...). Un usage regle sur « (aucun) »
// est conserve tel quel.
void App::select_default_templates() {
  bool changed = false;
  for (const CategoryInfo &info : template_categories()) {
    if (info.category == TemplateCategory::Generic || settings.label_templates.count(info.id))
      continue;
    for (const auto &candidate : glob_templates("*.qr")) {
      TemplateDocument document;
      std::string      ignored;
      if (load_template(document, candidate.string(), ignored) && document.category == info.category) {
        settings.label_templates[info.id] = candidate.filename().string();
        changed                           = true;
        break;
      }
    }
  }
  std::string error;
  if (changed && !settings.save(error))
    notify(error, true);
}

bool App::save_settings() {
  std::string error;
  if (!settings.save(error)) {
    notify(error, true);
    return false;
  }
  notify("Réglages enregistrés.");
  return true;
}

void App::apply_default_open_state() {
  for (auto &window : windows) {
    const auto found = settings.layout.find(window->id);
    window->open     = window->id == "scan" || (found != settings.layout.end() && found->second.open);
    window->place_pending = true;
  }
}

// Zone sous la barre de menu principale (calculee ici : la zone de travail d'ImGui n'est a jour
// qu'a la frame suivante, ce qui decalerait la disposition au demarrage).
void App::work_area(ImVec2 &origin, ImVec2 &size) const {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const float          top      = menu_bar_height_ > 0.0f ? menu_bar_height_ : ImGui::GetFrameHeight();
  origin                        = ImVec2(viewport->Pos.x, viewport->Pos.y + top);
  size                          = ImVec2(viewport->Size.x, viewport->Size.y - top);
}

void App::save_current_layout() {
  ImVec2 origin, size;
  work_area(origin, size);
  for (auto &window : windows) {
    WindowLayout &layout = settings.layout[window->id];
    layout.open          = window->open;
    if (window->last_size.x > 0.0f && size.x > 0.0f && size.y > 0.0f) {
      layout.x = (window->last_pos.x - origin.x) / size.x;
      layout.y = (window->last_pos.y - origin.y) / size.y;
      layout.w = window->last_size.x / size.x;
      layout.h = window->last_size.y / size.y;
    }
  }
  save_settings();
}

AppWindow *App::window(const std::string &id) {
  for (auto &window : windows)
    if (window->id == id)
      return window.get();
  return nullptr;
}

void App::open_window(const std::string &id) {
  if (AppWindow *target = window(id)) {
    target->open          = true;
    target->focus_pending = true;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Boucle principale

void App::note_activity() {
  last_activity_ = ImGui::GetTime();
  reset_done_    = false;
}

void App::begin_frame() {
  const ImGuiIO &io = ImGui::GetIO();
  if (last_activity_ == 0.0)
    note_activity();
  if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f || ImGui::IsAnyMouseDown()
      || io.InputQueueCharacters.Size > 0)
    note_activity();
  inateck.set_settings_unlocked(logged_in());
  inateck.update();
  api.poll();
  poll_remote();
  for (const ScanEvent &event : inateck.take_scans())
    handle_scan(event.code, event.source);
  check_inactivity();
  if (!setup_known_ && ImGui::GetTime() >= next_setup_check_)
    check_setup();
}

void App::check_inactivity() {
  if (settings.inactivity_minutes <= 0 || reset_done_)
    return;
  if (ImGui::GetTime() - last_activity_ < settings.inactivity_minutes * 60.0)
    return;
  reset_session();
  reset_done_ = true;
}

void App::reset_session() {
  stack.clear();
  cancel_verif();
  last_report = Json();
  last_report_lot.clear();
  logout();
  login_prompt_ = false;
  pin_          = PinPrompt{};
  pending_action_ = nullptr;
  declared_name_.clear();
  apply_default_open_state();
  refresh_lots();
  notify("Session réinitialisée après inactivité.");
}

ImVec4 App::background_color() const {
  return privileged() ? kPrivilegedOrange : kNormalBackground;
}

void App::draw() {
  draw_menu_bar();
  // mode privilegie : fenetres teintees d'orange en plus du fond, pour qu'il soit impossible a manquer
  const bool tint = privileged();
  if (tint) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 0.91f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 0.94f, 0.86f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.96f, 0.70f, 0.42f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kPrivilegedOrange);
  }
  draw_windows();
  if (tint)
    ImGui::PopStyleColor(4);
  inateck.draw_window();
  draw_setup_modal();
  draw_server_modal();
  draw_login_modal();
  draw_pin_modal();
  draw_label_preview(*this);
  draw_toasts();
  feedback.draw_overlay();
}

void App::draw_menu_bar() {
  // memorise : un clic sur "Se deconnecter" change privileged() pendant le dessin de la barre
  const bool tinted = privileged();
  if (tinted)
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPrivilegedOrange);
  if (ImGui::BeginMainMenuBar()) {
    menu_bar_height_ = ImGui::GetWindowSize().y;
    if (ImGui::BeginMenu("Fenêtres")) {
      for (auto &window : windows)
        if (!window->privileged && window->closable && ImGui::MenuItem(window->title.c_str(), nullptr, window->open))
          window->open ? (void)(window->open = false) : open_window(window->id);
      ImGui::Separator();
      if (ImGui::MenuItem("Remettre les fenêtres en place"))
        request_layout_reset();
      ImGui::EndMenu();
    }
    if (privileged() && ImGui::BeginMenu("Gestion")) {
      for (auto &window : windows)
        if (window->privileged && can_open(*window) && ImGui::MenuItem(window->title.c_str(), nullptr, window->open))
          window->open ? (void)(window->open = false) : open_window(window->id);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Douchette")) {
      if (ImGui::MenuItem("Téléphone comme douchette...", nullptr, remote.active))
        open_window("phone");
      ImGui::Separator();
      inateck.draw_menu();
      ImGui::EndMenu();
    }

    // Partie droite : file d'impression, etat de l'API et utilisateur connecte
    const PrintQueueStatus print  = printer.status();
    std::string            printing;
    if (print.paused)
      printing = "Impression en pause";
    else if (print.pending > 0)
      printing = "Impression : " + std::to_string(print.pending) + " en attente";
    const std::string status = api.online() ? "API en ligne" : "API hors ligne";
    const std::string phone  = !remote.active          ? ""
                             : remote.phone_connected ? "Téléphone connecté"
                             : remote.phone_seen      ? "Téléphone déconnecté"
                                                      : "Téléphone en attente";
    const std::string who    = logged_in() ? user->display() + user->role_suffix() : "Non connecté : scannez votre badge";
    const float       button = logged_in() ? ImGui::CalcTextSize("Se déconnecter").x + ImGui::GetStyle().FramePadding.x * 2 : 0.0f;
    const float       width  = ImGui::CalcTextSize((printing + phone + status + who).c_str()).x + button + 100.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
    if (!printing.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, print.paused ? ImVec4(0.8f, 0.1f, 0.1f, 1) : ImVec4(0.2f, 0.2f, 0.2f, 1));
      const bool menu = ImGui::BeginMenu(printing.c_str());
      ImGui::PopStyleColor();
      if (menu) {
        if (print.paused)
          ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", print.error.c_str());
        if (!print.current.empty())
          ImGui::Text("En cours : %s", print.current.c_str());
        if (print.paused && ImGui::MenuItem("Reprendre l'impression"))
          printer.resume();
        if (ImGui::MenuItem("Annuler les impressions en attente"))
          printer.cancel();
        ImGui::EndMenu();
      }
      ImGui::Separator();
    }
    if (!phone.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, remote.phone_connected ? ImVec4(0.1f, 0.55f, 0.1f, 1) : ImVec4(0.8f, 0.1f, 0.1f, 1));
      if (ImGui::MenuItem(phone.c_str()))
        open_window("phone");
      ImGui::PopStyleColor();
      ImGui::Separator();
    }
    if (api.online()) {
      ImGui::TextColored(ImVec4(0.1f, 0.55f, 0.1f, 1), "%s", status.c_str());
    } else {
      // sans API, personne ne peut se connecter pour ouvrir les Reglages : la connexion se regle d'ici
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.1f, 0.1f, 1));
      if (ImGui::MenuItem(status.c_str()))
        server_modal_requested_ = true;
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nCliquer pour régler la connexion au serveur.", api.last_error().c_str());
    }
    ImGui::Separator();
    if (logged_in()) {
      ImGui::TextUnformatted(who.c_str());
      if (danger_button("Se déconnecter"))
        logout("Déconnecté.");
    } else {
      ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1.0f), "%s", who.c_str());
    }
    ImGui::EndMainMenuBar();
  }
  if (tinted)
    ImGui::PopStyleColor();
}

void App::draw_windows() {
  ImVec2 origin, size;
  work_area(origin, size);
  for (auto &window : windows) {
    if (!can_open(*window) || !window->open) {
      window->was_open = false;
      continue;
    }
    if (!window->was_open)
      window->on_open(*this);
    const auto layout = settings.layout.find(window->id);
    if (layout != settings.layout.end()) {
      const ImGuiCond cond = (layout_pending_ || window->place_pending) ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
      ImGui::SetNextWindowPos(
        ImVec2(origin.x + layout->second.x * size.x, origin.y + layout->second.y * size.y), cond
      );
      ImGui::SetNextWindowSize(ImVec2(layout->second.w * size.x, layout->second.h * size.y), cond);
    }
    window->place_pending = false;
    if (window->focus_pending) {
      ImGui::SetNextWindowFocus();
      window->focus_pending = false;
    }
    bool              open  = true;
    bool             *p_open = window->can_close(*this) ? &open : nullptr;
    const std::string label = window->title + "###" + window->id;
    if (ImGui::Begin(label.c_str(), p_open, window->flags())) {
      window->last_pos  = ImGui::GetWindowPos();
      window->last_size = ImGui::GetWindowSize();
      window->draw(*this);
    }
    ImGui::End();
    if (!open)
      window->open = false;
    window->was_open = window->open;
  }
  layout_pending_ = false;
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
    if (declared_identity) {
      // reglage du serveur : nom declare, non verifie, garde seulement pour cette action
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextDisabled("Ou, sans badge (nom non vérifié) :");
      ImGui::SetNextItemWidth(360.0f);
      const bool enter = ImGui::InputTextWithHint("##declared", "prénom et nom", &declared_name_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
      const bool empty = declared_name_.find_first_not_of(" \t") == std::string::npos;
      ImGui::BeginDisabled(empty);
      if ((ImGui::Button("Continuer sans badge", ImVec2(360, 0)) || enter) && !empty) {
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
    if (ImGui::Button("Annuler", ImVec2(160, 0))) {
      login_prompt_   = false;
      pending_action_ = nullptr;
    }
    if (!login_prompt_ || logged_in() || pin_.active) // le PIN prend le relais (l'action en attente est gardee)
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  } else if (login_prompt_opened_) {
    login_prompt_opened_ = false;
    login_prompt_        = false;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
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
  });
  refresh_server_rules();
}

void App::refresh_server_rules() {
  api.get("/api/health/", [this](const ApiResult &result) {
    if (result.ok)
      declared_identity = result.data["declared_identity"].boolean();
  });
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
                         "(--api-url, --api-key, --local-api).");
    else
      draw_server_settings(*this);
  }
  ImGui::Separator();
  if (editable && primary_button("Enregistrer")) {
    apply_settings();
    save_settings();
    refresh_item_types();
    refresh_lots();
  }
  if (editable)
    ImGui::SameLine();
  if (ImGui::Button("Fermer"))
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
    if (ImGui::Button("Créer l'administrateur", ImVec2(-1, 0)))
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
                       "modèle « Badge utilisateur » choisi dans Gestion > Réglages (badge.qr est fourni).");
    std::string url = setup_created_["badge_url"].str();
    ImGui::TextUnformatted("URL du badge :");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##badge_url", &url, ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Imprimer mon badge"))
      preview_labels(TemplateCategory::User, { user_parameters(setup_created_) }, "Badge");
    ImGui::SameLine();
    if (ImGui::Button("Ouvrir les réglages")) {
      open_window("settings");
      notify("Choisissez le modèle de badge, puis imprimez votre badge depuis Gestion > Utilisateurs.");
      setup_created_ = Json();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Terminer")) {
      setup_created_ = Json();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndPopup();
}

void App::notify(const std::string &message, bool error) {
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

// ---------------------------------------------------------------------------------------------------------------------
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
    logout("Session expirée : scannez à nouveau votre badge.");
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
  if (privileged_session)
    refresh_lots(); // la liste ne garde pas les cles des lots apres la deconnexion
  pending_action_ = nullptr;
  if (was_logged && !reason.empty())
    notify(reason);
}

void App::require_login(const std::string &what, std::function< void() > action) {
  if (logged_in()) {
    action();
    return;
  }
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
  if (!new_pin.empty())
    body["new_pin"] = new_pin;
  pin_.busy = true;
  api.post("/api/auth/", body, [this, matricule, key, source](const ApiResult &result) {
    pin_.busy = false;
    if (result.ok) {
      pin_ = PinPrompt{};
      complete_login(result.data);
      return;
    }
    if (result.data["pin_blocked"].boolean()) {
      show_pin_blocked(result.data, source);
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
  user                = session;
  api.set_session(data["session"].str());
  notify("Bonjour " + session.display()
         + (session.admin() ? " : mode administrateur activé." : session.privileged ? " : mode gestion activé." : "."));
  if (const auto expires = Date::parse(session.key_expires); expires && *expires < today().plus_days(30))
    notify("Votre badge expire le " + expires->display() + ", demandez son renouvellement.", true);
  login_prompt_ = false;
  auto action   = std::move(pending_action_);
  pending_action_ = nullptr;
  if (action)
    action();
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
             + (contact.empty() ? std::string("un administrateur") : contact)
             + " doit le réinitialiser (Utilisateurs > Réinitialiser le PIN, ou lien affiché en scannant le badge "
               "avec un téléphone)."
             + (data["pin_reset"]["notified"].boolean() ? " Les administrateurs ont été prévenus." : ""),
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

void App::draw_pin_modal() {
  if (!pin_.active)
    return;
  if (!ImGui::IsPopupOpen("Code PIN"))
    ImGui::OpenPopup("Code PIN");
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("Code PIN", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted(pin_.setup ? "Choisissez votre code PIN" : "Code PIN");
  ImGui::PopFont();
  ImGui::TextDisabled("Badge %s", pin_.matricule.c_str());
  if (pin_.setup)
    ImGui::TextWrapped("Nouveau PIN de 4 à 8 chiffres (obligatoire pour les administrateurs, ou après une "
                       "réinitialisation). Il sera demandé à chaque connexion, après le badge.");
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
  if (!pin_.error.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "%s", pin_.error.c_str());
  const bool valid = pin_.pin.size() >= 4 && pin_.pin.size() <= 8 && (!pin_.setup || pin_.confirm == pin_.pin);
  if (pin_.setup && !pin_.confirm.empty() && pin_.confirm != pin_.pin)
    ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1.0f), "Les deux PIN sont différents.");
  ImGui::BeginDisabled(!valid || pin_.busy);
  if (ImGui::Button(pin_.busy ? "Vérification..." : "Valider", ImVec2(150, 0)) || (submit && valid && !pin_.busy)) {
    const ScanSource source = static_cast< ScanSource >(pin_.source);
    if (pin_.setup)
      send_auth(pin_.matricule, pin_.key, "", pin_.pin, source);
    else
      send_auth(pin_.matricule, pin_.key, pin_.pin, "", source);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Annuler", ImVec2(150, 0))) {
    pin_            = PinPrompt{};
    pending_action_ = nullptr;
  }
  // lien discret : bouton sans fond, texte grise
  if (!pin_.setup) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::BeginDisabled(pin_.busy);
    if (ImGui::SmallButton("Code oublié ?"))
      ImGui::OpenPopup("pin_forgot");
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (ImGui::BeginPopup("pin_forgot")) {
      ImGui::TextUnformatted("Votre code PIN sera bloqué jusqu'à ce qu'un administrateur le réinitialise.");
      if (danger_button("Confirmer")) {
        forgot_pin();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Annuler"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  if (!pin_.active)
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------------------
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
        const std::string what = entries.back().title.empty() ? entries.back().scan.raw : entries.back().title;
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

// ---------------------------------------------------------------------------------------------------------------------
// Scans

void App::handle_scan(const std::string &code, ScanSource source) {
  note_activity();
  const ParsedScan scan = parse_scan(code);
  // scanner deux fois la meme chose n'a pas de sens : le doublon est ignore, sans signal sonore
  if (scan.kind == ScanKind::Item || scan.kind == ScanKind::SealedPack) {
    if (ScanEntry *existing = stack.find_code(scan.raw)) {
      existing->highlight_until = ImGui::GetTime() + 1.5;
      last_duplicate_           = existing->title.empty() ? scan.raw : existing->title;
      last_duplicate_time_      = ImGui::GetTime();
      return;
    }
  }
  switch (scan.kind) {
    case ScanKind::User: login_with_badge(scan, source); return;
    case ScanKind::Lot: scan_lot(scan, source); return;
    case ScanKind::LotSeal: scan_lot_seal(scan, source); return;
    case ScanKind::Item: {
      ScanEntry &entry = stack.add(scan, source);
      const int  id    = entry.id;
      if (entry.expired) {
        feedback.error(source, settings);
        notify("PÉRIMÉ : " + scan.id + " (" + scan.peremption->display() + ")", true);
      }
      resolve_item(id);
      return;
    }
    case ScanKind::SealedPack: {
      const int id = stack.add(scan, source).id;
      resolve_pack(id);
      // le responsable peut ouvrir le paquet directement depuis sa fiche
      if (privileged())
        show_pack(scan.id);
      return;
    }
    case ScanKind::Unknown: {
      stack.add(scan, source);
      feedback.error(source, settings);
      notify("Code non reconnu : " + scan.raw, true);
      return;
    }
  }
}

void App::entry_error(int entry_id, const std::string &message, ScanSource source) {
  ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  entry->state  = EntryState::Error;
  entry->detail = message;
  feedback.error(source, settings);
  notify(entry->scan.raw + " : " + message, true);
}

void App::resolve_item(int entry_id) {
  const ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  const ScanSource source = entry->source;
  api.get("/api/items/" + url_encode(entry->scan.id) + "/", [this, entry_id, source](const ApiResult &result) {
    ScanEntry *entry = stack.find(entry_id);
    if (!entry)
      return;
    if (result.status == 404) {
      entry_error(entry_id, "Inconnu dans la base", source);
      return;
    }
    if (!result.ok) {
      entry->state  = EntryState::Pending;
      entry->detail = "Non vérifié : " + result.error;
      return;
    }
    const Json &item = result.data;
    entry->state     = EntryState::Ok;
    entry->data      = item;
    entry->title     = item["type_name"].str();
    std::string detail = item["peremption"].is_null() ? "Non périssable" : "Exp. " + display_date(item["peremption"]);
    detail += item["location"].is_null() ? " - en stock" : " - " + item["location_name"].str();
    const std::string status = item["status"].str();
    entry->warning           = status != "active";
    if (status == "missing")
      detail += " - signalé disparu";
    else if (status == "deleted")
      detail += " - marqué supprimé";
    else if (status == "replaced")
      detail += " - déjà remplacé";
    entry->detail = detail;
    if (item["expired"].boolean() && !entry->expired) {
      entry->expired = true;
      feedback.error(source, settings);
    }
  });
}

void App::resolve_pack(int entry_id) {
  const ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  const ScanSource source = entry->source;
  api.get("/api/packs/" + url_encode(entry->scan.id) + "/", [this, entry_id, source](const ApiResult &result) {
    ScanEntry *entry = stack.find(entry_id);
    if (!entry)
      return;
    if (!result.ok) {
      entry_error(entry_id, result.status == 404 ? "Paquet inconnu" : result.error, source);
      return;
    }
    entry->state = EntryState::Ok;
    entry->data  = result.data;
    entry->pack_items.clear();
    for (const Json &iid : result.data["items"].items())
      entry->pack_items.push_back(iid.str());
    entry->title  = "Paquet : " + result.data["count"].str() + " x " + result.data["type_name"].str();
    entry->detail = result.data["peremption"].is_null() ? "Non périssable"
                                                        : "Exp. " + display_date(result.data["peremption"]);
    if (!result.data["opened"].is_null())
      entry->detail += " - déjà ouvert";
    const auto date = Date::parse(result.data["peremption"].str());
    if (date && *date < today()) {
      entry->expired = true;
      feedback.error(source, settings);
    }
  });
}

// QR code d'un scelle : le lot est valide sans verif tant que le scelle est intact.
void App::scan_lot_seal(const ParsedScan &scan, ScanSource source) {
  show_lot(scan.id, scan.key);
  api.get("/api/lots/" + url_encode(scan.id) + "/?seal=" + url_encode(scan.key),
          [this, id = scan.id, source](const ApiResult &result) {
            if (!result.ok) {
              feedback.error(source, settings);
              notify("Lot inconnu : " + id, true);
              return;
            }
            const std::string check = result.data["seal_check"].str();
            const std::string name  = result.data["name"].str();
            if (check != "valid") {
              feedback.error(source, settings);
              notify(check == "wrong" ? name + " : étiquette d'un ancien scellé, vérif nécessaire."
                                      : name + " : scellé brisé, vérif nécessaire.",
                     true);
            } else if (result.data["expired_count"].integer() > 0) {
              feedback.error(source, settings);
              notify(name + " : scellé intact mais contient des périmés, à ouvrir.", true);
            } else {
              notify(name + " : scellé intact, lot valide.");
            }
          });
}

void App::scan_lot(const ParsedScan &scan, ScanSource source) {
  if (!scan.key.empty()) {
    stack.target = { scan.id, scan.key, scan.id };
    api.get("/api/lots/" + url_encode(scan.id) + "/", [this, id = scan.id, source](const ApiResult &result) {
      if (!result.ok) {
        if (stack.target.id == id)
          stack.target = {};
        feedback.error(source, settings);
        notify("Lot inconnu : " + id, true);
        return;
      }
      if (result.data["verif_key"].str() != stack.target.key && stack.target.id == id) {
        feedback.error(source, settings);
        notify("Clé de l'étiquette privée périmée : réimprimez l'étiquette du lot.", true);
        stack.target = {};
        if (verif.lot_id == id)
          verif.key.clear();
        verif.extras.erase(std::remove_if(verif.extras.begin(), verif.extras.end(),
                                          [&id](const VerifExtra &extra) { return extra.id == id; }),
                           verif.extras.end());
        return;
      }
      if (stack.target.id == id)
        stack.target.name = result.data["name"].str();
    });
  }
  // etiquette publique : fiche du lot d'abord, la verif se lance depuis la fiche
  if (scan.key.empty() && !(verif.active && verif_covers(scan.id))) {
    show_lot(scan.id);
    return;
  }
  if (verif.active && verif.lot_id == scan.id) {
    if (!scan.key.empty()) {
      verif.key = scan.key;
      notify("Étiquette privée du lot reconnue.");
    }
    open_window("verif");
    return;
  }
  if (verif.active && scan.key.empty()) {
    notify("Ce lot fait déjà partie de la vérif en cours.");
    open_window("verif");
    return;
  }
  // etiquette privee d'un autre lot du meme lot global : ses items attendus s'ajoutent a la verif en cours
  if (verif.active) {
    join_verif(scan.id, scan.key, source);
    return;
  }
  start_verif(scan.id, scan.key);
}

namespace {
// Lot global d'un lot (detail de l'API) : son identifiant
std::string root_of(const Json &lot) {
  if (!lot["global"].is_null())
    return lot["global"]["id"].str();
  if (lot["path"].size() > 0)
    return lot["path"][0]["id"].str();
  return lot["id"].str();
}
} // namespace

void App::join_verif(const std::string &lot_id, const std::string &key, ScanSource source) {
  for (VerifExtra &extra : verif.extras)
    if (extra.id == lot_id) {
      extra.key = key;
      notify(extra.lot["name"].str(lot_id) + " fait déjà partie de la vérif.");
      open_window("verif");
      return;
    }
  const bool covered = verif_covers(lot_id);
  api.get("/api/lots/" + url_encode(lot_id) + "/", [this, lot_id, key, source, covered](const ApiResult &result) {
    if (!verif.active || verif.loading || !result.ok)
      return; // lot inconnu : signale par scan_lot
    if (root_of(result.data) != root_of(verif.lot)) {
      feedback.error(source, settings);
      notify(result.data["name"].str(lot_id) + " n'est pas dans le même lot global : terminez ou annulez la vérif en "
               "cours.",
             true);
      open_window("verif");
      return;
    }
    verif.extras.push_back({ lot_id, key, result.data });
    notify(covered ? result.data["name"].str() + " faisait déjà partie de la vérif : étiquette privée enregistrée."
                   : "Vérif groupée : " + verif_title() + ".");
    open_window("verif");
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// Verifs et mouvements

void App::show_lot(const std::string &lot_id, const std::string &seal_code) {
  lot_to_show_  = lot_id;
  seal_to_show_ = seal_code;
  open_window("lots");
}

std::string App::take_seal_to_show() {
  std::string code;
  code.swap(seal_to_show_);
  return code;
}

std::string App::take_lot_to_show() {
  std::string lot_id;
  lot_id.swap(lot_to_show_);
  return lot_id;
}

void App::show_pack(const std::string &pack_id) {
  pack_to_show_ = pack_id;
  open_window("pack");
}

std::string App::take_pack_to_show() {
  std::string pack_id;
  pack_id.swap(pack_to_show_);
  return pack_id;
}

void App::pack_opened(const std::string &pack_id) {
  ++catalog.packs_version;
  for (const ScanEntry &entry : stack.entries())
    if (entry.scan.kind == ScanKind::SealedPack && entry.scan.id == pack_id)
      resolve_pack(entry.id);
}

// La connexion n'est demandee qu'a la validation de la verif (submit_verif).
void App::start_verif(const std::string &lot_id, const std::string &key) {
  {
    verif         = {};
    verif.active  = true;
    verif.lot_id  = lot_id;
    verif.key     = key;
    verif.loading = true;
    open_window("verif");
    api.get("/api/lots/" + url_encode(lot_id) + "/", [this, lot_id](const ApiResult &result) {
      if (!verif.active || verif.lot_id != lot_id)
        return;
      verif.loading = false;
      if (!result.ok) {
        notify("Lot " + lot_id + " : " + result.error, true);
        cancel_verif();
        return;
      }
      verif.lot = result.data;
    });
  }
}

void App::cancel_verif() {
  verif = {};
}

// La cle d'un lot couvre ses sous-lots : l'etiquette privee du lot global suffit pour verifier un sous-lot
bool App::verif_key_ok() const {
  // sans utilisateur connecte (nom declare), le serveur exige toujours l'etiquette privee
  if (!verif.key.empty() || privileged() || (logged_in() && !settings.require_private_label))
    return true;
  for (const VerifExtra &extra : verif.extras)
    for (const Json &parent : verif.lot["path"].items())
      if (!extra.key.empty() && parent["id"].str() == extra.id)
        return true;
  return false;
}

std::vector< const Json * > App::verif_lots() const {
  std::vector< const Json * > result;
  std::set< std::string >     seen;
  const auto                  add = [&](const Json &lot) {
    if (lot.is_null() || !seen.insert(lot["id"].str()).second)
      return;
    result.push_back(&lot);
  };
  const auto add_tree = [&](const Json &top) {
    add(top);
    for (const Json &sub : top["descendants"].items())
      add(sub);
  };
  add_tree(verif.lot);
  for (const VerifExtra &extra : verif.extras)
    add_tree(extra.lot);
  return result;
}

bool App::verif_covers(const std::string &lot_id) const {
  if (verif.lot_id == lot_id)
    return true;
  for (const Json *lot : verif_lots())
    if ((*lot)["id"].str() == lot_id)
      return true;
  return false;
}

std::string App::verif_title() const {
  std::string title = verif.lot["name"].str(verif.lot_id);
  for (const VerifExtra &extra : verif.extras)
    title += " + " + extra.lot["name"].str(extra.id);
  return title;
}

std::vector< VerifPlanLot > App::plan_verif() const {
  const std::vector< const Json * > lots = verif_lots();
  std::vector< VerifPlanLot >       plan;
  std::map< std::string, std::size_t > index;          // id du lot -> position dans plan
  std::map< std::string, std::string > location_of;    // iid connu -> lot
  std::map< std::string, std::map< std::string, int > > required, deficit;
  std::set< std::string >              holding;        // lots dont des items connus n'ont pas ete scannes
  const std::vector< std::string >     scanned = stack.iids();
  const std::set< std::string >        done(scanned.begin(), scanned.end());
  for (const Json *lot : lots) {
    const std::string id = (*lot)["id"].str();
    index[id]            = plan.size();
    plan.push_back({ lot, (*lot)["depth"].integer() });
    for (const char *list : { "items", "missing_items" })
      for (const Json &item : (*lot)[list].items()) {
        location_of[item["iid"].str()] = id;
        // un item a etiquette a dechirer non scanne a ete utilise : il ne retient pas le lot
        if (!done.count(item["iid"].str()) && !item["tear_off"].boolean())
          holding.insert(id);
      }
    for (const Json &row : (*lot)["requirements"].items())
      required[id][row["type"].str()] = row["required"].integer();
    deficit[id] = required[id];
  }
  if (plan.empty())
    return plan;
  struct Scanned {
      std::string iid, type;
      bool        expired = false;
  };
  const Date              today = this->today();
  std::vector< Scanned >  newcomers;
  std::map< std::string, std::string > target;
  std::vector< Scanned >  all;
  for (const std::string &iid : scanned) {
    const ParsedScan parsed = parse_scan(iid);
    Scanned          item{ iid, parsed.item_type, is_expired(parsed, today) };
    all.push_back(item);
    if (const auto found = location_of.find(iid); found != location_of.end()) {
      target[iid] = found->second;
      if (!item.expired && deficit[found->second][item.type] > 0)
        --deficit[found->second][item.type];
    } else {
      newcomers.push_back(item);
    }
  }
  std::sort(newcomers.begin(), newcomers.end(), [](const Scanned &a, const Scanned &b) {
    return a.expired != b.expired ? !a.expired : a.iid < b.iid;
  });
  for (const Scanned &item : newcomers) {
    std::vector< std::string > candidates;
    for (const VerifPlanLot &row : plan) {
      const std::string id = (*row.lot)["id"].str();
      if (required[id].count(item.type))
        candidates.push_back(id);
    }
    std::string choice;
    if (!item.expired)
      for (const std::string &id : candidates)
        if (deficit[id][item.type] > 0) {
          choice = id;
          --deficit[id][item.type];
          break;
        }
    if (choice.empty())
      choice = candidates.empty() ? (*plan.front().lot)["id"].str() : candidates.front();
    target[item.iid] = choice;
  }
  std::map< std::string, std::map< std::string, int > > expired, new_fresh;
  for (const Scanned &item : all) {
    const std::string id  = target[item.iid];
    VerifPlanLot     &row = plan[index[id]];
    ++row.touched;
    if (item.expired) {
      ++expired[id][item.type];
    } else {
      ++row.fresh[item.type];
      if (location_of[item.iid] != id)
        ++new_fresh[id][item.type];
    }
  }
  for (VerifPlanLot &row : plan) {
    const std::string id   = (*row.lot)["id"].str();
    int               left = 0; // perimes non remplaces par un item frais arrive dans le meme lot
    for (const auto &[type, count] : expired[id])
      left += std::max(0, count - new_fresh[id][type]);
    bool filled = true;
    for (const auto &[type, quantity] : required[id])
      filled = filled && row.fresh[type] >= quantity;
    row.complete = filled && left == 0 && (row.touched > 0 || !holding.count(id));
  }
  return plan;
}

std::vector< const Json * > App::partial_verif_lots() const {
  std::vector< const Json * > result;
  if (!verif.active || verif.loading || stack.iids().empty())
    return result;
  const std::vector< VerifPlanLot > plan = plan_verif();
  if (plan.size() < 2)
    return result;
  bool all = true;
  for (const VerifPlanLot &row : plan)
    all = all && row.complete;
  if (all)
    return result;
  for (const VerifPlanLot &row : plan)
    if (row.complete && row.touched > 0)
      result.push_back(row.lot);
  return result;
}

void App::submit_verif(bool partial) {
  require_login(partial ? "valider la vérif partielle" : "valider la vérif", [this, partial]() {
    if (!verif.active || verif.submitting)
      return;
    if (!verif_key_ok()) {
      notify("Scannez l'étiquette privée du lot pour valider la vérif.", true);
      return;
    }
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    add_identity(body);
    // le lot scanne puis les lots du meme lot global ajoutes par leur etiquette privee
    body["lots"] = Json::array();
    Json primary;
    primary["id"]  = verif.lot_id;
    primary["key"] = verif.key;
    body["lots"].push_back(primary);
    for (const VerifExtra &extra : verif.extras) {
      Json entry;
      entry["id"]  = extra.id;
      entry["key"] = extra.key;
      body["lots"].push_back(entry);
    }
    body["partial"]  = partial;
    verif.submitting  = true;
    const std::string title = verif_title();
    api.post("/api/verifs/", body, [this, title](const ApiResult &result) {
      verif.submitting = false;
      if (!result.ok) {
        identity_refused(result);
        notify("Vérif refusée : " + result.error, true);
        return;
      }
      last_report     = result.data;
      last_report_lot = title;
      stack.clear();
      cancel_verif();
      open_window("verif");
      const bool complete = result.data["complete"].boolean();
      const bool several  = result.data["lots"].size() > 1;
      if (result.data["partial"].boolean())
        notify("Vérif partielle enregistrée : seuls les lots complets ont été vérifiés.");
      else
        notify(complete ? (several ? "Vérif enregistrée : lots complets." : "Vérif enregistrée : lot complet.")
                        : "Vérif enregistrée : incomplet ou périmés, voir le compte rendu.",
               !complete);
      refresh_lots();
    });
  });
}

void App::verif_target_lot() {
  if (!stack.target.valid())
    return;
  if (verif.active && verif_covers(stack.target.id)) {
    submit_verif();
    return;
  }
  if (!verif.active || verif.lot_id != stack.target.id) {
    verif        = {};
    verif.active = true;
    verif.lot_id = stack.target.id;
    verif.lot["name"] = stack.target.name;
  }
  verif.key = stack.target.key;
  submit_verif();
}

// Reassort pendant une verif : seuls des items nouveaux ont ete scannes. Ils sont ajoutes au lot sans
// toucher aux autres ; le lot passe « verif recommandee » (orange) pour la personne suivante.
void App::restock_verif() {
  // verif groupee : le serveur range chaque item dans son lot (verif partielle sans lot complet = reassort)
  if (verif.extras.size() > 0 || verif.lot["descendants"].size() > 0) {
    submit_verif(true);
    return;
  }
  require_login("ajouter les items au lot", [this]() {
    if (!verif.active || verif.submitting)
      return;
    if (!verif_key_ok()) {
      notify("Scannez l'étiquette privée du lot pour ajouter des items.", true);
      return;
    }
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    if (body["items"].size() == 0)
      return;
    add_identity(body);
    body["key"]             = verif.key;
    const std::string lot   = verif.lot_id;
    const std::string name  = verif.lot["name"].str(lot);
    verif.submitting        = true;
    api.post("/api/lots/" + url_encode(lot) + "/add/", body, [this, lot, name](const ApiResult &result) {
      if (verif.lot_id == lot)
        verif.submitting = false;
      if (!result.ok) {
        identity_refused(result);
        notify("Ajout refusé : " + result.error, true);
        return;
      }
      notify("Réassort : " + std::to_string(result.data["moved"].size()) + " item(s) ajouté(s) à " + name
             + ". Vérif complète recommandée.");
      if (result.data["unknown"].size() > 0)
        notify(std::to_string(result.data["unknown"].size()) + " item(s) inconnu(s) ignoré(s).", true);
      stack.clear();
      if (verif.active && verif.lot_id == lot) {
        cancel_verif();
        if (AppWindow *verif_window = window("verif"))
          verif_window->open = false;
      }
      refresh_lots();
    });
  });
}

void App::add_stack_to_lot() {
  require_login("ajouter des items au lot", [this]() {
    if (!stack.target.valid())
      return;
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    if (body["items"].size() == 0) {
      notify("Aucun item à ajouter.", true);
      return;
    }
    add_identity(body);
    body["key"]  = stack.target.key;
    const TargetLot target = stack.target;
    api.post("/api/lots/" + url_encode(target.id) + "/add/", body, [this, target](const ApiResult &result) {
      if (!result.ok) {
        identity_refused(result);
        notify("Ajout refusé : " + result.error, true);
        return;
      }
      notify(std::to_string(result.data["moved"].size()) + " item(s) ajoute(s) au lot " + target.name + ".");
      if (result.data["unknown"].size() > 0)
        notify(std::to_string(result.data["unknown"].size()) + " item(s) inconnu(s) ignore(s).", true);
      stack.clear();
      if (verif.active && verif_covers(target.id)) {
        cancel_verif();
        if (last_report.is_null())
          if (AppWindow *verif_window = window("verif"))
            verif_window->open = false;
      }
      refresh_lots();
    });
  });
}

void App::stack_to_stock() {
  if (!privileged())
    return;
  Json body;
  body["items"] = Json::array();
  for (const std::string &iid : stack.iids())
    body["items"].push_back(iid);
  body["user"] = user_ref();
  api.post("/api/items/to-stock/", body, [this](const ApiResult &result) {
    if (!result.ok) {
      notify("Retour en stock refusé : " + result.error, true);
      return;
    }
    notify(std::to_string(result.data["moved"].size()) + " item(s) remis en stock.");
    stack.clear();
    refresh_lots();
  });
}

void App::stock_verif() {
  if (!privileged())
    return;
  Json body;
  body["items"] = Json::array();
  for (const std::string &iid : stack.iids())
    body["items"].push_back(iid);
  body["user"] = user_ref();
  api.post("/api/stock/verif/", body, [this](const ApiResult &result) {
    if (!result.ok) {
      notify("Vérif du stock refusée : " + result.error, true);
      return;
    }
    last_report     = result.data;
    last_report_lot = "Stock";
    stack.clear();
    open_window("verif");
    notify("Vérif du stock enregistrée.");
    refresh_stock();
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// Donnees

void App::refresh_item_types() {
  api.get("/api/item-types/", [this](const ApiResult &result) {
    if (result.ok) {
      catalog.item_types = result.data;
      ++catalog.item_types_version;
    }
  });
}

void App::refresh_lot_types() {
  api.get("/api/lot-types/", [this](const ApiResult &result) {
    if (result.ok) {
      catalog.lot_types = result.data;
      ++catalog.lot_types_version;
    }
    else
      notify("Types de lots : " + result.error, true);
  });
}

void App::refresh_lots() {
  catalog.loading_lots = true;
  api.get("/api/lots/", [this](const ApiResult &result) {
    catalog.loading_lots = false;
    if (result.ok) {
      catalog.lots = result.data;
      ++catalog.lots_version;
    }
    else
      notify("Lots : " + result.error, true);
  });
}

void App::refresh_users() {
  api.get("/api/users/", [this](const ApiResult &result) {
    if (result.ok) {
      catalog.users = result.data;
      ++catalog.users_version;
    }
    else
      notify("Utilisateurs : " + result.error, true);
  });
}

void App::refresh_stock() {
  catalog.loading_stock = true;
  api.get("/api/stock/?soon_days=" + std::to_string(settings.expiring_soon_days), [this](const ApiResult &result) {
    catalog.loading_stock = false;
    if (result.ok)
      catalog.stock = result.data;
    else
      notify("Stocks : " + result.error, true);
  });
}

void App::refresh_forecast() {
  catalog.loading_forecast = true;
  api.get("/api/stock/forecast/?months=6&lead_days=" + std::to_string(settings.order_lead_days)
            + "&history_months=" + std::to_string(settings.forecast_history_months),
          [this](const ApiResult &result) {
            catalog.loading_forecast = false;
            if (result.ok)
              catalog.forecast = result.data;
            else
              notify("Prévisions : " + result.error, true);
          });
}

namespace {
// Payload du QR code principal de chaque usage (utilise quand aucun modele n'est configure)
std::string main_qr_payload(TemplateCategory category) {
  switch (category) {
    case TemplateCategory::Item: return "{{iid}}";
    case TemplateCategory::ItemPack: return "{{pack_url}}";
    case TemplateCategory::LotPublic: return "{{lot_url}}";
    case TemplateCategory::LotPrivate: return "{{lot_private_url}}";
    case TemplateCategory::LotSeal: return "{{seal_url}}";
    case TemplateCategory::User: return "{{badge_url}}";
    case TemplateCategory::Generic: break;
  }
  return "{{iid}}";
}
} // namespace

TemplateDocument App::qr_only_template(TemplateCategory category) const {
  TemplateDocument document;
  document.name             = "QR seul";
  document.category         = category;
  document.media.width_mm   = settings.label_width_mm;
  document.media.height_mm  = settings.label_height_mm;
  document.media.orientation = Orientation::Landscape;
  const float margin = 1.5f;
  const float size   = std::max(5.0f, std::min(settings.label_width_mm, settings.label_height_mm) - 2.0f * margin);
  QrElement   qr{ main_qr_payload(category), (settings.label_width_mm - size) / 2.0f,
                (settings.label_height_mm - size) / 2.0f, size };
  document.elements.push_back({ "qr", ElementKind::QrCode, qr });
  return document;
}

bool App::build_label_jobs(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what,
                           std::vector< PrintJob > &jobs) {
  if (labels.empty())
    return false;
  const CategoryInfo &info = category_info(category);
  std::string         path;
  if (const auto configured = settings.label_templates.find(info.id); configured != settings.label_templates.end())
    path = configured->second;
  // pas de modele choisi : premier modele du dossier fait pour cet usage (ex : scelle.qr)
  for (const auto &candidate : path.empty() ? glob_templates("*.qr") : std::vector< std::filesystem::path >{}) {
    TemplateDocument document;
    std::string      ignored;
    if (load_template(document, candidate.string(), ignored) && document.category == category) {
      path = candidate.filename().string();
      break;
    }
  }
  TemplateDocument model;
  std::string      error;
  if (path.empty()) {
    // pas de modele : on imprime simplement le QR code centre sur l'etiquette
    model = qr_only_template(category);
    notify("Aucun modèle « " + info.label + " » : QR code seul (Gestion > Réglages pour en choisir un).");
  } else if (!build_label(path, {}, model, error)) {
    notify("Modèle d'étiquette « " + info.label + " » : " + error + " (Gestion > Réglages).", true);
    return false;
  }
  for (std::size_t index = 0; index < labels.size(); ++index) {
    PrintJob job;
    job.description = what + (labels.size() > 1 ? " " + std::to_string(index + 1) + "/" + std::to_string(labels.size()) : "");
    job.document    = model;
    job.document.parameters["today"]      = today().display();
    job.document.parameters["printed_by"] = user_name();
    job.document.parameters["titre"]      = settings.label_title;
    for (const auto &[name, value] : labels[index])
      job.document.parameters[name] = value;
    jobs.push_back(std::move(job));
  }
  return true;
}

bool App::print_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what) {
  std::vector< PrintJob > jobs;
  if (!build_label_jobs(category, labels, what, jobs))
    return false;
  print_documents(std::move(jobs));
  return true;
}

bool App::preview_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what) {
  std::vector< PrintJob > jobs;
  if (!build_label_jobs(category, labels, what, jobs))
    return false;
  preview_jobs(std::move(jobs), what);
  return true;
}

void App::preview_jobs(std::vector< PrintJob > jobs, const std::string &title) {
  if (jobs.empty())
    return;
  preview.jobs    = std::move(jobs);
  preview.title   = title;
  preview.index   = 0;
  preview.open    = true;
  preview.dirty   = true;
  preview.focus   = true;
}

void App::print_documents(std::vector< PrintJob > jobs) {
  const std::size_t count = jobs.size();
  printer.enqueue(std::move(jobs), settings.print, settings.physical_label());
  notify(std::to_string(count) + " étiquette(s) envoyée(s) à l'imprimante.");
}

} // namespace qrprotec
