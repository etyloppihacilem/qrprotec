/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "app.hpp"
#include "../core/plural.hpp"
#include "../printer/logging.hpp"

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
#include <cstdlib>
#include <ctime>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace qrprotec {

namespace {
const ImVec4 kPrivilegedOrange(0.93f, 0.49f, 0.13f, 1.0f);
const ImVec4 kNormalBackground(0.12f, 0.14f, 0.16f, 1.0f);
} // namespace

std::string display_date(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : "-";
}

std::string display_datetime(const Json &value, bool long_form) {
  return relative_datetime(value.str(), Date::today(), long_form);
}

std::string display_on(const Json &value, bool long_form) {
  return relative_datetime(value.str(), Date::today(), long_form, true);
}

std::string Catalog::item_type_name(const std::string &type) const {
  for (const Json &item_type : item_types.items())
    if (item_type["type"].str() == type)
      return item_type["name"].str();
  return type;
}

std::string Catalog::item_type_description(const std::string &type) const {
  for (const Json &item_type : item_types.items())
    if (item_type["type"].str() == type)
      return item_type["description"].str();
  return "";
}

App::App(Inateck &inateck_ref) : feedback(inateck_ref), inateck(inateck_ref) {
  // volume choisi par l'utilisateur connecte : repris a sa prochaine connexion, sur n'importe quel poste
  inateck.set_volume_changed_callback([this](int volume) {
    if (!logged_in())
      return;
    Json body;
    body["volume"] = volume;
    api.patch("/api/me/scanner-volume/", body, [this](const ApiResult &result) {
      if (!result.ok)
        notify("Volume de la douchette non enregistré : " + result.error, true);
    });
  });
  settings = AppSettings::defaults();
  settings.load();
  apply_settings();
  // meme delai et meme defaut que swayidle dans qrprotec-kiosk-session
  if (std::getenv("QRPROTEC_KIOSK") != nullptr) {
    const char *blank    = std::getenv("QRPROTEC_SCREEN_BLANK_SECONDS");
    screen_blank_seconds_ = blank != nullptr ? std::atof(blank) : 1800.0;
  }
  select_default_templates();
  windows.push_back(make_scan_window());
  windows.push_back(make_lots_window());
  windows.push_back(make_verif_window());
  windows.push_back(make_stock_window());
  windows.push_back(make_inventory_window());
  windows.push_back(make_pack_window());
  windows.push_back(make_lot_admin_window());
  windows.push_back(make_journal_window());
  windows.push_back(make_users_window());
  windows.push_back(make_editor_window());
  windows.push_back(make_reports_window());
  windows.push_back(make_settings_window()); // derniere du menu Administration : toujours F12
  windows.push_back(make_phone_window());
  windows.push_back(make_lists_window());
  assign_shortcuts();
  feedback.on_phone_error = [this]() {
    remote.feedback_pending = true;
    remote.feedback_message.clear();
  };
  apply_default_open_state();
  // session refusee par le serveur (badge renouvele, PIN bloque, 12 h ecoulees) : on redemande le badge
  api.on_login_required = [this]() {
    if (logged_in())
      session_expired();
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
  apply_shared_machine();
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

// Raccourcis d'ouverture : touches de fonction, F1 a F4 pour les fenetres de tous (pile de scans, lots, listes de
// materiel, telephone ; la verif s'ouvre d'elle-meme au scan d'un lot) puis F5 a F12 pour celles des
// menus Gestion puis Administration (dans l'ordre de windows, sauf les paquets fermes, ouverts depuis le scan d'un
// paquet) : Signalements en F11, Reglages toujours en F12. Elles ne tapent rien dans les champs. Ctrl+chiffre
// choisit l'onglet d'une fenetre (tab_item dans widgets.hpp).
void App::assign_shortcuts() {
  std::vector< AppWindow * > ordered;
  for (const char *id : { "scan", "lots", "lists", "phone" })
    if (AppWindow *target = window(id))
      ordered.push_back(target);
  for (auto &target : windows)
    if (target->privileged && target->id != "pack")
      ordered.push_back(target.get());
  for (std::size_t index = 0; index < ordered.size() && index < 12; ++index) {
    const ImGuiKey key         = static_cast< ImGuiKey >(ImGuiKey_F1 + index);
    ordered[index]->shortcut       = key;
    ordered[index]->shortcut_label = ImGui::GetKeyName(key);
  }
}

void App::handle_shortcuts() {
  // Super+L : se deconnecter, depuis n'importe ou (aussi pendant la saisie du PIN)
  if (logged_in() && ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_L)) {
    manual_logout();
    return;
  }
  // fenetre modale (connexion, PIN...) : elle garde la main
  if (ImGui::GetTopMostPopupModal() != nullptr)
    return;
  for (auto &target : windows)
    if (target->shortcut && can_open(*target) && ImGui::IsKeyChordPressed(target->shortcut)) {
      // deja au premier plan : le meme raccourci la ferme (comme le menu Fenetres)
      if (target->open && focused_window_ == target->id && target->can_close(*this))
        target->open = false;
      else
        open_window(target->id);
    }
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F11))
    toggle_fullscreen();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W))
    if (AppWindow *target = window(focused_window_); target && target->open && target->can_close(*this))
      target->open = false;
}

void App::focus_next_window(int direction) {
  std::vector< AppWindow * > open;
  for (auto &target : windows)
    if (target->open && can_open(*target))
      open.push_back(target.get());
  if (open.empty())
    return;
  std::size_t index = 0;
  for (std::size_t i = 0; i < open.size(); ++i)
    if (open[i]->id == focused_window_)
      index = (i + open.size() + direction) % open.size();
  open[index]->focus_pending = true;
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
  const double now = ImGui::GetTime();
  // premiere activite apres le delai d'extinction : l'ecran se rallume, ce qu'il affiche date
  if (screen_blank_seconds_ > 0.0 && last_activity_ > 0.0 && now - last_activity_ >= screen_blank_seconds_)
    reload_after_wake();
  last_activity_ = now;
  reset_done_    = false;
}

void App::reload_after_wake() {
  info_log("Écran rallumé : rechargement des fenêtres ouvertes");
  for (auto &window : windows)
    if (window->open && window->reload_on_wake)
      window->reload_pending = true;
  next_lots_poll_    = 0.0; // voyant des lots a traiter
  next_reports_poll_ = 0.0; // rappel des signalements (admins)
}

void App::begin_frame() {
  const ImGuiIO &io = ImGui::GetIO();
  if (last_activity_ == 0.0)
    note_activity();
  if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f || ImGui::IsAnyMouseDown()
      || io.InputQueueCharacters.Size > 0)
    note_activity();
  inateck.set_access(logged_in(), admin());
  inateck.update();
  api.poll();
  poll_remote();
  for (const ScanEvent &event : inateck.take_scans())
    handle_scan(event.code, event.source);
  check_inactivity();
  if (!setup_known_ && ImGui::GetTime() >= next_setup_check_)
    check_setup();
  if (admin() && ImGui::GetTime() >= next_reports_poll_)
    refresh_report_counts();
  // voyant des lots a traiter : liste tenue a jour meme fenetre Lots fermee (pas pendant une verif)
  if (!verif.active && !catalog.loading_lots && ImGui::GetTime() >= next_lots_poll_) {
    next_lots_poll_ = ImGui::GetTime() + 120.0;
    if (catalog.lots_version > 0)
      refresh_lots(true);
  }
}

void App::check_inactivity() {
  if (!settings.shared_machine || settings.inactivity_minutes <= 0 || reset_done_)
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
  name_login_   = NameLogin{};
  pending_action_ = nullptr;
  declared_name_.clear();
  fullscreen_ = false;
  apply_default_open_state();
  refresh_lots();
  notify("Session réinitialisée après inactivité.");
}

ImVec4 App::background_color() const {
  return privileged() ? kPrivilegedOrange : kNormalBackground;
}

void App::draw() {
  handle_shortcuts();
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
  draw_douchette_modal();
  draw_tasks_modal();
  draw_problem_modal();
  draw_revision_modal();
  draw_reports_alert();
  draw_seal_opening_modal();
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
        if (!window->privileged)
          window_menu_item(*window);
      ImGui::Separator();
      if (ImGui::MenuItem("Plein écran", "Ctrl+F11", fullscreen_))
        toggle_fullscreen();
      if (ImGui::MenuItem("Fenêtre suivante", "Ctrl+Tab"))
        focus_next_window(1);
      const AppWindow *focused = window(focused_window_);
      if (ImGui::MenuItem("Fermer la fenêtre", "Ctrl+W", false, focused && focused->open && focused->can_close(*this)))
        window(focused_window_)->open = false;
      if (ImGui::MenuItem("Remettre les fenêtres en place"))
        request_layout_reset();
      ImGui::EndMenu();
    }
    // Gestion : le stock (roles gestion et admin) ; Administration : ce qui est reserve aux admins
    if (privileged() && ImGui::BeginMenu("Gestion")) {
      for (auto &window : windows)
        if (window->privileged && !window->admin_only && can_open(*window))
          window_menu_item(*window);
      ImGui::EndMenu();
    }
    if (admin() && ImGui::BeginMenu("Administration")) {
      for (auto &window : windows)
        if (window->privileged && window->admin_only && can_open(*window))
          window_menu_item(*window);
      ImGui::EndMenu();
    }
    // voyant : lots qui demandent de l'attention (rouge s'il y en a d'incomplets ou perimes, orange sinon)
    if (const LotsAttention lots = lots_attention(); lots.total > 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, lots.red > 0 ? colors::red : colors::orange);
      const std::string label = "● " + std::to_string(lots.total) + " lot" + (lots.total > 1 ? "s" : "") + " à traiter";
      if (ImGui::MenuItem(label.c_str()))
        show_lots_to_treat();
      ImGui::PopStyleColor();
      if (ImGui::BeginItemTooltip()) {
        if (lots.red > 0)
          ImGui::Text("%d %s, %s ou à sceller", lots.red, pl(lots.red, "incomplet").c_str(), pl(lots.red, "jamais vérifié").c_str());
        if (lots.expired > 0)
          ImGui::Text("%d avec des périmés", lots.expired);
        if (lots.recommended > 0)
          ImGui::Text("%s", nb(lots.recommended, "vérif recommandée", "vérifs recommandées").c_str());
        ImGui::TextDisabled("Cliquer : liste des lots à traiter.");
        ImGui::EndTooltip();
      }
    }
    // admins : signalements pas encore pris en charge
    const int fresh_reports = admin() ? report_counts["new"].integer() : 0;
    if (fresh_reports > 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, colors::red);
      const std::string label = "⚠ " + std::to_string(fresh_reports) + " signalement" + (fresh_reports > 1 ? "s" : "");
      if (ImGui::MenuItem(label.c_str()))
        open_window("reports");
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Problèmes signalés pas encore pris en charge : ouvrir la gestion des signalements.");
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
      const bool clicked = danger_button("Se déconnecter", ImVec2(0, 0), false);
      ImGui::SetItemTooltip("Super+L");
      if (clicked)
        manual_logout();
    } else {
      ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1.0f), "%s", who.c_str());
    }
    ImGui::EndMainMenuBar();
  }
  if (tinted)
    ImGui::PopStyleColor();
}

// Fenetre ouverte au premier plan : on la ferme (sauf la pile de scans, toujours ouverte). Ouverte mais derriere une autre (en plein ecran surtout) :
// on la ramene devant. Fermee : on l'ouvre.
void App::window_menu_item(AppWindow &target) {
  const char *shortcut = target.shortcut_label.empty() ? nullptr : target.shortcut_label.c_str();
  if (!ImGui::MenuItem(target.title.c_str(), shortcut, target.open))
    return;
  if (target.open && focused_window_ == target.id && target.can_close(*this))
    target.open = false;
  else
    open_window(target.id);
}

void App::draw_windows() {
  ImVec2 origin, size;
  work_area(origin, size);
  // ecran redimensionne (kiosk : Cage agrandit la fenetre a la taille de l'ecran apres la premiere
  // image, dessinee en 1280x800) : les fenetres deja placees gardent leur place en proportion
  if (last_work_size_.x > 0.0f && last_work_size_.y > 0.0f && size.x > 0.0f && size.y > 0.0f
      && (size.x != last_work_size_.x || size.y != last_work_size_.y || origin.y != last_work_origin_.y)) {
    const ImVec2 scale(size.x / last_work_size_.x, size.y / last_work_size_.y);
    for (auto &window : windows) {
      if (window->place_pending || window->last_size.x <= 0.0f)
        continue;
      window->last_pos = ImVec2(
        origin.x + (window->last_pos.x - last_work_origin_.x) * scale.x,
        origin.y + (window->last_pos.y - last_work_origin_.y) * scale.y
      );
      window->last_size      = ImVec2(window->last_size.x * scale.x, window->last_size.y * scale.y);
      window->resize_pending = true;
    }
  }
  last_work_origin_ = origin;
  last_work_size_   = size;
  for (auto &window : windows) {
    if (!can_open(*window) || !window->open) {
      window->was_open = false;
      continue;
    }
    if (!window->was_open || window->reload_pending)
      window->on_open(*this);
    window->reload_pending = false;
    const bool place  = layout_pending_ || window->place_pending;
    const auto layout = settings.layout.find(window->id);
    if (layout != settings.layout.end()) {
      const ImGuiCond cond = place ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
      ImGui::SetNextWindowPos(
        ImVec2(origin.x + layout->second.x * size.x, origin.y + layout->second.y * size.y), cond
      );
      ImGui::SetNextWindowSize(ImVec2(layout->second.w * size.x, layout->second.h * size.y), cond);
    }
    if (place) {
      window->resize_pending = false;
    } else if (window->resize_pending && !fullscreen_) {
      ImGui::SetNextWindowPos(window->last_pos, ImGuiCond_Always);
      ImGui::SetNextWindowSize(window->last_size, ImGuiCond_Always);
      window->resize_pending = false;
    }
    window->place_pending = false;
    // plein ecran : last_pos et last_size gardent la place d'avant, reprise a la sortie
    ImGuiWindowFlags flags = window->flags();
    if (fullscreen_) {
      window->fullscreen = true;
      ImGui::SetNextWindowPos(origin, ImGuiCond_Always);
      ImGui::SetNextWindowSize(size, ImGuiCond_Always);
      ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
      flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
      flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
    } else if (window->fullscreen) {
      window->fullscreen = false;
      if (window->last_size.x > 0.0f) {
        ImGui::SetNextWindowPos(window->last_pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(window->last_size, ImGuiCond_Always);
      } else {
        window->place_pending = true; // ouverte en plein ecran : disposition par defaut
      }
    }
    if (window->focus_pending) {
      ImGui::SetNextWindowFocus();
      window->focus_pending = false;
    }
    bool              open  = true;
    bool             *p_open = window->can_close(*this) ? &open : nullptr;
    const std::string label = window->title + "###" + window->id;
    if (ImGui::Begin(label.c_str(), p_open, flags)) {
      if (!fullscreen_) {
        window->last_pos  = ImGui::GetWindowPos();
        window->last_size = ImGui::GetWindowSize();
      }
      window->draw(*this);
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
      focused_window_ = window->id;
    ImGui::End();
    if (!open)
      window->open = false;
    window->was_open = window->open;
  }
  layout_pending_ = false;
}

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
  if (button("Pardon", ImVec2(-FLT_MIN, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)
      || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || escape_pressed())
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
      draw_server_settings(*this, true);
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
  if (button("Fermer") || escape_pressed())
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
    if (button("Créer l'administrateur", ImVec2(-1, 0)))
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
    if (button("Terminer")) {
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
  if (!new_pin.empty())
    body["new_pin"] = new_pin;
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
  user                = session;
  api.set_session(data["session"].str());
  // volume de la douchette propre a l'utilisateur (sinon celui par defaut du serveur)
  if (const Json &volume = data["scanner_volume"]; volume.is_number())
    inateck.set_user_volume(volume.integer());
  notify("Bonjour " + session.display()
         + (session.admin() ? " : mode administrateur activé." : session.privileged ? " : mode gestion activé." : "."));
  if (const auto expires = Date::parse(session.key_expires); expires && *expires < today().plus_days(30))
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
  if (button(pin_.busy ? "Vérification..." : "Valider", ImVec2(150, 0)) || (submit && valid && !pin_.busy)) {
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
      if (danger_button("Confirmer")) {
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

// ---------------------------------------------------------------------------------------------------------------------
// Scans

void App::handle_scan(const std::string &code, ScanSource source) {
  note_activity();
  if (is_douchette(code)) {
    // tirage sans repetir la phrase precedente
    static std::mt19937 rng{std::random_device{}()};
    const std::size_t   count = douchette_phrases().size();
    std::size_t         next  = std::uniform_int_distribution< std::size_t >(0, count - 2)(rng);
    douchette_index_          = next >= douchette_index_ ? next + 1 : next;
    douchette_pending_        = true;
    return;
  }
  // popup « Passer a … ? » ignoree : le scan suivant vaut « Passer » (on passe au lot scanne, scans gardes)
  if (verif.leave_prompt.open)
    answer_leave(1);
  const ParsedScan scan = parse_scan(code);
  // QR code d'une liste de materiel : la liste s'ouvre ; liste ouverte en mode scan : les lots et items y comptent
  if (scan.kind == ScanKind::Checklist) {
    checklist_to_open = scan.id;
    open_window("lists");
    return;
  }
  if (const AppWindow *lists = window("lists"); lists && lists->open && !checklist_scan.id.empty()
      && checklist_scan.handle && (scan.kind == ScanKind::Item || scan.kind == ScanKind::Lot)) {
    checklist_scan.handle(scan, source);
    return;
  }
  const bool       same_item = tasks_.entry_id && scan.kind == ScanKind::Item && stack.find(tasks_.entry_id)
                        && stack.find(tasks_.entry_id)->scan.id == scan.id;
  // fenetre des taches ouverte et autre code scanne : l'item reste a faire (le rescanner reprend ses taches)
  if (tasks_.entry_id && !same_item)
    finish_tasks(false);
  // item a taches rescanne : un administrateur valide toutes ses taches en scannant deux fois de suite son
  // etiquette, sinon la fenetre des taches se rouvre
  if (scan.kind == ScanKind::Item) {
    ScanEntry *existing = stack.find_item(scan.id);
    if (existing && existing->tasks_pending) {
      if (same_item && user && user->admin()) {
        finish_tasks(true);
      } else {
        feedback.task(source, settings);
        open_tasks(existing->id);
      }
      return;
    }
  }
  // scanner deux fois la meme chose n'a pas de sens : le doublon est ignore, sans signal sonore
  if (scan.kind == ScanKind::Item || scan.kind == ScanKind::SealedPack) {
    ScanEntry *existing = scan.kind == ScanKind::Item ? stack.find_item(scan.id) : stack.find_code(scan.raw);
    if (existing) {
      existing->highlight_until = ImGui::GetTime() + 1.5;
      last_duplicate_           = existing->title.empty() ? describe_scan(scan) : existing->title;
      last_duplicate_time_      = ImGui::GetTime();
      return;
    }
  }
  switch (scan.kind) {
    case ScanKind::User: login_with_badge(scan, source); return;
    case ScanKind::Lot: scan_lot(scan, source); return;
    case ScanKind::LotSeal: scan_lot_seal(scan, source); return;
    case ScanKind::LotSealOpen: scan_lot_seal_open(scan, source); return;
    case ScanKind::Item: {
      ScanEntry &entry = stack.add(scan, source);
      const int  id    = entry.id;
      entry.place      = verif_place();
      if (entry.expired) {
        feedback.error(source, settings);
        notify("PÉRIMÉ : " + scan.id + " (" + scan.peremption->display() + ")", true);
      } else if (scan.task) {
        feedback.task(source, settings);
      }
      resolve_item(id);
      return;
    }
    case ScanKind::SealedPack: {
      ScanEntry &entry = stack.add(scan, source);
      entry.place      = verif_place();
      const int id     = entry.id;
      resolve_pack(id);
      // le responsable peut ouvrir le paquet directement depuis sa fiche
      if (privileged())
        show_pack(scan.id);
      return;
    }
    case ScanKind::Checklist: return;
    case ScanKind::Unknown: {
      stack.add(scan, source);
      feedback.error(source, settings);
      notify("Code non reconnu : " + describe_scan(scan), true);
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
  notify(describe_scan(entry->scan) + " : " + message, true);
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
    detail += item["status"].str() == "out" || item["status"].str() == "loose" ? "" : item["location"].is_null() ? " - en stock" : " - " + item["location_name"].str();
    const std::string status = item["status"].str();
    entry->warning           = status != "active";
    if (status == "missing")
      detail += " - signalé disparu";
    else if (status == "deleted")
      detail += " - marqué supprimé";
    else if (status == "replaced")
      detail += " - déjà remplacé";
    else if (status == "loose")
      detail += " - hors lot (retiré de son lot), pas encore en stock";
    else if (status == "out")
      detail += item["out_until"].is_null()
                  ? std::string(" - sorti du stock (non consommable)")
                  : " - sorti du stock, compté utilisé le " + display_date(item["out_until"]) + " s'il ne revient pas";
    if (item["disabled"].boolean())
      detail += " - désactivé (" + item["disabled_reason"].str() + ") : à remplacer";
    else if (item["unusable"].boolean())
      detail += " - signalé inutilisable : à remplacer";
    else if (item["reported"].boolean())
      detail += " - problème signalé";
    entry->detail = detail;
    // verif en cascade : un item d'un lot deja valide pendant la verif n'est pas recompte ; un item d'un sous-lot du
    // lot en cours fait entrer dans ce sous-lot
    if (verif_cascade() && !item["location"].str().empty() && verif_lot(item["location"].str())) {
      const std::string where = item["location"].str();
      const std::string name  = (*verif_lot(where))["name"].str();
      if (verif.done.count(where)) {
        stack.remove(entry_id);
        feedback.notice(source, settings);
        notify(item["type_name"].str() + " : " + name + " a déjà été validé, item non recompté (scannez l'étiquette de "
                 + name + " pour le refaire).",
               true);
        return;
      }
      // item d'un autre lot alors que le lot en cours est complet : celui-ci est valide automatiquement, puis on entre
      // dans le lot de l'item ; l'item compte dans son lot, pas dans celui qui est valide. Lot en cours incomplet et
      // deja commence : popup « Passer a … ? » ; sinon on passe au lot de l'item sans question.
      entry->place       = where; // pas envoye avec le lot valide automatiquement
      const bool closing = !verif_under(where, verif_level())
                        && (leave_complete_level(where, true) || ask_leave(where, entry_id));
      if (!closing && where != verif_level()) {
        enter_level(where);
        const std::string parent = verif_parent(where);
        notify("Entrée dans " + name + " : scannez son contenu, puis validez-le"
               + (verif_lot(parent) ? " pour revenir à " + (*verif_lot(parent))["name"].str() : "") + ".");
      }
    }
    if (item["expired"].boolean() && !entry->expired) {
      entry->expired = true;
      feedback.error(source, settings);
    }
    if (item["reported"].boolean())
      alert_reports("item:" + item["iid"].str(), "Item signalé : " + item["type_name"].str(), item["reports"], source);
    // item a taches perime ou desactive : a remplacer, ses taches n'ont plus de sens. L'ecran clignote en rouge
    // (deja fait au scan ou plus haut), sans fenetre des taches, et l'item compte comme un perime.
    if (item["tasks"].size() > 0 && entry->expired)
      return;
    // item a taches : compte comme vu seulement une fois ses taches cochees et validees
    if (item["tasks"].size() > 0 && !entry->tasks_done) {
      entry->tasks_pending = true;
      if (!entry->scan.task && !entry->expired)
        feedback.task(source, settings);
      open_tasks(entry_id);
    }
  });
}

void App::open_tasks(int entry_id) {
  const ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  tasks_.open_pending = true;
  if (tasks_.entry_id == entry_id)
    return; // deja ouverte : les cases cochees restent
  tasks_.entry_id = entry_id;
  tasks_.tasks.clear();
  for (const Json &task : entry->data["tasks"].items())
    tasks_.tasks.push_back(task.str());
  tasks_.checked.assign(tasks_.tasks.size(), 0);
}

void App::finish_tasks(bool done) {
  ScanEntry *entry = stack.find(tasks_.entry_id);
  tasks_.entry_id  = 0;
  tasks_.open_pending = false;
  if (!entry)
    return;
  if (!done) {
    notify(entry->title + " : non validé, toutes ses tâches doivent être cochées (rescannez-le pour les reprendre).", true);
    return;
  }
  entry->tasks_pending = false;
  entry->tasks_done    = true;
  entry->detail        = "Tâches faites - " + entry->detail;
  notify(entry->title + " : tâches faites, item validé.");
}

void App::draw_tasks_modal() {
  static const char *kTitle = "Tâches de l'item###tasks";
  if (tasks_.open_pending) {
    tasks_.open_pending = false;
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const ScanEntry *entry = stack.find(tasks_.entry_id);
  if (!entry || !entry->tasks_pending) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted(entry->title.c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s", entry->scan.id.c_str());
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
  ImGui::TextUnformatted("Vous devez compléter toutes les tâches pour valider cet item.");
  ImGui::Separator();
  bool all = true;
  for (std::size_t index = 0; index < tasks_.tasks.size(); ++index) {
    bool checked = tasks_.checked[index] != 0;
    ImGui::PushID(static_cast< int >(index));
    if (ImGui::Checkbox(tasks_.tasks[index].c_str(), &checked))
      tasks_.checked[index] = checked ? 1 : 0;
    ImGui::PopID();
    all = all && checked;
  }
  ImGui::Separator();
  if (user && user->admin())
    ImGui::TextDisabled("Administrateur : scanner à nouveau l'étiquette valide toutes les tâches.");
  ImGui::PopTextWrapPos();
  bool close = false, done = false;
  ImGui::BeginDisabled(!all);
  if (button("Valider", ImVec2(ImGui::GetFontSize() * 10.0f, 0)))
    close = done = true;
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(ImGui::GetFontSize() * 10.0f, 0)) || escape_pressed())
    close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    finish_tasks(done);
  }
  ImGui::EndPopup();
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
    if (result.data["disabled"].integer() > 0) {
      // items desactives (rappel produit) : a remplacer comme des perimes
      entry->detail += " - " + nb(result.data["disabled"].integer(), "item désactivé", "items désactivés");
      if (!entry->expired) {
        entry->expired = true;
        feedback.error(source, settings);
      }
    }
    alert_reports("pack:" + result.data["id"].str(),
                  "Paquet signalé : " + result.data["count"].str() + " x " + result.data["type_name"].str(),
                  result.data["reports"], source);
  });
}

// QR code d'un scelle : le lot est valide sans verif tant que le scelle est intact.
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

void App::scan_lot_seal(const ParsedScan &scan, ScanSource source) {
  // verif en cours d'un autre lot : le scelle d'un lot du meme lot global est constate intact (VerifSession::sealed)
  const bool during_verif = verif.active && !verif.loading && scan.id != verif.lot_id;
  if (!during_verif)
    show_lot(scan.id, scan.key);
  api.get("/api/lots/" + url_encode(scan.id) + "/?seal=" + url_encode(scan.key),
          [this, id = scan.id, code = scan.key, source, during_verif](const ApiResult &result) {
            if (!result.ok) {
              feedback.error(source, settings);
              notify("Lot inconnu : " + id, true);
              return;
            }
            const std::string check = result.data["seal_check"].str();
            const std::string name  = result.data["name"].str();
            if (during_verif && verif.active && root_of(result.data) == root_of(verif.lot)) {
              if (check != "valid" || result.data["expired_count"].integer() > 0) {
                feedback.error(source, settings);
                notify(name + (check != "valid" ? " : scellé brisé ou étiquette d'un ancien scellé"
                                                : " : scellé intact mais contient des périmés")
                         + ", ouvrez le lot et scannez son contenu.",
                       true);
                return;
              }
              const bool known = std::any_of(verif.sealed.begin(), verif.sealed.end(),
                                             [&](const VerifSealed &sealed) { return sealed.id == id; })
                                 || (verif.done.count(id) && verif.done[id].sealed);
              // scelle d'un lot hors du lot en cours, alors que celui-ci est complet : il est valide automatiquement
              if (verif_cascade() && verif_lot(id) && !verif_under(id, verif_level()))
                leave_complete_level(id, false);
              if (!known)
                verif.sealed.push_back({ id, code, name });
              notify(name + " : scellé intact, compté comme vérifié sans être ouvert.");
              flush_cascade_seals();
              return;
            }
            if (during_verif)
              show_lot(id, code);
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

// Etiquette d'ouverture (rangee dans le lot scelle) : son scan ouvre le scelle, que quelqu'un soit connecte ou non.
void App::scan_lot_seal_open(const ParsedScan &scan, ScanSource source) {
  Json body;
  body["code"] = scan.key;
  add_identity(body);
  api.post("/api/lots/" + url_encode(scan.id) + "/seal-open/", body, [this, id = scan.id, source](const ApiResult &result) {
    if (!result.ok) {
      feedback.error(source, settings);
      notify(result.error.empty() ? "Lot inconnu : " + id : result.error, true);
      return;
    }
    const Json       &lot  = result.data["lot"];
    const std::string name = lot["name"].str();
    const std::string what = result.data["result"].str();
    if (what == "signed") {
      notify(name + " : ouverture du scellé signée par " + lot["unsealed_by"].str() + ".");
      if (seal_opening_.result["lot"]["id"].str() == id)
        seal_opening_.result = result.data;
    } else {
      // ecran en grand : rassurer, dire que les responsables sont au courant, pas de verif
      if (!result.data["seal"]["valid"].boolean())
        feedback.notice(source, settings);
      seal_opening_.result       = result.data;
      seal_opening_.open_pending = true;
    }
    refresh_lots();
  });
}

void App::draw_seal_opening_modal() {
  static const char *kTitle = "Ouverture du scellé###seal_opening";
  if (seal_opening_.open_pending) {
    seal_opening_.open_pending = false;
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  // en grand : presque tout l'ecran, visible de loin
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.85f), ImGuiCond_Always);
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    return;
  const Json       &result = seal_opening_.result;
  const Json       &lot    = result["lot"];
  const Json       &seal   = result["seal"];
  const float       base   = ImGui::GetStyle().FontSizeBase;
  const float       footer = ImGui::GetFrameHeightWithSpacing() * 1.6f + ImGui::GetStyle().ItemSpacing.y;
  ImGui::BeginChild("seal_opening_body", ImVec2(0, -footer));
  ImGui::PushTextWrapPos(0.0f);

  ImGui::PushFont(nullptr, base * 1.8f);
  ImGui::TextUnformatted(result["result"].str() == "already" ? "Scellé déjà ouvert"
                                                             : "Ouverture du scellé enregistrée");
  ImGui::PopFont();
  ImGui::PushFont(nullptr, base * 1.2f);
  ImGui::TextDisabled("%s - %s", lot["name"].str().c_str(), lot["lot_type_name"].str().c_str());
  ImGui::PopFont();
  ImGui::Spacing();

  std::string sealed;
  if (!seal["sealed"].is_null()) {
    sealed = "Scellé" + (seal["seal_number"].str().empty() ? "" : " n°" + seal["seal_number"].str()) + " posé " +
             display_on(seal["sealed"], true) +
             (seal["sealed_by"].str().empty() ? "" : " par " + seal["sealed_by"].str());
  }
  ImGui::Separator();
  if (seal["valid"].boolean()) {
    ImGui::PushFont(nullptr, base * 1.6f);
    ImGui::TextColored(colors::green, "✔ Le scellé était valide");
    ImGui::PopFont();
    if (seal["valid_until"].is_null()) {
      ImGui::PushFont(nullptr, base * 1.3f);
      ImGui::TextUnformatted("Contenu validé, sans date de péremption");
      ImGui::PopFont();
    } else {
      ImGui::PushFont(nullptr, base * 1.3f);
      ImGui::TextUnformatted("Contenu validé jusqu'au");
      ImGui::PopFont();
      ImGui::PushFont(nullptr, base * 2.6f);
      ImGui::TextColored(colors::green, "%s", display_date(seal["valid_until"]).c_str());
      ImGui::PopFont();
    }
  } else {
    ImGui::PushFont(nullptr, base * 1.6f);
    ImGui::TextColored(colors::orange, "⚠ Une partie du contenu n'était plus valide");
    ImGui::PopFont();
    ImGui::TextUnformatted("Vérifiez les dates avant d'utiliser un item (en rouge ci-dessous).");
  }
  if (!sealed.empty())
    ImGui::TextDisabled("%s", sealed.c_str());

  ImGui::Separator();
  ImGui::PushFont(nullptr, base * 1.4f);
  ImGui::TextUnformatted("Le TECH est au courant");
  ImGui::PopFont();
  const std::string tech = std::string("L'ouverture est enregistrée et le TECH en est informé. ") +
                           (lot["seal_required"].boolean() ? "Le lot sera vérifié puis scellé à nouveau plus tard."
                                                           : "Le lot sera vérifié plus tard.");
  ImGui::TextUnformatted(tech.c_str());
  if (!lot["unsealed"].is_null())
    ImGui::TextDisabled("Ouvert %s%s", display_on(lot["unsealed"], true).c_str(),
                        result["identified"].boolean() && !lot["unsealed_by"].str().empty()
                          ? (" par " + lot["unsealed_by"].str()).c_str()
                          : "");

  // contenu du scelle : le lot puis ses sous-lots, items regroupes par type
  ImGui::Separator();
  ImGui::PushFont(nullptr, base * 1.4f);
  ImGui::TextUnformatted("Contenu du scellé");
  ImGui::PopFont();
  std::vector< const Json * > lots{ &lot };
  for (const Json &sub : lot["descendants"].items())
    lots.push_back(&sub);
  bool any = false;
  for (const Json *sub : lots) {
    struct Group {
      std::string name;
      int         count   = 0;
      int         expired = 0;
      std::string first; // premiere peremption (AAAA-MM-JJ)
      Json        first_json;
    };
    std::vector< Group > groups;
    for (const Json &item : (*sub)["items"].items()) {
      const std::string name  = item["type_name"].str();
      auto              found = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.name == name; });
      if (found == groups.end()) {
        groups.push_back({ name });
        found = groups.end() - 1;
      }
      found->count += 1;
      if (item["expired"].boolean())
        found->expired += 1;
      const std::string date = item["peremption"].str();
      if (!date.empty() && (found->first.empty() || date < found->first)) {
        found->first      = date;
        found->first_json = item["peremption"];
      }
    }
    if (groups.empty())
      continue;
    any = true;
    std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) { return a.name < b.name; });
    if (lots.size() > 1) {
      ImGui::Spacing();
      ImGui::Indent(std::max(0, (*sub)["depth"].integer() - 1) * ImGui::GetFontSize());
      ImGui::TextUnformatted((*sub)["name"].str().c_str());
      ImGui::Unindent(std::max(0, (*sub)["depth"].integer() - 1) * ImGui::GetFontSize());
    }
    ImGui::PushID(sub);
    if (ImGui::BeginTable("seal_content", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.0f);
      ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
      for (const Group &group : groups) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d ×", group.count);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(group.name.c_str());
        ImGui::TableNextColumn();
        if (group.expired)
          ImGui::TextColored(colors::red, "%d à remplacer", group.expired);
        else if (!group.first.empty())
          ImGui::TextDisabled("jusqu'au %s", display_date(group.first_json).c_str());
      }
      ImGui::EndTable();
    }
    ImGui::PopID();
  }
  if (!any)
    ImGui::TextDisabled("Aucun item enregistré dans ce lot.");
  ImGui::PopTextWrapPos();
  ImGui::EndChild();

  ImGui::Separator();
  // pas de fermeture par Entree : la douchette en mode clavier en envoie une a chaque scan
  if (primary_button("J'ai compris", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f)) ||
      ImGui::IsKeyPressed(ImGuiKey_Escape))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::scan_lot(const ParsedScan &scan, ScanSource source) {
  if (!scan.key.empty()) {
    stack.target = { scan.id, scan.key, scan.id };
    // le serveur compare la cle : sans connexion (ou role normal) il ne la renvoie pas
    const std::string path = "/api/lots/" + url_encode(scan.id) + "/?key=" + url_encode(scan.key);
    api.get(path, [this, id = scan.id, source](const ApiResult &result) {
      if (!result.ok) {
        if (stack.target.id == id)
          stack.target = {};
        feedback.error(source, settings);
        notify("Lot inconnu : " + id, true);
        return;
      }
      if (result.data["key_check"].str() != "valid" && stack.target.id == id) {
        feedback.error(source, settings);
        notify(result.data["key_error"].str("Étiquette clé non reconnue : demandez à un responsable de la "
                                            "réimprimer."),
               true);
        stack.target = {};
        if (verif.lot_id == id)
          verif.key.clear();
        verif.keys.erase(id);
        verif.extras.erase(std::remove_if(verif.extras.begin(), verif.extras.end(),
                                          [&id](const VerifExtra &extra) { return extra.id == id; }),
                           verif.extras.end());
        return;
      }
      if (stack.target.id == id)
        stack.target.name = result.data["name"].str();
    });
  }
  // etiquette publique hors verif : fiche du lot d'abord, la verif se lance depuis la fiche
  if (scan.key.empty() && !verif.active) {
    show_lot(scan.id);
    return;
  }
  // verif en cascade : l'etiquette d'un lot du meme lot global fait entrer dans ce lot
  if (verif.active && verif_cascade() && verif_covers(scan.id)) {
    if (!scan.key.empty()) {
      if (scan.id == verif.lot_id)
        verif.key = scan.key;
      else
        verif.keys[scan.id] = scan.key;
    }
    // etiquette d'un lot hors du lot en cours, alors que celui-ci est complet : il est valide automatiquement d'abord
    if (scan.id != verif_level() && !verif_under(scan.id, verif_level())
        && (leave_complete_level(scan.id, true) || ask_leave(scan.id, -1))) {
      open_window("verif");
      return;
    }
    const bool reopened = verif.done.count(scan.id) > 0;
    enter_level(scan.id);
    const std::string name = (*verif_lot(scan.id))["name"].str();
    notify(reopened ? name + " : déjà validé pendant cette vérif, vous le refaites."
           : scan.id == verif.lot_id ? name + " : scannez l'étiquette d'un sous-lot pour y entrer."
                                     : "Entrée dans " + name + " : scannez son contenu, puis validez-le.");
    open_window("verif");
    flush_cascade_seals();
    return;
  }
  if (verif.active && verif.lot_id == scan.id) {
    verif.current = scan.id;
    if (!scan.key.empty()) {
      verif.key = scan.key;
      notify("Étiquette clé du lot reconnue.");
    }
    open_window("verif");
    return;
  }
  if (verif.active && scan.key.empty() && verif_covers(scan.id)) {
    verif.current = scan.id;
    notify("Ce lot fait partie de la vérif en cours : les items venus du stock scannés maintenant y sont rangés.");
    open_window("verif");
    return;
  }
  // etiquette (publique ou cle) d'un autre lot du meme lot global : ses items attendus s'ajoutent a la verif en
  // cours, et ses items deja scannes y ont bien ete trouves
  if (verif.active) {
    join_verif(scan.id, scan.key, source);
    return;
  }
  start_verif(scan.id, scan.key);
}


void App::join_verif(const std::string &lot_id, const std::string &key, ScanSource source) {
  for (VerifExtra &extra : verif.extras)
    if (extra.id == lot_id) {
      verif.current = lot_id;
      if (!key.empty())
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
      if (key.empty()) { // etiquette publique d'un autre lot global : sa fiche, la verif continue
        show_lot(lot_id);
        return;
      }
      feedback.error(source, settings);
      notify(result.data["name"].str(lot_id) + " n'est pas dans le même lot global : terminez ou annulez la vérif en "
               "cours.",
             true);
      open_window("verif");
      return;
    }
    verif.extras.push_back({ lot_id, key, result.data });
    verif.current = lot_id;
    notify(covered ? result.data["name"].str() + " faisait déjà partie de la vérif : étiquette clé enregistrée."
                   : "Vérif groupée : " + verif_title() + "." +
                       (verif_key_ok() ? "" : " Scannez l'étiquette clé de " + result.data["name"].str() +
                                                  " (ou d'un autre lot de la vérif) pour valider."));
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

void App::show_journal(const std::string &lot_id) {
  journal_lot_ = lot_id;
  open_window("journal");
}

std::string App::take_journal_lot() {
  std::string lot_id;
  lot_id.swap(journal_lot_);
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
    verif.current = lot_id;
    verif.loading = true;
    open_window("verif");
    api.get("/api/lots/" + url_encode(lot_id) + "/", [this, lot_id](const ApiResult &result) {
      if (!verif.active || verif.lot_id != lot_id)
        return;
      if (!result.ok) {
        verif.loading = false;
        notify("Lot " + lot_id + " : " + result.error, true);
        cancel_verif();
        return;
      }
      // sous-lot d'un lot global : vérif en cascade du lot global, en entrant dans ce sous-lot
      const std::string root = result.data["global"].is_null() ? std::string() : result.data["global"]["id"].str();
      if (!root.empty() && root != lot_id) {
        verif.lot_id     = root;
        verif.started_in = lot_id;
        if (!verif.key.empty())
          verif.keys[lot_id] = verif.key;
        verif.key.clear();
        api.get("/api/lots/" + url_encode(root) + "/", [this, root, lot_id](const ApiResult &result) {
          if (!verif.active || verif.lot_id != root)
            return;
          verif.loading = false;
          if (!result.ok) {
            notify("Lot " + root + " : " + result.error, true);
            cancel_verif();
            return;
          }
          verif.lot = result.data;
          enter_level(lot_id);
        });
        return;
      }
      verif.loading = false;
      verif.lot     = result.data;
    });
  }
}

void App::cancel_verif() {
  verif = {};
}

// Meme texte que Lots.key_hint cote serveur et keyHint dans le front web
std::string key_hint(const Json &lot) {
  const std::string location = lot["key_location"].str();
  return "L'étiquette clé est un QR code rangé à l'intérieur du lot" +
         (location.empty() ? std::string(".") : " (" + location + ").");
}

// Une seule etiquette cle d'un lot de la verif suffit : celle du lot global couvre ses sous-lots, et celle d'un
// sous-lot (ajoute a la verif en la scannant) valide la verif de tout le lot global.
bool App::verif_key_ok() const {
  // sans utilisateur connecte (nom declare), le serveur exige toujours l'etiquette privee
  if (privileged() || (logged_in() && !settings.require_private_label))
    return true;
  std::set< std::string > keyed;
  if (!verif.key.empty())
    keyed.insert(verif.lot_id);
  for (const VerifExtra &extra : verif.extras)
    if (!extra.key.empty())
      keyed.insert(extra.id);
  for (const auto &[id, key] : verif.keys) // vérif en cascade : etiquettes cles des sous-lots
    if (!key.empty())
      keyed.insert(id);
  // une seule etiquette cle d'un lot de la verif (un sous-lot suffit) valide toute la verif, comme cote serveur
  return !keyed.empty();
}

// Lot ou vont les items venus du stock scannes maintenant : derniere etiquette de lot scannee pendant la verif
std::string App::verif_place() const {
  if (!verif.active)
    return "";
  if (verif_cascade())
    return verif_level(); // vérif en cascade : le lot ou l'on est entre
  return verif.current.empty() ? verif.lot_id : verif.current;
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

std::set< std::string > App::verif_intact() const {
  std::set< std::string > result;
  if (verif.sealed.empty())
    return result;
  const std::vector< const Json * >    lots = verif_lots();
  std::map< std::string, std::string > parent_of;
  for (const Json *lot : lots) {
    const Json &path = (*lot)["path"];
    parent_of[(*lot)["id"].str()] =
      !(*lot)["parent"].str().empty() ? (*lot)["parent"].str()
                                      : (path.size() > 0 ? path[path.size() - 1]["id"].str() : std::string());
  }
  const auto under = [&](std::string id, const std::string &top) {
    for (int guard = 0; !id.empty() && guard < 64; ++guard) {
      if (id == top)
        return true;
      const auto found = parent_of.find(id);
      id               = found == parent_of.end() ? std::string() : found->second;
    }
    return false;
  };
  const std::vector< std::string > scanned = stack.iids();
  const std::set< std::string >    done(scanned.begin(), scanned.end());
  for (const VerifSealed &sealed : verif.sealed) {
    std::vector< std::string > subtree;
    bool                       opened = false; // un item du lot scanne : le lot a ete ouvert
    for (const Json *lot : lots) {
      if (!under((*lot)["id"].str(), sealed.id))
        continue;
      subtree.push_back((*lot)["id"].str());
      for (const char *list : { "items", "missing_items" })
        for (const Json &item : (*lot)[list].items())
          opened = opened || done.count(item["iid"].str()) > 0;
    }
    if (!opened)
      result.insert(subtree.begin(), subtree.end());
  }
  return result;
}

std::vector< VerifPlanLot > App::plan_verif() const {
  // lots scelles intacts : ni attendus ni destinataires d'items (services.perform_verif)
  const std::set< std::string > intact = verif_intact();
  std::vector< const Json * >   lots   = verif_lots();
  // vérif en cascade : les lots deja valides ne sont plus attendus
  lots.erase(std::remove_if(lots.begin(), lots.end(),
                            [&](const Json *lot) {
                              return intact.count((*lot)["id"].str()) > 0 || verif.done.count((*lot)["id"].str()) > 0;
                            }),
             lots.end());
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
        // un item a etiquette a dechirer non scanne a ete utilise, un item non consommable non scanne d'un lot
        // ordinaire passe hors lot (services.leaves_lot) : ils ne retiennent pas le lot
        if (!done.count(item["iid"].str()) && !item["tear_off"].boolean()
            && (item["consumable"].boolean(true) || (*lot)["storage"].boolean()))
          holding.insert(id);
      }
    for (const Json &row : (*lot)["requirements"].items())
      required[id][row["type"].str()] = row["required"].integer();
    deficit[id] = required[id];
  }
  if (plan.empty())
    return plan;
  struct Scanned {
      std::string iid, type, place;
      bool        expired = false;
  };
  // lot de la derniere etiquette scannee avant chaque item, et lot parent de chaque lot de la verif
  std::map< std::string, std::string > place_of, parent_of;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.place.empty())
      continue;
    if (entry.scan.kind == ScanKind::Item)
      place_of.emplace(entry.scan.id, entry.place);
    else if (entry.scan.kind == ScanKind::SealedPack)
      for (const std::string &iid : entry.pack_items)
        place_of.emplace(iid, entry.place);
  }
  for (const Json *lot : lots) {
    const Json &path = (*lot)["path"];
    parent_of[(*lot)["id"].str()] =
      !(*lot)["parent"].str().empty() ? (*lot)["parent"].str()
                                      : (path.size() > 0 ? path[path.size() - 1]["id"].str() : std::string());
  }
  const auto under = [&](std::string id, const std::string &place) {
    for (int guard = 0; !id.empty() && guard < 64; ++guard) {
      if (id == place)
        return true;
      const auto found = parent_of.find(id);
      id               = found == parent_of.end() ? std::string() : found->second;
    }
    return false;
  };
  const Date              today = this->today();
  // items signales inutilisables (signalement ouvert) ou desactives : a remplacer comme des perimes
  std::set< std::string > unusable;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.scan.kind == ScanKind::SealedPack) {
      // paquet ferme entierement desactive : tous ses items sont a remplacer
      if (entry.data["disabled"].integer() > 0 && entry.data["disabled"].integer() >= entry.data["count"].integer())
        unusable.insert(entry.pack_items.begin(), entry.pack_items.end());
    } else if (entry.data["unusable"].boolean() || entry.data["disabled"].boolean())
      unusable.insert(entry.scan.id);
  }
  for (const Json *lot : lots)
    for (const Json &item : (*lot)["items"].items())
      if (item["unusable"].boolean() || item["disabled"].boolean())
        unusable.insert(item["iid"].str());
  std::vector< Scanned >  newcomers;
  std::map< std::string, std::string > target;
  std::vector< Scanned >  all;
  for (const std::string &iid : scanned) {
    const ParsedScan parsed = parse_scan(iid);
    const auto       place = place_of.find(iid);
    Scanned item{ iid, parsed.item_type, place == place_of.end() ? std::string() : place->second,
                  is_expired(parsed, today) || unusable.count(iid) > 0 };
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
    const bool                 placed = index.count(item.place) > 0;
    std::vector< std::string > scope, candidates;
    for (const VerifPlanLot &row : plan) {
      const std::string id = (*row.lot)["id"].str();
      if (placed && !under(id, item.place))
        continue;
      scope.push_back(id);
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
      choice = candidates.empty() ? scope.front() : candidates.front();
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
    // rangement du stock : ses items sont ses attendus, complet seulement si tous sont scannes
    const bool storage = (*row.lot)["storage"].boolean();
    row.complete = filled && left == 0 && (storage ? !holding.count(id) : (row.touched > 0 || !holding.count(id)));
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

// ---------------------------------------------------------------------------------------------------------------------
// Vérif en cascade : un lot global qui contient des sous-lots (ex : VPS > armoires > sacs) se verifie lot par lot. On
// entre dans un sous-lot en scannant son etiquette (ou un de ses items), on le valide : sa verif est enregistree tout
// de suite (POST /api/verifs/ avec "exclude" et "keys") et l'on revient au lot qui le contient. Les sous-lots non faits
// restent tels quels. Meme fonctionnement que le front web (renderCascade, validateStep).

namespace {
bool own_content(const Json &lot) {
  return lot["requirements"].size() > 0 || lot["items"].size() > 0 || lot["missing_items"].size() > 0;
}

std::string now_hhmm() {
  const std::time_t now = std::time(nullptr);
  std::tm           local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[8];
  std::strftime(text, sizeof(text), "%H:%M", &local);
  return text;
}
} // namespace

bool App::verif_cascade() const {
  return verif.active && !verif.loading && verif.extras.empty() && verif.lot["descendants"].size() > 0;
}

std::string App::verif_level() const {
  return verif.path.empty() ? verif.lot_id : verif.path.back();
}

const Json *App::verif_lot(const std::string &id) const {
  for (const Json *lot : verif_lots())
    if ((*lot)["id"].str() == id)
      return lot;
  return nullptr;
}

std::string App::verif_parent(const std::string &id) const {
  const Json *lot = verif_lot(id);
  if (!lot)
    return "";
  const Json &path = (*lot)["path"];
  return !(*lot)["parent"].str().empty() ? (*lot)["parent"].str()
                                         : (path.size() > 0 ? path[path.size() - 1]["id"].str() : std::string());
}

std::vector< const Json * > App::verif_children(const std::string &id) const {
  std::vector< const Json * > result;
  for (const Json *lot : verif_lots())
    if ((*lot)["id"].str() != id && verif_parent((*lot)["id"].str()) == id)
      result.push_back(lot);
  return result;
}

bool App::verif_under(const std::string &id, const std::string &top) const {
  std::string current = id;
  for (int guard = 0; !current.empty() && guard < 64; ++guard) {
    if (current == top)
      return true;
    current = verif_parent(current);
  }
  return false;
}

std::set< std::string > App::verif_subtree(const std::string &id) const {
  std::set< std::string > result;
  for (const Json *lot : verif_lots())
    if (verif_under((*lot)["id"].str(), id))
      result.insert((*lot)["id"].str());
  return result;
}

bool App::verif_touched(const std::string &id) const {
  const std::set< std::string > ids = verif_subtree(id);
  for (const std::string &lot : verif.entered)
    if (ids.count(lot))
      return true;
  for (const ScanEntry &entry : stack.entries())
    if (entry.state != EntryState::Error && ids.count(entry.place.empty() ? verif.lot_id : entry.place))
      return true;
  return false;
}

std::vector< std::string > App::verif_excluded(const std::string &top) const {
  std::vector< std::string > result;
  const std::function< void(const std::string &) > walk = [&](const std::string &id) {
    for (const Json *child : verif_children(id)) {
      const std::string child_id = (*child)["id"].str();
      if (verif.done.count(child_id) || !verif_touched(child_id))
        result.push_back(child_id);
      else
        walk(child_id);
    }
  };
  walk(top);
  return result;
}

std::set< std::string > App::verif_scope(const std::string &top) const {
  std::set< std::string > result = verif_subtree(top);
  for (const std::string &excluded : verif_excluded(top))
    for (const std::string &id : verif_subtree(excluded))
      result.erase(id);
  return result;
}

void App::enter_level(const std::string &id) {
  if (!verif_lot(id))
    return;
  verif.path.clear();
  for (std::string current = id; !current.empty() && current != verif.lot_id && verif.path.size() < 64;
       current = verif_parent(current))
    verif.path.insert(verif.path.begin(), current);
  verif.current = id;
  verif.entered.insert(id);
  verif.done.erase(id);
}

void App::leave_level() {
  if (!verif.path.empty())
    verif.path.pop_back();
  verif.current = verif_level();
}

void App::finish_cascade(const std::string &message) {
  const std::size_t done = verif.done.size();
  const std::string name = verif.lot["name"].str(verif.lot_id);
  stack.clear();
  cancel_verif();
  notify(!message.empty() ? message : "Vérif de " + name + " terminée : " + nb(done, "lot validé", "lots validés") + ".");
  refresh_lots();
}

void App::submit_step(bool partial) {
  require_login(partial ? "valider la vérif partielle" : "valider le lot", [this, partial]() {
    if (!verif_cascade() || verif.submitting)
      return;
    const std::string                top     = verif_level();
    const bool                       at_root = top == verif.lot_id;
    const std::vector< std::string > exclude = verif_excluded(top);
    const std::set< std::string >    scope   = verif_scope(top);
    const Json                      *lot     = verif_lot(top);
    // scans faits dans les lots verifies (lieu du scan : le lot ou l'on etait entre)
    Json             items  = Json::array();
    Json             places = Json::object();
    std::vector< int > sent;
    std::set< std::string > seen;
    for (const ScanEntry &entry : stack.entries()) {
      if (entry.state == EntryState::Error || entry.tasks_pending)
        continue;
      const std::string place = entry.place.empty() ? verif.lot_id : entry.place;
      if (!scope.count(place))
        continue;
      std::vector< std::string > iids;
      if (entry.scan.kind == ScanKind::Item)
        iids.push_back(entry.scan.id);
      else if (entry.scan.kind == ScanKind::SealedPack)
        iids = entry.pack_items;
      else
        continue;
      sent.push_back(entry.id);
      for (const std::string &iid : iids)
        if (seen.insert(iid).second) {
          items.push_back(iid);
          places[iid] = place;
        }
    }
    if (!own_content(*lot) && !(*lot)["storage"].boolean() && sent.empty() && scope.size() <= 1 && verif.sealed.empty()) {
      // lot de regroupement sans rien a enregistrer : ses sous-lots sont faits ou laisses tels quels
      if (at_root)
        finish_cascade();
      else
        leave_level();
      return;
    }
    if (!verif_key_ok()) {
      notify("Scannez l'étiquette clé du lot pour valider. " + key_hint(*lot), true);
      return;
    }
    Json body;
    body["items"]  = items;
    body["places"] = places;
    add_identity(body);
    Json primary;
    primary["id"]  = top;
    primary["key"] = at_root ? verif.key : (verif.keys.count(top) ? verif.keys.at(top) : std::string());
    body["lots"]   = Json::array();
    body["lots"].push_back(primary);
    // etiquettes cles des autres lots du lot global scannees pendant la verif
    body["keys"] = Json::array();
    const auto add_key = [&](const std::string &id, const std::string &key) {
      if (key.empty() || id == top)
        return;
      Json entry;
      entry["id"]  = id;
      entry["key"] = key;
      body["keys"].push_back(entry);
    };
    add_key(verif.lot_id, verif.key);
    for (const auto &[id, key] : verif.keys)
      add_key(id, key);
    body["exclude"] = Json::array();
    for (const std::string &id : exclude)
      body["exclude"].push_back(id);
    body["sealed"] = Json::array();
    std::set< std::string > sent_seals;
    for (const VerifSealed &sealed : verif.sealed) {
      Json entry;
      entry["id"]   = sealed.id;
      entry["code"] = sealed.code;
      body["sealed"].push_back(entry);
      sent_seals.insert(sealed.id);
    }
    body["partial"]  = partial;
    body["step"]     = true;
    verif.submitting = true;
    const std::string root = verif.lot_id;
    const std::string name = (*lot)["name"].str(top);
    api.post("/api/verifs/", body, [this, root, top, name, at_root, partial, sent, sent_seals](const ApiResult &result) {
      if (!verif.active || verif.lot_id != root)
        return;
      verif.submitting = false;
      if (!result.ok) {
        identity_refused(result);
        notify("Vérif refusée : " + result.error, true);
        verif.after_step.clear();
        verif.auto_done.clear();
        return;
      }
      const std::string at       = now_hhmm();
      bool              finished = !partial;
      int               verified = 0, incomplete = 0;
      for (const Json &row : result.data["lots"].items()) {
        if (!row["verified"].boolean() && !row["sealed"].boolean())
          continue;
        verif.done[row["id"].str()] = { at, row["complete"].boolean(), row["sealed"].boolean() };
        if (row["verified"].boolean()) {
          ++verified;
          incomplete += row["complete"].boolean() ? 0 : 1;
          finished = finished || row["id"].str() == top;
        }
      }
      for (int id : sent)
        stack.remove(id);
      verif.sealed.erase(std::remove_if(verif.sealed.begin(), verif.sealed.end(),
                                        [&](const VerifSealed &sealed) { return sent_seals.count(sealed.id) > 0; }),
                         verif.sealed.end());
      // kit a scelle obligatoire dont on a lance la verif (dans le VPS) : valide complet, la verif s'arrete la, sur le
      // compte rendu qui propose de le sceller (on ne voit que l'etiquette exterieure des lots scelles du VPS)
      const Json *top_lot = verif_lot(top);
      if (!at_root && finished && top == verif.started_in && top_lot && (*top_lot)["seal_required"].boolean()
          && verif.done.count(top) && verif.done[top].complete) {
        last_report     = result.data;
        last_report_lot = name;
        verif.after_step.clear();
        finish_cascade(name + (privileged() ? " validé complet : scellez-le depuis le compte rendu."
                                            : " validé complet : à resceller par le TECH."));
        open_window("verif");
        return;
      }
      if (at_root && finished) {
        last_report     = result.data;
        last_report_lot = name;
        finish_cascade();
        open_window("verif");
        return;
      }
      const std::size_t missing = result.data["missing"].size();
      if (!finished)
        notify(name + (verified ? " : vérif partielle enregistrée." : " : réassort enregistré."));
      else if (verif.after_step.empty()) // validation automatique : message plus bas
        notify(name + " validé" + (incomplete ? " : incomplet, " + nb(missing, "item manquant", "items manquants") + "." : ".")
                 + " Retour au lot qui le contient.",
               incomplete > 0);
      if (finished)
        leave_level();
      // validation automatique : on continue vers le lot scanne (autre lot complet a quitter, ou entree dans ce lot)
      if (finished && !verif.after_step.empty()) {
        const std::string target = verif.after_step;
        const bool        enter  = verif.after_enter;
        const int         entry  = verif.after_entry;
        verif.after_step.clear();
        verif.after_entry = -1;
        verif.auto_done.push_back(name + (verif.done.count(top) && verif.done[top].complete ? " complet" : " incomplet"));
        if (!verif_under(target, verif_level())
            && (leave_complete_level(target, enter) || (enter && ask_leave(target, entry)))) {
          reload_verif_lots();
          return;
        }
        std::string done_names;
        for (const std::string &done_name : verif.auto_done)
          done_names += (done_names.empty() ? "" : ", ") + done_name;
        verif.auto_done.clear();
        if (enter && target != verif_level() && verif_lot(target)) {
          enter_level(target);
          notify(done_names + " : validé. Entrée dans " + (*verif_lot(target))["name"].str()
                 + " : scannez son contenu, puis validez-le.");
        } else {
          notify(done_names + " : validé.");
        }
      }
      flush_cascade_seals();
      reload_verif_lots();
      refresh_lots();
    });
  });
}

// Lot en cours (pas le lot global) complet : rien d'attendu n'y manque, il peut etre valide sans question (meme regle
// que levelComplete du front web)
bool App::verif_level_complete() const {
  if (!verif_cascade() || verif.loading || verif.submitting || !logged_in() || !verif_key_ok())
    return false;
  const std::string top = verif_level();
  const Json       *lot = verif_lot(top);
  if (top == verif.lot_id || !lot)
    return false;
  const std::set< std::string > scope = verif_scope(top);
  bool scanned = false;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.state == EntryState::Error || entry.tasks_pending)
      continue;
    scanned = scanned || scope.count(entry.place.empty() ? verif.lot_id : entry.place) > 0;
  }
  if (!scanned && own_content(*lot))
    return false;
  for (const VerifPlanLot &row : plan_verif()) {
    if (!scope.count((*row.lot)["id"].str()))
      continue;
    if (!row.complete || (*row.lot)["is_sealed"].boolean())
      return false;
  }
  return true;
}

// Scan d'un lot `id` hors du lot en cours : si celui-ci est complet, il est valide automatiquement (submit_step), puis
// on continue vers `id`. true si la validation est lancee.
bool App::leave_complete_level(const std::string &id, bool enter) {
  if (!verif_level_complete())
    return false;
  verif.after_step  = id;
  verif.after_enter = enter;
  submit_step(false);
  if (!verif.submitting) { // pas envoyee (lot de regroupement sans rien a enregistrer : deja remonte)
    verif.after_step.clear();
    return false;
  }
  return true;
}

// Scan d'un lot `target` hors du lot en cours, incomplet : popup « Passer a target ? » (valider incomplet, passer sans
// valider, rester), seulement si des scans y ont ete faits (le poste n'a pas de vérif a plusieurs). true si ouverte.
bool App::ask_leave(const std::string &target, int entry_id) {
  if (!verif_cascade() || verif.loading || verif.submitting || verif.leave_prompt.open)
    return false;
  const std::string level = verif_level();
  if (level == verif.lot_id || verif_under(target, level) || !verif_lot(target))
    return false;
  const std::set< std::string > scope = verif_scope(level);
  std::set< std::string >       scanned;
  bool                          started = false;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.state == EntryState::Error || entry.tasks_pending || entry.id == entry_id)
      continue;
    started = started || scope.count(entry.place.empty() ? verif.lot_id : entry.place) > 0;
    if (entry.scan.kind == ScanKind::Item)
      scanned.insert(entry.scan.id);
  }
  if (!started)
    return false;
  int missing = 0;
  for (const VerifPlanLot &row : plan_verif()) {
    const Json &lot = *row.lot;
    if (!scope.count(lot["id"].str()) || lot["is_sealed"].boolean())
      continue;
    if (lot["storage"].boolean()) {
      for (const Json &item : lot["items"].items())
        missing += scanned.count(item["iid"].str()) ? 0 : 1;
      continue;
    }
    for (const Json &requirement : lot["requirements"].items()) {
      const auto found = row.fresh.find(requirement["type"].str());
      missing += std::max< int >(0, static_cast< int >(requirement["required"].integer())
                                       - (found == row.fresh.end() ? 0 : found->second));
    }
  }
  verif.leave_prompt = { true, level, target, entry_id, missing };
  feedback.notice(ScanSource::Manual, settings);
  open_window("verif");
  return true;
}

void App::answer_leave(int choice) {
  const LeavePrompt prompt = verif.leave_prompt;
  verif.leave_prompt       = {};
  if (!prompt.open || !verif.active || verif_level() != prompt.level || !verif_lot(prompt.target))
    return;
  const std::string level  = (*verif_lot(prompt.level))["name"].str();
  const std::string target = (*verif_lot(prompt.target))["name"].str();
  if (choice == 0) { // valider incomplet : les items non scannes passent manquants, puis on entre dans le lot scanne
    verif.after_step  = prompt.target;
    verif.after_enter = true;
    verif.after_entry = prompt.entry_id;
    submit_step(false);
    if (!verif.submitting) { // connexion demandee puis abandonnee, ou rien a envoyer
      verif.after_step.clear();
      verif.after_entry = -1;
    }
    return;
  }
  if (choice == 1) { // passer sans valider : le lot reste a faire, ses scans sont gardes
    enter_level(prompt.target);
    notify(level + " reste à faire (scans gardés). Entrée dans " + target + " : scannez son contenu, puis validez-le.");
    return;
  }
  // rester : l'item scanne compte dans le lot ou l'on est (il y a ete trouve)
  if (ScanEntry *entry = prompt.entry_id >= 0 ? stack.find(prompt.entry_id) : nullptr) {
    entry->place = prompt.level;
    notify(entry->title + " est compté dans " + level + ", où vous l'avez trouvé.");
  } else {
    notify("Vous restez dans " + level + ".");
  }
}

// Scelles intacts constates (etiquette du scelle scannee) : enregistres tout de suite si l'on peut deja valider, sinon
// a la validation suivante
void App::flush_cascade_seals() {
  if (!verif_cascade() || verif.sealed.empty() || verif.submitting || !logged_in() || !verif_key_ok())
    return;
  Json body;
  body["items"] = Json::array();
  add_identity(body);
  Json primary;
  primary["id"]  = verif.sealed.front().id;
  primary["key"] = verif.keys.count(primary["id"].str()) ? verif.keys.at(primary["id"].str()) : std::string();
  body["lots"]   = Json::array();
  body["lots"].push_back(primary);
  body["keys"] = Json::array();
  const auto add_key = [&](const std::string &id, const std::string &key) {
    if (key.empty())
      return;
    Json entry;
    entry["id"]  = id;
    entry["key"] = key;
    body["keys"].push_back(entry);
  };
  add_key(verif.lot_id, verif.key);
  for (const auto &[id, key] : verif.keys)
    add_key(id, key);
  body["sealed"] = Json::array();
  for (const VerifSealed &sealed : verif.sealed) {
    Json entry;
    entry["id"]   = sealed.id;
    entry["code"] = sealed.code;
    body["sealed"].push_back(entry);
  }
  body["step"]     = true;
  verif.submitting = true;
  const std::string root = verif.lot_id;
  api.post("/api/verifs/", body, [this, root](const ApiResult &result) {
    if (!verif.active || verif.lot_id != root)
      return;
    verif.submitting = false;
    if (!result.ok) {
      notify("Scellé non enregistré : " + result.error, true);
      return;
    }
    const std::string at = now_hhmm();
    for (const Json &row : result.data["lots"].items())
      if (row["sealed"].boolean())
        verif.done[row["id"].str()] = { at, true, true };
    verif.sealed.clear();
    refresh_lots();
  });
}

void App::submit_verif(bool partial) {
  if (verif_cascade()) {
    submit_step(partial);
    return;
  }
  require_login(partial ? "valider la vérif partielle" : "valider la vérif", [this, partial]() {
    if (!verif.active || verif.submitting)
      return;
    if (!verif_key_ok()) {
      notify("Scannez l'étiquette clé du lot pour valider la vérif. " + key_hint(verif.lot), true);
      return;
    }
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    // lot de la derniere etiquette scannee avant chaque item (services.assign_items)
    body["places"] = Json::object();
    for (const ScanEntry &entry : stack.entries()) {
      if (entry.place.empty() || entry.state == EntryState::Error)
        continue;
      if (entry.scan.kind == ScanKind::Item && !body["places"].contains(entry.scan.id))
        body["places"][entry.scan.id] = entry.place;
      else if (entry.scan.kind == ScanKind::SealedPack)
        for (const std::string &iid : entry.pack_items)
          if (!body["places"].contains(iid))
            body["places"][iid] = entry.place;
    }
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
    // etiquettes de scelle scannees : lots scelles intacts, comptes comme verifies
    body["sealed"] = Json::array();
    for (const VerifSealed &sealed : verif.sealed) {
      Json entry;
      entry["id"]   = sealed.id;
      entry["code"] = sealed.code;
      body["sealed"].push_back(entry);
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
    submit_verif(true); // vérif en cascade : reassort du lot en cours
    return;
  }
  require_login("ajouter les items au lot", [this]() {
    if (!verif.active || verif.submitting)
      return;
    if (!verif_key_ok()) {
      notify("Scannez l'étiquette clé du lot pour ajouter des items. " + key_hint(verif.lot), true);
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
      notify("Réassort : " + nb(result.data["moved"].size(), "item ajouté", "items ajoutés") + " à " + name
             + ". Vérif complète recommandée.");
      if (result.data["unknown"].size() > 0)
        notify(nb(result.data["unknown"].size(), "item inconnu ignoré", "items inconnus ignorés") + ".", true);
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
      notify(nb(result.data["moved"].size(), "item ajouté", "items ajoutés") + " au lot " + target.name + ".");
      if (result.data["unknown"].size() > 0)
        notify(nb(result.data["unknown"].size(), "item inconnu ignoré", "items inconnus ignorés") + ".", true);
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
    notify(nb(result.data["moved"].size(), "item remis", "items remis") + " en stock.");
    stack.clear();
    refresh_lots();
  });
}

void App::stack_out() {
  require_login("sortir des items du stock", [this]() {
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    if (body["items"].size() == 0) {
      notify("Aucun item à sortir.", true);
      return;
    }
    add_identity(body);
    api.post("/api/items/out/", body, [this](const ApiResult &result) {
      if (!result.ok) {
        identity_refused(result);
        notify("Sortie refusée : " + result.error, true);
        return;
      }
      const std::size_t out = result.data["out"].size();
      notify(nb(out, "item sorti", "items sortis") + " du stock : à scanner à une vérif dans les "
             + std::to_string(result.data["days"].integer()) + " jours, sinon " + pl(out, "compté utilisé", "comptés utilisés")
             + ".");
      for (const Json &row : result.data["refused"].items())
        notify("Non sorti : " + row["iid"].str() + " (" + row["reason"].str() + ")", true);
      stack.clear();
      refresh_stock();
    });
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

void App::refresh_lots(bool quiet) {
  next_lots_poll_      = ImGui::GetTime() + 120.0;
  catalog.loading_lots = true;
  api.get("/api/lots/", [this, quiet](const ApiResult &result) {
    catalog.loading_lots = false;
    if (result.ok) {
      catalog.lots = result.data;
      ++catalog.lots_version;
    }
    else if (!quiet)
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

void App::refresh_tasks() {
  catalog.loading_tasks = true;
  api.get("/api/tasks/", [this](const ApiResult &result) {
    catalog.loading_tasks = false;
    if (result.ok)
      catalog.tasks = result.data;
    else
      notify("Tâches : " + result.error, true);
  });
}

void App::mark_lot_used(const std::string &lot_id, const std::string &name) {
  require_login("marquer ce lot comme utilisé", [this, lot_id, name]() {
    Json body = Json::object();
    add_identity(body);
    api.post("/api/lots/" + lot_id + "/used/", body, [this, name](const ApiResult &result) {
      if (identity_refused(result))
        return;
      if (!result.ok) {
        notify("Marquer comme utilisé : " + result.error, true);
        return;
      }
      notify(name + " marqué utilisé : vérif recommandée pour les suivants.");
      refresh_lots();
    });
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
    case TemplateCategory::Item: return "{{item_url}}";
    case TemplateCategory::ItemPack: return "{{pack_url}}";
    case TemplateCategory::LotPublic: return "{{lot_url}}";
    case TemplateCategory::LotPrivate: return "{{lot_private_url}}";
    case TemplateCategory::LotSeal: return "{{seal_url}}";
    case TemplateCategory::LotSealOpen: return "{{seal_open_url}}";
    case TemplateCategory::LotStorage: return "{{lot_private_url}}";
    case TemplateCategory::User: return "{{badge_url}}";
    case TemplateCategory::TaskItem: return "{{task_url}}";
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
  const auto model_for = [this](TemplateCategory wanted, TemplateDocument &model) {
    const CategoryInfo &info = category_info(wanted);
    std::string         path;
    if (const auto configured = settings.label_templates.find(info.id); configured != settings.label_templates.end())
      path = configured->second;
    // pas de modele choisi : premier modele du dossier fait pour cet usage (ex : scelle.qr)
    for (const auto &candidate : path.empty() ? glob_templates("*.qr") : std::vector< std::filesystem::path >{}) {
      TemplateDocument document;
      std::string      ignored;
      if (load_template(document, candidate.string(), ignored) && document.category == wanted) {
        path = candidate.filename().string();
        break;
      }
    }
    std::string error;
    if (path.empty()) {
      // pas de modele : on imprime simplement le QR code centre sur l'etiquette
      model = qr_only_template(wanted);
      notify("Aucun modèle « " + info.label + " » : QR code seul (Administration > Réglages pour en choisir un).");
    } else if (!build_label(path, {}, model, error)) {
      notify("Modèle d'étiquette « " + info.label + " » : " + error + " (Administration > Réglages).", true);
      return false;
    }
    return true;
  };
  // items dont le type a des taches : etiquette de taches (TaskItem) a la place de l'etiquette d'item
  const auto has_tasks = [category](const Parameters &label) {
    const auto count = label.find("task_count");
    return category == TemplateCategory::Item && count != label.end() && !count->second.empty() && count->second != "0";
  };
  TemplateDocument model, task_model;
  bool             need_plain = false, need_tasks = false;
  for (const Parameters &label : labels)
    (has_tasks(label) ? need_tasks : need_plain) = true;
  if (need_plain && !model_for(category, model))
    return false;
  if (need_tasks && !model_for(TemplateCategory::TaskItem, task_model))
    return false;
  for (std::size_t index = 0; index < labels.size(); ++index) {
    PrintJob job;
    job.description = what + (labels.size() > 1 ? " " + std::to_string(index + 1) + "/" + std::to_string(labels.size()) : "");
    job.document    = has_tasks(labels[index]) ? task_model : model;
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

// Etiquettes d'un scellage : celle collee sur le scelle (elle tient lieu d'etiquette publique) et l'etiquette
// d'ouverture rangee dans le lot (son scan ouvre le scelle). Meme apercu partout ou l'on scelle un lot.
void App::preview_seal_labels(const Json &lot) {
  const Parameters        parameters = lot_parameters(lot);
  std::vector< PrintJob > jobs;
  build_label_jobs(TemplateCategory::LotSeal, { parameters }, "Scellé " + lot["name"].str(), jobs);
  // scelle pose avant l'etiquette d'ouverture : il n'en a pas
  if (!lot["seal_open_url"].str().empty())
    build_label_jobs(TemplateCategory::LotSealOpen, { parameters }, "Ouverture " + lot["name"].str(), jobs);
  preview_jobs(std::move(jobs), "Scellé " + lot["name"].str());
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
  notify(nb(count, "étiquette envoyée", "étiquettes envoyées") + " à l'imprimante.");
}

} // namespace qrprotec

namespace qrprotec {

// ---------------------------------------------------------------------------------------------------------------------
// Signalements de problemes (database/inventory/reports.py)

void App::report_problem(const ProblemTarget &target) {
  require_login("signaler un problème", [this, target]() {
    problem_              = ProblemPrompt{};
    problem_.target       = target;
    problem_.open_pending = true;
  }, false);
}

void App::draw_problem_modal() {
  static const char *kTitle = "Signaler un problème###problem";
  if (problem_.open_pending) {
    problem_.open_pending = false;
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const bool  pack   = !problem_.target.pack.empty();
  const bool  object = !problem_.target.iid.empty() || !problem_.target.type.empty();
  const float width  = ImGui::GetFontSize() * 26.0f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
  ImGui::TextUnformatted(problem_.target.label.c_str());
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::TextUnformatted(pack     ? "Le paquet est-il toujours utilisable ?"
                         : object ? "L'objet est-il toujours utilisable ?"
                                  : "Le lot est-il toujours utilisable ?");
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.88f, 0.88f, 0.88f, 1.0f)); // cases visibles sur le fond blanc
  if (ImGui::RadioButton("Oui, toujours utilisable", problem_.usable == 1))
    problem_.usable = 1;
  if (ImGui::RadioButton("Non, à remplacer", problem_.usable == 0))
    problem_.usable = 0;
  ImGui::Spacing();
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  ImGui::InputTextMultiline("##comment", &problem_.comment, ImVec2(width, ImGui::GetTextLineHeight() * 5));
  ImGui::PopStyleColor();
  if (problem_.comment.size() > 1000)
    problem_.comment.resize(1000);
  ImGui::TextDisabled("Commentaire facultatif. Envoyé tout de suite au TECH, indépendamment de la vérif.");
  if (!problem_.error.empty())
    ImGui::TextColored(colors::red, "%s", problem_.error.c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(problem_.busy);
  if (primary_button(problem_.busy ? "Envoi..." : "Envoyer", ImVec2(width * 0.5f, 0))) {
    if (problem_.usable < 0)
      problem_.error = object ? "Indiquez si l'objet est toujours utilisable." : "Indiquez si le lot est toujours utilisable.";
    else
      send_problem();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(-1, 0)) || (!problem_.busy && escape_pressed())) {
    problem_ = ProblemPrompt{};
    ImGui::CloseCurrentPopup();
  }
  if (problem_.target.label.empty() && !problem_.busy) // envoye
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::send_problem() {
  Json body;
  body["user"]    = user_ref();
  body["item"]    = problem_.target.iid;
  body["pack"]    = problem_.target.pack;
  body["lot"]     = problem_.target.iid.empty() && problem_.target.pack.empty() ? problem_.target.lot : std::string();
  body["type"]    = problem_.target.type;
  body["comment"] = problem_.comment;
  body["usable"]  = problem_.usable == 1;
  problem_.busy   = true;
  problem_.error.clear();
  api.post("/api/reports/", body, [this](const ApiResult &result) {
    problem_.busy = false;
    if (!result.ok) {
      problem_.error = result.error;
      return;
    }
    // son propre signalement : pas de popup quand l'item est relu
    reports_alert_.last_key = problem_.target.pack.empty() ? "item:" + problem_.target.iid : "pack:" + problem_.target.pack;
    reports_alert_.last_at  = ImGui::GetTime();
    problem_ = ProblemPrompt{}; // ferme la fenetre
    notify("Signalement envoyé au TECH.");
    if (admin())
      refresh_report_counts();
    const Json &item = result.data["item"];
    if (item.is_null() || !verif.active || verif.loading || !verif_covers(item["location"].str()))
      return;
    // item du lot en cours de verif : il est la, il compte comme vu (jamais marque disparu)
    const std::string iid = item["iid"].str();
    if (const ScanEntry *entry = stack.find_item(iid))
      resolve_item(entry->id); // deja scanne : etat relu (inutilisable = a remplacer)
    else
      handle_scan(iid, ScanSource::Manual);
    reload_verif_lots();
  });
}

namespace {
// meme jour `months` mois plus tard (dernier jour du mois s'il n'existe pas), comme models.add_months du serveur
Date add_months(const Date &day, int months) {
  static const int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  const int index = day.month - 1 + months;
  Date      result{ day.year + index / 12, index % 12 + 1, 1 };
  const bool leap  = (result.year % 4 == 0 && result.year % 100 != 0) || result.year % 400 == 0;
  const int  last  = lengths[result.month - 1] + (result.month == 2 && leap ? 1 : 0);
  result.day       = std::min(day.day, last);
  return result;
}
} // namespace

void App::open_revision(const Json &item) {
  require_login("faire la révision", [this, item]() {
    revision_           = RevisionPrompt{};
    revision_.iid       = item["iid"].str();
    revision_.type_name = item["type_name"].str();
    revision_.due       = item["revision_due"].str();
    revision_.months    = std::max(1, item["revision_months"].integer(1));
    for (const Json &task : item["revision_tasks"].items())
      revision_.tasks.push_back(task.str());
    revision_.checked.assign(revision_.tasks.size(), 0);
    revision_.next_due     = add_months(today(), revision_.months).display();
    revision_.open_pending = true;
  }, false);
}

void App::draw_revision_modal() {
  static const char *kTitle = "Révision###revision";
  if (revision_.open_pending) {
    revision_.open_pending = false;
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (revision_.iid.empty()) { // enregistree ou annulee
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const float width = ImGui::GetFontSize() * 26.0f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::Text("Révision : %s", revision_.type_name.c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s%s", revision_.iid.c_str(),
                      revision_.due.empty() ? "" : (" · prévue le " + display_date(Json(revision_.due))).c_str());
  ImGui::PushTextWrapPos(width);
  ImGui::TextUnformatted(revision_.tasks.empty() ? "Pas de tâche définie pour ce type."
                                                 : "Cochez toutes les tâches de la révision.");
  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.88f, 0.88f, 0.88f, 1.0f)); // cases visibles sur le fond blanc
  bool all = true;
  for (std::size_t index = 0; index < revision_.tasks.size(); ++index) {
    bool checked = revision_.checked[index] != 0;
    ImGui::PushID(static_cast< int >(index));
    if (ImGui::Checkbox(revision_.tasks[index].c_str(), &checked))
      revision_.checked[index] = checked ? 1 : 0;
    ImGui::PopID();
    all = all && checked;
  }
  ImGui::Separator();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
  ImGui::InputText("Prochaine révision", &revision_.next_due);
  const auto next = parse_user_date(revision_.next_due);
  if (ImGui::IsItemDeactivatedAfterEdit() && next)
    revision_.next_due = next->display();
  const bool next_ok = next && today() < *next;
  ImGui::SameLine();
  if (!next)
    ImGui::TextColored(colors::red, "date non reconnue");
  else if (!next_ok)
    ImGui::TextColored(colors::red, "doit être après aujourd'hui");
  else
    ImGui::TextColored(colors::green, "→ %s", next->display().c_str());
  ImGui::TextDisabled("Par défaut dans %d mois (périodicité du type).", revision_.months);
  ImGui::InputTextWithHint("##note", "Remarque (facultatif)", &revision_.note);
  ImGui::PopStyleColor();
  if (revision_.note.size() > 256)
    revision_.note.resize(256);
  if (!revision_.error.empty())
    ImGui::TextColored(colors::red, "%s", revision_.error.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::BeginDisabled(revision_.busy || !all || !next_ok);
  if (primary_button(revision_.busy ? "Envoi..." : "Enregistrer la révision", ImVec2(width * 0.6f, 0)))
    send_revision();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(-1, 0)) || (!revision_.busy && escape_pressed())) {
    revision_ = RevisionPrompt{};
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::send_revision() {
  const auto next = parse_user_date(revision_.next_due);
  if (!next)
    return;
  Json body;
  body["user"]     = user_ref();
  body["tasks"]    = Json::array();
  for (const std::string &task : revision_.tasks)
    body["tasks"].push_back(Json(task));
  body["next_due"] = next->iso();
  body["note"]     = revision_.note;
  revision_.busy   = true;
  revision_.error.clear();
  const std::string iid = revision_.iid;
  api.post("/api/items/" + iid + "/revision/", body, [this, iid](const ApiResult &result) {
    revision_.busy = false;
    if (!result.ok) {
      identity_refused(result);
      revision_.error = result.error;
      return;
    }
    revision_ = RevisionPrompt{}; // ferme la fenetre
    notify(result.data["type_name"].str() + " : révision enregistrée, prochaine le "
           + display_date(result.data["revision_due"]) + ".");
    if (const ScanEntry *entry = stack.find_item(iid))
      resolve_item(entry->id); // etat relu (plus en retard)
    refresh_lots(true);
  });
}

void App::alert_reports(const std::string &key, const std::string &title, const Json &reports, ScanSource source) {
  if (reports.size() == 0)
    return;
  const double now = ImGui::GetTime();
  if (reports_alert_.last_key == key && now - reports_alert_.last_at < 30.0)
    return;
  reports_alert_.last_key     = key;
  reports_alert_.last_at      = now;
  reports_alert_.title        = title;
  reports_alert_.reports      = reports;
  reports_alert_.open_pending = true;
  feedback.notice(source, settings);
}

void App::draw_reports_alert() {
  static const char *kTitle = "Objet signalé###reports_alert";
  if (reports_alert_.open_pending && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    reports_alert_.open_pending = false;
    ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const float width = ImGui::GetFontSize() * 28.0f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
  ImGui::TextColored(colors::orange, "%s", reports_alert_.title.c_str());
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  for (const Json &report : reports_alert_.reports.items()) {
    ImGui::Separator();
    const bool usable = report["usable"].boolean();
    ImGui::TextColored(usable ? colors::orange : colors::red, "%s",
                       usable ? "Toujours utilisable" : "Inutilisable : à remplacer");
    ImGui::TextDisabled("%s - signalé par %s %s", report["status_label"].str().c_str(), report["by"].str().c_str(),
                        display_on(report["created"]).c_str());
    if (report["comment"].str().empty())
      ImGui::TextDisabled("Sans commentaire.");
    else
      ImGui::TextWrapped("« %s »", report["comment"].str().c_str());
    if (!report["note"].str().empty())
      ImGui::TextWrapped("Réponse : %s", report["note"].str().c_str());
  }
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  // pas de fermeture par Entree : la douchette en mode clavier en envoie une a chaque scan
  if (primary_button("J'ai compris", ImVec2(width, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::reload_verif_lots() {
  const std::string lot_id = verif.lot_id;
  api.get("/api/lots/" + url_encode(lot_id) + "/", [this, lot_id](const ApiResult &result) {
    if (result.ok && verif.active && verif.lot_id == lot_id)
      verif.lot = result.data;
  });
  for (const VerifExtra &extra : verif.extras) {
    const std::string id = extra.id;
    api.get("/api/lots/" + url_encode(id) + "/", [this, id](const ApiResult &result) {
      if (!result.ok || !verif.active)
        return;
      for (VerifExtra &row : verif.extras)
        if (row.id == id)
          row.lot = result.data;
    });
  }
}

void App::refresh_report_counts() {
  next_reports_poll_ = ImGui::GetTime() + 60.0;
  Json body;
  body["user"] = user_ref();
  body["show"] = "open";
  api.post("/api/reports/list/", body, [this](const ApiResult &result) {
    if (result.ok)
      report_counts = result.data["counts"];
    if (reports_on_login_ && admin() && !verif.active && report_counts["new"].integer() > 0)
      open_reports_beside_lots();
    reports_on_login_ = false;
  });
}

void App::open_reports_beside_lots() {
  // place pour la session (la disposition enregistree ne change pas) : Lots a gauche, Signalements a droite
  const auto place = [this](const std::string &id, float x, float width) {
    AppWindow *target = window(id);
    if (!target)
      return;
    open_window(id);
    if (last_work_size_.x <= 0.0f)
      return;
    target->place_pending  = false;
    target->resize_pending = true;
    target->last_pos       = ImVec2(last_work_origin_.x + x * last_work_size_.x, last_work_origin_.y);
    target->last_size      = ImVec2(width * last_work_size_.x, last_work_size_.y);
  };
  const auto  scan = settings.layout.find("scan");
  const float work = scan != settings.layout.end() && scan->second.x > 0.0f ? scan->second.x : 0.72f;
  reports_beside_lots_ = true;
  place("lots", 0.0f, work * 0.5f);
  place("reports", work * 0.5f, work * 0.5f);
}

App::LotsAttention App::lots_attention() const {
  LotsAttention count;
  for (const Json &lot : catalog.lots.items()) {
    if (lot["storage"].boolean() || lot_is_group(lot))
      continue;
    const LotStatus status  = lot_status(lot);
    const bool      expired = lot["expired_count"].integer() > 0;
    if (lot_ok(status) && !expired)
      continue;
    ++count.total;
    if (expired)
      ++count.expired;
    if (status == LotStatus::Recommended && !expired)
      ++count.recommended;
    else
      ++count.red;
  }
  return count;
}

void App::show_lots_to_treat() {
  lots_to_treat_ = true;
  open_window("lots");
}

bool App::take_lots_to_treat() {
  return std::exchange(lots_to_treat_, false);
}

} // namespace qrprotec
