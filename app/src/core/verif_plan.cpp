/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################
               """          verif_plan.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr
##################################################################################################################### */

#include "verif_plan.hpp"

#include <algorithm>

namespace qrprotec {

std::vector< std::pair< std::string, int > > row_weights(const Json &requirements, const std::string &type) {
  std::vector< std::pair< std::string, int > > result;
  for (const Json &row : requirements.items()) {
    if (row["check"].boolean())
      continue;
    if (row["type"].str() == type)
      result.emplace_back(type, 1);
    else if (row["fills"].contains(type))
      result.emplace_back(row["type"].str(), std::max(1, row["fills"][type].integer(1)));
  }
  return result;
}

std::pair< std::string, int > main_row(const std::vector< std::pair< std::string, int > > &weights,
                                       const std::string                                &type) {
  for (const auto &weight : weights)
    if (weight.first != type)
      return weight;
  return { type, 1 };
}

std::vector< VerifPlanLot > plan_verif(const std::vector< const Json * > &lots, const std::vector< VerifPlanScan > &scanned,
                                       const std::map< std::string, std::set< std::string > > &checked, bool cascade) {
  std::vector< VerifPlanLot >       plan;
  std::map< std::string, std::size_t > index;          // id du lot -> position dans plan
  std::map< std::string, std::string > location_of;    // iid connu -> lot
  std::map< std::string, std::map< std::string, int > > required, deficit;
  std::set< std::string >              holding;        // lots dont des items connus n'ont pas ete scannes
  std::set< std::string >              unticked;       // lots dont un item a cocher ne l'est pas
  std::set< std::string >              done;
  for (const VerifPlanScan &scan : scanned)
    done.insert(scan.iid);
  std::map< std::string, const Json * > rows_of; // attendus de chaque lot (types a unites : row_weights)
  for (const Json *lot : lots) {
    const std::string id = (*lot)["id"].str();
    rows_of[id]          = &(*lot)["requirements"];
    index[id]            = plan.size();
    plan.push_back({ lot, (*lot)["depth"].integer() });
    for (const char *list : { "items", "missing_items" })
      for (const Json &item : (*lot)[list].items()) {
        location_of[item["iid"].str()] = id;
        // un item a etiquette a dechirer non scanne a ete utilise, un item non consommable non scanne d'un lot
        // ordinaire passe hors lot (services.leaves_lot), un item facultatif reste ou il etait : ils ne retiennent
        // pas le lot
        if (!done.count(item["iid"].str()) && !item["tear_off"].boolean() && !item["optional"].boolean()
            && (item["consumable"].boolean(true) || (*lot)["storage"].boolean()))
          holding.insert(id);
      }
    for (const Json &row : (*lot)["requirements"].items()) {
      if (row["check"].boolean()) { // item a cocher : jamais scanne, coche a la main
        if (row["required"].integer() > 0 && !(checked.count(id) && checked.at(id).count(row["type"].str())))
          unticked.insert(id);
        continue;
      }
      required[id][row["type"].str()] = row["required"].integer();
    }
    deficit[id] = required[id];
  }
  if (plan.empty())
    return plan;
  // attendus du lot que remplit un item de ce type, en unites pour un type a unites (200 mL de serum phy)
  const auto weights_in = [&](const std::string &id, const std::string &type) {
    std::vector< std::pair< std::string, int > > result;
    for (const auto &weight : row_weights(*rows_of[id], type))
      if (required[id].count(weight.first))
        result.push_back(weight);
    return result;
  };
  // l'item remplit un manque du lot : il y est compte
  const auto take = [&](const std::string &id, const std::string &type) {
    const auto weights = weights_in(id, type);
    bool       useful  = false;
    for (const auto &weight : weights)
      useful = useful || deficit[id][weight.first] > 0;
    if (!useful)
      return false;
    for (const auto &weight : weights)
      deficit[id][weight.first] -= weight.second;
    return true;
  };
  // lot parent de chaque lot de la verif
  std::map< std::string, std::string > parent_of;
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
  // items des lots signales inutilisables (signalement ouvert) ou desactives : a remplacer comme des perimes
  std::set< std::string > unusable;
  for (const Json *lot : lots)
    for (const Json &item : (*lot)["items"].items())
      if (item["unusable"].boolean() || item["disabled"].boolean())
        unusable.insert(item["iid"].str());
  std::vector< VerifPlanScan > newcomers;
  std::map< std::string, std::string > target;
  std::vector< VerifPlanScan > all;
  // vérif en cascade : un item scanne hors de son lot (dans un lot qui ne le contient pas) y a ete trouve, il y va
  for (VerifPlanScan item : scanned) {
    item.expired = item.expired || unusable.count(item.iid) > 0;
    all.push_back(item);
    if (const auto found = location_of.find(item.iid);
        found != location_of.end() && (!cascade || item.place.empty() || under(found->second, item.place))) {
      target[item.iid] = found->second;
      if (!item.expired)
        take(found->second, item.type);
    } else {
      newcomers.push_back(item);
    }
  }
  std::sort(newcomers.begin(), newcomers.end(), [](const VerifPlanScan &a, const VerifPlanScan &b) {
    return a.expired != b.expired ? !a.expired : a.iid < b.iid;
  });
  for (const VerifPlanScan &item : newcomers) {
    const bool                 placed = index.count(item.place) > 0;
    std::vector< std::string > scope, candidates;
    for (const VerifPlanLot &row : plan) {
      const std::string id = (*row.lot)["id"].str();
      if (placed && !under(id, item.place))
        continue;
      scope.push_back(id);
      if (!weights_in(id, item.type).empty())
        candidates.push_back(id);
    }
    std::string choice;
    if (!item.expired)
      for (const std::string &id : candidates)
        if (take(id, item.type)) {
          choice = id;
          break;
        }
    if (choice.empty())
      choice = candidates.empty() ? scope.front() : candidates.front();
    target[item.iid] = choice;
  }
  for (const VerifPlanScan &item : all) {
    const std::string id  = target[item.iid];
    VerifPlanLot     &row = plan[index[id]];
    ++row.touched;
    row.iids.insert(item.iid);
    const auto found  = location_of.find(item.iid);
    const bool inside = found != location_of.end() && found->second == id;
    if (item.expired) {
      if (!inside)
        row.outsiders.insert(item.iid);
      continue;
    }
    // par attendu, en unites pour un type a unites (le sous-type compte aussi pour lui-meme)
    auto weights = weights_in(id, item.type);
    if (weights.empty())
      weights.emplace_back(item.type, 1);
    for (const auto &weight : weights)
      row.fresh[weight.first] += weight.second;
    if (!inside) {
      const auto main = main_row(weights, item.type);
      row.added[main.first] += main.second;
    }
  }
  for (VerifPlanLot &row : plan) {
    const std::string id = (*row.lot)["id"].str();
    row.required         = required[id];
    // seul compte le minimum d'items frais : un perime en trop ne retient pas le lot (il est retire, a jeter)
    bool filled = !unticked.count(id);
    for (const auto &[type, quantity] : required[id])
      filled = filled && row.fresh[type] >= quantity;
    // rangement du stock : ses items sont ses attendus, complet seulement si tous sont scannes
    const bool storage = (*row.lot)["storage"].boolean();
    row.complete = filled && (storage ? !holding.count(id) : (row.touched > 0 || !holding.count(id)));
  }
  return plan;
}

DiscardCheck discard_check(const std::vector< VerifPlanLot > &plan, const std::string &iid, const std::string &type) {
  DiscardCheck check;
  for (const VerifPlanLot &row : plan)
    if (row.iids.count(iid)) {
      if (row.outsiders.count(iid)) {
        check.outsider = true;
        return check;
      }
      // type a unites : besoin et frais en unites de son attendu (200 mL de serum phy)
      const std::string main     = main_row(row_weights((*row.lot)["requirements"], type), type).first;
      const auto        required = row.required.find(main);
      const auto        fresh    = row.fresh.find(main);
      check.need                 = required == row.required.end() ? 0 : required->second;
      check.have                 = fresh == row.fresh.end() ? 0 : fresh->second;
    }
  return check;
}

} // namespace qrprotec
