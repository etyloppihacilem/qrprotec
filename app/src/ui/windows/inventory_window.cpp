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
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("##typefilter", "Rechercher un type", &type_filter_);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(-1.0f);
      if (json_combo("##type", app.catalog.item_types, "type", "name", reception_type_, type_filter_.c_str())) {
        if (const Json *type = find_type(app, reception_type_))
          quantity_ = std::max(1, (*type)["default_pack_size"].integer(1));
      }
      const Json *type       = find_type(app, reception_type_);
      const bool  perishable = type && (*type)["perissable"].boolean();
      ImGui::BeginDisabled(!perishable);
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("Péremption", "JJ/MM/AAAA", &peremption_);
      ImGui::EndDisabled();
      if (type && !perishable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(non périssable)");
      }
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputInt("Quantité", &quantity_);
      quantity_ = std::clamp(quantity_, 1, 10000);
      ImGui::Checkbox("Paquet fermé : imprimer une étiquette de paquet", &sealed_);
      help_marker("Pour un paquet que l'on n'ouvre pas tout de suite (ex: boîte de compresses). Les étiquettes "
                  "individuelles seront imprimées à l'ouverture (onglet Paquets fermés).");
      ImGui::Checkbox("Imprimer les étiquettes individuelles maintenant", &print_items_);

      const auto date = Date::parse(peremption_);
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
      const std::string label = "Créer " + std::to_string(quantity_) + " item(s) et imprimer";
      if (primary_button(label.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.5f)))
        create_batch(app, perishable && date ? date->iso() : "");
      ImGui::EndDisabled();

      if (last_batch_.is_null())
        return;
      ImGui::SeparatorText("Dernière réception");
      const Json &items = last_batch_["items"];
      ImGui::Text("%zu item(s) créés : %s", items.size(), items[0]["type_name"].str().c_str());
      if (ImGui::Button("Réimprimer toutes les étiquettes"))
        app.print_labels(TemplateCategory::Item, item_labels(items), "Réception");
      if (!last_batch_["sealed_pack"].is_null()) {
        ImGui::SameLine();
        if (ImGui::Button("Réimprimer l'étiquette du paquet"))
          app.print_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(last_batch_["sealed_pack"]) }, "Paquet");
      }
      ImGui::BeginChild("batch", ImVec2(0, 0), ImGuiChildFlags_Borders);
      for (std::size_t index = 0; index < items.size(); ++index) {
        const Json &item = items[index];
        ImGui::PushID(static_cast< int >(index));
        if (ImGui::SmallButton("Imprimer"))
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
      const bool sealed = sealed_, print_items = print_items_;
      app.api.post("/api/items/batch/", body, [this, &app, sealed, print_items](const ApiResult &result) {
        creating_ = false;
        if (!result.ok) {
          app.notify("Création refusée : " + result.error, true);
          return;
        }
        last_batch_ = result.data;
        app.notify(std::to_string(result.data["items"].size()) + " item(s) créé(s).");
        if (sealed)
          app.print_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(result.data["sealed_pack"]) }, "Paquet");
        if (print_items)
          app.print_labels(TemplateCategory::Item, item_labels(result.data["items"]), "Réception");
        load_packs(app);
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Types d'items
    void draw_types(App &app) {
      if (ImGui::Button("Rafraîchir"))
        app.refresh_item_types();
      if (ImGui::BeginTable("types", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Minimum", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Périssable", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Par paquet", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();
        for (const Json &item_type : app.catalog.item_types.items()) {
          const std::string code = item_type["type"].str();
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::Selectable(code.c_str(), editing_type_ == code, ImGuiSelectableFlags_SpanAllColumns))
            edit_type(item_type);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["min_quantity"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["perissable"].boolean() ? "oui" : "non");
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["default_pack_size"].integer(1));
        }
        ImGui::EndTable();
      }
      ImGui::SeparatorText(editing_type_.empty() ? "Nouveau type d'item" : "Modifier le type");
      ImGui::BeginDisabled(!editing_type_.empty());
      ImGui::SetNextItemWidth(120.0f);
      ImGui::InputText("Code (6 caractères)", &form_code_, ImGuiInputTextFlags_CharsNoBlank);
      ImGui::EndDisabled();
      help_marker("Début de l'identifiant de chaque item, ex: serphy pour du sérum phy. Non modifiable ensuite.");
      ImGui::InputText("Nom", &form_name_);
      ImGui::InputTextMultiline("Description", &form_description_, ImVec2(-1, 60));
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Quantité minimale en stock", &form_min_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Items par paquet (réception)", &form_pack_size_);
      ImGui::Checkbox("Périssable (date de péremption obligatoire)", &form_perishable_);
      form_min_       = std::max(0, form_min_);
      form_pack_size_ = std::max(1, form_pack_size_);

      Json body;
      body["name"]              = form_name_;
      body["description"]       = form_description_;
      body["min_quantity"]      = form_min_;
      body["perissable"]        = form_perishable_;
      body["default_pack_size"] = form_pack_size_;
      if (editing_type_.empty()) {
        ImGui::BeginDisabled(form_code_.size() != 6 || form_name_.empty());
        if (primary_button("Créer le type")) {
          body["type"] = form_code_;
          app.api.post("/api/item-types/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Type " + result.data["type"].str() + " créé.");
            clear_type_form();
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
      } else {
        if (primary_button("Enregistrer")) {
          app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [&app](const ApiResult &result) {
            app.notify(result.ok ? "Type enregistre." : result.error, !result.ok);
            app.refresh_item_types();
          });
        }
        ImGui::SameLine();
        if (ImGui::Button("Nouveau type"))
          clear_type_form();
      }
    }

    void edit_type(const Json &item_type) {
      editing_type_     = item_type["type"].str();
      form_code_        = editing_type_;
      form_name_        = item_type["name"].str();
      form_description_ = item_type["description"].str();
      form_min_         = item_type["min_quantity"].integer();
      form_pack_size_   = item_type["default_pack_size"].integer(1);
      form_perishable_  = item_type["perissable"].boolean();
    }

    void clear_type_form() {
      editing_type_.clear();
      form_code_.clear();
      form_name_.clear();
      form_description_.clear();
      form_min_        = 0;
      form_pack_size_  = 1;
      form_perishable_ = true;
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
        if (result.ok)
          items_ = result.data;
        else
          app.notify("Items : " + result.error, true);
      });
    }

    void draw_items(App &app) {
      bool changed = false;
      ImGui::SetNextItemWidth(220.0f);
      changed |= ImGui::InputTextWithHint("##q", "Rechercher un iid", &search_text_, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(220.0f);
      Json types = Json::array();
      Json all;
      all["type"] = "";
      all["name"] = "Tous les types";
      types.push_back(all);
      for (const Json &item_type : app.catalog.item_types.items())
        types.push_back(item_type);
      changed |= json_combo("##stype", types, "type", "name", search_type_);
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
          ImGui::InputTextWithHint("##reason", "Raison (obligatoire)", &delete_reason_);
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
                         "sont imprimées. Vous pouvez aussi scanner l'étiquette d'un paquet dans la pile.");
      if (ImGui::Button("Rafraîchir"))
        load_packs(app);
      ImGui::SameLine();
      if (ImGui::Checkbox("Afficher les paquets déjà ouverts", &show_opened_))
        load_packs(app);
      if (!ImGui::BeginTable("packs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Paquet", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Contenu", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableSetupColumn("Ouvert", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 280.0f);
      ImGui::TableHeadersRow();
      for (const Json &pack : packs_.items()) {
        const std::string id = pack["id"].str();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(id.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d x %s", pack["count"].integer(), pack["type_name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(pack["peremption"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(pack["opened"]).c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(id.c_str());
        if (primary_button("Ouvrir et imprimer"))
          open_pack(app, id);
        ImGui::SameLine();
        if (ImGui::Button("Étiquette paquet"))
          app.print_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) }, "Paquet");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    void open_pack(App &app, const std::string &id) {
      Json body;
      body["user"] = app.user_ref();
      app.api.post("/api/packs/" + url_encode(id) + "/open/", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.print_labels(TemplateCategory::Item, item_labels(result.data["items"]), "Paquet ouvert");
        load_packs(app);
      });
    }

    // Reception
    std::string reception_type_;
    std::string type_filter_;
    std::string peremption_;
    int         quantity_    = 1;
    bool        sealed_      = false;
    bool        print_items_ = true;
    bool        creating_    = false;
    Json        last_batch_;
    // Types
    std::string editing_type_;
    std::string form_code_;
    std::string form_name_;
    std::string form_description_;
    int         form_min_        = 0;
    int         form_pack_size_  = 1;
    bool        form_perishable_ = true;
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
    bool show_opened_ = false;
};

} // namespace

std::unique_ptr< AppWindow > make_inventory_window() {
  return std::make_unique< InventoryWindow >();
}

} // namespace qrprotec
