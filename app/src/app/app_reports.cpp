/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_reports.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Signalements de problemes, revisions, changement de rouleau, lots a traiter.

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
  // plein ecran, panneau attention jaune dans le titre : on previent le TECH d'un probleme, ce n'est pas un
  // formulaire « est-ce valide » qui validerait la verif
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
  if (!ImGui::BeginPopupModal(kTitle, nullptr,
                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                | ImGuiWindowFlags_NoSavedSettings))
    return;
  const bool  pack   = !problem_.target.pack.empty();
  const bool  object = !problem_.target.iid.empty() || !problem_.target.type.empty();
  const float base   = ImGui::GetStyle().FontSizeBase;
  const float width  = std::min(ImGui::GetContentRegionAvail().x, base * 40.0f);
  const float left   = std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - width) * 0.5f);
  ImGui::Dummy(ImVec2(0, base * 0.5f));
  ImGui::SetCursorPosX(left);
  warning_sign(base * 3.2f);
  ImGui::SameLine(0.0f, base);
  ImGui::PushFont(nullptr, base * 2.0f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (base * 3.2f - ImGui::GetTextLineHeight()) * 0.5f);
  ImGui::TextUnformatted("Signaler un problème");
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::Indent(left - ImGui::GetStyle().WindowPadding.x);
  ImGui::PushTextWrapPos(left + width);
  ImGui::PushFont(nullptr, base * 1.4f);
  ImGui::TextUnformatted(problem_.target.label.c_str());
  ImGui::PopFont();
  ImGui::Spacing();
  {
    // explication sur fond jaune pale, liseré jaune a gauche (comme le front web)
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float  pad   = base * 0.6f;
    ImDrawList  *draw  = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::Dummy(ImVec2(0, pad * 0.5f));
    ImGui::Indent(pad * 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));
    ImGui::PushTextWrapPos(left + width - pad);
    ImGui::TextUnformatted("Un problème sur le matériel (cassé, sale, abîmé…) ? Ce message part au TECH.");
    ImGui::PushFont(nullptr, base * 1.1f);
    ImGui::TextUnformatted("Ce n'est pas la validation de la vérif. Si tout va bien, annulez.");
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(pad * 2.0f);
    ImGui::Dummy(ImVec2(0, pad * 0.5f));
    const ImVec2 end(start.x + width, ImGui::GetItemRectMax().y);
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(start, end, IM_COL32(255, 248, 204, 255), pad);
    draw->AddRectFilled(start, ImVec2(start.x + pad, end.y), IM_COL32(255, 212, 0, 255), pad,
                        ImDrawFlags_RoundCornersLeft);
    draw->ChannelsMerge();
  }
  ImGui::Spacing();
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
  ImGui::TextDisabled("Commentaire facultatif.");
  if (!problem_.error.empty())
    ImGui::TextColored(colors::red, "%s", problem_.error.c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(problem_.busy);
  const ImVec2 action(width * 0.5f - ImGui::GetStyle().ItemSpacing.x * 0.5f, ImGui::GetFrameHeight() * 1.6f);
  if (primary_button(problem_.busy ? "Envoi..." : "Envoyer au TECH", action) || enter_pressed()) {
    if (problem_.usable < 0)
      problem_.error = object ? "Indiquez si l'objet est toujours utilisable." : "Indiquez si le lot est toujours utilisable.";
    else
      send_problem();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", action) || (!problem_.busy && escape_pressed())) {
    problem_ = ProblemPrompt{};
    ImGui::CloseCurrentPopup();
  }
  ImGui::PopTextWrapPos();
  ImGui::Unindent(left - ImGui::GetStyle().WindowPadding.x);
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
  if (primary_button(revision_.busy ? "Envoi..." : "Enregistrer la révision", ImVec2(width * 0.6f, 0)) || enter_pressed())
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

void App::draw_roll_change_modal() {
  static const char     *kTitle = "Impression en pause###roll_change";
  const PrintQueueStatus print  = printer.status();
  const std::string     &needed = print.needed_label_type;
  if (!print.paused || needed.empty())
    roll_change_dismissed_.clear();
  else if (needed != roll_change_dismissed_ && !ImGui::IsPopupOpen(kTitle)
           && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    ImGui::OpenPopup(kTitle);
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (needed.empty()) { // reprise ou annulation ailleurs (menu, Reglages, fenetre des consommables)
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const float width = ImGui::GetFontSize() * 30.0f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextColored(colors::orange, "Changez de rouleau");
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextUnformatted("L'impression est en pause : l'étiquette suivante n'est pas du même type que le rouleau en "
                         "place.");
  ImGui::Spacing();
  ImGui::TextUnformatted("Mettez le rouleau :");
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
  ImGui::TextUnformatted(("« " + needed + " »").c_str());
  ImGui::PopFont();
  if (!print.needed_label_summary.empty())
    ImGui::TextDisabled("%s", print.needed_label_summary.c_str());
  if (print.needed_label_summary.find("transfert") != std::string::npos)
    ImGui::TextUnformatted("Avec le ruban de transfert.");
  const std::string &loaded = settings.print.loaded_label_type;
  ImGui::TextDisabled("En place : %s", loaded.empty() ? "format libre" : ("« " + loaded + " »").c_str());
  if (!print.needed_for.empty())
    ImGui::TextDisabled("Pour : %s%s", print.needed_for.c_str(),
                        print.pending > 1 ? (" (et " + nb(static_cast< int >(print.pending) - 1, "autre étiquette",
                                                           "autres étiquettes") + ")").c_str()
                                          : "");
  ImGui::Spacing();
  ImGui::TextUnformatted("Puis confirmez ci-dessous, ou scannez à la douchette le QR de ce rouleau.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::Separator();
  // Entree : « C'est fait » (jamais l'Entree d'un scan, voir enter_pressed()), Echap : plus tard
  if (primary_button("Me guider pas à pas", ImVec2(width, 0))) {
    roll_change_dismissed_ = needed; // l'assistant prend le relais ; il declare le rouleau a la fin
    open_roll_wizard(needed);
    ImGui::CloseCurrentPopup();
  }
  if (button(("C'est fait : « " + needed + " » est en place").c_str(), ImVec2(width, 0)) || enter_pressed()) {
    set_loaded_label_type(needed);
    ImGui::CloseCurrentPopup();
  }
  if (button("Plus tard (l'impression reste en pause)") || escape_pressed()) {
    roll_change_dismissed_ = needed;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (danger_button("Annuler ces impressions")) {
    printer.cancel();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
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
    draw_report_replies(report);
  }
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  // Entree : jamais celle d'un scan (douchette en mode clavier), voir enter_pressed()
  if (primary_button("J'ai compris", ImVec2(width, 0)) || enter_pressed() || escape_pressed())
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
