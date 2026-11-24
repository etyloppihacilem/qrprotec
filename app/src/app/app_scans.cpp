/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_scans.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Scans : traitement de chaque code lu (douchette, telephone, saisie).

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
  // code d'action rapide du poste (rouleau en place...) : execute tout de suite, sans toucher a la verif ni a la pile
  if (const ParsedScan action = parse_scan(code); action.kind == ScanKind::PosteAction) {
    handle_poste_action(action, source);
    return;
  }
  // fenetre « Impressions en attente » ouverte : un badge (meme avec code PIN) montre les impressions de cette
  // personne, sans la connecter (bouton « Se connecter » dans la fenetre)
  if (label_prints_badge(code, source))
    return;
  // popup « Passer a … ? » ignoree : le scan suivant vaut « Passer » (on passe au lot scanne, scans gardes)
  if (verif.leave_prompt.open)
    answer_leave(1);
  const ParsedScan scan = parse_scan(code);
  // etiquette publique scannee deux fois de suite hors verif : la 1re ouvre la fiche du lot, la 2e lance sa verif
  const bool label_again = scan.kind == ScanKind::Lot && scan.key.empty() && !scan.id.empty()
                        && scan.id == public_label_scanned_;
  public_label_scanned_.clear();
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
    case ScanKind::Lot:
      if (label_again && !verif.active)
        start_verif(scan.id, "", true);
      else
        scan_lot(scan, source);
      return;
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
        show_pack(scan.id, scan.key);
      return;
    }
    case ScanKind::Checklist: return;
    case ScanKind::PosteAction: return; // deja traite plus haut
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

// Ligne de detail d'un item scanne (peremption, lot, statut, desactivation, signalement)
static std::string item_detail_text(const Json &item) {
  std::string detail = item["peremption"].is_null() ? "Non périssable" : "Exp. " + display_date(item["peremption"]);
  detail += item["status"].str() == "out" || item["status"].str() == "loose" ? "" : item["location"].is_null() ? " - en stock, sans rangement" : " - " + item["location_name"].str();
  const std::string status = item["status"].str();
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
  return detail;
}

void App::resolve_item(int entry_id) {
  const ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  const ScanSource source = entry->source;
  // cache de vérif : l'item est connu tout de suite (signalement, desactivation, lot), le serveur confirme ensuite et
  // seule une difference est signalee (item_confirmed). Hors connexion, le cache fait foi.
  const Json *cached = vcache.item(entry->scan.id);
  const bool  had    = cached != nullptr;
  if (had)
    item_resolved(entry_id, *cached, source);
  api.get("/api/items/" + url_encode(entry->scan.id) + "/", [this, entry_id, source, had](const ApiResult &result) {
    ScanEntry *entry = stack.find(entry_id);
    if (!entry)
      return;
    if (result.status == 404) {
      entry_error(entry_id, "Inconnu dans la base", source);
      return;
    }
    if (!result.ok) {
      if (!had) {
        entry->state  = EntryState::Pending;
        entry->detail = "Non vérifié : " + result.error;
      }
      return;
    }
    if (had)
      item_confirmed(entry_id, result.data, source);
    else
      item_resolved(entry_id, result.data, source);
  });
}

// Faits d'un item qui changent ce que la verif en dit (meme liste que itemFacts du front web)
static std::string item_facts(const Json &item) {
  std::string facts = std::string(item["expired"].boolean() ? "E" : "-") + (item["disabled"].boolean() ? "D" : "-")
                    + (item["unusable"].boolean() ? "U" : "-") + "|" + item["status"].str() + "|" + item["location"].str();
  for (const Json &report : item["reports"].items())
    facts += "|" + report["id"].str() + ":" + report["status"].str() + ":" + (report["usable"].boolean() ? "1" : "0") + ":"
           + report["comment"].str() + ":" + report["reply"].str();
  for (const Json &task : item["tasks"].items())
    facts += "|t:" + task.str();
  return facts;
}

// Etat actuel (serveur) d'un item scanne traite depuis le cache de vérif : une difference qui compte est appliquee, avec
// le bip d'erreur s'il devient a remplacer et la fenetre des signalements si un signalement est arrive.
void App::item_confirmed(int entry_id, const Json &fresh, ScanSource source) {
  ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  if (item_facts(entry->data) == item_facts(fresh)) {
    entry->data = fresh;
    return;
  }
  std::set< std::string > known;
  for (const Json &report : entry->data["reports"].items())
    known.insert(report["id"].str());
  entry->data    = fresh;
  entry->title   = fresh["type_name"].str();
  entry->warning = fresh["status"].str() != "active";
  entry->detail  = (entry->tasks_done ? "Tâches faites - " : "") + item_detail_text(fresh);
  if (fresh["expired"].boolean() && !entry->expired) {
    entry->expired = true;
    feedback.error(source, settings);
    notify(fresh["type_name"].str() + " : à remplacer (" + item_detail_text(fresh) + ").", true);
  }
  bool added = false;
  for (const Json &report : fresh["reports"].items())
    added = added || !known.count(report["id"].str());
  if (fresh["reported"].boolean() && added) {
    reports_alert_.last_key.clear(); // nouveau signalement : la fenetre s'ouvre meme juste apres la precedente
    alert_reports("item:" + fresh["iid"].str(), "Item signalé : " + fresh["type_name"].str(), fresh["reports"], source);
  }
}

void App::item_resolved(int entry_id, const Json &resolved, ScanSource source) {
  ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  const Json item  = resolved; // copie : `resolved` peut etre dans le cache de vérif
  entry->state     = EntryState::Ok;
  entry->data      = item;
  entry->title     = item["type_name"].str();
  entry->warning   = item["status"].str() != "active";
  entry->detail    = item_detail_text(item);
  // verif en cascade : un item est compte dans le lot ou l'on est (on l'y a trouve), meme range ailleurs dans la verif ;
  // on ne change de lot qu'en scannant son etiquette. Seul un item d'un sous-lot du lot en cours fait entrer dans ce
  // sous-lot (on n'est encore dans aucun d'eux), sauf s'il a deja ete valide.
  if (verif_cascade() && !item["location"].str().empty() && verif_lot(item["location"].str())) {
    const std::string where = item["location"].str();
    const std::string level = verif_level();
    const std::string name  = (*verif_lot(where))["name"].str();
    if (!verif_under(where, level)) {
      notify(item["type_name"].str() + " : rangé dans " + name + ", compté dans " + (*verif_lot(level))["name"].str()
             + " où vous l'avez trouvé (il y sera déplacé).");
    } else if (verif.done.count(where)) {
      stack.remove(entry_id);
      feedback.notice(source, settings);
      notify(item["type_name"].str() + " : " + name + " a déjà été validé, item non recompté (scannez l'étiquette de "
               + name + " pour le refaire).",
             true);
      return;
    } else if (where != level) {
      entry->place = where;
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
  if (entry->expired && verif.active) {
    const std::string advice = discard_advice(item);
    if (!advice.empty())
      notify(advice, true);
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
  if (button("Valider", ImVec2(ImGui::GetFontSize() * 10.0f, 0)) || enter_pressed())
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
  // cache de vérif : le paquet est connu tout de suite, le serveur confirme ensuite (seule une difference compte)
  const Json *cached = vcache.pack(entry->scan.id);
  const bool  had    = cached != nullptr;
  if (had)
    pack_resolved(entry_id, *cached, source);
  // etiquette de paquet ouvert : sa cle, controlee par le serveur (open_label.scanned)
  const std::string query = entry->scan.key.empty() ? "" : "?o=" + url_encode(entry->scan.key);
  api.get("/api/packs/" + url_encode(entry->scan.id) + "/" + query, [this, entry_id, source, had](const ApiResult &result) {
    ScanEntry *entry = stack.find(entry_id);
    if (!entry)
      return;
    if (!result.ok) {
      if (!had || result.status == 404)
        entry_error(entry_id, result.status == 404 ? "Paquet inconnu" : result.error, source);
      return;
    }
    const auto facts = [](const Json &pack) {
      std::string text = pack["disabled"].str() + "|" + pack["opened"].str() + "|" + pack["items"].dump() + "|"
                       + pack["open_label"].dump();
      for (const Json &report : pack["reports"].items())
        text += "|" + report["id"].str() + ":" + report["status"].str() + ":" + (report["usable"].boolean() ? "1" : "0");
      return text;
    };
    if (had && facts(entry->data) == facts(result.data)) {
      entry->data = result.data;
      return;
    }
    if (had)
      reports_alert_.last_key.clear(); // difference : la fenetre des signalements se rouvre s'il y en a
    pack_resolved(entry_id, result.data, source);
  });
}

// Ce que compte un paquet scanne en vérif : vide s'il compte, sinon pourquoi pas (meme regle que le front web).
// Etiquette du paquet ferme : tout son contenu tant qu'il n'est pas ouvert ; ouvert, ses items se scannent un par un.
// Etiquette de paquet ouvert (paquet pre-etiquete, cle o=) : les items qu'elle couvre, tant qu'elle est valable. Hors
// connexion (cache de vérif), la cle n'est pas controlee : une etiquette valable compte.
static std::string pack_refusal(const Json &pack, const std::string &key) {
  const Json &label = pack["open_label"];
  if (!key.empty()) {
    if (label.is_null() || (label.contains("scanned") && !label["scanned"].boolean()))
      return "ancienne étiquette de paquet ouvert, elle ne vaut plus rien : scannez les items un par un";
    if (!label["valid"].boolean())
      return "étiquette de paquet ouvert plus valable (" + label["problem"].str() + ") : scannez les items un par un";
    return "";
  }
  if (pack["opened"].is_null())
    return "";
  if (!label.is_null() && label["valid"].boolean())
    return "paquet ouvert : scannez son étiquette de paquet ouvert, ou ses items un par un";
  return "paquet ouvert, son étiquette ne compte plus : scannez ses items un par un";
}

void App::pack_resolved(int entry_id, const Json &resolved, ScanSource source) {
  ScanEntry *entry = stack.find(entry_id);
  if (!entry)
    return;
  const Json        pack     = resolved; // copie : `resolved` peut etre dans le cache de vérif
  const bool        prelabel = !entry->scan.key.empty();
  const std::string refusal  = pack_refusal(pack, entry->scan.key);
  entry->data = pack;
  entry->pack_items.clear();
  if (!refusal.empty()) {
    entry_error(entry_id, refusal, source);
    return;
  }
  entry->state = EntryState::Ok;
  for (const Json &iid : (prelabel ? pack["open_label"]["items"] : pack["items"]).items())
    entry->pack_items.push_back(iid.str());
  const std::string count = prelabel ? pack["open_label"]["count"].str() : pack["count"].str();
  entry->title  = (prelabel ? "Paquet pré-étiqueté : " : "Paquet : ") + count + " x " + pack["type_name"].str();
  entry->detail = pack["peremption"].is_null() ? "Non périssable" : "Exp. " + display_date(pack["peremption"]);
  const auto date = Date::parse(pack["peremption"].str());
  if (date && *date < today() && !entry->expired) {
    entry->expired = true;
    feedback.error(source, settings);
  }
  if (pack["disabled"].integer() > 0) {
    // items desactives (rappel produit) : a remplacer comme des perimes
    entry->detail += " - " + nb(pack["disabled"].integer(), "item désactivé", "items désactivés");
    if (!entry->expired) {
      entry->expired = true;
      feedback.error(source, settings);
    }
  }
  alert_reports("pack:" + pack["id"].str(), "Paquet signalé : " + pack["count"].str() + " x " + pack["type_name"].str(),
                pack["reports"], source);
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
  // verif lancee sur le lot scelle lui-meme : son etiquette du scelle suffit, la verif est validee aussitot
  const bool own_seal     = verif.active && !verif.loading && scan.id == verif.lot_id && verif_seal_waiting(scan.id);
  const bool during_verif = verif.active && !verif.loading && (scan.id != verif.lot_id || own_seal);
  if (!during_verif)
    show_lot(scan.id, scan.key);
  api.get("/api/lots/" + url_encode(scan.id) + "/?seal=" + url_encode(scan.key),
          [this, id = scan.id, code = scan.key, source, during_verif](const ApiResult &result) {
            // hors connexion : lot du cache de vérif, scelle accepte s'il est pose (code controle a l'envoi)
            const Json data = result.ok ? result.data : result.status == 0 ? cached_seal_lot(id) : Json();
            if (data.is_null()) {
              feedback.error(source, settings);
              notify("Lot inconnu : " + id, true);
              return;
            }
            const std::string check = data["seal_check"].str();
            const std::string name  = data["name"].str();
            if (during_verif && verif.active && root_of(data) == root_of(verif.lot)) {
              if (check != "valid" || data["expired_count"].integer() > 0) {
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
              verif.labels.insert(id); // l'etiquette du scelle montre aussi que la verif est bien celle de ce lot
              notify(name + " : scellé intact, compté comme vérifié sans être ouvert.");
              if (id == verif.lot_id) {
                submit_verif(); // la verif du lot, c'est ce scan (connexion demandee si besoin)
                return;
              }
              // lot scelle ou l'on etait entre : rien d'autre a y scanner, on revient au lot qui le contient (vérif
              // lancee sur ce lot : elle s'arrete la, une fois le scelle enregistre)
              if (verif_cascade() && verif_under(verif_level(), id)) {
                if (id == verif.started_in)
                  verif.seal_finish = id;
                enter_level(verif_parent(id));
              }
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
            } else if (data["expired_count"].integer() > 0) {
              feedback.error(source, settings);
              notify(name + " : scellé intact mais contient des périmés, à ouvrir.", true);
            } else {
              notify(name + " : scellé intact, lot valide.");
            }
          });
}

// Lot du cache de vérif pour une etiquette de scelle lue hors connexion : 'valid' s'il est scelle (le serveur controle
// son code a l'envoi), avec son lot global (root_of). Nul s'il n'est pas dans le cache.
Json App::cached_seal_lot(const std::string &id) const {
  const std::string root = vcache.root_of(id);
  if (root.empty())
    return Json();
  const Json lot = vcache.lot(root);
  Json       row = lot;
  if (root != id) {
    row = Json();
    for (const Json &sub : lot["descendants"].items())
      if (sub["id"].str() == id)
        row = sub;
    if (row.is_null())
      return Json();
  }
  Json global;
  global["id"]   = root;
  global["name"] = lot["name"].str();
  row["global"]  = global;
  row["seal_check"] = row["is_sealed"].boolean() ? "valid" : "unsealed";
  return row;
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
      if (verif.active && verif_lot(id))
        reload_verif_lots(); // lot de la verif ouvert : son contenu est maintenant a verifier
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
  // Entree : jamais celle d'un scan (douchette en mode clavier), voir enter_pressed()
  if (primary_button("J'ai compris", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.6f)) || enter_pressed()
      || escape_pressed())
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void App::scan_lot(const ParsedScan &scan, ScanSource source) {
  if (!scan.key.empty()) {
    stack.target = { scan.id, scan.key, scan.id };
    // le serveur compare la cle : sans connexion (ou role normal) il ne la renvoie pas
    const std::string path = "/api/lots/" + url_encode(scan.id) + "/?key=" + url_encode(scan.key);
    api.get(path, [this, id = scan.id, source](const ApiResult &result) {
      // hors connexion : etiquette cle acceptee si le lot est dans le cache de vérif (controlee a l'envoi)
      if (!result.ok && result.status == 0 && !vcache.root_of(id).empty())
        return;
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
    label_shown_          = scan.id; // « Lancer une vérif » depuis la fiche : l'etiquette du lot compte (verif_label_missing)
    public_label_scanned_ = scan.id; // la rescanner tout de suite lance la verif
    return;
  }
  if (verif.active)
    verif.labels.insert(scan.id);
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
    const std::string entered = verif_visible(scan.id); // etiquette d'un lot cache : son parent affiche
    const bool reopened = verif.done.count(entered) > 0;
    enter_level(entered);
    const std::string name = (*verif_lot(entered))["name"].str();
    notify(reopened ? name + " : déjà validé pendant cette vérif, vous le refaites."
           : entered == verif.lot_id ? name + " : scannez l'étiquette " + verif_children_de(entered) + " pour y entrer."
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
  start_verif(scan.id, scan.key, true);
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
        label_shown_ = lot_id;
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

} // namespace qrprotec
