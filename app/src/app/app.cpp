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
  refresh_item_types();
  refresh_lots();
}

App::~App() = default;

void App::apply_settings() {
  api.configure(settings.api_url, settings.api_token);
  set_templates_dir(settings.templates_dir);
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
  pending_action_ = nullptr;
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
  draw_login_modal();
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
    ImGui::TextColored(api.online() ? ImVec4(0.1f, 0.55f, 0.1f, 1) : ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", status.c_str());
    if (!api.online() && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", api.last_error().c_str());
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
    if (ImGui::Button("Annuler", ImVec2(160, 0))) {
      login_prompt_   = false;
      pending_action_ = nullptr;
    }
    if (!login_prompt_ || logged_in())
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
}

void App::create_first_admin() {
  Json body;
  body["matricule"]  = setup_matricule_;
  body["nom"]        = setup_nom_;
  body["prenom"]     = setup_prenom_;
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
    // le poste local est de confiance : le nouveau responsable est connecte directement
    SessionUser session;
    session.matricule   = result.data["matricule"].str();
    session.nom         = result.data["nom"].str();
    session.prenom      = result.data["prenom"].str();
    session.key_expires = result.data["key_expires"].str();
    session.privileged  = true;
    session.role        = "admin";
    user                = session;
    refresh_item_types();
    refresh_lot_types();
    notify("Administrateur créé : mode privilégié activé.");
  });
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
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(setup_busy_ || setup_matricule_.empty() || setup_nom_.empty() || setup_prenom_.empty());
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

std::string App::user_name() const {
  return user ? user->display() : std::string();
}

void App::logout(const std::string &reason) {
  const bool was_logged = logged_in();
  user.reset();
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
}

void App::login_with_badge(const ParsedScan &scan, ScanSource source) {
  Json body;
  body["matricule"] = scan.id;
  body["key"]       = scan.key;
  api.post("/api/auth/", body, [this, source](const ApiResult &result) {
    if (!result.ok) {
      feedback.error(source, settings);
      notify("Badge refusé : " + result.error, true);
      return;
    }
    SessionUser session;
    session.matricule   = result.data["matricule"].str();
    session.nom         = result.data["nom"].str();
    session.prenom      = result.data["prenom"].str();
    session.key_expires = result.data["key_expires"].str();
    session.privileged  = result.data["privileged"].boolean();
    session.role        = result.data["role"].str(session.privileged ? "admin" : "normal");
    user                = session;
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
    }
  });
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
    remote_link.start(settings.api_url, settings.api_token, remote.id);
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
        return;
      }
      if (stack.target.id == id)
        stack.target.name = result.data["name"].str();
    });
  }
  // etiquette publique : fiche du lot d'abord, la verif se lance depuis la fiche
  if (scan.key.empty() && !(verif.active && verif.lot_id == scan.id)) {
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
  if (verif.active) {
    notify("Une vérif est déjà en cours : terminez-la ou annulez-la.", true);
    open_window("verif");
    return;
  }
  start_verif(scan.id, scan.key);
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

bool App::verif_key_ok() const {
  return !verif.key.empty() || privileged() || !settings.require_private_label;
}

void App::submit_verif() {
  require_login("valider la vérif", [this]() {
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
    body["user"]     = user_ref();
    body["key"]      = verif.key;
    verif.submitting = true;
    const std::string lot_id   = verif.lot_id;
    const std::string lot_name = verif.lot["name"].str(lot_id);
    api.post("/api/lots/" + url_encode(lot_id) + "/verif/", body, [this, lot_id, lot_name](const ApiResult &result) {
      verif.submitting = false;
      if (!result.ok) {
        notify("Vérif refusée : " + result.error, true);
        return;
      }
      last_report     = result.data;
      last_report_lot = lot_name;
      stack.clear();
      cancel_verif();
      open_window("verif");
      notify(result.data["complete"].boolean() ? "Vérif enregistrée : lot complet."
                                               : "Vérif enregistrée : le lot est incomplet ou contient des périmés.",
             !result.data["complete"].boolean());
      refresh_lots();
    });
  });
}

void App::verif_target_lot() {
  if (!stack.target.valid())
    return;
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
    body["user"]            = user_ref();
    body["key"]             = verif.key;
    const std::string lot   = verif.lot_id;
    const std::string name  = verif.lot["name"].str(lot);
    verif.submitting        = true;
    api.post("/api/lots/" + url_encode(lot) + "/add/", body, [this, lot, name](const ApiResult &result) {
      if (verif.lot_id == lot)
        verif.submitting = false;
      if (!result.ok) {
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
    body["user"] = user_ref();
    body["key"]  = stack.target.key;
    const TargetLot target = stack.target;
    api.post("/api/lots/" + url_encode(target.id) + "/add/", body, [this, target](const ApiResult &result) {
      if (!result.ok) {
        notify("Ajout refusé : " + result.error, true);
        return;
      }
      notify(std::to_string(result.data["moved"].size()) + " item(s) ajoute(s) au lot " + target.name + ".");
      if (result.data["unknown"].size() > 0)
        notify(std::to_string(result.data["unknown"].size()) + " item(s) inconnu(s) ignore(s).", true);
      stack.clear();
      if (verif.active && verif.lot_id == target.id) {
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
