/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_verifs.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Verifs et mouvements : repartition, envoi, reassort, scelles.

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

// Verifs et mouvements

void App::show_lot(const std::string &lot_id, const std::string &seal_code) {
  lot_to_show_  = lot_id;
  seal_to_show_ = seal_code;
  label_shown_.clear();
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
void App::start_verif(const std::string &lot_id, const std::string &key, bool label) {
  {
    verif         = {};
    verif.active  = true;
    verif.lot_id  = lot_id;
    verif.key     = key;
    verif.current = lot_id;
    verif.loading = true;
    if (label || !key.empty())
      verif.labels.insert(lot_id);
    label_shown_.clear();
    open_window("verif");
    // cache de vérif : le lot global est deja la, la verif commence sans attendre le serveur
    const std::string cached_root = vcache.root_of(lot_id);
    if (!cached_root.empty()) {
      verif.loading = false;
      verif.lot     = vcache.lot(cached_root);
      if (cached_root != lot_id) {
        verif.lot_id     = cached_root;
        verif.started_in = lot_id;
        if (!verif.key.empty())
          verif.keys[lot_id] = verif.key;
        verif.key.clear();
        enter_level(lot_id);
      }
      adopt_prescans();
      sync_cache(30.0);
      return;
    }
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
          adopt_prescans();
        });
        return;
      }
      verif.loading = false;
      verif.lot     = result.data;
      adopt_prescans();
    });
  }
}

// Scans faits avant de lancer la verif (pile non videe) : ils en font partie. Vérif en cascade : chaque item va dans
// son lot s'il est de la verif (compte en entrant dans ce lot), sinon dans le lot ou la verif commence. Un item a
// taches dont les taches n'ont pas ete faites ne compte pas encore : sa fenetre des taches se rouvre.
void App::adopt_prescans() {
  int tasks = 0;
  for (ScanEntry &entry : stack.entries()) {
    if (entry.state != EntryState::Ok)
      continue;
    if (entry.place.empty() && verif_cascade()) { // sinon : lieu vide = lot de la verif, repartition par manque
      const std::string where = entry.data["location"].str();
      if (entry.scan.kind == ScanKind::Item && !where.empty() && verif_lot(where))
        entry.place = where;
      else if (entry.scan.kind == ScanKind::Item || entry.scan.kind == ScanKind::SealedPack)
        entry.place = verif_place();
    }
    if (entry.tasks_pending && !entry.expired && !tasks)
      tasks = entry.id;
  }
  if (tasks) {
    notify(stack.find(tasks)->title + " : scanné avant la vérif, faites ses tâches pour qu'il compte.");
    open_tasks(tasks);
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

std::string display_row(const Json &requirements, const std::string &type) {
  const auto weights = row_weights(requirements, type);
  for (const auto &weight : weights)
    if (weight.first == type)
      return type;
  return main_row(weights, type).first;
}

bool unit_row(const Json &row) {
  return row["is_group"].boolean() && row["within"].str().empty();
}

std::string unit_suffix(const Json &row) {
  return row["unit"].str().empty() ? std::string() : " " + row["unit"].str();
}

std::string amount_text(int count, const std::string &unit, const std::string &name) {
  return unit.empty() ? std::to_string(count) + " × " + name : std::to_string(count) + " " + unit + " de " + name;
}

int row_items(const Json &row, int units) {
  if (!unit_row(row) || units <= 0)
    return units;
  int most = 1;
  for (const auto &member : row["fills"].members())
    most = std::max(most, member.second.integer(1));
  return (units + most - 1) / most;
}

std::string requirement_name(const Json &row) {
  if (!row["within"].str().empty())
    return "dont " + row["type_name"].str();
  if (unit_row(row) && !row["unit"].str().empty())
    return row["type_name"].str() + " (" + row["unit"].str() + ")";
  return row["type_name"].str();
}

std::string lacking_text(const Json &row, int lacking) {
  if (!unit_row(row))
    return std::to_string(lacking) + " × " + row["type_name"].str();
  std::string text = amount_text(lacking, row["unit"].str(), row["type_name"].str());
  // sous-type a prendre : celui propose par le serveur (sous-type par defaut), sinon celui qui en remplit le plus
  const Json *pick = nullptr;
  for (const Json &sub : row["subtypes"].items())
    if (!pick || sub["type"].str() == row["suggest"]["type"].str())
      pick = &sub;
  if (pick && lacking > 0) {
    const int fills = std::max(1, (*pick)["fills"].integer(1));
    text += " (" + std::to_string((lacking + fills - 1) / fills) + " × " + (*pick)["type_name"].str() + ")";
  }
  return text;
}

// Une seule etiquette cle d'un lot de la verif suffit : celle du lot global couvre ses sous-lots, et celle d'un
// sous-lot (ajoute a la verif en la scannant) valide la verif de tout le lot global.
bool App::verif_key_ok() const {
  // l'etiquette cle n'est exigee que sans utilisateur connecte (nom declare, option du serveur) ; connecte,
  // l'etiquette d'un des lots suffit (verif_label_missing)
  if (logged_in())
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

// Lot a valider dont aucune etiquette (publique, cle ou du scelle) n'a ete scannee : celle du lot, d'un de ses
// sous-lots ou d'un lot qui le contient, pour ne pas valider la verif d'un autre lot. Vide si au moins une l'a ete.
// Meme regle que le serveur (views.require_label) et le front web (labelMissing).
std::string App::verif_label_missing() const {
  if (!verif.active || verif.loading)
    return "";
  std::set< std::string > scanned = verif.labels;
  if (!verif.key.empty())
    scanned.insert(verif.lot_id);
  for (const VerifExtra &extra : verif.extras)
    if (!extra.key.empty())
      scanned.insert(extra.id);
  for (const auto &[id, key] : verif.keys)
    if (!key.empty())
      scanned.insert(id);
  for (const VerifSealed &sealed : verif.sealed)
    scanned.insert(sealed.id);
  std::vector< std::string > tops;
  if (verif_cascade())
    tops.push_back(verif_level());
  else {
    tops.push_back(verif.lot_id);
    for (const VerifExtra &extra : verif.extras)
      tops.push_back(extra.id);
  }
  for (const std::string &top : tops)
    for (const std::string &id : scanned)
      if (id == top || verif_under(id, top) || verif_under(top, id))
        return "";
  const Json *lot = verif_lot(tops.front());
  const std::string name = lot ? (*lot)["name"].str(tops.front()) : verif.lot["name"].str(tops.front());
  const std::string subs = verif_children_de(tops.front());
  // lot d'un referentiel (B+, services.virtual_lot) : pas d'etiquette a lui, celles de ses sacs
  if (!subs.empty() && (lot ? (*lot)["virtual"].boolean() : verif.lot["virtual"].boolean()))
    return subs;
  return de_name(name) + (subs.empty() ? "" : " (ou celle " + subs + ")");
}

// Sous-lots de la verif d'un lot, nommes : « de Sac de soin ou de Sac O2 » (jusqu'a 3), sinon « d'un des lots qu'il
// contient » ; vide sans sous-lot. Les messages disent le nom des lots, pas « lot ou sous-lot ».
std::string App::verif_children_de(const std::string &id) const {
  std::vector< std::string > names;
  for (const Json *child : verif_children(id))
    names.push_back((*child)["name"].str((*child)["id"].str()));
  if (names.size() > 3)
    return "d'un des lots qu'il contient";
  return de_names(names);
}

// Lots dont l'etiquette a ete scannee, envoyes avec la verif (views.require_label)
Json App::verif_labels() const {
  Json labels = Json::array();
  for (const std::string &id : verif.labels)
    labels.push_back(id);
  return labels;
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

bool App::verif_checked(const std::string &lot, const std::string &type) const {
  const auto found = verif.checked.find(lot);
  return found != verif.checked.end() && found->second.count(type) > 0;
}

void App::toggle_verif_check(const std::string &lot, const std::string &type) {
  std::set< std::string > &types = verif.checked[lot];
  if (!types.erase(type))
    types.insert(type);
}

Json App::verif_checked_body(const std::set< std::string > *scope) const {
  Json body = Json::object();
  for (const auto &[lot, types] : verif.checked) {
    if (scope && !scope->count(lot))
      continue;
    Json list = Json::array();
    for (const std::string &type : types)
      list.push_back(type);
    body[lot] = list;
  }
  return body;
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
  // lot de la derniere etiquette scannee avant chaque item
  std::map< std::string, std::string > place_of;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.place.empty())
      continue;
    if (entry.scan.kind == ScanKind::Item)
      place_of.emplace(entry.scan.id, entry.place);
    else if (entry.scan.kind == ScanKind::SealedPack)
      for (const std::string &iid : entry.pack_items)
        place_of.emplace(iid, entry.place);
  }
  // items signales inutilisables (signalement ouvert) ou desactives au scan : a remplacer comme des perimes
  std::set< std::string > unusable;
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.scan.kind == ScanKind::SealedPack) {
      // paquet ferme entierement desactive : tous ses items sont a remplacer
      if (entry.data["disabled"].integer() > 0 && entry.data["disabled"].integer() >= entry.data["count"].integer())
        unusable.insert(entry.pack_items.begin(), entry.pack_items.end());
    } else if (entry.data["unusable"].boolean() || entry.data["disabled"].boolean())
      unusable.insert(entry.scan.id);
  }
  const Date                   today = this->today();
  std::vector< VerifPlanScan > scanned;
  for (const std::string &iid : stack.iids()) {
    const ParsedScan parsed = parse_scan(iid);
    const auto       place  = place_of.find(iid);
    scanned.push_back({ iid, parsed.item_type, place == place_of.end() ? std::string() : place->second,
                        is_expired(parsed, today) || unusable.count(iid) > 0 });
  }
  return qrprotec::plan_verif(lots, scanned, verif.checked, verif_cascade());
}

// Perime, inutilisable ou desactive (pas une revision en retard) : la verif le retire du lot, a jeter, des que le lot
// a son minimum d'items frais de ce type, meme deja presents (services.perform_verif) ; sinon il est a remplacer. Un
// perime qui n'etait pas dans le lot n'y entre jamais : il est a jeter.
std::string App::discard_advice(const Json &item) const {
  if (!item["outdated"].boolean() && !item["unusable"].boolean() && !item["disabled"].boolean())
    return {};
  const std::string  iid   = item["iid"].str();
  const DiscardCheck check = discard_check(plan_verif(), iid, parse_scan(iid).item_type);
  if (check.outsider)
    return item["type_name"].str() + " : ne le mettez pas dans le lot, jetez-le.";
  if (check.have >= check.need)
    return item["type_name"].str() + " : retirez-le du lot et jetez-le, le lot a son compte sans lui.";
  return item["type_name"].str() + " : à remplacer, jetez-le dès qu'un item non périmé le remplace (non "
         + pl(check.need, "périmé") + " : " + std::to_string(check.have) + "/" + std::to_string(check.need) + ").";
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

} // namespace qrprotec
