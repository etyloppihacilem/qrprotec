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

namespace {
// Revision ou desinfection d'un item sous la forme du serveur (item["maintenance"], ou objet maintenance d'un
// evenement a venir) ; repli sur les champs de la revision pour une reponse d'avant la desinfection.
Json maintenance_of(const Json &item, const std::string &kind) {
  if (item["kind"].str() == kind && !item["months"].is_null())
    return item; // l'objet maintenance lui-meme (evenement a venir)
  if (item["maintenance"].is_object())
    return item["maintenance"]["kind"].str() == kind ? item["maintenance"] : Json();
  for (const Json &row : item["maintenance"].items())
    if (row["kind"].str() == kind)
      return row;
  if (kind != "revision" || !item["revision"].boolean())
    return Json();
  Json row = Json::object();
  row["kind"]   = Json(std::string("revision"));
  row["label"]  = Json(std::string("Révision"));
  row["noun"]   = Json(std::string("révision"));
  row["due"]    = item["revision_due"];
  row["months"] = item["revision_months"];
  row["tasks"]  = item["revision_tasks"];
  return row;
}
} // namespace

void App::open_revision(const Json &item, const std::string &kind) {
  const Json m = maintenance_of(item, kind);
  if (!m.is_object())
    return;
  const std::string noun = m["noun"].str("révision");
  require_login("faire la " + noun, [this, item, m, kind, noun]() {
    revision_           = RevisionPrompt{};
    revision_.kind      = kind;
    revision_.label     = m["label"].str("Révision");
    revision_.noun      = noun;
    revision_.iid       = item["iid"].str(m["iid"].str());
    revision_.type_name = item["type_name"].str(m["type_name"].str());
    revision_.due       = m["due"].str();
    revision_.months    = std::max(1, m["months"].integer(1));
    for (const Json &task : m["tasks"].items())
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
  ImGui::Text("%s : %s", revision_.label.c_str(), revision_.type_name.c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s%s", revision_.iid.c_str(),
                      revision_.due.empty() ? "" : (" · prévue le " + display_date(Json(revision_.due))).c_str());
  ImGui::PushTextWrapPos(width);
  ImGui::TextUnformatted(revision_.tasks.empty() ? "Pas de tâche définie pour ce type."
                                                 : ("Cochez toutes les tâches de la " + revision_.noun + ".").c_str());
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
  const std::string next_label = "Prochaine " + revision_.noun;
  ImGui::InputText(next_label.c_str(), &revision_.next_due);
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
  const std::string save_label = revision_.busy ? "Envoi..." : "Enregistrer la " + revision_.noun;
  if (primary_button(save_label.c_str(), ImVec2(width * 0.6f, 0)) || enter_pressed())
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
  body["kind"]     = Json(revision_.kind);
  body["tasks"]    = Json::array();
  for (const std::string &task : revision_.tasks)
    body["tasks"].push_back(Json(task));
  body["next_due"] = next->iso();
  body["note"]     = revision_.note;
  revision_.busy   = true;
  revision_.error.clear();
  const std::string iid  = revision_.iid;
  const std::string kind = revision_.kind;
  const std::string label = revision_.label;
  api.post("/api/items/" + iid + "/revision/", body, [this, iid, kind, label](const ApiResult &result) {
    revision_.busy = false;
    if (!result.ok) {
      identity_refused(result);
      revision_.error = result.error;
      return;
    }
    revision_ = RevisionPrompt{}; // ferme la fenetre
    notify(result.data["type_name"].str() + " : " + label + " enregistrée, prochaine le "
           + display_date(result.data[kind + "_due"]) + ".");
    after_maintenance(iid);
  });
}

void App::after_maintenance(const std::string &iid) {
  if (const ScanEntry *entry = stack.find_item(iid))
    resolve_item(entry->id); // etat relu (plus en retard)
  refresh_lots(true);
  if (privileged()) {
    refresh_events();
    refresh_tasks();
  }
}

void App::open_maintenance_date(const Json &item, const std::string &kind) {
  const Json m = maintenance_of(item, kind);
  if (!m.is_object() || !privileged())
    return;
  maintenance_date_           = MaintenanceDatePrompt{};
  maintenance_date_.kind      = kind;
  maintenance_date_.label     = m["label"].str("Révision");
  maintenance_date_.noun      = m["noun"].str("révision");
  maintenance_date_.iid       = item["iid"].str(m["iid"].str());
  maintenance_date_.type_name = item["type_name"].str(m["type_name"].str());
  maintenance_date_.due       = m["due"].str();
  maintenance_date_.months    = std::max(1, m["months"].integer(1));
  if (m["cycle_month"].integer() > 0)
    maintenance_date_.cycle = "mois " + std::to_string(m["cycle_month"].integer()) + " sur "
                            + std::to_string(maintenance_date_.months) + " du cycle";
  if (const auto done = Date::parse(m["done_at"].str().substr(0, 10)))
    maintenance_date_.last_done = done->display();
  if (const auto due = Date::parse(maintenance_date_.due))
    maintenance_date_.next_due = due->display();
  maintenance_date_.open_pending = true;
}

void App::draw_maintenance_date_modal() {
  static const char *kTitle = "Date d'entretien###maintenance_date";
  MaintenanceDatePrompt &p = maintenance_date_;
  if (p.open_pending) {
    p.open_pending = false;
    if (!ImGui::IsPopupOpen(kTitle))
      ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (p.iid.empty()) { // enregistree ou annulee
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const float width = ImGui::GetFontSize() * 28.0f;
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::Text("Date de %s : %s", p.noun.c_str(), p.type_name.c_str());
  ImGui::PopFont();
  std::string sub = p.iid + " · " + (p.due.empty() ? "pas encore prévue" : "prévue le " + display_date(Json(p.due)))
                  + " · tous les " + std::to_string(p.months) + " mois" + (p.cycle.empty() ? "" : " · " + p.cycle);
  ImGui::TextDisabled("%s", sub.c_str());
  ImGui::PushTextWrapPos(width);
  ImGui::TextUnformatted("Pour du matériel entretenu ailleurs ou un cycle à recaler : indiquez la dernière faite (la "
                         "prochaine se calcule avec la périodicité), ou directement la prochaine. Aucun entretien "
                         "n'est enregistré comme fait aujourd'hui.");
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
  const float field = ImGui::GetFontSize() * 10.0f;
  ImGui::SetNextItemWidth(field);
  const std::string last_label = "Dernière " + p.noun + " faite le";
  if (ImGui::InputTextWithHint(last_label.c_str(), "JJ/MM/AAAA", &p.last_done) && !p.next_touched) {
    if (const auto last = parse_user_date(p.last_done))
      p.next_due = add_months(*last, p.months).display();
  }
  const auto last = parse_user_date(p.last_done);
  if (ImGui::IsItemDeactivatedAfterEdit() && last)
    p.last_done = last->display();
  const bool last_ok = p.last_done.empty() || (last && !(today() < *last));
  if (!p.last_done.empty() && (!last || !last_ok)) {
    ImGui::SameLine();
    ImGui::TextColored(colors::red, "%s", !last ? "date non reconnue" : "pas dans le futur");
  }
  ImGui::SetNextItemWidth(field);
  const std::string next_label = "Prochaine " + p.noun + " le";
  if (ImGui::InputTextWithHint(next_label.c_str(), "JJ/MM/AAAA", &p.next_due))
    p.next_touched = true;
  const auto next = parse_user_date(p.next_due);
  if (ImGui::IsItemDeactivatedAfterEdit() && next)
    p.next_due = next->display();
  const bool next_ok = p.next_due.empty() ? (last && last_ok) : (next && (!last || *last < *next));
  if (!p.next_due.empty()) {
    if (!next || (last && !(*last < *next))) {
      ImGui::SameLine();
      ImGui::TextColored(colors::red, "%s", !next ? "date non reconnue" : "doit être après la dernière");
    } else if (*next < today()) {
      ImGui::SameLine();
      ImGui::TextColored(colors::orange, "passée : en retard");
    }
  }
  ImGui::PushTextWrapPos(width);
  ImGui::TextDisabled(p.next_touched ? "La prochaine peut être passée si le retard est constaté."
                                     : "La prochaine se recalcule depuis la dernière (périodicité du type).");
  ImGui::InputTextWithHint("##maintenance_note", "Remarque (facultatif)", &p.note);
  ImGui::PopStyleColor();
  if (p.note.size() > 256)
    p.note.resize(256);
  if (!p.error.empty())
    ImGui::TextColored(colors::red, "%s", p.error.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::BeginDisabled(p.busy || !last_ok || !next_ok || (p.last_done.empty() && p.next_due.empty()));
  if (primary_button(p.busy ? "Envoi..." : "Enregistrer la date", ImVec2(width * 0.6f, 0)) || enter_pressed())
    send_maintenance_date();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler", ImVec2(-1, 0)) || (!p.busy && escape_pressed())) {
    p = MaintenanceDatePrompt{};
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::send_maintenance_date() {
  MaintenanceDatePrompt &p    = maintenance_date_;
  const auto             last = parse_user_date(p.last_done);
  const auto             next = parse_user_date(p.next_due);
  if (!last && !next)
    return;
  Json body;
  body["user"]      = user_ref();
  body["kind"]      = Json(p.kind);
  body["last_done"] = last ? Json(last->iso()) : Json();
  body["next_due"]  = next ? Json(next->iso()) : Json();
  body["note"]      = p.note;
  p.busy            = true;
  p.error.clear();
  const std::string iid   = p.iid;
  const std::string kind  = p.kind;
  const std::string label = p.label;
  api.post("/api/items/" + iid + "/maintenance/date/", body, [this, iid, kind, label](const ApiResult &result) {
    maintenance_date_.busy = false;
    if (!result.ok) {
      identity_refused(result);
      maintenance_date_.error = result.error;
      return;
    }
    maintenance_date_ = MaintenanceDatePrompt{}; // ferme la fenetre
    notify(result.data["type_name"].str() + " : " + label + " prévue le " + display_date(result.data[kind + "_due"])
           + ".");
    after_maintenance(iid);
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
