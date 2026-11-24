/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

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
#include "../core/system.hpp"
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

namespace {
const ImVec4 kNormalBackground(0.12f, 0.14f, 0.16f, 1.0f);
// gestion / admin connecte : seule la barre des menus generale passe en bleu fonce (marine du web), le reste garde
// le theme de base pour que les couleurs d'alerte et les voyants de la barre ressortent
const ImVec4 kPrivilegedMenuBar(0.08f, 0.16f, 0.29f, 1.0f); // #14284b

// Couleurs des elements poses directement sur la barre des menus : claires sur le bleu fonce, celles du theme sinon.
// Poussees seulement autour de l'element de la barre, jamais pendant le contenu d'un menu (fond clair).
struct BarColors {
    bool   dark;
    ImVec4 text, red, orange, blue, green, grey;
};

BarColors bar_colors(bool dark) {
  if (dark)
    return { true,
             ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
             ImVec4(1.0f, 0.47f, 0.47f, 1.0f),
             ImVec4(1.0f, 0.72f, 0.32f, 1.0f),
             ImVec4(0.58f, 0.78f, 1.0f, 1.0f),
             ImVec4(0.50f, 0.88f, 0.50f, 1.0f),
             ImVec4(0.82f, 0.85f, 0.90f, 1.0f) };
  return { false, ImGui::GetStyleColorVec4(ImGuiCol_Text), ImVec4(0.8f, 0.1f, 0.1f, 1), colors::orange, colors::blue,
           ImVec4(0.1f, 0.55f, 0.1f, 1), ImVec4(0.2f, 0.2f, 0.2f, 1) };
}

int push_bar_item(const BarColors &bar, const ImVec4 &text) {
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  if (!bar.dark)
    return 1;
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.0f, 1.0f, 1.0f, 0.26f));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
  return 5;
}

bool bar_menu(const BarColors &bar, const char *label, const ImVec4 &text) {
  const int  pushed = push_bar_item(bar, text);
  const bool open   = ImGui::BeginMenu(label);
  ImGui::PopStyleColor(pushed);
  return open;
}

bool bar_menu(const BarColors &bar, const char *label) { return bar_menu(bar, label, bar.text); }

bool bar_item(const BarColors &bar, const char *label, const ImVec4 &text) {
  const int  pushed  = push_bar_item(bar, text);
  const bool clicked = ImGui::MenuItem(label);
  ImGui::PopStyleColor(pushed);
  return clicked;
}

// Bouton de la barre allume quand il est choisi (bascule Normal / Gestion)
bool bar_toggle(const BarColors &bar, const char *label, bool selected) {
  const int  pushed  = push_bar_item(bar, bar.text);
  const bool clicked = ImGui::MenuItem(label, nullptr, selected);
  ImGui::PopStyleColor(pushed);
  return clicked;
}

void bar_separator(const BarColors &bar) {
  const int pushed = push_bar_item(bar, bar.text);
  ImGui::Separator();
  ImGui::PopStyleColor(pushed);
}
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

void draw_report_replies(const Json &report) {
  ImGui::PushStyleColor(ImGuiCol_Text, colors::blue);
  if (!report["note"].str().empty())
    ImGui::TextWrapped("Réponse : %s", report["note"].str().c_str());
  if (!report["resolution_note"].str().empty())
    ImGui::TextWrapped("Résolution : %s", report["resolution_note"].str().c_str());
  ImGui::PopStyleColor();
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
  // envois de verif faits hors connexion avant un redemarrage : renvoyes au retour du reseau
  outbox.load(AppSettings::file_path().parent_path() / "verif-outbox.json");
  // etiquettes imprimees pas encore comptees par le serveur (hors connexion, redemarrage)
  print_counter.load(AppSettings::file_path().parent_path() / "print-counts.json");
  printer.set_counter(&print_counter);
  updater.start(settings.auto_update);
  apply_settings();
  // meme delai et meme defaut que swayidle dans qrprotec-kiosk-session
  if (std::getenv("QRPROTEC_KIOSK") != nullptr) {
    const char *blank    = std::getenv("QRPROTEC_SCREEN_BLANK_SECONDS");
    screen_blank_seconds_ = blank != nullptr ? std::atof(blank) : 1800.0;
  }
  select_default_templates();
  add_missing_label_types();
  windows.push_back(make_scan_window());
  windows.push_back(make_lots_window());
  windows.push_back(make_verif_window());
  windows.push_back(make_stock_window());
  windows.push_back(make_inventory_window()); // Stock
  windows.push_back(make_items_window());
  windows.push_back(make_lot_types_window());
  windows.push_back(make_reception_window());
  windows.push_back(make_pack_window());
  windows.push_back(make_journal_window());
  windows.push_back(make_users_window());
  windows.push_back(make_editor_window());
  windows.push_back(make_print_counts_window());
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
  updater.set_enabled(settings.auto_update);
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

void App::add_missing_label_types() {
  const LabelSupport support = settings.print.kind != PrinterKind::NiimbotB1 && settings.print.raw.ribbon
                               ? LabelSupport::Transfer
                               : LabelSupport::Direct;
  std::vector< LabelType >   types;
  std::vector< std::string > errors;
  const int updated = qrprotec::add_missing_label_types(settings.physical_label(), support, &types, &errors);
  for (const std::string &error : errors)
    error_log("Type d'étiquette non enregistré : " + error, "modèles");
  if (updated > 0)
    notify("Type d'étiquette ajouté à " + nb(updated, "modèle") + ".");
  if (!settings.print.loaded_label_type.empty() || types.empty())
    return;
  // rouleau en place : le type le plus courant (celui de presque tous les modeles a l'installation)
  std::map< std::string, int > counts;
  for (const LabelType &type : types)
    ++counts[type.name];
  const auto common = std::max_element(counts.begin(), counts.end(), [](const auto &a, const auto &b) {
    return a.second < b.second;
  });
  settings.print.loaded_label_type = common->first;
  std::string error;
  if (!settings.save(error))
    notify(error, true);
  printer.set_loaded_label_type(common->first);
}

void App::set_loaded_label_type(const std::string &name) {
  settings.print.loaded_label_type = name;
  std::string error;
  if (!settings.save(error))
    notify(error, true);
  printer.set_loaded_label_type(name);
  notify(name.empty() ? "Étiquettes chargées : format libre (Réglages)." : "Étiquettes chargées : « " + name + " ».");
}

bool App::import_front_config(const std::string &path, const std::string &password) {
  if (AppSettings::api_env("QRPROTEC_KIOSK") || AppSettings::api_env("QRPROTEC_API_URL")
      || AppSettings::api_env("QRPROTEC_API_KEY") || AppSettings::api_env("QRPROTEC_API_CA_FILE")) {
    notify("Connexion imposée par l'environnement : import impossible ici. Borne : sudo qrprotec-setup "
           "--front-config FICHIER.", true);
    return false;
  }
  // chemin colle depuis un gestionnaire de fichiers : espaces, guillemets, file://, ~/
  std::string file = path;
  while (!file.empty() && std::isspace(static_cast< unsigned char >(file.back())))
    file.pop_back();
  file.erase(0, file.find_first_not_of(" \t"));
  if (file.size() >= 2 && (file.front() == '\'' || file.front() == '"') && file.back() == file.front())
    file = file.substr(1, file.size() - 2);
  if (file.rfind("file://", 0) == 0)
    file.erase(0, 7);
  if (file.rfind("~/", 0) == 0)
    if (const std::filesystem::path home = home_dir(); !home.empty())
      file = (home / file.substr(2)).string();
  FrontConfig config;
  std::string error;
  if (!read_front_config(file, password, config, error)) {
    notify(error, true);
    return false;
  }
  std::string ca_file;
  if (!config.api_ca_pem.empty()) {
    const std::filesystem::path ca_path = AppSettings::file_path().parent_path() / "server-ca.crt";
    if (!write_server_ca(ca_path, config.api_ca_pem, error)) {
      notify(error, true);
      return false;
    }
    ca_file = ca_path.string();
  }
  settings.api_url     = config.api_url;
  settings.api_key     = config.api_key;
  settings.api_ca_file = ca_file;
  settings.api_token.clear(); // jeton de l'API locale, sans objet pour un serveur distant
  apply_settings();
  if (!save_settings())
    return false;
  notify("Configuration importée : front « " + config.name + " », serveur " + config.api_url
         + (ca_file.empty() ? std::string() : ", certificat de l'autorité enregistré") + ".");
  // meme verification que « Tester la connexion »
  api.get("/api/health/", [this](const ApiResult &result) {
    if (!result.ok) {
      notify("Connexion impossible : " + result.error, true);
      return;
    }
    poste_key = result.data["poste"].boolean();
    notify("Connecté au serveur (front " + result.data["front"].str()
           + (poste_key ? ", poste principal" : "") + ").");
    refresh_item_types();
    refresh_lots();
  });
  return true;
}

void App::file_dropped(const std::string &path) {
  if (path.size() < 9 || path.compare(path.size() - 9, 9, ".qrprotec") != 0)
    return;
  front_config_path = path;
  if (!api.online())
    server_modal_requested_ = true;
  else if (!user || !user->admin())
    notify("Fichier de configuration : connectez-vous en administrateur, puis Réglages > Serveur.", true);
  else
    open_window("settings");
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

// Raccourcis d'ouverture : touches de fonction, toujours les memes. F1 a F4 pour les fenetres de tous (pile de scans,
// lots, listes de materiel, telephone ; la verif s'ouvre d'elle-meme au scan d'un lot), puis en mode gestion une
// touche par domaine : Gestion de la TECH F5, Stock F6, Items F7, Types de lots F8, Reception F9, Editeur
// d'etiquettes F10, Signalements F11, Reglages F12. Les autres (journal, utilisateurs, impressions, paquet ferme) sont
// dans les menus et dans « Aller a... » (Ctrl+K). Elles ne tapent rien dans les champs. Ctrl+chiffre choisit l'onglet
// d'une fenetre (tab_item dans widgets.hpp).
void App::assign_shortcuts() {
  static const char *const kOrder[] = { "scan",      "lots",   "lists",     "phone",  "stock",   "inventory",
                                        "items",     "lot_types", "reception", "editor", "reports", "settings" };
  for (std::size_t index = 0; index < std::size(kOrder); ++index)
    if (AppWindow *target = window(kOrder[index])) {
      const ImGuiKey key         = static_cast< ImGuiKey >(ImGuiKey_F1 + index);
      target->shortcut       = key;
      target->shortcut_label = ImGui::GetKeyName(key);
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
  // Ctrl+K : « Aller a... » (lot, item, type, fenetre, action), meme pendant une saisie ; de nouveau : ferme
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive)) {
    if (hub_.open)
      hub_.close = true;
    else
      open_hub();
    return;
  }
  // Ctrl+G : mode gestion / mode normal (roles gestion et admin)
  if (manager() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive)) {
    set_gestion_mode(!gestion_mode_);
    return;
  }
  if (hub_.open)
    return; // fleches, Entree et touches F a la recherche
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
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space))
    request_layout_reset();
  // Super+fleches : ranger la fenetre sur une moitie ou un quart de l'ecran
  if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_LeftArrow))
    snap_focused(-1, 0);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_RightArrow))
    snap_focused(1, 0);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_UpArrow))
    snap_focused(0, -1);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Super | ImGuiKey_DownArrow))
    snap_focused(0, 1);
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

// Onglet d'une fenetre de gestion : chaque onglet a sa fenetre (Stock, Items, Reception)
void App::show_stock_tab(const std::string &tab) {
  stock_tab_to_show_ = tab;
  if (tab == "reception" || tab == "past_receptions")
    open_window("reception");
  else if (tab == "types" || tab == "items" || tab == "equipment" || tab == "packs")
    open_window("items");
  else
    open_window("inventory");
}

void App::set_gestion_mode(bool on) {
  if (!manager() || gestion_mode_ == on)
    return;
  // fenetres communes : on garde leur place dans le mode quitte, on reprend celle du mode choisi (sinon la
  // disposition par defaut : en mode normal, le poste comme pour un secouriste)
  for (auto &target : windows) {
    if (target->privileged)
      continue;
    mode_places_[gestion_mode_ ? 1 : 0][target->id] = { target->last_pos, target->last_size, target->snap_h,
                                                       target->snap_v, target->snap_restore_pos, target->snap_restore_size };
    const auto &places = mode_places_[on ? 1 : 0];
    if (const auto found = places.find(target->id); found != places.end() && found->second.size.x > 0.0f) {
      target->last_pos          = found->second.pos;
      target->last_size         = found->second.size;
      target->snap_h            = found->second.snap_h;
      target->snap_v            = found->second.snap_v;
      target->snap_restore_pos  = found->second.restore_pos;
      target->snap_restore_size = found->second.restore_size;
      target->resize_pending    = true;
    } else {
      target->place_pending = true;
    }
  }
  gestion_mode_ = on;
  if (on) {
    refresh_item_types();
    refresh_lot_types();
    refresh_lots(); // avec les cles des lots (etiquettes privees)
    refresh_report_counts();
    refresh_tasks();
    notify("Mode gestion : TECH, lots, items, stock, réception, étiquettes et impression dans la barre.");
  } else {
    notify("Mode normal : le poste comme pour un secouriste ou un chef. Ctrl+G pour revenir en gestion.");
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
  label_prints_.next_poll = 0.0;
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
#ifdef QRPROTEC_HOTSPOT
  if (Hotspot::supported())
    hotspot.poll(ImGui::GetTime());
#endif
  if (!setup_known_ && ImGui::GetTime() >= next_setup_check_)
    check_setup();
  if (privileged() && ImGui::GetTime() >= next_reports_poll_)
    refresh_report_counts();
  // impressions demandees depuis le web ou un autre ordinateur : verifiees meme sans personne de connecte, sur le
  // poste principal seulement (API locale, ou cle marquee poste sur un back sans kiosk)
  // Sur un autre ordinateur, les admins voient les files du poste tant que leur fenetre est ouverte. Le poste
  // principal n'imprime que sous Linux (kLocalPrinting).
  // Apres une nouvelle impression, le poste principal la recoit en direct pendant 15 min (WebSocket) : il relit
  // alors la file a chaque signal, et toutes les minutes seulement au lieu de 20 s (filet si un signal se perd).
  update_label_prints_live();
  if (((kLocalPrinting && main_poste()) || (label_prints_.open && label_prints_.remote && privileged())) && !label_prints_.loading
      && ImGui::GetTime() >= label_prints_.next_poll)
    refresh_label_prints();
  // cache de vérif tenu a jour toutes les minutes (vérif possible hors connexion), envois en attente reessayes
  sync_cache(60.0);
  if (!outbox.empty() && !outbox_busy_ && ImGui::GetTime() >= next_outbox_try_)
    flush_outbox();
  if (!print_counts_busy_ && ImGui::GetTime() >= next_print_counts_try_)
    flush_print_counts();
  // voyant des lots a traiter : liste tenue a jour meme fenetre Lots fermee (pas pendant une verif)
  if (!verif.active && !catalog.loading_lots && ImGui::GetTime() >= next_lots_poll_) {
    next_lots_poll_ = ImGui::GetTime() + 120.0;
    if (catalog.lots_version > 0)
      refresh_lots(true);
  }
}


// Compteur d'impressions (inventory/print_counts.py) : envoye toutes les 30 s s'il y a des etiquettes a compter, le
// meme lot renvoye tant que le serveur ne l'a pas recu. Refuse comme invalide : retire (ne passerait jamais).
void App::flush_print_counts() {
  next_print_counts_try_ = ImGui::GetTime() + 30.0;
  const Json body        = print_counter.next_batch();
  if (body.is_null())
    return;
  print_counts_busy_ = true;
  const std::string id = body["batch"].str();
  api.post("/api/print-counts/", body, [this, id](const ApiResult &result) {
    print_counts_busy_ = false;
    if (result.ok || result.status == 400) {
      if (!result.ok)
        error_log("Compteur d'impressions refusé par le serveur : " + result.error, "imprimante");
      print_counter.batch_done(id);
      return;
    }
    // hors connexion : reessaye a 30 s ; serveur pas encore a jour (404) ou autre refus : plus tard
    if (result.status != 0)
      next_print_counts_try_ = ImGui::GetTime() + 600.0;
  });
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
  reset_windows_view();
  refresh_lots();
  notify("Session réinitialisée après inactivité.");
}

ImVec4 App::background_color() const {
  return kNormalBackground;
}

void App::draw() {
  handle_shortcuts();
  draw_menu_bar();
  draw_windows();
  draw_hub();
  draw_label_prints();
  draw_roll_change_modal();
  draw_roll_wizard();
  inateck.draw_window();
  draw_setup_modal();
  draw_server_modal();
  draw_login_modal();
  draw_pin_modal();
  draw_douchette_modal();
  draw_tasks_modal();
  draw_problem_modal();
  draw_revision_modal();
  draw_maintenance_date_modal();
#ifdef QRPROTEC_HOTSPOT
  draw_hotspot_modal();
#endif
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
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPrivilegedMenuBar);
  if (ImGui::BeginMainMenuBar()) {
    const BarColors bar = bar_colors(tinted);
    menu_bar_height_    = ImGui::GetWindowSize().y;
    const auto douchette_items = [this]() {
      if (ImGui::MenuItem("Téléphone comme douchette...", nullptr, remote.active))
        open_window("phone");
      ImGui::Separator();
      inateck.draw_menu();
      // bip de l'ordinateur sur un mauvais scan (section des Reglages, admins)
      if (admin()) {
        ImGui::Separator();
        if (ImGui::MenuItem("Signal de mauvais scan..."))
          open_settings_section("Signal de mauvais scan");
      }
    };
    draw_mode_switch(tinted);
    if (bar_menu(bar, "Fenêtres")) {
      if (ImGui::MenuItem("Aller à…", "Ctrl+K"))
        open_hub();
      ImGui::Separator();
      // fenetres du mode normal ; en mode gestion, celles des domaines sont dans leurs menus
      std::vector< AppWindow * > common;
      for (auto &window : windows)
        if (!window->privileged && window->id != "verif" && can_open(*window))
          common.push_back(window.get());
      std::sort(common.begin(), common.end(), [](const AppWindow *a, const AppWindow *b) { return a->shortcut < b->shortcut; });
      for (AppWindow *window : common)
        window_menu_item(*window);
      if (privileged() && ImGui::BeginMenu("Douchette")) {
        douchette_items();
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Plein écran", "Ctrl+F11", fullscreen_))
        toggle_fullscreen();
      if (ImGui::MenuItem("Fenêtre suivante", "Ctrl+Tab"))
        focus_next_window(1);
      const AppWindow *focused = window(focused_window_);
      if (ImGui::MenuItem("Fermer la fenêtre", "Ctrl+W", false, focused && focused->open && focused->can_close(*this)))
        window(focused_window_)->open = false;
      if (ImGui::MenuItem("Remettre les fenêtres en place", "Ctrl+Espace"))
        request_layout_reset();
      // rangement de la fenetre qui a le focus (aussi en la glissant sur un bord ou dans un coin de l'ecran)
      AppWindow *snap_target = window(focused_window_);
      const bool snappable   = snap_target && snap_target->open && can_open(*snap_target) && can_snap(*snap_target);
      if (ImGui::BeginMenu("Ranger la fenêtre", snappable)) {
        struct SnapEntry {
            const char *label;
            const char *shortcut;
            int         h, v;
        };
        static const SnapEntry kEntries[] = {
          { "Moitié gauche", "Super+Gauche", -1, 0 },          { "Moitié droite", "Super+Droite", 1, 0 },
          { "Moitié haute", "Super+Haut", 0, -1 },             { "Moitié basse", "Super+Bas", 0, 1 },
          { "Quart en haut à gauche", nullptr, -1, -1 },       { "Quart en haut à droite", nullptr, 1, -1 },
          { "Quart en bas à gauche", nullptr, -1, 1 },         { "Quart en bas à droite", nullptr, 1, 1 },
        };
        for (std::size_t index = 0; index < std::size(kEntries); ++index) {
          const SnapEntry &entry = kEntries[index];
          if (index == 4)
            ImGui::Separator();
          const bool current = snap_target->snap_h == entry.h && snap_target->snap_v == entry.v;
          if (ImGui::MenuItem(entry.label, entry.shortcut, current))
            snap_window(*snap_target, entry.h, entry.v);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Place d'avant", nullptr, false, snap_target->snap_h != 0 || snap_target->snap_v != 0))
          snap_window(*snap_target, 0, 0);
        ImGui::TextDisabled("Ou glissez la fenêtre sur un bord (moitié)\nou dans un coin de l'écran (quart).");
        ImGui::EndMenu();
      }
      // fenetres ouvertes au demarrage (section des Reglages, admins)
      if (admin() && ImGui::MenuItem("Disposition par défaut..."))
        open_settings_section("Disposition par défaut");
      ImGui::EndMenu();
    }
    if (privileged())
      draw_gestion_menus(tinted);
    draw_update_menu(tinted);
    const PrintQueueStatus print = printer.status();
    draw_print_menu(tinted, print);
    // mode gestion : la douchette est dans Fenetres (place pour les menus des domaines)
    if (!privileged() && bar_menu(bar, "Douchette")) {
      douchette_items();
      ImGui::EndMenu();
    }
#ifdef QRPROTEC_HOTSPOT
    // wifi de secours en marche : voyant pour tout le monde, il ouvre les QR codes a montrer aux telephones
    if (const HotspotState wifi = hotspot.state(); wifi.active) {
      if (bar_item(bar, "● Wifi de secours", bar.blue))
        open_hotspot();
      ImGui::SetItemTooltip("La borne diffuse le wifi « %s » : cliquer pour les QR codes à scanner avec les téléphones.", wifi.ssid.c_str());
    }
#endif
    // voyant : lots qui demandent de l'attention (rouge s'il y en a d'incomplets ou perimes, orange sinon)
    if (const LotsAttention lots = lots_attention(); lots.total > 0) {
      // mode gestion : libelle court, les menus des domaines prennent la place
      const std::string label = "● " + std::to_string(lots.total) + (privileged() ? "" : lots.total > 1 ? " lots" : " lot") + " à traiter";
      if (bar_item(bar, label.c_str(), lots.red > 0 ? bar.red : bar.orange))
        show_lots_to_treat();
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
    // gestion et admins : signalements pas encore pris en charge
    const int fresh_reports = privileged() ? report_counts["new"].integer() : 0;
    if (fresh_reports > 0) {
      // libelle court : le menu TECH les compte aussi
      const std::string label = "⚠ " + std::to_string(fresh_reports) + (fresh_reports > 1 ? " signalés" : " signalé");
      if (bar_item(bar, label.c_str(), bar.red))
        open_window("reports");
      ImGui::SetItemTooltip("Problèmes signalés pas encore pris en charge : ouvrir les signalements.");
    }
    // etiquettes demandees depuis le web pas encore imprimees (tout le monde, connecte ou non)
    if (const int requests = static_cast< int >(label_prints_.data["pending"].size()); requests > 0) {
      const std::string label = "● " + nb(requests, "impression") + " en attente";
      if (bar_item(bar, label.c_str(), bar.blue))
        open_label_prints();
      if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Étiquettes demandées depuis le web ou un autre ordinateur, une file par personne :");
        for (const Json &queue : label_prints_.data["queues"].items())
          ImGui::BulletText("%s : %s", queue["name"].str().c_str(), nb(queue["labels"].integer(), "étiquette").c_str());
        ImGui::TextDisabled("Cliquer : ouvrir pour les imprimer.");
        ImGui::EndTooltip();
      }
    }

    // Partie droite : etat de l'API et utilisateur connecte (l'impression est dans son menu, a gauche)
    // vérif hors connexion : envois gardes sur le poste, renvoyes au retour du reseau
    const std::string waiting = outbox.empty() ? std::string()
                              : " · " + nb(static_cast< int >(outbox.size()), "vérif en attente", "vérifs en attente");
    const std::string status  = api.online() ? (outbox.empty() ? "En ligne" : "Envoi en cours" + waiting)
                                             : "Hors connexion" + waiting;
    const std::string phone  = !remote.active          ? ""
                             : remote.phone_connected ? "Téléphone connecté"
                             : remote.phone_seen      ? "Téléphone déconnecté"
                                                      : "Téléphone en attente";
    // role entre parenthèses, sauf pour un gestionnaire : la bascule Normal / Gestion le montre deja
    const std::string who    = logged_in() ? user->display() + (manager() ? std::string() : user->role_suffix())
                                           : "Non connecté : scannez votre badge";
    const float       button = logged_in() ? ImGui::CalcTextSize("Se déconnecter").x + ImGui::GetStyle().FramePadding.x * 2 : 0.0f;
    const float       width  = ImGui::CalcTextSize((phone + status + who).c_str()).x + button + 80.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - width));
    if (!phone.empty()) {
      if (bar_item(bar, phone.c_str(), remote.phone_connected ? bar.green : bar.red))
        open_window("phone");
      bar_separator(bar);
    }
    if (api.online()) {
      ImGui::TextColored(bar.green, "%s", status.c_str());
    } else {
      // sans API, personne ne peut se connecter pour ouvrir les Reglages : la connexion se regle d'ici
      if (bar_item(bar, status.c_str(), bar.red))
        server_modal_requested_ = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nCliquer pour régler la connexion au serveur.", api.last_error().c_str());
    }
    bar_separator(bar);
    if (logged_in()) {
      ImGui::TextColored(bar.text, "%s", who.c_str());
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

// Bascule Normal / Gestion, a gauche de la barre, pour les roles gestion et admin : on choisit pourquoi on est devant
// le poste. Mode normal : ce que fait un secouriste ou un chef. Mode gestion : en plus, un menu par domaine.
void App::draw_mode_switch(bool dark_bar) {
  if (!manager())
    return;
  const BarColors bar = bar_colors(dark_bar);
  if (bar_toggle(bar, "Normal", !gestion_mode_))
    set_gestion_mode(false);
  ImGui::SetItemTooltip("Mode normal : vérifs, lots, listes, comme un secouriste ou un chef (Ctrl+G)");
  if (bar_toggle(bar, "Gestion", gestion_mode_))
    set_gestion_mode(true);
  ImGui::SetItemTooltip("Mode gestion : le mode normal, plus la TECH, les lots, les items, le stock, la réception, "
                        "les étiquettes et l'impression (Ctrl+G)");
  bar_separator(bar);
}

namespace {

// Entree de menu qui ouvre une fenetre a un onglet ; touche F de la fenetre a droite
bool tab_menu_item(App &app, const char *label, const char *window_id, bool show_key = false) {
  const AppWindow *target = app.window(window_id);
  if (target == nullptr || !app.can_open(*target))
    return false;
  const char *key = show_key && !target->shortcut_label.empty() ? target->shortcut_label.c_str() : nullptr;
  return ImGui::MenuItem(label, key);
}

} // namespace

// Mode gestion : un menu par domaine, dans l'ordre de la journee du TECH. Chaque domaine a sa fenetre et sa touche F,
// chaque entree ouvre la fenetre sur le bon onglet.
void App::draw_gestion_menus(bool dark_bar) {
  const BarColors bar = bar_colors(dark_bar);
  // TECH : savoir quoi faire
  if (bar_menu(bar, "TECH")) {
    ImGui::TextDisabled("Savoir quoi faire");
    if (tab_menu_item(*this, "À faire", "stock", true))
      show_tech_tab("tasks");
    if (tab_menu_item(*this, "À venir (péremptions, révisions, commandes...)", "stock"))
      show_tech_tab("events");
    const int         fresh   = report_counts["new"].integer();
    const std::string reports = "Signalements" + (fresh > 0 ? " (" + nb(fresh, "nouveau", "nouveaux") + ")" : std::string());
    if (tab_menu_item(*this, reports.c_str(), "reports", true))
      open_window("reports");
    if (tab_menu_item(*this, "Journal des opérations", "journal"))
      open_window("journal");
#ifdef QRPROTEC_HOTSPOT
    if (Hotspot::supported()) {
      ImGui::Separator();
      ImGui::TextDisabled("Panne du wifi du local");
      if (ImGui::MenuItem("Wifi de secours...", nullptr, hotspot.state().active))
        open_hotspot();
    }
#endif
    ImGui::EndMenu();
  }
  if (bar_menu(bar, "Lots")) {
    if (tab_menu_item(*this, "État des lots et fiches", "lots", true))
      open_window("lots");
    if (ImGui::MenuItem("Lots à traiter"))
      show_lots_to_treat();
    ImGui::Separator();
    if (ImGui::MenuItem("Nouveau lot..."))
      show_lots_mode("new");
    if (ImGui::MenuItem("Ordre des lots..."))
      show_lots_mode("order");
    if (ImGui::MenuItem("Lots archivés..."))
      show_lots_mode("archived");
    ImGui::Separator();
    if (tab_menu_item(*this, "Types de lots", "lot_types", true))
      open_window("lot_types");
    ImGui::EndMenu();
  }
  if (bar_menu(bar, "Items")) {
    if (tab_menu_item(*this, "Types d'items", "items", true))
      show_stock_tab("types");
    if (tab_menu_item(*this, "Items (recherche, fiche)", "items"))
      show_stock_tab("items");
    if (tab_menu_item(*this, "Entretien : révisions et désinfections", "items"))
      show_stock_tab("equipment");
    if (tab_menu_item(*this, "Paquets fermés", "items"))
      show_stock_tab("packs");
    ImGui::EndMenu();
  }
  if (bar_menu(bar, "Stock")) {
    if (tab_menu_item(*this, "État du stock", "inventory", true))
      show_stock_tab("stock");
    if (tab_menu_item(*this, "Projection", "inventory"))
      show_stock_tab("projection");
    if (tab_menu_item(*this, "Commandes à passer", "inventory"))
      show_stock_tab("orders");
    if (tab_menu_item(*this, "Transferts entre lots", "inventory"))
      show_stock_tab("transfers");
    ImGui::EndMenu();
  }
  if (bar_menu(bar, "Réception")) {
    if (tab_menu_item(*this, "Nouvelle réception", "reception", true))
      show_stock_tab("reception");
    if (tab_menu_item(*this, "Réceptions passées", "reception"))
      show_stock_tab("past_receptions");
    ImGui::EndMenu();
  }
  if (bar_menu(bar, "Étiquettes")) {
    if (const AppWindow *editor = window("editor"); editor && can_open(*editor)) {
      if (ImGui::MenuItem("Éditeur d'étiquettes", editor->shortcut_label.c_str()))
        open_window("editor");
    } else {
      ImGui::MenuItem("Éditeur d'étiquettes (admins)", nullptr, false, false);
    }
    ImGui::TextDisabled("Étiquettes d'un lot : sa fiche, onglet Étiquettes.");
    ImGui::TextDisabled("Étiquette d'un item : Items, ou la pile de scans.");
    ImGui::Separator();
    const int requests = static_cast< int >(label_prints_.data["pending"].size());
    const std::string queues = "Étiquettes demandées (web, autres ordinateurs)"
                             + (requests > 0 ? " : " + std::to_string(requests) : std::string()) + "...";
    if (ImGui::MenuItem(queues.c_str())) {
      label_prints_.next_poll = 0.0;
      open_label_prints();
    }
    ImGui::EndMenu();
  }
  // Impression (imprimante, files, consommables) : draw_print_menu ; Administration : admins
  if (admin() && bar_menu(bar, "Administration")) {
    if (const AppWindow *users = window("users"); users && can_open(*users))
      window_menu_item(*window("users"));
    if (const AppWindow *settings_window = window("settings"); settings_window && can_open(*settings_window))
      window_menu_item(*window("settings"));
    ImGui::EndMenu();
  }
}

// Menu Impression : tout ce qui touche a l'imprimante et aux etiquettes au meme endroit (file en cours, rouleau,
// files demandees depuis le web, compteur et consommables, reglages). Visible en gestion et admin, et pour tout le
// monde des qu'une impression attend ou que l'imprimante est en pause. Gris au repos, bleu quand des etiquettes
// attendent, rouge en pause.
void App::draw_print_menu(bool dark_bar, const PrintQueueStatus &print) {
  const BarColors bar = bar_colors(dark_bar);
  const int  requests = static_cast< int >(label_prints_.data["pending"].size());
  if (!privileged() && !print.paused && print.pending == 0)
    return;
  std::string label = "Impression";
  if (print.paused)
    label = "Impression en pause";
  else if (print.pending > 0)
    label = "Impression : " + std::to_string(print.pending) + " en cours";
  const ImVec4 color = print.paused ? bar.red : print.pending > 0 ? bar.blue : bar.text;
  if (!bar_menu(bar, (label + "###print_menu").c_str(), color))
    return;
  if (print.paused)
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", print.error.c_str());
  if (!print.current.empty())
    ImGui::Text("En cours : %s", print.current.c_str());
  if (print.paused && !print.needed_label_type.empty()) {
    if (ImGui::MenuItem("Changer de rouleau (pas à pas)..."))
      open_roll_wizard(print.needed_label_type);
    if (ImGui::MenuItem(("Les étiquettes « " + print.needed_label_type + " » sont chargées").c_str()))
      set_loaded_label_type(print.needed_label_type);
  } else if (print.paused) {
    if (ImGui::MenuItem("Reprendre l'impression"))
      printer.resume();
    if (ImGui::MenuItem("Changer de rouleau (pas à pas)..."))
      open_roll_wizard();
  } else if (kLocalPrinting && privileged() && ImGui::MenuItem("Changer de rouleau (pas à pas)..."))
    open_roll_wizard();
  if (print.pending > 0 && ImGui::MenuItem("Annuler les impressions en cours"))
    printer.cancel();
  if (privileged()) {
    ImGui::Separator();
    // demandes du web ou d'un autre ordinateur : sur le poste, la fenetre qui les imprime ; ailleurs, les files du
    // poste gerees a distance
    const std::string queues = main_poste() && kLocalPrinting
                               ? "Impressions demandées" + (requests > 0 ? " (" + std::to_string(requests) + ")" : std::string()) + "..."
                               : std::string("Files d'impression du poste...");
    if (ImGui::MenuItem(queues.c_str())) {
      label_prints_.next_poll = 0.0;
      open_label_prints();
    }
    if (const AppWindow *counts = window("print_counts"); counts && can_open(*counts)
        && ImGui::MenuItem("Impressions et consommables...", nullptr, counts->open))
      open_window("print_counts");
  }
  if (admin() && ImGui::MenuItem("Réglages d'impression..."))
    open_settings_section("Étiquettes et impression");
  ImGui::EndMenu();
}

// Mise a jour telechargee (macOS) : installee a la fermeture, ou tout de suite d'ici (tout le monde, connecte ou non :
// sur un poste personnel, l'utilisateur n'est pas forcement admin de QRProtec)
void App::draw_update_menu(bool dark_bar) {
  const Updater::Status update = updater.status();
  if (update.state != Updater::State::Ready || update.launched)
    return;
  const BarColors   bar   = bar_colors(dark_bar);
  const std::string label = "↑ Mise à jour " + update.available;
  if (!bar_menu(bar, label.c_str(), bar.blue))
    return;
  ImGui::Text("QRProtec %s est téléchargée (version actuelle : %s).", update.available.c_str(),
              Updater::current_version().c_str());
  if (update.needs_admin)
    ImGui::TextDisabled("Le mot de passe d'un administrateur du Mac sera demandé.");
  else
    ImGui::TextDisabled("Elle s'installe toute seule à la fermeture de QRProtec.");
  if (ImGui::MenuItem("Installer et redémarrer maintenant")) {
    std::string error;
    if (updater.install(true, error))
      quit_requested = true;
    else
      notify("Mise à jour impossible : " + error, true);
  }
  ImGui::EndMenu();
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

namespace {

// Double-clic sur la barre de titre d'une fenetre (hors boutons) : ImGui la replierait. Detecte avant Begin() avec
// les memes conditions qu'ImGui, et le clic est pris (comme le fait ImGui) pour qu'il ne la replie pas : le poste
// passe en plein ecran a la place. Le repli reste sur la fleche.
bool title_bar_double_clicked(const char *label) {
  ImGuiContext &g      = *GImGui;
  ImGuiWindow  *window = ImGui::FindWindowByName(label);
  if (window == nullptr || window->Hidden || (window->Flags & ImGuiWindowFlags_NoTitleBar) || g.HoveredWindow != window
      || g.HoveredIdPreviousFrame != 0 || g.ActiveId != 0 || g.IO.MouseClickedCount[0] != 2
      || ImGui::GetKeyOwner(ImGuiKey_MouseLeft) != ImGuiKeyOwner_NoOwner)
    return false;
  // clip = false : avant Begin(), la fenetre courante est une autre, son rectangle de dessin couperait le test
  const ImRect bar = window->TitleBarRect();
  if (!ImGui::IsMouseHoveringRect(bar.Min, bar.Max, false))
    return false;
  ImGui::SetKeyOwner(ImGuiKey_MouseLeft, window->MoveId);
  return true;
}

} // namespace

void App::draw_windows() {
  ImVec2 origin, size;
  work_area(origin, size);
  AppWindow *moving_window = nullptr; // fenetre glissee a la souris
  AppWindow *fullscreen_toggled = nullptr; // barre de titre double-cliquee a cette frame
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
      window->snap_h = window->snap_v = 0; // disposition par defaut : plus rangee
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
    if (title_bar_double_clicked(label.c_str()))
      fullscreen_toggled = window.get();
    const bool        visible = ImGui::Begin(label.c_str(), p_open, flags);
    if (const ImGuiWindow *moving = GImGui->MovingWindow;
        moving != nullptr && moving->RootWindow == ImGui::GetCurrentWindow())
      moving_window = window.get();
    if (visible) {
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
  // double-clic sur une barre de titre : plein ecran (ou sortie), applique a toutes les fenetres a la frame suivante,
  // celle double-cliquee au premier plan
  if (fullscreen_toggled != nullptr) {
    toggle_fullscreen();
    fullscreen_toggled->focus_pending = true;
  }
  update_snap_drag(moving_window);
}

} // namespace qrprotec
