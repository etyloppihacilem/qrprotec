/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_data.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : Donnees du serveur (types, lots, stock, taches, previsions) et impressions d'etiquettes.

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

void App::refresh_events() {
  catalog.loading_events = true;
  api.get("/api/events/?days=" + std::to_string(catalog.events_days), [this](const ApiResult &result) {
    catalog.loading_events = false;
    if (result.ok)
      catalog.events = result.data;
    else
      notify("À venir : " + result.error, true);
  });
}

void App::place_order(const std::string &type, const std::string &name, int quantity) {
  Json body = Json::object();
  add_identity(body);
  body["type"]     = Json(type);
  body["quantity"] = Json(quantity);
  api.post("/api/orders/", body, [this, name, quantity](const ApiResult &result) {
    if (identity_refused(result))
      return;
    if (!result.ok) {
      notify("Commande : " + result.error, true);
      return;
    }
    catalog.tasks = result.data;
    notify("Commande de " + std::to_string(quantity) + " × " + name + " enregistrée : en attente de livraison.");
  });
}

void App::cancel_order(long long id, const std::string &name) {
  Json body = Json::object();
  add_identity(body);
  api.post("/api/orders/" + std::to_string(id) + "/cancel/", body, [this, name](const ApiResult &result) {
    if (identity_refused(result))
      return;
    if (!result.ok) {
      notify("Commande : " + result.error, true);
      return;
    }
    catalog.tasks = result.data;
    notify("Commande de " + name + " décochée.");
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
    case TemplateCategory::LotLocation: return "{{emplacement}}"; // texte seul (qr_only_template)
    case TemplateCategory::User:
    case TemplateCategory::GuestCard: return "{{badge_url}}";
    case TemplateCategory::TaskItem: return "{{task_url}}";
    case TemplateCategory::ItemTear: return "{{item_url}}";
    case TemplateCategory::ItemSmall:
    case TemplateCategory::ItemSmallTear:
    case TemplateCategory::TaskItemSmall:
    case TemplateCategory::ItemBarcode:
    case TemplateCategory::ItemBarcodeTear:
    case TemplateCategory::TaskItemBarcode:
    case TemplateCategory::ItemSheet: return "{{iid}}";
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
  if (category == TemplateCategory::LotLocation) { // emplacement : son nom en grand, jamais de code
    document.name = "Nom seul";
    TextElement text{ main_qr_payload(category), margin, margin, settings.label_width_mm - 2.0f * margin,
                      settings.label_height_mm - 2.0f * margin, 4.0f, true, TextAlign::Center };
    document.elements.push_back({ "emplacement", ElementKind::Text, text });
    return document;
  }
  if (category == TemplateCategory::ItemBarcode || category == TemplateCategory::ItemBarcodeTear
      || category == TemplateCategory::TaskItemBarcode) { // code-barres sur toute la largeur
    document.name = "Code-barres seul";
    document.elements.push_back({ "code-barres", ElementKind::Barcode,
                                  BarcodeElement{ main_qr_payload(category), margin, margin,
                                                  settings.label_width_mm - 2.0f * margin,
                                                  settings.label_height_mm - 2.0f * margin } });
    return document;
  }
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
  // modele configure pour cet usage, sinon premier modele du dossier fait pour lui (ex : scelle.qr) ; vide sinon
  const auto model_path = [this](TemplateCategory wanted) {
    const CategoryInfo &info = category_info(wanted);
    std::string         path;
    if (const auto configured = settings.label_templates.find(info.id); configured != settings.label_templates.end())
      path = configured->second;
    for (const auto &candidate : path.empty() ? glob_templates("*.qr") : std::vector< std::filesystem::path >{}) {
      TemplateDocument document;
      std::string      ignored;
      if (load_template(document, candidate.string(), ignored) && document.category == wanted) {
        path = candidate.filename().string();
        break;
      }
    }
    return path;
  };
  // etiquette d'item remplacee selon le type d'item : son style (QR de l'URL, QR reduit ou code-barres de l'iid
  // seul), en version liste de taches (son code ouvre la liste a cocher) ou a dechirer. Sans modele pour cette
  // version : celui du style (version normale), puis l'etiquette d'item de base.
  const auto label_categories = [category](const Parameters &label) {
    std::vector< TemplateCategory > chain;
    if (category != TemplateCategory::Item)
      return std::vector< TemplateCategory >{ category };
    const auto value = [&label](const char *name) {
      const auto found = label.find(name);
      return found == label.end() ? std::string() : found->second;
    };
    const std::string style = value("label_style");
    // planche : plusieurs petits QR de l'iid par etiquette ; un item a taches ou a dechirer garde une etiquette a
    // lui, au style QR reduit (comme sans modele de planche)
    const int         index = style == "small" || style == "sheet" ? 1 : style == "barcode" ? 2 : 0;
    static const TemplateCategory plain[] = { TemplateCategory::Item, TemplateCategory::ItemSmall,
                                              TemplateCategory::ItemBarcode };
    static const TemplateCategory tasks[] = { TemplateCategory::TaskItem, TemplateCategory::TaskItemSmall,
                                              TemplateCategory::TaskItemBarcode };
    static const TemplateCategory torn[]  = { TemplateCategory::ItemTear, TemplateCategory::ItemSmallTear,
                                              TemplateCategory::ItemBarcodeTear };
    const std::string task_count = value("task_count");
    if (!task_count.empty() && task_count != "0")
      chain.push_back(tasks[index]);
    else if (!value("tear_notice").empty())
      chain.push_back(torn[index]);
    else if (style == "sheet")
      chain.push_back(TemplateCategory::ItemSheet);
    chain.push_back(plain[index]);
    if (index != 0)
      chain.push_back(TemplateCategory::Item);
    return chain;
  };
  std::map< TemplateCategory, TemplateDocument > models;
  const auto model_for = [&](const Parameters &label) -> TemplateDocument * {
    const std::vector< TemplateCategory > chain = label_categories(label);
    for (const TemplateCategory wanted : chain)
      if (models.count(wanted))
        return &models[wanted];
    for (const TemplateCategory wanted : chain) {
      const std::string path = model_path(wanted);
      if (path.empty())
        continue;
      std::string error;
      if (!build_label(path, {}, models[wanted], error)) {
        models.erase(wanted);
        notify("Modèle d'étiquette « " + category_info(wanted).label + " » : " + error + " (Administration > Réglages).",
               true);
        return nullptr;
      }
      return &(models[chain.front()] = models[wanted]);
    }
    // pas de modele : on imprime simplement le code principal centre sur l'etiquette
    const CategoryInfo &info = category_info(chain.front());
    notify("Aucun modèle « " + info.label + " » : "
           + (chain.front() == TemplateCategory::LotLocation ? "nom seul" : "code seul")
           + " (Administration > Réglages pour en choisir un).");
    return &(models[chain.front()] = qr_only_template(chain.front()));
  };
  std::vector< const TemplateDocument * > chosen;
  for (const Parameters &label : labels) {
    const TemplateDocument *model = model_for(label);
    if (!model)
      return false;
    chosen.push_back(model);
  }
  // une etiquette par item, sauf sur une planche : les items qui se suivent avec le meme modele de planche la
  // remplissent dans l'ordre ({{code1}}, {{code2}}...), la derniere garde des places vides
  std::vector< std::pair< const TemplateDocument *, Parameters > > printed;
  for (std::size_t index = 0; index < labels.size();) {
    const TemplateDocument *model = chosen[index];
    if (model->category != TemplateCategory::ItemSheet) {
      printed.emplace_back(model, labels[index++]);
      continue;
    }
    const std::size_t         slots = static_cast< std::size_t >(sheet_slots(*model, labels[index]));
    std::vector< Parameters > items;
    while (index < labels.size() && chosen[index] == model && items.size() < slots)
      items.push_back(labels[index++]);
    printed.emplace_back(model, sheet_parameters(*model, items, static_cast< int >(slots)));
  }
  for (std::size_t index = 0; index < printed.size(); ++index) {
    PrintJob job;
    job.description = what + (printed.size() > 1 ? " " + std::to_string(index + 1) + "/" + std::to_string(printed.size()) : "");
    job.document    = *printed[index].first;
    job.document.parameters["today"]      = today().display();
    job.document.parameters["printed_by"] = user_name();
    job.document.parameters["titre"]      = settings.label_title;
    for (const auto &[name, value] : printed[index].second)
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
  if (prints_to_poste()) {
    send_to_poste(std::move(jobs));
    return;
  }
  const std::size_t count = jobs.size();
  // premiere etiquette a code-barres : des codes-barres circulent, le front web charge desormais leur decodeur
  const bool barcode = std::any_of(jobs.begin(), jobs.end(), [](const PrintJob &job) {
    return std::any_of(job.document.elements.begin(), job.document.elements.end(),
                       [](const TemplateElement &element) { return element.kind == ElementKind::Barcode; });
  });
  if (barcode && !web_barcodes)
    api.post("/api/barcodes-printed/", Json::object(), [this](const ApiResult &result) {
      if (result.ok)
        web_barcodes = true;
    });
  printer.enqueue(std::move(jobs), settings.print, settings.physical_label());
  notify(nb(count, "étiquette envoyée", "étiquettes envoyées") + " à l'imprimante.");
}

} // namespace qrprotec

namespace qrprotec {

} // namespace qrprotec
