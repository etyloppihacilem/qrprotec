/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "app.hpp"

#include "../ui/inateck.hpp"
#include "../ui/widgets.hpp"
#include "../ui/windows/windows.hpp"
#include "labels.hpp"

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
  if (text.size() >= 16 && text[10] == 'T')
    return date->display() + " " + text.substr(11, 5);
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
  windows.push_back(make_lot_admin_window());
  windows.push_back(make_users_window());
  windows.push_back(make_editor_window());
  windows.push_back(make_settings_window());
  apply_default_open_state();
  refresh_item_types();
  refresh_lots();
}

App::~App() = default;

void App::apply_settings() {
  api.configure(settings.api_url, settings.api_token);
}

bool App::save_settings() {
  std::string error;
  if (!settings.save(error)) {
    notify(error, true);
    return false;
  }
  notify("Reglages enregistres.");
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
  api.poll();
  for (const ScanEvent &event : inateck.take_scans())
    handle_scan(event.code, event.source);
  check_inactivity();
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
  notify("Session reinitialisee apres inactivite.");
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
  draw_login_modal();
  draw_toasts();
  feedback.draw_overlay();
}

void App::draw_menu_bar() {
  if (privileged())
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, kPrivilegedOrange);
  if (ImGui::BeginMainMenuBar()) {
    menu_bar_height_ = ImGui::GetWindowSize().y;
    if (ImGui::BeginMenu("Fenetres")) {
      for (auto &window : windows)
        if (!window->privileged && window->closable && ImGui::MenuItem(window->title.c_str(), nullptr, window->open))
          window->open ? (void)(window->open = false) : open_window(window->id);
      ImGui::Separator();
      if (ImGui::MenuItem("Remettre les fenetres en place"))
        request_layout_reset();
      ImGui::EndMenu();
    }
    if (privileged() && ImGui::BeginMenu("Gestion")) {
      for (auto &window : windows)
        if (window->privileged && ImGui::MenuItem(window->title.c_str(), nullptr, window->open))
          window->open ? (void)(window->open = false) : open_window(window->id);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Douchette")) {
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
    const std::string who    = logged_in() ? user->display() + (privileged() ? " (responsable)" : "") : "Non connecte : scannez votre badge";
    const float       button = logged_in() ? ImGui::CalcTextSize("Se deconnecter").x + ImGui::GetStyle().FramePadding.x * 2 : 0.0f;
    const float       width  = ImGui::CalcTextSize((printing + status + who).c_str()).x + button + 80.0f;
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
    ImGui::TextColored(api.online() ? ImVec4(0.1f, 0.55f, 0.1f, 1) : ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", status.c_str());
    if (!api.online() && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", api.last_error().c_str());
    ImGui::Separator();
    if (logged_in()) {
      ImGui::TextUnformatted(who.c_str());
      if (danger_button("Se deconnecter"))
        logout("Deconnecte.");
    } else {
      ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1.0f), "%s", who.c_str());
    }
    ImGui::EndMainMenuBar();
  }
  if (privileged())
    ImGui::PopStyleColor();
}

void App::draw_windows() {
  ImVec2 origin, size;
  work_area(origin, size);
  for (auto &window : windows) {
    if ((window->privileged && !privileged()) || !window->open) {
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

void App::notify(const std::string &message, bool error) {
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
      notify("Badge refuse : " + result.error, true);
      return;
    }
    SessionUser session;
    session.matricule   = result.data["matricule"].str();
    session.nom         = result.data["nom"].str();
    session.prenom      = result.data["prenom"].str();
    session.key_expires = result.data["key_expires"].str();
    session.privileged  = result.data["privileged"].boolean();
    user                = session;
    notify("Bonjour " + session.display() + (session.privileged ? " : mode privilegie active." : "."));
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
// Scans

void App::handle_scan(const std::string &code, ScanSource source) {
  note_activity();
  const ParsedScan scan = parse_scan(code);
  switch (scan.kind) {
    case ScanKind::User: login_with_badge(scan, source); return;
    case ScanKind::Lot: scan_lot(scan, source); return;
    case ScanKind::Item: {
      ScanEntry &entry = stack.add(scan, source);
      const int  id    = entry.id;
      if (entry.duplicate)
        notify("Deja scanne : " + scan.id + " (retirez le doublon si c'est une erreur).");
      if (entry.expired) {
        feedback.error(source, settings);
        notify("PERIME : " + scan.id + " (" + scan.peremption->display() + ")", true);
      }
      resolve_item(id);
      return;
    }
    case ScanKind::SealedPack: {
      const int id = stack.add(scan, source).id;
      resolve_pack(id);
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
      entry->detail = "Non verifie : " + result.error;
      return;
    }
    const Json &item = result.data;
    entry->state     = EntryState::Ok;
    entry->data      = item;
    entry->title     = item["type_name"].str();
    std::string detail = item["peremption"].is_null() ? "Non perissable" : "Exp. " + display_date(item["peremption"]);
    detail += item["location"].is_null() ? " - en stock" : " - " + item["location_name"].str();
    const std::string status = item["status"].str();
    entry->warning           = status != "active";
    if (status == "missing")
      detail += " - signale disparu";
    else if (status == "deleted")
      detail += " - marque supprime";
    else if (status == "replaced")
      detail += " - deja remplace";
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
    entry->detail = result.data["peremption"].is_null() ? "Non perissable"
                                                        : "Exp. " + display_date(result.data["peremption"]);
    if (!result.data["opened"].is_null())
      entry->detail += " - deja ouvert";
    const auto date = Date::parse(result.data["peremption"].str());
    if (date && *date < today()) {
      entry->expired = true;
      feedback.error(source, settings);
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
        notify("Cle de l'etiquette privee perimee : reimprimez l'etiquette du lot.", true);
        stack.target = {};
        if (verif.lot_id == id)
          verif.key.clear();
        return;
      }
      if (stack.target.id == id)
        stack.target.name = result.data["name"].str();
    });
  }
  if (verif.active && verif.lot_id == scan.id) {
    if (!scan.key.empty()) {
      verif.key = scan.key;
      notify("Etiquette privee du lot reconnue.");
    }
    open_window("verif");
    return;
  }
  if (verif.active) {
    notify("Une verif est deja en cours : terminez-la ou annulez-la.", true);
    open_window("verif");
    return;
  }
  start_verif(scan.id, scan.key);
}

// ---------------------------------------------------------------------------------------------------------------------
// Verifs et mouvements

void App::start_verif(const std::string &lot_id, const std::string &key) {
  require_login("lancer la verif du lot", [this, lot_id, key]() {
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
  });
}

void App::cancel_verif() {
  verif = {};
}

bool App::verif_key_ok() const {
  return !verif.key.empty() || privileged() || !settings.require_private_label;
}

void App::submit_verif() {
  require_login("valider la verif", [this]() {
    if (!verif.active || verif.submitting)
      return;
    if (!verif_key_ok()) {
      notify("Scannez l'etiquette privee du lot pour valider la verif.", true);
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
        notify("Verif refusee : " + result.error, true);
        return;
      }
      last_report     = result.data;
      last_report_lot = lot_name;
      stack.clear();
      cancel_verif();
      open_window("verif");
      notify(result.data["complete"].boolean() ? "Verif enregistree : lot complet."
                                               : "Verif enregistree : le lot est incomplet ou contient des perimes.",
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

void App::add_stack_to_lot() {
  require_login("ajouter des items au lot", [this]() {
    if (!stack.target.valid())
      return;
    Json body;
    body["items"] = Json::array();
    for (const std::string &iid : stack.iids())
      body["items"].push_back(iid);
    if (body["items"].size() == 0) {
      notify("Aucun item a ajouter.", true);
      return;
    }
    body["user"] = user_ref();
    body["key"]  = stack.target.key;
    const TargetLot target = stack.target;
    api.post("/api/lots/" + url_encode(target.id) + "/add/", body, [this, target](const ApiResult &result) {
      if (!result.ok) {
        notify("Ajout refuse : " + result.error, true);
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
      notify("Retour en stock refuse : " + result.error, true);
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
      notify("Verif du stock refusee : " + result.error, true);
      return;
    }
    last_report     = result.data;
    last_report_lot = "Stock";
    stack.clear();
    open_window("verif");
    notify("Verif du stock enregistree.");
    refresh_stock();
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// Donnees

void App::refresh_item_types() {
  api.get("/api/item-types/", [this](const ApiResult &result) {
    if (result.ok)
      catalog.item_types = result.data;
  });
}

void App::refresh_lot_types() {
  api.get("/api/lot-types/", [this](const ApiResult &result) {
    if (result.ok)
      catalog.lot_types = result.data;
    else
      notify("Types de lots : " + result.error, true);
  });
}

void App::refresh_lots() {
  catalog.loading_lots = true;
  api.get("/api/lots/", [this](const ApiResult &result) {
    catalog.loading_lots = false;
    if (result.ok)
      catalog.lots = result.data;
    else
      notify("Lots : " + result.error, true);
  });
}

void App::refresh_users() {
  api.get("/api/users/", [this](const ApiResult &result) {
    if (result.ok)
      catalog.users = result.data;
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

bool App::print_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what) {
  if (labels.empty())
    return false;
  const CategoryInfo &info = category_info(category);
  const auto          path = settings.label_templates.find(info.id);
  TemplateDocument    model;
  std::string         error;
  if (path == settings.label_templates.end() || !build_label(path->second, {}, model, error)) {
    notify("Modele d'etiquette \"" + info.label + "\" : " + (error.empty() ? "aucun modele configure" : error)
             + " (Gestion > Reglages).",
           true);
    return false;
  }
  std::vector< PrintJob > jobs;
  for (std::size_t index = 0; index < labels.size(); ++index) {
    PrintJob job;
    job.description = what + (labels.size() > 1 ? " " + std::to_string(index + 1) + "/" + std::to_string(labels.size()) : "");
    job.document    = model;
    job.document.parameters["today"]      = today().display();
    job.document.parameters["printed_by"] = user_name();
    for (const auto &[name, value] : labels[index])
      job.document.parameters[name] = value;
    jobs.push_back(std::move(job));
  }
  printer.enqueue(std::move(jobs), settings.print);
  notify(std::to_string(labels.size()) + " etiquette(s) envoyee(s) a l'imprimante.");
  return true;
}

} // namespace qrprotec
