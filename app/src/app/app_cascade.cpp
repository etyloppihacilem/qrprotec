/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_cascade.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Vérif en cascade d'un lot global (VPS > armoires > sacs), lot par lot.

#include "app.hpp"
#include "app_internal.hpp"
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

// Vérif en cascade : un lot global qui contient des sous-lots (ex : VPS > armoires > sacs) se verifie lot par lot. On
// entre dans un sous-lot en scannant son etiquette (ou un de ses items), on le valide : sa verif est enregistree tout
// de suite (POST /api/verifs/ avec "exclude" et "keys") et l'on revient au lot qui le contient. Les sous-lots non faits
// restent tels quels. Meme fonctionnement que le front web (renderCascade, validateStep).

namespace {
bool own_content(const Json &lot) {
  return lot["requirements"].size() > 0 || lot["items"].size() > 0 || lot["missing_items"].size() > 0;
}

} // namespace

// Heure locale « HH:MM » (envois en attente, etapes de la cascade), declaree dans app_internal.hpp
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

static std::string real_parent(const Json &lot) {
  const Json &path = lot["path"];
  return !lot["parent"].str().empty() ? lot["parent"].str()
                                      : (path.size() > 0 ? path[path.size() - 1]["id"].str() : std::string());
}

// Lot qui contient id dans la verif en cascade. Un lot cache (Lots.hidden_in_verif, ex : le B+ du VPS) est saute :
// ses sous-lots apparaissent directement dans son parent (meme regle que le web, parentIdOf).
std::string App::verif_parent(const std::string &id) const {
  const Json *lot = verif_lot(id);
  if (!lot)
    return "";
  std::string parent = real_parent(*lot);
  for (int guard = 0; guard < 64; ++guard) {
    const Json *above = verif_lot(parent);
    if (!above || !(*above)["hidden"].boolean())
      break;
    parent = real_parent(*above);
  }
  return parent;
}

std::vector< const Json * > App::verif_children(const std::string &id) const {
  std::vector< const Json * > result;
  for (const Json *lot : verif_lots())
    if ((*lot)["id"].str() != id && !(*lot)["hidden"].boolean() && verif_parent((*lot)["id"].str()) == id)
      result.push_back(lot);
  return result;
}

// Lot ou l'on entre a la place d'un lot cache : le premier lot affiche qui le contient
std::string App::verif_visible(const std::string &id) const {
  const Json *lot = verif_lot(id);
  if (!lot || !(*lot)["hidden"].boolean())
    return id;
  const std::string parent = verif_parent(id);
  return parent.empty() ? verif.lot_id : parent;
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

// Lot scelle de la verif, scelle pas encore constate : la fenetre Vérif ne demande que l'etiquette du scelle (meme regle
// que le web, sealWaiting). Plus le cas si un item du lot a ete scanne (il a ete ouvert), si l'on est entre plus bas,
// ou si l'on a choisi d'en verifier le contenu.
bool App::verif_seal_waiting(const std::string &id) const {
  const Json *lot = verif_lot(id);
  if (!lot || !(*lot)["is_sealed"].boolean() || verif.open_seals.count(id) || verif.done.count(id))
    return false;
  const std::set< std::string > ids = verif_subtree(id);
  for (const VerifSealed &sealed : verif.sealed)
    if (ids.count(sealed.id))
      return false;
  for (const std::string &entered : verif.entered)
    if (entered != id && ids.count(entered))
      return false;
  const std::vector< std::string > scanned = stack.iids();
  const std::set< std::string >    done(scanned.begin(), scanned.end());
  for (const Json *sub : verif_lots()) {
    if (!ids.count((*sub)["id"].str()))
      continue;
    for (const char *key : { "items", "missing_items" })
      for (const Json &item : (*sub)[key].items())
        if (done.count(item["iid"].str()))
          return false;
  }
  if (verif_cascade())
    for (const ScanEntry &entry : stack.entries())
      if (entry.state != EntryState::Error
          && (entry.scan.kind == ScanKind::Item || entry.scan.kind == ScanKind::SealedPack)
          && ids.count(entry.place.empty() ? verif.lot_id : entry.place))
        return false;
  return true;
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

void App::enter_level(const std::string &target) {
  const std::string id = verif_visible(target); // lot cache : on entre dans le lot affiche qui le contient
  if (!verif_lot(id))
    return;
  verif.path.clear();
  for (std::string current = id; !current.empty() && current != verif.lot_id && verif.path.size() < 64;
       current = verif_parent(current))
    verif.path.insert(verif.path.begin(), current);
  verif.current = id;
  verif.entered.insert(id);
  if (const auto done = verif.done.find(id); done != verif.done.end()) {
    verif.reopened[id] = done->second;
    verif.done.erase(done);
  }
}

// Retour au lot qui contient le lot en cours, sans le valider. Rien scanne dedans : il n'est pas « commence », et un lot
// valide qu'on avait rouvert redevient valide (meme regle que le web, goUp).
void App::leave_level() {
  const std::string level = verif_level();
  if (level != verif.lot_id && !verif.done.count(level)) { // pas juste apres sa validation (submit_step)
    const std::set< std::string > scope   = verif_scope(level);
    bool                          started = false;
    for (const ScanEntry &entry : stack.entries())
      if (entry.state != EntryState::Error && !entry.tasks_pending)
        started = started || scope.count(entry.place.empty() ? verif.lot_id : entry.place) > 0;
    bool below = false;
    for (const std::string &other : verif.entered)
      below = below || (other != level && verif_under(other, level));
    if (!started && !below) {
      verif.entered.erase(level);
      if (const auto before = verif.reopened.find(level); before != verif.reopened.end())
        verif.done[level] = before->second;
    }
  }
  verif.reopened.erase(level);
  if (!verif.path.empty())
    verif.path.pop_back();
  verif.current = verif_level();
}

// Bouton « ← Revenir à … » de la vérif en cascade : le lot en cours est validé en le quittant, complet ou non (les non
// scannés passent manquants), puis on remonte (submit_step). Sans scan dedans, on remonte simplement ; étiquette du lot
// ou étiquette clé manquante : on remonte sans valider, scans gardés. Même règle que le web (leaveUp).
void App::leave_level_validated() {
  const std::string level = verif_level();
  if (!verif_cascade() || verif.submitting || level == verif.lot_id)
    return;
  const std::set< std::string > scope   = verif_scope(level);
  bool                          started = false;
  for (const ScanEntry &entry : stack.entries())
    if (entry.state != EntryState::Error && !entry.tasks_pending)
      started = started || scope.count(entry.place.empty() ? verif.lot_id : entry.place) > 0;
  if (!started) {
    leave_level();
    return;
  }
  const Json       *lot  = verif_lot(level);
  const std::string name = lot ? (*lot)["name"].str(level) : level;
  if (!verif_key_ok() || !verif_label_ok()) {
    notify(name + " n'est pas validé ("
             + (verif_key_ok() ? "scannez l'étiquette " + verif_label_missing() : std::string("étiquette clé manquante"))
             + ") : scans gardés.",
           true);
    leave_level();
    return;
  }
  submit_step(false);
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
    if (!verif_label_ok()) {
      notify("Scannez l'étiquette " + verif_label_missing() + " pour valider.", true);
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
    body["labels"]  = verif_labels();
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
    body["checked"]  = verif_checked_body(&scope);
    verif_timing(body);
    body["partial"]  = partial;
    body["step"]     = true;
    verif.submitting = true;
    const std::string root = verif.lot_id;
    const std::string name = (*lot)["name"].str(top);
    // compte rendu local (hors connexion) : les lots de l'etape, sans les sous-lots laisses de cote ni les scelles
    std::set< std::string > verified = scope;
    for (const std::string &id : exclude)
      verified.erase(id);
    for (const std::string &id : verif_intact())
      verified.erase(id);
    post_verif(body, name, offline_report(verified, partial, sent_seals),
               [this, root, top, name, at_root, partial, sent, sent_seals](const ApiResult &result) {
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
      verif.started_at           = utc_now_iso(); // l'etape suivante de la vérif en cascade commence
      bool              finished = !partial;
      int               verified = 0, incomplete = 0;
      for (const Json &row : result.data["lots"].items()) {
        if (!row["verified"].boolean() && !row["sealed"].boolean())
          continue;
        verif.done[row["id"].str()] = { at, row["complete"].boolean(), row["sealed"].boolean() };
        verif.checked.erase(row["id"].str());
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
      // rangement du stock : y ranger des items est l'usage normal, « Ajouter au lot » clot la verif comme une verif
      // complete du rangement (celui dont on a lance la verif ; dans une tournee du stock, retour au lot parent)
      const bool stored = partial && !finished && top_lot && (*top_lot)["storage"].boolean();
      if (stored && (at_root || top == verif.started_in)) {
        verif.after_step.clear();
        finish_cascade(name + " : " + nb(result.data["restocked"].size(), "item rangé", "items rangés")
                       + ", vérif terminée.");
        return;
      }
      const std::size_t missing = result.data["missing"].size();
      if (stored) {
        notify(name + " : " + nb(result.data["restocked"].size(), "item rangé", "items rangés")
               + ". Retour au lot qui le contient.");
        finished = true;
      } else if (!finished)
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
  if (!verif_cascade() || verif.loading || verif.submitting || !logged_in() || !verif_ready())
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
      if (requirement["check"].boolean()) {
        missing += requirement["required"].integer() > 0 && !verif_checked(lot["id"].str(), requirement["type"].str());
        continue;
      }
      const auto found = row.fresh.find(requirement["type"].str());
      missing += row_items(requirement, std::max< int >(0, static_cast< int >(requirement["required"].integer())
                                                              - (found == row.fresh.end() ? 0 : found->second)));
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
  body["labels"]   = verif_labels();
  body["step"]     = true;
  verif.submitting = true;
  const std::string       root = verif.lot_id;
  std::set< std::string > seals;
  for (const VerifSealed &sealed : verif.sealed)
    seals.insert(sealed.id);
  const Json *first = verif_lot(verif.sealed.front().id);
  post_verif(body, first ? (*first)["name"].str() : verif.sealed.front().id, offline_report({}, false, seals),
             [this, root](const ApiResult &result) {
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
    const std::string finish = verif.seal_finish;
    verif.seal_finish.clear();
    if (!finish.empty() && verif.done.count(finish)) {
      const Json *lot = verif_lot(finish);
      finish_cascade((lot ? (*lot)["name"].str() : finish) + " : scellé intact constaté, vérif terminée.");
      return;
    }
    refresh_lots();
  });
}

void App::submit_verif(bool partial) {
  // lot scelle dont le scelle n'est pas encore constate : sa verif est le scan de l'etiquette du scelle (valider sans
  // briserait le scelle et compterait tout son contenu manquant)
  const std::string seal_lot = verif_cascade() ? verif_level() : verif.extras.empty() ? verif.lot_id : std::string();
  if (verif.active && !seal_lot.empty() && verif_seal_waiting(seal_lot)) {
    const Json *lot = verif_lot(seal_lot);
    notify((lot ? (*lot)["name"].str() : seal_lot) + " est scellé : scannez l'étiquette collée sur le scellé.", true);
    return;
  }
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
    if (!verif_label_ok()) {
      notify("Scannez l'étiquette " + verif_label_missing() + " pour valider la vérif.", true);
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
    body["labels"]   = verif_labels();
    body["checked"]  = verif_checked_body();
    verif_timing(body);
    body["partial"]  = partial;
    verif.submitting  = true;
    const std::string       title = verif_title();
    std::set< std::string > lots, seals;
    for (const VerifPlanLot &row : plan_verif())
      lots.insert((*row.lot)["id"].str());
    for (const VerifSealed &sealed : verif.sealed)
      seals.insert(sealed.id);
    post_verif(body, title, offline_report(lots, partial, seals), [this, title](const ApiResult &result) {
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
      if (result.data["offline"].boolean())
        notify("Hors connexion : vérif gardée sur ce poste, envoyée dès le retour du réseau.");
      else if (result.data["partial"].boolean())
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
    verif            = {};
    verif.active     = true;
    verif.started_at = utc_now_iso();
    verif.lot_id     = stack.target.id;
    verif.lot["name"] = stack.target.name;
  }
  verif.key = stack.target.key;
  verif.labels.insert(stack.target.id);
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
    if (!verif_label_ok()) {
      notify("Scannez l'étiquette " + verif_label_missing() + " pour ajouter des items.", true);
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
    body["labels"]          = verif_labels();
    if (debug_options())
      body["debug"] = true;
    const std::string lot   = verif.lot_id;
    const std::string name  = verif.lot["name"].str(lot);
    // credit de confiance du reassort (reglage du serveur decoche) : le lot reste au vert
    const bool credit       = verif.lot["restock_credit"].is_object();
    verif.submitting        = true;
    api.post("/api/lots/" + url_encode(lot) + "/add/", body, [this, lot, name, credit](const ApiResult &result) {
      if (verif.lot_id == lot)
        verif.submitting = false;
      if (!result.ok) {
        identity_refused(result);
        notify("Ajout refusé : " + result.error, true);
        return;
      }
      notify("Réassort : " + nb(result.data["moved"].size(), "item ajouté", "items ajoutés") + " à " + name
             + (credit ? ". Lot réassorti, toujours vérifié." : ". Vérif complète recommandée."));
      if (result.data["unknown"].size() > 0)
        notify(nb(result.data["unknown"].size(), "item inconnu ignoré", "items inconnus ignorés") + ".", true);
      notify_refused(result.data["refused"], "Non ajouté");
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
    if (debug_options())
      body["debug"] = true;
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
      notify_refused(result.data["refused"], "Non ajouté");
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
  if (debug_options())
    body["debug"] = true;
  api.post("/api/items/to-stock/", body, [this](const ApiResult &result) {
    if (!result.ok) {
      notify("Retour en stock refusé : " + result.error, true);
      return;
    }
    notify(nb(result.data["moved"].size(), "item remis", "items remis") + " en stock.");
    notify_refused(result.data["refused"], "Non remis en stock");
    stack.clear();
    refresh_lots();
  });
}

// Items laisses de cote par le serveur (supprime, perime pour un lot, dans un autre lot sous nom declare...)
void App::notify_refused(const Json &refused, const std::string &prefix) {
  for (const Json &row : refused.items())
    notify(prefix + " : " + row["iid"].str() + " (" + row["reason"].str() + ")", true);
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

} // namespace qrprotec
