/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inventory_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>

namespace qrprotec {

namespace {

const Json *find_type(const App &app, const std::string &type) {
  for (const Json &item_type : app.catalog.item_types.items())
    if (item_type["type"].str() == type)
      return &item_type;
  return nullptr;
}

// Item fictif pour l'apercu d'un type d'item (avant toute reception)
Json sample_item(const std::string &type, const std::string &name, bool perishable, const std::string &peremption_iso,
                 bool tear_off = false) {
  const auto  date = Date::parse(peremption_iso);
  const Date  when = date ? *date : Date::today().plus_days(365);
  std::string code = type;
  code.resize(6, 'x');
  Json item;
  item["iid"]        = code + (perishable ? when.iso().substr(0, 4) + when.iso().substr(5, 2) + when.iso().substr(8, 2) : "00000000") + "00000001";
  item["type"]       = type;
  item["type_name"]  = name;
  item["peremption"] = perishable ? Json(when.iso()) : Json();
  item["tear_off"]   = tear_off;
  return item;
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
      if (ImGui::BeginTabItem("Réception")) {
        draw_reception(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Types d'items")) {
        draw_types(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Items")) {
        draw_items(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Paquets fermés")) {
        draw_packs(app);
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
      ImGui::Checkbox("Paquet fermé : seulement une étiquette de paquet", &sealed_);
      help_marker("Pour un paquet que l'on n'ouvre pas tout de suite (ex: boîte de compresses). Les étiquettes "
                  "individuelles seront imprimées à l'ouverture : scannez l'étiquette du paquet puis « Ouvrir ».");
      if (sealed_)
        ImGui::TextDisabled("Étiquettes des %d item(s) : à l'ouverture du paquet.", quantity_);
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
      const std::string label = sealed_ ? "Créer le paquet de " + std::to_string(quantity_) + " item(s) et son étiquette"
                              : print_items_ ? "Créer " + std::to_string(quantity_) + " item(s) et voir les étiquettes"
                                             : "Créer " + std::to_string(quantity_) + " item(s)";
      if (primary_button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFrameHeight() * 1.5f)))
        create_batch(app, perishable && date ? date->iso() : "");
      ImGui::SameLine();
      const bool preview = ImGui::Button("Aperçu", ImVec2(-1, ImGui::GetFrameHeight() * 1.5f));
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
                                                         date ? date->iso() : "", (*type)["tear_off"].boolean())) },
                           "Exemple d'étiquette (avant création)");
      }
      ImGui::EndDisabled();

      if (last_batch_.is_null())
        return;
      ImGui::SeparatorText("Dernière réception");
      const Json &items = last_batch_["items"];
      ImGui::Text("%zu item(s) créés : %s", items.size(), items[0]["type_name"].str().c_str());
      if (!last_batch_["sealed_pack"].is_null()) {
        // paquet ferme : etiquette du paquet, et fiche du paquet pour l'ouvrir plus tard
        if (ImGui::Button("Étiquette du paquet"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(last_batch_["sealed_pack"]) }, "Paquet");
        ImGui::SameLine();
        if (ImGui::Button("Fiche du paquet (ouverture)"))
          app.show_pack(last_batch_["sealed_pack"]["id"].str());
      } else if (ImGui::Button("Aperçu et impression des étiquettes")) {
        app.preview_labels(TemplateCategory::Item, item_labels(items), "Réception");
      }
      const bool sealed_batch = !last_batch_["sealed_pack"].is_null();
      ImGui::BeginChild("batch", ImVec2(0, 0), ImGuiChildFlags_Borders);
      for (std::size_t index = 0; index < items.size(); ++index) {
        const Json &item = items[index];
        ImGui::PushID(static_cast< int >(index));
        if (sealed_batch)
          ImGui::TextDisabled("dans le paquet");
        else if (ImGui::SmallButton("Imprimer"))
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
        app.notify(std::to_string(result.data["items"].size()) + " item(s) créé(s).");
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
      if (ImGui::Button("Rafraîchir"))
        app.refresh_item_types();
      ImGui::SameLine();
      ImGui::Checkbox("Afficher les types archivés", &show_archived_);
      if (ImGui::BeginTable("types", 6,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Minimum", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Périssable", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Par paquet", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("À déchirer", ImGuiTableColumnFlags_WidthFixed, 90.0f);
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
      ImGui::Checkbox("Étiquette à déchirer avant utilisation", &form_tear_off_);
      help_marker("Pour un ensemble étiqueté une seule fois (ex : sachet de plusieurs sérums phy). L'étiquette porte "
                  "« Déchirer avant utilisation » : on l'arrache dès qu'on entame l'ensemble. À la vérif suivante, "
                  "l'étiquette manquante compte l'ensemble comme utilisé tout de suite : il faut remettre un "
                  "ensemble complet, les restes de l'ancien sont considérés comme perdus.");
      form_min_       = std::max(0, form_min_);
      form_pack_size_ = std::max(1, form_pack_size_);

      Json body;
      body["name"]              = form_name_;
      body["description"]       = form_description_;
      body["min_quantity"]      = form_min_;
      body["perissable"]        = form_perishable_;
      body["default_pack_size"] = form_pack_size_;
      body["tear_off"]          = form_tear_off_;
      if (editing_type_.empty()) {
        ImGui::BeginDisabled(form_code_.size() != 6 || form_name_.empty() || name_taken || code_taken);
        if (primary_button("Créer le type")) {
          body["type"] = form_code_;
          app.api.post("/api/item-types/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Type " + result.data["type"].str() + " créé.");
            clear_type_form();
            app.preview_labels(TemplateCategory::Item,
                               { item_parameters(sample_item(result.data["type"].str(), result.data["name"].str(),
                                                             result.data["perissable"].boolean(), "",
                                                             result.data["tear_off"].boolean())) },
                               "Étiquette d'un item " + result.data["name"].str() + " (exemple)");
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
      } else {
        ImGui::BeginDisabled(form_name_.empty() || name_taken);
        if (primary_button("Enregistrer")) {
          app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [&app](const ApiResult &result) {
            app.notify(result.ok ? "Type enregistré." : result.error, !result.ok);
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Nouveau type"))
          clear_type_form();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(form_code_.empty());
      if (ImGui::Button("Aperçu de l'étiquette"))
        app.preview_labels(TemplateCategory::Item,
                           { item_parameters(sample_item(form_code_, form_name_, form_perishable_, "", form_tear_off_)) },
                           "Étiquette d'un item " + form_name_ + " (exemple)");
      ImGui::EndDisabled();
      if (!editing_type_.empty())
        draw_archive_type(app);
    }

    // Archivage : le type n'est plus propose (reception, contenu des lots, stock), ses items restent en base
    void draw_archive_type(App &app) {
      ImGui::SameLine();
      const bool archived = editing_type_archived_;
      if (archived ? ImGui::Button("Désarchiver le type") : ImGui::Button("Archiver le type…")) {
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
        if (ImGui::Button("Annuler"))
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
      editing_type_archived_ = false;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Items : recherche, reimpression unitaire, suppression manuelle
    void search(App &app) {
      std::string query = "/api/items/?limit=500";
      if (!search_text_.empty())
        query += "&q=" + url_encode(search_text_);
      if (!search_type_.empty())
        query += "&type=" + url_encode(search_type_);
      static const char *statuses[] = { "", "active", "missing", "replaced", "deleted" };
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
      changed |= ImGui::Combo("##status", &search_status_, "Tous statuts\0Presents\0Disparus\0Remplaces\0Supprimes\0");
      ImGui::SameLine();
      changed |= ImGui::Checkbox("En stock", &search_stock_only_);
      ImGui::SameLine();
      if (ImGui::Button("Rechercher") || changed)
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
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
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
          ImGui::TextUnformatted(item["location"].is_null() ? "Stock" : item["location_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["status"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::PushID(iid.c_str());
          if (ImGui::SmallButton("Réimprimer"))
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
      ImGui::Text("%s - %s - vu le %s par %s", iid.c_str(), selected_item_["type_name"].str().c_str(),
                  display_datetime(selected_item_["last_seen"]).c_str(), selected_item_["last_seen_by"].str().c_str());
      if (ImGui::CollapsingHeader("Actions avancées")) {
        ImGui::TextWrapped("Les items ne sont normalement pas supprimés : ils sont détectés comme disparus par les "
                           "vérifs. N'utilisez la suppression que pour un cas exceptionnel (casse, erreur de saisie).");
        if (selected_item_["status"].str() == "deleted") {
          if (ImGui::Button("Restaurer l'item")) {
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
      if (ImGui::Button("Rafraîchir"))
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
        std::string opened = display_datetime(pack["opened"]); // "JJ/MM/AAAA à HH:MM"
        if (const std::size_t at = opened.find(" à "); at != std::string::npos)
          opened.replace(at, 4, " ");
        ImGui::TextUnformatted(opened.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(id.c_str());
        if (pack["opened"].is_null() ? primary_button("Ouvrir...") : ImGui::Button("Fiche"))
          app.show_pack(id);
        ImGui::SameLine();
        if (ImGui::Button("Étiquette"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) }, "Paquet");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Reception
    std::string reception_type_;
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
