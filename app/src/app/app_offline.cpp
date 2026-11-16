/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_offline.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Vérif hors connexion : cache de vérif et file des envois en attente.

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

// Vérif hors connexion : cache de vérif (inventory/verif_cache.py) et file des envois en attente

void App::sync_cache(double max_age) {
  if (cache_syncing_ || ImGui::GetTime() - cache_synced_at_ < max_age)
    return;
  cache_syncing_ = true;
  api.post("/api/verifs/cache/", vcache.request(), [this](const ApiResult &result) {
    cache_syncing_   = false;
    cache_synced_at_ = ImGui::GetTime();
    offline_         = !result.ok && result.status == 0;
    if (!result.ok)
      return;
    const bool                       first   = !vcache.loaded();
    const std::vector< std::string > changed = vcache.apply(result.data);
    if (!first && !changed.empty())
      cache_changed(changed);
    if (!outbox.empty())
      next_outbox_try_ = 0.0; // reseau retrouve : on renvoie tout de suite
  });
}

// Cache de vérif mis a jour : lot de la verif en cours remplace par son nouveau detail, items deja scannes compares a
// leur nouvel etat (seule une difference est signalee)
void App::cache_changed(const std::vector< std::string > &keys) {
  const std::set< std::string > changed(keys.begin(), keys.end());
  if (verif.active && !verif.loading && changed.count("l:" + verif.lot_id)) {
    Json fresh = vcache.lot(verif.lot_id);
    if (!fresh.is_null())
      verif.lot = fresh;
  }
  for (const ScanEntry &entry : stack.entries()) {
    if (entry.scan.kind != ScanKind::Item || entry.state != EntryState::Ok || !changed.count("i:" + entry.scan.id))
      continue;
    if (const Json *item = vcache.item(entry.scan.id))
      item_confirmed(entry.id, *item, entry.source);
  }
}

void App::post_verif(const Json &request, const std::string &label, Json offline, ApiClient::Callback done) {
  Json body       = request;
  body["done_at"] = utc_now_iso(); // heure des scans, gardee si la verif attend dans la file (verif_stamped)
  const auto queue = [this, body, label, offline, done]() {
    outbox.push({ body, label, now_hhmm() });
    offline_ = true;
    ApiResult local;
    local.ok              = true;
    local.status          = 200;
    local.data            = offline;
    local.data["offline"] = true;
    done(local);
  };
  if (!outbox.empty()) { // l'ordre compte (etapes de la verif en cascade) : derriere ceux qui attendent
    queue();
    return;
  }
  api.post("/api/verifs/", verif_stamped(body), [this, queue, done](const ApiResult &result) {
    if (!result.ok && result.status == 0) {
      queue();
      return;
    }
    offline_ = false;
    done(result);
  });
}

// Compte rendu local d'une verif mise en attente : les lots valides d'apres la repartition des scans (complets ou
// non) et les lots scelles intacts. Le vrai compte rendu viendra du serveur a l'envoi.
Json App::offline_report(const std::set< std::string > &lots, bool partial, const std::set< std::string > &sealed) const {
  Json report;
  report["lots"]  = Json::array();
  bool complete   = true;
  for (const VerifPlanLot &row : plan_verif()) {
    const std::string id = (*row.lot)["id"].str();
    if (!lots.count(id) || (partial && !row.complete))
      continue;
    Json line;
    line["id"]       = id;
    line["name"]     = (*row.lot)["name"].str();
    line["depth"]    = row.depth;
    line["verified"] = true;
    line["complete"] = row.complete;
    complete         = complete && row.complete;
    report["lots"].push_back(line);
  }
  Json names = Json::array();
  for (const std::string &id : sealed) {
    Json        line;
    const Json *lot = verif_lot(id);
    line["id"]     = id;
    line["name"]   = lot ? (*lot)["name"].str() : id;
    line["sealed"] = true;
    names.push_back(line["name"]);
    report["lots"].push_back(line);
  }
  report["sealed_lots"] = names;
  report["complete"]    = complete;
  report["partial"]     = partial;
  report["present"]     = Json::array();
  for (const std::string &iid : stack.iids())
    report["present"].push_back(iid);
  return report;
}

// Envois en attente renvoyes dans l'ordre. Refuse par le serveur (etiquette cle perimee, scelle brise entre-temps) :
// retire de la file et signale, a refaire.
void App::flush_outbox() {
  if (outbox.empty() || outbox_busy_)
    return;
  outbox_busy_ = true;
  api.post("/api/verifs/", verif_stamped(outbox.front().body), [this](const ApiResult &result) {
    outbox_busy_ = false;
    if (outbox.empty())
      return;
    if (!result.ok && result.status == 0) {
      offline_         = true;
      next_outbox_try_ = ImGui::GetTime() + 20.0;
      return;
    }
    offline_                = false;
    const OutboxEntry entry = outbox.front();
    outbox.pop_front();
    if (!result.ok)
      notify("Vérif de " + entry.label + " (faite hors connexion à " + entry.at + ") refusée par le serveur : "
               + result.error + ". À refaire.",
             true);
    else if (outbox.empty())
      notify("Réseau retrouvé : vérifs faites hors connexion envoyées (dernière : " + entry.label + ").");
    if (!outbox.empty())
      flush_outbox();
    else {
      sync_cache();
      refresh_lots(true);
    }
  });
}

} // namespace qrprotec
