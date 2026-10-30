/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inventory_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../../core/plural.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <utility>

namespace qrprotec {

namespace {

const Json *find_type(const App &app, const std::string &type) {
  for (const Json &item_type : app.catalog.item_types.items())
    if (item_type["type"].str() == type)
      return &item_type;
  return nullptr;
}

// Rangements du stock actifs ou ranger une reception, nommes par leur chemin sous le Stock (« Armoire › Tiroir 2 »).
// Le Stock lui-meme n'en est pas un : sans rangement, les items restent dans le stock en vrac.
Json storage_places(const App &app) {
  const Json &lots = app.catalog.lots;
  auto        find = [&lots](const std::string &id) -> const Json * {
    for (const Json &lot : lots.items())
      if (lot["id"].str() == id)
        return &lot;
    return nullptr;
  };
  Json places = Json::array();
  for (const Json &lot : lots.items()) {
    if (!lot["storage"].boolean() || !lot["active"].boolean(true) || lot["lot_type"].str() == "stock0")
      continue;
    std::string label  = lot["name"].str();
    const Json *parent = find(lot["parent"].str());
    for (int depth = 0; parent && (*parent)["lot_type"].str() != "stock0" && depth < 16; ++depth) {
      label  = (*parent)["name"].str() + " › " + label;
      parent = find((*parent)["parent"].str());
    }
    Json place     = lot;
    place["label"] = label;
    places.push_back(std::move(place));
  }
  return places;
}

std::string place_label(const App &app, const std::string &id) {
  for (const Json &place : storage_places(app).items())
    if (place["id"].str() == id)
      return place["label"].str();
  return id;
}

// Item fictif pour l'apercu d'un type d'item (avant toute reception)
Json sample_item(const std::string &type, const std::string &name, bool perishable, const std::string &peremption_iso,
                 bool tear_off = false, const std::vector< std::string > &tasks = {},
                 const std::string &label_style = "qr") {
  const auto  date = Date::parse(peremption_iso);
  const Date  when = date ? *date : Date::today().plus_days(365);
  std::string code = type;
  code.resize(6, 'x');
  Json item;
  item["iid"]        = code + (perishable ? when.iso().substr(0, 4) + when.iso().substr(5, 2) + when.iso().substr(8, 2) : "00000000") + "00000001";
  item["url"]        = "https://example.com/item?id=" + compact_iid(item["iid"].str()); // taille du QR proche du reel
  item["type"]       = type;
  item["type_name"]  = name;
  item["peremption"] = perishable ? Json(when.iso()) : Json();
  item["tear_off"]   = tear_off;
  item["label_style"] = label_style;
  item["task_url"]   = "https://example.com/task?id=" + compact_iid(item["iid"].str());
  item["tasks"]      = Json::array();
  for (const std::string &task : tasks)
    item["tasks"].push_back(task);
  return item;
}

// Styles d'etiquette d'un type d'item (ItemType.label_style) : chacun a sa version liste de taches et a dechirer
const char *const kLabelStyles[]     = { "qr", "small", "barcode" };
const char *const kLabelStyleNames[] = { "QR code", "QR code réduit", "Code-barres" };

int label_style_index(const std::string &style) {
  return style == "small" ? 1 : style == "barcode" ? 2 : 0;
}

std::vector< std::string > tasks_of(const Json &item_type) {
  std::vector< std::string > tasks;
  for (const Json &task : item_type["tasks"].items())
    tasks.push_back(task.str());
  return tasks;
}

std::vector< Parameters > item_labels(const Json &items) {
  std::vector< Parameters > labels;
  const int                 count = static_cast< int >(items.size());
  for (int index = 0; index < count; ++index)
    labels.push_back(item_parameters(items[index], index + 1, count));
  return labels;
}

// Gestion de l'inventaire par le responsable : reception des commandes, types d'items, items.
class InventoryWindow final : public AppWindow {
  public:
    InventoryWindow() : AppWindow("inventory", "Inventaire", true, true) {}

    void on_open(App &app) override {
      app.refresh_item_types();
      search(app);
      load_packs(app);
      load_receptions(app);
    }

    void reset_view(App &) override {
      clear_type_form();
      show_archived_ = false;
      search_text_.clear();
      search_type_.clear();
      search_status_     = 0;
      search_stock_only_ = false;
      selected_item_     = Json();
      first_tab_         = true;
      reload_pending     = true; // recherche refaite sans filtre au prochain affichage
    }

    void draw(App &app) override {
      if (seen_packs_version_ != app.catalog.packs_version) {
        seen_packs_version_ = app.catalog.packs_version;
        load_packs(app);
      }
      if (seen_types_version_ != app.catalog.item_types_version) {
        seen_types_version_ = app.catalog.item_types_version;
        for (const Json &item_type : app.catalog.item_types.items())
          if (!editing_type_.empty() && item_type["type"].str() == editing_type_)
            edit_type(item_type);
      }
      if (!ImGui::BeginTabBar("inventory_tabs"))
        return;
      if (tab_item("Réception", std::exchange(first_tab_, false) ? ImGuiTabItemFlags_SetSelected : 0)) {
        draw_reception(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Types d'items")) {
        draw_types(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Items")) {
        draw_items(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Paquets fermés")) {
        draw_packs(app);
        ImGui::EndTabItem();
      }
      const bool select = show_receptions_;
      show_receptions_  = false;
      if (tab_item("Réceptions passées", select ? ImGuiTabItemFlags_SetSelected : 0)) {
        draw_receptions(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // ---------------------------------------------------------------------------------------------------------------
    // Reception : creation des items et impression des etiquettes en serie
    void draw_reception(App &app) {
      ImGui::TextWrapped("Réception d'une commande : choisissez le type, la date de péremption et la quantité. "
                         "Chaque item recoit un identifiant unique et sa propre étiquette.");
      ImGui::TextUnformatted("Type d'item");
      if (search_select("reception_type", without_archived(app.catalog.item_types), "type", "name", reception_type_,
                        "Tapez le nom ou le code du type…")) {
        if (const Json *type = find_type(app, reception_type_))
          quantity_ = std::max(1, (*type)["default_pack_size"].integer(1));
      }
      const Json *type       = find_type(app, reception_type_);
      const bool  perishable = type && (*type)["perissable"].boolean();
      ImGui::BeginDisabled(!perishable);
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("Péremption", "ex: 09/2026, 020926", &peremption_);
      // saisie libre : la date comprise est affichee, et remise en forme en quittant le champ
      const auto typed = parse_user_date(peremption_);
      if (ImGui::IsItemDeactivatedAfterEdit() && typed)
        peremption_ = typed->display();
      ImGui::EndDisabled();
      help_marker("Tapez la date comme elle vient : 02/09/2026, 2/9/26, 020926, ou seulement le mois "
                  "(09/2026, 09/26, 0926 = dernier jour du mois).");
      if (type && !perishable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(non périssable)");
      } else if (!peremption_.empty()) {
        ImGui::SameLine();
        if (typed)
          ImGui::TextColored(colors::green, "→ %s", typed->display().c_str());
        else
          ImGui::TextColored(colors::red, "date non reconnue");
      }
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputInt("Quantité", &quantity_);
      quantity_ = std::clamp(quantity_, 1, 10000);
      ImGui::TextUnformatted("Rangement du stock");
      search_select("reception_place", storage_places(app), "id", "label", reception_place_,
                    "Tapez le nom du rangement…", "Stock (sans rangement)");
      help_marker("Toute la réception est rangée d'un coup dans ce rangement du stock (ex : 50 compresses dans "
                  "l'armoire), sans scanner les items un par un. Sans rangement, les items restent dans le stock.");
      ImGui::Checkbox("Paquet fermé : seulement une étiquette de paquet", &sealed_);
      help_marker("Pour un paquet que l'on n'ouvre pas tout de suite (ex: boîte de compresses). Les étiquettes "
                  "individuelles seront imprimées à l'ouverture : scannez l'étiquette du paquet puis « Ouvrir ».");
      if (sealed_)
        ImGui::TextDisabled("%s : à l'ouverture du paquet.",
                            quantity_ > 1 ? ("Étiquettes des " + nb(quantity_, "item")).c_str() : "Étiquette de l'item");
      else
        ImGui::Checkbox("Imprimer les étiquettes des items (aperçu puis impression)", &print_items_);

      const auto date = parse_user_date(peremption_);
      std::string problem;
      if (!type)
        problem = "Choisissez un type d'item.";
      else if (perishable && !date)
        problem = "Date de péremption invalide.";
      else if (perishable && date && *date < app.today())
        problem = "Attention : cette date est déjà passée.";
      if (!problem.empty())
        ImGui::TextColored(colors::orange, "%s", problem.c_str());

      ImGui::BeginDisabled(!type || (perishable && !date) || creating_);
      const std::string label = sealed_ ? "Créer le paquet de " + nb(quantity_, "item") + " et son étiquette"
                              : print_items_ ? "Créer " + nb(quantity_, "item") + " et "
                                                 + pl(quantity_, "voir l'étiquette", "voir les étiquettes")
                                             : "Créer " + nb(quantity_, "item");
      if (primary_button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFrameHeight() * 1.5f)))
        create_batch(app, perishable && date ? date->iso() : "");
      ImGui::SameLine();
      const bool preview = button("Aperçu", ImVec2(-1, ImGui::GetFrameHeight() * 1.5f));
      if (preview && sealed_) {
        Json pack   = sample_item(reception_type_, (*type)["name"].str(), perishable, date ? date->iso() : "");
        pack["id"]    = "00000000";
        pack["url"]   = "https://example.com/pack?id=00000000";
        pack["count"] = quantity_;
        app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) },
                           "Exemple d'étiquette de paquet (avant création)");
      } else if (preview) {
        app.preview_labels(TemplateCategory::Item,
                           { item_parameters(sample_item(reception_type_, (*type)["name"].str(), perishable,
                                                         date ? date->iso() : "", (*type)["tear_off"].boolean(),
                                                         tasks_of(*type), (*type)["label_style"].str("qr"))) },
                           "Exemple d'étiquette (avant création)");
      }
      ImGui::EndDisabled();

      if (last_batch_.is_null())
        return;
      ImGui::SeparatorText("Dernière réception");
      const Json &items = last_batch_["items"];
      ImGui::Text("%s : %s", nb(items.size(), "item créé", "items créés").c_str(), items[0]["type_name"].str().c_str());
      if (!items[0]["location"].is_null() && !items[0]["location"].str().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(colors::green, "rangés dans %s", place_label(app, items[0]["location"].str()).c_str());
      }
      if (!last_batch_["sealed_pack"].is_null()) {
        // paquet ferme : etiquette du paquet, et fiche du paquet pour l'ouvrir plus tard
        if (button("Étiquette du paquet"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(last_batch_["sealed_pack"]) }, "Paquet");
        ImGui::SameLine();
        if (button("Fiche du paquet (ouverture)"))
          app.show_pack(last_batch_["sealed_pack"]["id"].str());
      } else if (button("Aperçu et impression des étiquettes")) {
        app.preview_labels(TemplateCategory::Item, item_labels(items), "Réception");
      }
      const bool sealed_batch = !last_batch_["sealed_pack"].is_null();
      ImGui::BeginChild("batch", ImVec2(0, 0), ImGuiChildFlags_Borders);
      for (std::size_t index = 0; index < items.size(); ++index) {
        const Json &item = items[index];
        ImGui::PushID(static_cast< int >(index));
        if (sealed_batch)
          ImGui::TextDisabled("dans le paquet");
        else if (small_button("Imprimer"))
          app.print_labels(TemplateCategory::Item,
                           { item_parameters(item, static_cast< int >(index) + 1, static_cast< int >(items.size())) },
                           "Item");
        ImGui::SameLine();
        ImGui::Text("%s", item["iid"].str().c_str());
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    void create_batch(App &app, const std::string &peremption) {
      Json body;
      body["type"]        = reception_type_;
      body["peremption"]  = peremption.empty() ? Json() : Json(peremption);
      body["count"]       = quantity_;
      body["sealed_pack"] = sealed_;
      body["location"]    = reception_place_.empty() ? Json() : Json(reception_place_);
      body["user"]        = app.user_ref();
      creating_           = true;
      // paquet ferme : seule l'etiquette du paquet, les items seront etiquetes a l'ouverture
      const bool sealed = sealed_, print_items = print_items_ && !sealed_;
      app.api.post("/api/items/batch/", body, [this, &app, sealed, print_items](const ApiResult &result) {
        creating_ = false;
        if (!result.ok) {
          app.notify("Création refusée : " + result.error, true);
          return;
        }
        last_batch_ = result.data;
        if (sealed)
          ++app.catalog.packs_version;
        app.notify(nb(result.data["items"].size(), "item créé", "items créés") + ".");
        // apercu avant impression : etiquette du paquet puis etiquettes individuelles
        std::vector< PrintJob > jobs;
        if (sealed)
          app.build_label_jobs(TemplateCategory::ItemPack, { sealed_pack_parameters(result.data["sealed_pack"]) }, "Paquet", jobs);
        if (print_items)
          app.build_label_jobs(TemplateCategory::Item, item_labels(result.data["items"]), "Réception", jobs);
        app.preview_jobs(std::move(jobs), "Réception");
        load_packs(app);
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Types d'items
    void draw_types(App &app) {
      if (button("Rafraîchir"))
        app.refresh_item_types();
      ImGui::SameLine();
      ImGui::Checkbox("Afficher les types archivés", &show_archived_);
      if (ImGui::BeginTable("types", 10,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Minimum", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Périssable", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Par paquet", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("À déchirer", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Étiquette", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Consommable", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Tâches", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Révision", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const Json &item_type : app.catalog.item_types.items()) {
          const bool archived = item_type["archived"].boolean();
          if (archived && !show_archived_)
            continue;
          const std::string code = item_type["type"].str();
          ImGui::TableNextRow();
          if (archived)
            row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          if (ImGui::Selectable(code.c_str(), editing_type_ == code, ImGuiSelectableFlags_SpanAllColumns))
            edit_type(item_type);
          ImGui::TableNextColumn();
          ImGui::Text("%s%s", item_type["name"].str().c_str(), archived ? " (archivé)" : "");
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["min_quantity"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["perissable"].boolean() ? "oui" : "non");
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["default_pack_size"].integer(1));
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["tear_off"].boolean() ? "oui" : "non");
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(kLabelStyleNames[label_style_index(item_type["label_style"].str())]);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["consumable"].boolean(true) ? "oui" : "non");
          ImGui::TableNextColumn();
          if (item_type["tasks"].size() > 0)
            ImGui::Text("%zu", item_type["tasks"].size());
          else
            ImGui::TextDisabled("-");
          ImGui::TableNextColumn();
          if (item_type["revision_months"].integer() > 0 && !item_type["consumable"].boolean(true))
            ImGui::Text("%d mois", item_type["revision_months"].integer());
          else
            ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
      }
      ImGui::SeparatorText(editing_type_.empty() ? "Nouveau type d'item" : "Modifier le type");
      ImGui::BeginDisabled(!editing_type_.empty());
      ImGui::SetNextItemWidth(120.0f);
      input_limited("Code (6 caractères)", form_code_, 6, nullptr, 0, true);
      ImGui::EndDisabled();
      help_marker("Début de l'identifiant de chaque item, ex: serphy pour du sérum phy. Non modifiable ensuite.");
      bool              code_taken = false;
      const std::string code_name  = editing_type_.empty() ? name_taken_code(app, form_code_) : std::string();
      if (!code_name.empty()) {
        ImGui::TextColored(colors::red, "Le code %s est déjà celui de « %s ».", form_code_.c_str(), code_name.c_str());
        code_taken = true;
      }
      input_limited("Nom", form_name_, 64);
      const bool name_taken = name_taken_warning(app.catalog.item_types, "type", form_name_, editing_type_,
                                                 "Le type d'item");
      ImGui::TextUnformatted("Description");
      ImGui::InputTextMultiline("##description", &form_description_, ImVec2(-FLT_MIN, 60));
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Quantité minimale en stock", &form_min_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Items par paquet (réception)", &form_pack_size_);
      ImGui::Checkbox("Périssable (date de péremption obligatoire)", &form_perishable_);
      ImGui::SetNextItemWidth(200.0f);
      ImGui::Combo("Étiquette", &form_label_style_, kLabelStyleNames, IM_ARRAYSIZE(kLabelStyleNames));
      help_marker("QR code : contient l'URL de l'item, l'appareil photo d'un téléphone ouvre sa fiche web.\n"
                  "QR code réduit : petit QR de l'identifiant seul, moins déformé sur un objet rond.\n"
                  "Code-barres : identifiant seul, pour les objets ronds (flacon, tube) : collez-le avec les barres "
                  "qui font le tour de l'objet.\n\n"
                  "Chaque style a sa version liste de tâches et à déchirer (modèles « Item - ... » des Réglages).");
      ImGui::Checkbox("Étiquette à déchirer avant utilisation", &form_tear_off_);
      help_marker("Pour un ensemble étiqueté une seule fois (ex : sachet de plusieurs sérums phy). L'étiquette porte "
                  "« Déchirer avant utilisation » : on l'arrache dès qu'on entame l'ensemble. À la vérif suivante, "
                  "l'étiquette manquante compte l'ensemble comme utilisé tout de suite : il faut remettre un "
                  "ensemble complet, les restes de l'ancien sont considérés comme perdus.");
      ImGui::Checkbox("Non consommable (matériel réutilisable)", &form_non_consumable_);
      help_marker("Ex : lecteur de glycémie, oxymètre. Un item non consommable ne s'use pas : s'il manque à la vérif "
                  "d'un lot, il a été retiré du lot : il passe « hors lot » au lieu d'être compté comme utilisé ou "
                  "perdu, et rejoint le stock quand il y est scanné ou remis en stock à la main. Sorti du stock sans lot, il reste « sorti » jusqu'à son retour. Dans un rangement du "
                  "stock, il manque comme les autres items.");
      ImGui::TextUnformatted("Tâches à la vérif (une par ligne)");
      help_marker("Ex : DAE « Voyant vert allumé », « Électrodes dans leur date ». Les items de ce type ont une "
                  "étiquette de tâches : la scanner pendant une vérif ouvre la liste à cocher, et l'item n'est "
                  "compté qu'une fois toutes ses tâches cochées et validées. Un administrateur peut scanner deux "
                  "fois de suite l'étiquette pour toutes les valider. Vide : item ordinaire.");
      ImGui::InputTextMultiline("##tasks", &form_tasks_, ImVec2(-FLT_MIN, 70));
      // l'etiquette a dechirer disparait a l'ouverture : elle ne peut pas porter la liste des taches
      const bool tear_and_tasks = form_tear_off_ && !form_task_list().empty();
      if (tear_and_tasks)
        ImGui::TextColored(colors::red, "Une étiquette à déchirer ne peut pas avoir de tâches : décochez-la ou videz "
                                        "les tâches.");
      // revision periodique : materiel non consommable seulement
      ImGui::BeginDisabled(!form_non_consumable_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Révision tous les (mois)", &form_revision_months_);
      help_marker("Ex : aspirateur de mucosités révisé tous les 3 mois. 0 : pas de révision. À la révision (bouton "
                  "« Réviser » de l'item scanné, ici ou sur sa fiche web), la prochaine date vaut par défaut la date "
                  "de la révision + ce nombre de mois, et peut être changée. Les lots affichent leurs prochaines "
                  "révisions comme les péremptions.");
      if (form_revision_months_ > 0) {
        ImGui::Checkbox("Utilisable même si la révision est en retard", &form_revision_usable_);
        help_marker("Coché : en retard, l'item reste dans son lot avec un avertissement. Décoché : il compte comme "
                    "à remplacer, comme un périmé, jusqu'à sa révision.");
        ImGui::TextUnformatted("Tâches de la révision (une par ligne)");
        ImGui::InputTextMultiline("##revision_tasks", &form_revision_tasks_, ImVec2(-FLT_MIN, 70));
      }
      ImGui::EndDisabled();
      form_revision_months_ = std::clamp(form_revision_months_, 0, 120);
      form_min_       = std::max(0, form_min_);
      form_pack_size_ = std::max(1, form_pack_size_);

      Json body;
      body["name"]              = form_name_;
      body["description"]       = form_description_;
      body["min_quantity"]      = form_min_;
      body["perissable"]        = form_perishable_;
      body["default_pack_size"] = form_pack_size_;
      body["tear_off"]          = form_tear_off_;
      body["label_style"]       = kLabelStyles[form_label_style_];
      body["consumable"]        = !form_non_consumable_;
      body["tasks"]             = Json::array();
      for (const std::string &task : form_task_list())
        body["tasks"].push_back(task);
      body["revision_months"] = form_non_consumable_ ? form_revision_months_ : 0;
      body["revision_usable"] = form_revision_usable_;
      body["revision_tasks"]  = Json::array();
      for (const std::string &task : task_lines(form_revision_tasks_))
        body["revision_tasks"].push_back(task);
      if (editing_type_.empty()) {
        ImGui::BeginDisabled(form_code_.size() != 6 || form_name_.empty() || name_taken || code_taken || tear_and_tasks);
        if (primary_button("Créer le type")) {
          body["type"] = form_code_;
          app.api.post("/api/item-types/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Type " + result.data["type"].str() + " créé.");
            app.preview_labels(TemplateCategory::Item,
                               { item_parameters(sample_item(result.data["type"].str(), result.data["name"].str(),
                                                             result.data["perissable"].boolean(), "",
                                                             result.data["tear_off"].boolean(), form_task_list(),
                                                             result.data["label_style"].str("qr"))) },
                               "Étiquette d'un item " + result.data["name"].str() + " (exemple)");
            clear_type_form();
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
      } else {
        ImGui::BeginDisabled(form_name_.empty() || name_taken || tear_and_tasks);
        if (primary_button("Enregistrer")) {
          app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [&app](const ApiResult &result) {
            app.notify(result.ok ? "Type enregistré." : result.error, !result.ok);
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (button("Nouveau type"))
          clear_type_form();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(form_code_.empty());
      if (button("Aperçu de l'étiquette"))
        app.preview_labels(TemplateCategory::Item,
                           { item_parameters(sample_item(form_code_, form_name_, form_perishable_, "", form_tear_off_,
                                                         form_task_list(), kLabelStyles[form_label_style_])) },
                           "Étiquette d'un item " + form_name_ + " (exemple)");
      ImGui::EndDisabled();
      if (!editing_type_.empty())
        draw_archive_type(app);
    }

    // Archivage : le type n'est plus propose (reception, contenu des lots, stock), ses items restent en base
    void draw_archive_type(App &app) {
      ImGui::SameLine();
      const bool archived = editing_type_archived_;
      if (archived ? button("Désarchiver le type") : button("Archiver le type…")) {
        if (archived)
          archive_type(app, false);
        else
          ImGui::OpenPopup("archive_item_type");
      }
      if (ImGui::BeginPopup("archive_item_type")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::Text("Archiver le type « %s » ?", form_name_.c_str());
        ImGui::TextWrapped("Il ne sera plus proposé à la réception ni dans le contenu des lots, et disparaît de "
                           "l'état des stocks. Les items existants restent en base. Il doit d'abord être retiré du "
                           "contenu attendu des types de lots.");
        ImGui::TextWrapped("Pour le désarchiver : cochez « Afficher les types archivés » au-dessus de la liste, "
                           "sélectionnez-le puis cliquez sur « Désarchiver le type ».");
        ImGui::PopTextWrapPos();
        if (danger_button("Archiver")) {
          archive_type(app, true);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Annuler"))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }

    void archive_type(App &app, bool archived) {
      Json body;
      body["archived"] = archived;
      app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [this, &app, archived](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify(archived ? "Type archivé." : "Type désarchivé.");
        if (archived)
          show_archived_ = true; // le type reste visible et selectionne
        edit_type(result.data);
        app.refresh_item_types();
      });
    }

    // Nom du type qui a deja ce code (vide si libre)
    static std::string name_taken_code(const App &app, const std::string &code) {
      const Json *type = find_type(app, code);
      return type ? (*type)["name"].str() : std::string();
    }

    void edit_type(const Json &item_type) {
      editing_type_     = item_type["type"].str();
      form_code_        = editing_type_;
      form_name_        = item_type["name"].str();
      form_description_ = item_type["description"].str();
      form_min_         = item_type["min_quantity"].integer();
      form_pack_size_   = item_type["default_pack_size"].integer(1);
      form_perishable_  = item_type["perissable"].boolean();
      form_tear_off_    = item_type["tear_off"].boolean();
      form_label_style_ = label_style_index(item_type["label_style"].str());
      form_non_consumable_ = !item_type["consumable"].boolean(true);
      form_tasks_.clear();
      for (const std::string &task : tasks_of(item_type))
        form_tasks_ += (form_tasks_.empty() ? "" : "\n") + task;
      form_revision_months_ = item_type["revision_months"].integer();
      form_revision_usable_ = item_type["revision_usable"].boolean(true);
      form_revision_tasks_.clear();
      for (const Json &task : item_type["revision_tasks"].items())
        form_revision_tasks_ += (form_revision_tasks_.empty() ? "" : "\n") + task.str();
      editing_type_archived_ = item_type["archived"].boolean();
    }

    void clear_type_form() {
      editing_type_.clear();
      form_code_.clear();
      form_name_.clear();
      form_description_.clear();
      form_min_        = 0;
      form_pack_size_  = 1;
      form_perishable_ = true;
      form_tear_off_   = false;
      form_label_style_ = 0;
      form_non_consumable_ = false;
      form_tasks_.clear();
      form_revision_months_ = 0;
      form_revision_usable_ = true;
      form_revision_tasks_.clear();
      editing_type_archived_ = false;
    }

    // Taches saisies, une par ligne (lignes vides ignorees)
    std::vector< std::string > form_task_list() const { return task_lines(form_tasks_); }

    static std::vector< std::string > task_lines(const std::string &text) {
      std::vector< std::string > tasks;
      std::size_t                start = 0;
      while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
          end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && std::isspace(static_cast< unsigned char >(line.back())))
          line.pop_back();
        while (!line.empty() && std::isspace(static_cast< unsigned char >(line.front())))
          line.erase(line.begin());
        if (!line.empty())
          tasks.push_back(line);
        start = end + 1;
      }
      return tasks;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Items : recherche, reimpression unitaire, suppression manuelle
    void search(App &app) {
      std::string query = "/api/items/?limit=500";
      if (!search_text_.empty())
        query += "&q=" + url_encode(search_text_);
      if (!search_type_.empty())
        query += "&type=" + url_encode(search_type_);
      static const char *statuses[] = { "", "active", "missing", "replaced", "deleted", "out", "loose" };
      if (search_status_ > 0)
        query += std::string("&status=") + statuses[search_status_];
      if (search_stock_only_)
        query += "&location=stock";
      app.api.get(query, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Items : " + result.error, true);
          return;
        }
        items_ = result.data;
        // l'item selectionne est remplace par sa version a jour
        const std::string selected = selected_item_["iid"].str();
        selected_item_             = Json();
        for (const Json &item : items_.items())
          if (!selected.empty() && item["iid"].str() == selected)
            selected_item_ = item;
      });
    }

    void draw_items(App &app) {
      bool changed = false;
      ImGui::SetNextItemWidth(220.0f);
      changed |= ImGui::InputTextWithHint("##q", "Rechercher un iid", &search_text_, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      changed |= search_select("search_type", app.catalog.item_types, "type", "name", search_type_,
                               "Filtrer par type…", "Tous les types", 240.0f);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(150.0f);
      changed |= ImGui::Combo("##status", &search_status_, "Tous statuts\0Presents\0Disparus\0Remplaces\0Supprimes\0Sortis\0Hors lot\0");
      ImGui::SameLine();
      changed |= ImGui::Checkbox("En stock", &search_stock_only_);
      ImGui::SameLine();
      if (button("Rechercher") || changed)
        search(app);

      if (ImGui::BeginTable("items", 6,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 5))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Emplacement", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Statut", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("AperçuRéimprimer").x + ImGui::GetStyle().FramePadding.x * 4
                                  + ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableHeadersRow();
        for (const Json &item : items_.items()) {
          const std::string iid = item["iid"].str();
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.35f);
          else if (item["status"].str() != "active")
            row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          if (ImGui::Selectable(iid.c_str(), selected_item_["iid"].str() == iid))
            selected_item_ = item;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["status"].str() == "loose" ? "Hors lot"
                                 : item["location"].is_null() ? "Stock" : item["location_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["status"].str().c_str());
          if (item["disabled"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "désactivé");
            ImGui::SetItemTooltip("%s", item["disabled_reason"].str().c_str());
          }
          ImGui::TableNextColumn();
          ImGui::PushID(iid.c_str());
          if (small_button("Aperçu"))
            app.preview_labels(TemplateCategory::Item, { item_parameters(item) }, "Item " + iid);
          ImGui::SameLine();
          if (small_button("Réimprimer"))
            app.print_labels(TemplateCategory::Item, { item_parameters(item) }, "Remplacement");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (selected_item_.is_null()) {
        ImGui::TextDisabled("Sélectionnez un item pour les actions avancées.");
        return;
      }
      const std::string iid = selected_item_["iid"].str();
      ImGui::Text("%s - %s - vu %s par %s", iid.c_str(), selected_item_["type_name"].str().c_str(),
                  display_on(selected_item_["last_seen"], true).c_str(), selected_item_["last_seen_by"].str().c_str());
      if (ImGui::CollapsingHeader("Actions avancées")) {
        ImGui::TextWrapped("Les items ne sont normalement pas supprimés : ils sont détectés comme disparus par les "
                           "vérifs. N'utilisez la suppression que pour un cas exceptionnel (casse, erreur de saisie).");
        if (selected_item_["status"].str() == "loose") {
          // non consommable retire de son lot : il rejoint le stock a la main (ou en le scannant a une verif du stock)
          if (button("Remettre en stock")) {
            Json body;
            body["user"]  = app.user_ref();
            body["items"] = Json::array();
            body["items"].push_back(iid);
            app.api.post("/api/items/to-stock/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item remis en stock." : result.error, !result.ok);
              selected_item_ = Json();
              search(app);
            });
          }
          ImGui::SameLine();
        }
        if (selected_item_["status"].str() == "deleted") {
          if (button("Restaurer l'item")) {
            Json body;
            body["user"] = app.user_ref();
            app.api.post("/api/items/" + url_encode(iid) + "/restore/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item restauré." : result.error, !result.ok);
              selected_item_ = Json();
              search(app);
            });
          }
        } else {
          ImGui::SetNextItemWidth(300.0f);
          input_limited("##reason", delete_reason_, 128, "Raison (obligatoire)");
          ImGui::SameLine();
          ImGui::BeginDisabled(delete_reason_.empty());
          if (confirm_button("Marquer comme supprimé", "Marquer cet item comme supprimé ?", "confirm_delete")) {
            Json body;
            body["user"]   = app.user_ref();
            body["reason"] = delete_reason_;
            app.api.post("/api/items/" + url_encode(iid) + "/delete/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item marqué comme supprimé." : result.error, !result.ok);
              delete_reason_.clear();
              selected_item_ = Json();
              search(app);
            });
          }
          ImGui::EndDisabled();
        }
        if (selected_item_["status"].str() != "deleted" && selected_item_["status"].str() != "replaced")
          draw_disable(app, iid);
      }
    }

    // Desactivation (rappel produit, non-conformite), par un admin : l'item est invalide comme un perime et doit etre
    // remplace. « Toute sa reception » : tous les items recus avec lui (paquet ouvert a la reception, rappel d'un lot).
    void send_disable(App &app, Json body, const std::string &done) {
      body["user"] = app.user_ref();
      app.api.post("/api/items/disable/", body, [this, &app, done](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify(done + " (" + nb(result.data["count"].integer(), "item modifié", "items modifiés") + ").");
        disable_reason_.clear();
        disable_reception_ = false;
        search(app);
        load_receptions(app);
      });
    }

    void draw_disable(App &app, const std::string &iid) {
      ImGui::SeparatorText("Rappel produit, non-conformité");
      if (item_reception_iid_ != iid) {
        item_reception_iid_ = iid;
        item_reception_     = Json();
        app.api.get("/api/receptions/?item=" + url_encode(iid), [this, iid](const ApiResult &result) {
          if (result.ok && result.data.size() > 0 && item_reception_iid_ == iid)
            item_reception_ = result.data[0];
        });
      }
      if (!item_reception_.is_null()) {
        ImGui::TextDisabled("Reçu %s par %s, avec %s%s.",
                            display_on(item_reception_["added"], true).c_str(),
                            item_reception_["added_by"].str("-").c_str(),
                            nb(item_reception_["count"].integer() - 1, "autre item", "autres items").c_str(),
                            item_reception_["sealed_pack"].is_null()
                              ? ""
                              : (" (paquet fermé " + item_reception_["sealed_pack"].str() + ")").c_str());
        ImGui::SameLine();
        if (small_button("Voir les réceptions de ce type")) {
          receptions_type_ = selected_item_["type"].str();
          show_receptions_ = true;
          load_receptions(app);
        }
      }
      if (!app.admin()) {
        ImGui::TextDisabled("La désactivation d'un item est réservée aux administrateurs.");
        return;
      }
      if (selected_item_["disabled"].boolean()) {
        ImGui::TextColored(colors::red, "Désactivé %s par %s : %s",
                           display_on(selected_item_["disabled_at"], true).c_str(),
                           selected_item_["disabled_by"].str("-").c_str(),
                           selected_item_["disabled_reason"].str().c_str());
        if (button("Réactiver l'item")) {
          Json body;
          body["disabled"] = false;
          body["items"]    = Json::array();
          body["items"].push_back(iid);
          send_disable(app, body, "Item réactivé");
        }
        return;
      }
      ImGui::TextWrapped("Un item désactivé est invalide comme s'il était périmé : il ne compte plus dans son lot et "
                         "doit être remplacé (le remplaçant le sort du lot à la vérif).");
      ImGui::SetNextItemWidth(300.0f);
      input_limited("##disable_reason", disable_reason_, 128, "Raison (obligatoire), ex : rappel produit");
      ImGui::SameLine();
      const int count = item_reception_.is_null() ? 1 : item_reception_["count"].integer();
      ImGui::BeginDisabled(count <= 1);
      const std::string whole = "Toute sa réception (" + std::to_string(count) + " items)";
      ImGui::Checkbox(whole.c_str(), &disable_reception_);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(disable_reason_.empty());
      const bool all = disable_reception_ && count > 1;
      if (confirm_button(all ? "Désactiver la réception" : "Désactiver l'item",
                         all ? "Désactiver tous les items reçus avec celui-ci ?" : "Désactiver cet item ?",
                         "confirm_disable")) {
        Json body;
        body["disabled"] = true;
        body["reason"]   = disable_reason_;
        if (all)
          body["reception"] = iid;
        else {
          body["items"] = Json::array();
          body["items"].push_back(iid);
        }
        send_disable(app, body, all ? "Réception désactivée" : "Item désactivé");
      }
      ImGui::EndDisabled();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Receptions passees : items recus ensemble (meme type, meme date, meme instant), pour ranger d'un coup ce qui a
    // ete recu ensemble dans un rangement du stock, ou le desactiver (paquet ouvert des la reception, rappel produit).
    void load_receptions(App &app) {
      std::string query = "/api/receptions/?limit=200";
      if (!receptions_type_.empty())
        query += "&type=" + url_encode(receptions_type_);
      app.api.get(query, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Réceptions : " + result.error, true);
          return;
        }
        receptions_ = result.data;
      });
    }

    void draw_receptions(App &app) {
      if (search_select("receptions_type", app.catalog.item_types, "type", "name", receptions_type_,
                        "Filtrer par type…", "Tous les types", 240.0f))
        load_receptions(app);
      ImGui::SameLine();
      if (button("Rafraîchir"))
        load_receptions(app);
      const float detail =
        selected_reception_.empty() ? 0 : ImGui::GetFrameHeightWithSpacing() * (app.admin() ? 6.0f : 3.5f);
      if (ImGui::BeginTable("receptions", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, -detail))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reçu le", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00/00/0000 à 00:00").x);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Présents", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Désactivés", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Par", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const Json &row : receptions_.items()) {
          const std::string first = row["item"].str();
          ImGui::TableNextRow();
          if (row["disabled"].integer() > 0)
            row_color(colors::red, row["disabled"].integer() == row["count"].integer() ? 0.35f : 0.18f);
          ImGui::TableNextColumn();
          ImGui::PushID(first.c_str());
          if (ImGui::Selectable(display_datetime(row["added"]).c_str(), selected_reception_ == first,
                                ImGuiSelectableFlags_SpanAllColumns))
            selected_reception_ = first;
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["type_name"].str().c_str());
          if (!row["sealed_pack"].is_null()) {
            ImGui::SameLine();
            ImGui::TextDisabled("paquet %s", row["sealed_pack"].str().c_str());
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["peremption"].is_null() ? "-" : display_date(row["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["count"].integer());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["active"].integer());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["disabled"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["added_by"].str().c_str());
        }
        ImGui::EndTable();
      }
      if (receptions_.size() == 0)
        ImGui::TextDisabled("Aucune réception.");
      if (selected_reception_.empty())
        return;
      const Json *selected = nullptr;
      for (const Json &row : receptions_.items())
        if (row["item"].str() == selected_reception_)
          selected = &row;
      if (!selected)
        return;
      const Json row = *selected;
      ImGui::SeparatorText((row["type_name"].str() + " reçus " + display_on(row["added"], true)).c_str());
      // toute la reception rangee d'un coup dans un rangement du stock (ceux deja mis dans un lot restent)
      const int in_stock = row["in_stock"].integer();
      search_select("store_place", storage_places(app), "id", "label", store_place_, "Tapez le nom du rangement…",
                    "Stock (sans rangement)", 300.0f);
      ImGui::SameLine();
      ImGui::BeginDisabled(in_stock == 0 || storing_);
      const std::string store_label = "Ranger " + pl(in_stock, "l'item", "les " + std::to_string(in_stock) + " items")
                                    + (store_place_.empty() ? " en vrac dans le stock" : " dans " + place_label(app, store_place_));
      if (button(store_label.c_str())) {
        Json body;
        body["reception"] = selected_reception_;
        body["location"]  = store_place_.empty() ? Json() : Json(store_place_);
        body["user"]      = app.user_ref();
        storing_          = true;
        app.api.post("/api/receptions/store/", body, [this, &app](const ApiResult &result) {
          storing_ = false;
          if (!result.ok) {
            app.notify("Rangement refusé : " + result.error, true);
            return;
          }
          app.notify(nb(result.data["count"].integer(), "item rangé", "items rangés") + ".");
          load_receptions(app);
          app.refresh_lots(true);
        });
      }
      ImGui::EndDisabled();
      help_marker("Range d'un coup tous les items de cette réception encore dans le stock. Ceux déjà mis dans un lot, "
                  "utilisés ou supprimés ne bougent pas.");
      if (in_stock < row["count"].integer())
        ImGui::TextDisabled("%s sur %d encore dans le stock.", nb(in_stock, "item").c_str(), row["count"].integer());
      if (!app.admin())
        return;
      ImGui::SetNextItemWidth(300.0f);
      input_limited("##reception_reason", disable_reason_, 128, "Raison (obligatoire), ex : rappel produit");
      ImGui::SameLine();
      ImGui::BeginDisabled(disable_reason_.empty());
      const std::string label = "Désactiver les " + row["count"].str() + " items";
      if (confirm_button(label.c_str(), "Désactiver tous les items de cette réception ?", "confirm_reception")) {
        Json body;
        body["disabled"]  = true;
        body["reason"]    = disable_reason_;
        body["reception"] = selected_reception_;
        send_disable(app, body, "Réception désactivée");
      }
      ImGui::EndDisabled();
      if (row["disabled"].integer() > 0) {
        ImGui::SameLine();
        if (button("Réactiver la réception")) {
          Json body;
          body["disabled"]  = false;
          body["reception"] = selected_reception_;
          send_disable(app, body, "Réception réactivée");
        }
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Paquets fermes : ouverture et impression des etiquettes individuelles
    void load_packs(App &app) {
      app.api.get(std::string("/api/packs/") + (show_opened_ ? "" : "?opened=0"), [this](const ApiResult &result) {
        if (result.ok)
          packs_ = result.data;
      });
    }

    void draw_packs(App &app) {
      ImGui::TextWrapped("Paquets fermés reçus en stock. À l'ouverture, les étiquettes individuelles des items "
                         "sont imprimées. Scanner l'étiquette d'un paquet ouvre directement sa fiche.");
      if (button("Rafraîchir"))
        load_packs(app);
      ImGui::SameLine();
      if (ImGui::Checkbox("Afficher les paquets déjà ouverts", &show_opened_))
        load_packs(app);
      if (!ImGui::BeginTable("packs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                             | ImGuiTableFlags_Resizable))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      // largeurs calculees sur le texte : la date d'ouverture reste lisible, le contenu prend le reste
      const float digits = ImGui::CalcTextSize("00/00/0000").x;
      ImGui::TableSetupColumn("Paquet", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000000").x);
      ImGui::TableSetupColumn("Contenu", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, digits);
      ImGui::TableSetupColumn("Ouvert le", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00/00/0000 00:00").x);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize("Ouvrir...Étiquette").x + ImGui::GetStyle().FramePadding.x * 4
                                + ImGui::GetStyle().ItemSpacing.x);
      ImGui::TableHeadersRow();
      for (const Json &pack : packs_.items()) {
        const std::string id = pack["id"].str();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(id.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d x %s", pack["count"].integer(), pack["type_name"].str().c_str());
        ImGui::SetItemTooltip("%d x %s", pack["count"].integer(), pack["type_name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(pack["peremption"]).c_str());
        ImGui::TableNextColumn();
        std::string opened = display_datetime(pack["opened"]); // "JJ/MM/AAAA à HH:MM", "aujourd'hui à HH:MM"...
        if (const std::size_t at = opened.find(" à "); at != std::string::npos)
          opened.replace(at, 4, " ");
        ImGui::TextUnformatted(opened.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(id.c_str());
        if (pack["opened"].is_null() ? primary_button("Ouvrir...") : button("Fiche"))
          app.show_pack(id);
        ImGui::SameLine();
        if (button("Étiquette"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) }, "Paquet");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Reception
    std::string reception_type_;
    std::string reception_place_; // rangement du stock de la reception (vide : le stock en vrac)
    std::string peremption_;
    int         quantity_    = 1;
    bool        sealed_      = false;
    bool        print_items_ = true;
    bool        creating_    = false;
    Json        last_batch_;
    // Types
    std::string editing_type_;
    int         seen_types_version_ = -1;
    std::string form_code_;
    std::string form_name_;
    std::string form_description_;
    int         form_min_        = 0;
    int         form_pack_size_  = 1;
    bool        form_perishable_ = true;
    bool        form_tear_off_   = false;
    int         form_label_style_ = 0; // index dans kLabelStyles
    bool        form_non_consumable_ = false;
    std::string form_tasks_;
    int         form_revision_months_ = 0;
    bool        form_revision_usable_ = true;
    std::string form_revision_tasks_;
    bool        editing_type_archived_ = false;
    bool        show_archived_   = false;
    // Items
    std::string search_text_;
    std::string search_type_;
    int         search_status_     = 0;
    bool        search_stock_only_ = false;
    Json        items_ = Json::array();
    Json        selected_item_;
    std::string delete_reason_;
    std::string disable_reason_;
    bool        disable_reception_ = false;
    std::string item_reception_iid_; // reception de l'item selectionne (actions avancees)
    Json        item_reception_;
    Json        receptions_ = Json::array();
    std::string receptions_type_;
    std::string selected_reception_;
    std::string store_place_; // rangement ou ranger une reception passee
    bool        storing_ = false;
    bool        show_receptions_ = false; // onglet Receptions a selectionner (bouton d'un item)
    bool        first_tab_       = false; // onglet Reception a selectionner (vue par defaut)
    // Paquets
    Json packs_       = Json::array();
    bool show_opened_        = false;
    int  seen_packs_version_ = 0;
};

} // namespace

std::unique_ptr< AppWindow > make_inventory_window() {
  return std::make_unique< InventoryWindow >();
}

} // namespace qrprotec
