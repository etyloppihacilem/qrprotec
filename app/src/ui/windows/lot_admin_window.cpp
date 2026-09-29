/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lot_admin_window.cpp
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

struct RequirementRow {
    std::string type;
    int         quantity = 1;
};

// Creation et agrement des lots, types de lots et edition des etiquettes publique / privee.
class LotAdminWindow final : public AppWindow {
  public:
    LotAdminWindow() : AppWindow("lot_admin", "Gestion des lots", true, true) {}

    void on_open(App &app) override {
      app.refresh_lots();
      app.refresh_lot_types();
      app.refresh_item_types();
    }

    void draw(App &app) override {
      // listes rechargees : on recharge aussi le lot et le type de lot affiches
      if (seen_lots_version_ != app.catalog.lots_version) {
        seen_lots_version_ = app.catalog.lots_version;
        if (!selected_lot_.empty())
          select_lot(app, selected_lot_);
      }
      if (seen_lot_types_version_ != app.catalog.lot_types_version) {
        seen_lot_types_version_ = app.catalog.lot_types_version;
        for (const Json &lot_type : app.catalog.lot_types.items())
          if (!editing_lot_type_.empty() && lot_type["type"].str() == editing_lot_type_)
            edit_lot_type(lot_type);
      }
      if (!ImGui::BeginTabBar("lot_tabs"))
        return;
      if (ImGui::BeginTabItem("Lots")) {
        draw_lots(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Types de lots")) {
        draw_lot_types(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // ---------------------------------------------------------------------------------------------------------------
    void draw_lots(App &app) {
      ImGui::BeginChild("lots_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0), ImGuiChildFlags_Borders);
      if (ImGui::Button("Rafraîchir")) {
        app.refresh_lots();
        app.refresh_lot_types();
      }
      ImGui::SameLine();
      if (ImGui::Button("Nouveau lot"))
        selected_lot_.clear();
      for (const Json &lot : app.catalog.lots.items()) {
        const std::string id    = lot["id"].str();
        const std::string label = lot["name"].str() + "  (" + lot["lot_type_name"].str() + ")##" + id;
        if (ImGui::Selectable(label.c_str(), selected_lot_ == id))
          select_lot(app, id);
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("lot_details", ImVec2(0, 0), ImGuiChildFlags_Borders);
      if (selected_lot_.empty())
        draw_new_lot(app);
      else
        draw_lot_details(app);
      ImGui::EndChild();
    }

    void draw_new_lot(App &app) {
      ImGui::SeparatorText("Nouveau lot");
      json_combo("Type de lot", app.catalog.lot_types, "type", "name", new_lot_type_);
      ImGui::InputText("Nom", &new_lot_name_);
      ImGui::InputText("Nom court (16 car.)", &new_lot_short_);
      ImGui::Checkbox("Voir les étiquettes publique et privée après création", &print_new_);
      ImGui::BeginDisabled(new_lot_type_.empty() || new_lot_name_.empty());
      if (primary_button("Créer le lot")) {
        Json body;
        body["lot_type"]   = new_lot_type_;
        body["name"]       = new_lot_name_;
        body["name_short"] = new_lot_short_;
        body["user"]       = app.user_ref();
        const bool print   = print_new_;
        app.api.post("/api/lots/", body, [this, &app, print](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          app.notify("Lot " + result.data["name"].str() + " créé.");
          if (print)
            preview_lot(app, result.data);
          new_lot_name_.clear();
          new_lot_short_.clear();
          selected_lot_ = result.data["id"].str();
          lot_          = result.data;
          app.refresh_lots();
        });
      }
      ImGui::EndDisabled();
      ImGui::Spacing();
      ImGui::TextWrapped("Pour remplir un lot : scannez les items puis l'étiquette privée du lot, et cliquez sur "
                         "\"Ajouter au lot\" dans la pile de scans.");
    }

    void select_lot(App &app, const std::string &id) {
      selected_lot_ = id;
      lot_          = Json();
      app.api.get("/api/lots/" + url_encode(id) + "/", [this, &app, id](const ApiResult &result) {
        if (selected_lot_ != id)
          return;
        if (result.ok) {
          lot_        = result.data;
          edit_name_  = lot_["name"].str();
          edit_short_ = lot_["name_short"].str();
        } else {
          app.notify(result.error, true);
        }
      });
    }

    // apercu des deux etiquettes du lot (publique puis privee), impression depuis la fenetre d'apercu
    void preview_lot(App &app, const Json &lot) {
      const Parameters        parameters = lot_parameters(lot);
      std::vector< PrintJob > jobs;
      app.build_label_jobs(TemplateCategory::LotPublic, { parameters }, "Lot " + lot["name"].str() + " (publique)", jobs);
      app.build_label_jobs(TemplateCategory::LotPrivate, { parameters }, "Lot " + lot["name"].str() + " (privée)", jobs);
      app.preview_jobs(std::move(jobs), "Lot " + lot["name"].str());
    }

    void print_lot(App &app, const Json &lot, bool public_label, bool private_label) {
      const Parameters parameters = lot_parameters(lot);
      if (public_label)
        app.print_labels(TemplateCategory::LotPublic, { parameters }, "Lot " + lot["name"].str() + " (publique)");
      if (private_label)
        app.print_labels(TemplateCategory::LotPrivate, { parameters }, "Lot " + lot["name"].str() + " (privée)");
    }

    void update_lot(App &app, const Json &body) {
      app.api.patch("/api/lots/" + url_encode(selected_lot_) + "/update/", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        lot_ = result.data;
        app.notify("Lot enregistre.");
        app.refresh_lots();
      });
    }

    void draw_lot_details(App &app) {
      if (lot_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        return;
      }
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
      ImGui::TextUnformatted(lot_["name"].str().c_str());
      ImGui::PopFont();
      ImGui::TextDisabled("%s - %s - version %d", lot_["id"].str().c_str(), lot_["lot_type_name"].str().c_str(),
                          lot_["version"].integer());
      ImGui::Text("Dernière vérif : %s%s", display_datetime(lot_["last_verif"]).c_str(),
                  lot_["last_verif_by"].str().empty() ? "" : (" par " + lot_["last_verif_by"].str()).c_str());
      const LotStatus status = lot_status(lot_);
      status_banner(status == LotStatus::Verified ? "✔ Lot vérifié et complet"
                    : status == LotStatus::Never  ? "✘ Lot jamais vérifié"
                                                  : "✘ Lot incomplet",
                    lot_status_color(status), 1.1f);

      ImGui::SeparatorText("Étiquettes");
      if (primary_button("Aperçu des étiquettes publique et privée", ImVec2(-FLT_MIN, 0)))
        preview_lot(app, lot_);
      ImGui::TextDisabled("Impression directe :");
      ImGui::SameLine();
      if (ImGui::SmallButton("publique"))
        print_lot(app, lot_, true, false);
      ImGui::SameLine();
      if (ImGui::SmallButton("privée"))
        print_lot(app, lot_, false, true);
      ImGui::SameLine();
      if (ImGui::SmallButton("les deux"))
        print_lot(app, lot_, true, true);
      ImGui::TextDisabled("Clé valable jusqu'au %s", display_date(lot_["verif_key_expires"]).c_str());
      if (confirm_button("Régénérer la clé",
                         "L'ancienne étiquette privée ne fonctionnera plus. Continuer ?", "rotate_key")) {
        app.api.post("/api/lots/" + url_encode(selected_lot_) + "/rotate-key/", Json::object(),
                     [this, &app](const ApiResult &result) {
                       if (!result.ok) {
                         app.notify(result.error, true);
                         return;
                       }
                       lot_ = result.data;
                       app.notify("Nouvelle clé générée : imprimez la nouvelle étiquette privée.");
                     });
      }

      ImGui::SeparatorText("Informations");
      ImGui::InputText("Nom", &edit_name_);
      ImGui::InputText("Nom court", &edit_short_);
      if (ImGui::Button("Enregistrer")) {
        Json body;
        body["name"]       = edit_name_;
        body["name_short"] = edit_short_;
        update_lot(app, body);
      }
      ImGui::SameLine();
      const bool active = lot_["active"].boolean(true);
      if (ImGui::Button(active ? "Archiver le lot" : "Réactiver le lot")) {
        Json body;
        body["active"] = !active;
        update_lot(app, body);
      }
      ImGui::SameLine();
      if (ImGui::Button("Lancer une vérif"))
        app.start_verif(selected_lot_, lot_["verif_key"].str());

      ImGui::SeparatorText("Contenu");
      for (const Json &row : lot_["requirements"].items()) {
        ImGui::TextUnformatted(row["type_name"].str().c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
        stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(-FLT_MIN, 0));
      }
      ImGui::Text("%d item(s), dont %d périmé(s).", lot_["item_count"].integer(), lot_["expired_count"].integer());
      if (ImGui::BeginTable("lot_items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const Json &item : lot_["items"].items()) {
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.4f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", item["iid"].str().c_str());
        }
        ImGui::EndTable();
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    void draw_lot_types(App &app) {
      ImGui::BeginChild("types_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.35f, 0), ImGuiChildFlags_Borders);
      if (ImGui::Button("Nouveau type"))
        edit_lot_type(Json());
      for (const Json &lot_type : app.catalog.lot_types.items()) {
        const std::string code  = lot_type["type"].str();
        const std::string label = lot_type["name"].str() + " (" + code + ")";
        if (ImGui::Selectable(label.c_str(), editing_lot_type_ == code))
          edit_lot_type(lot_type);
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("type_form", ImVec2(0, 0), ImGuiChildFlags_Borders);
      ImGui::SeparatorText(editing_lot_type_.empty() ? "Nouveau type de lot" : "Type de lot");
      ImGui::BeginDisabled(!editing_lot_type_.empty());
      ImGui::SetNextItemWidth(120.0f);
      ImGui::InputText("Code (6 caractères)", &type_code_, ImGuiInputTextFlags_CharsNoBlank);
      ImGui::EndDisabled();
      ImGui::InputText("Nom", &type_name_);
      ImGui::TextUnformatted("Description");
      ImGui::InputTextMultiline("##description", &type_description_, ImVec2(-FLT_MIN, 50));
      if (editing_lot_type_.empty()) {
        ImGui::BeginDisabled(type_code_.size() != 6 || type_name_.empty());
        if (primary_button("Créer le type de lot")) {
          Json body;
          body["type"]        = type_code_;
          body["name"]        = type_name_;
          body["description"] = type_description_;
          body["user"]        = app.user_ref();
          app.api.post("/api/lot-types/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Type de lot créé : ajoutez maintenant son contenu attendu.");
            edit_lot_type(result.data);
            app.refresh_lot_types();
          });
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        return;
      }
      if (ImGui::Button("Enregistrer le nom")) {
        Json body;
        body["name"]        = type_name_;
        body["description"] = type_description_;
        app.api.patch("/api/lot-types/" + url_encode(editing_lot_type_) + "/", body, [&app](const ApiResult &result) {
          app.notify(result.ok ? "Type de lot enregistre." : result.error, !result.ok);
          app.refresh_lot_types();
        });
      }

      ImGui::SeparatorText("Contenu attendu");
      int remove = -1;
      for (std::size_t index = 0; index < requirements_.size(); ++index) {
        RequirementRow &row = requirements_[index];
        ImGui::PushID(static_cast< int >(index));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        json_combo("##type", app.catalog.item_types, "type", "name", row.type);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("##qty", &row.quantity);
        row.quantity = std::max(1, row.quantity);
        ImGui::SameLine();
        if (ImGui::SmallButton("Retirer"))
          remove = static_cast< int >(index);
        ImGui::PopID();
      }
      if (remove >= 0)
        requirements_.erase(requirements_.begin() + remove);
      if (ImGui::Button("+ Ajouter une ligne"))
        requirements_.push_back({});
      ImGui::SameLine();
      if (primary_button("Enregistrer le contenu")) {
        Json body;
        body["requirements"] = Json::array();
        for (const RequirementRow &row : requirements_) {
          if (row.type.empty())
            continue;
          Json entry;
          entry["type"]     = row.type;
          entry["quantity"] = row.quantity;
          body["requirements"].push_back(entry);
        }
        app.api.put("/api/lot-types/" + url_encode(editing_lot_type_) + "/requirements/", body,
                    [this, &app](const ApiResult &result) {
                      if (!result.ok) {
                        app.notify(result.error, true);
                        return;
                      }
                      app.notify("Contenu enregistre (version " + result.data["version"].str() + ").");
                      edit_lot_type(result.data);
                      app.refresh_lot_types();
                    });
      }
      ImGui::EndChild();
    }

    void edit_lot_type(const Json &lot_type) {
      editing_lot_type_ = lot_type["type"].str();
      type_code_        = editing_lot_type_;
      type_name_        = lot_type["name"].str();
      type_description_ = lot_type["description"].str();
      requirements_.clear();
      for (const Json &row : lot_type["requirements"].items())
        requirements_.push_back({ row["type"].str(), row["quantity"].integer(1) });
    }

    // Lots
    std::string selected_lot_;
    int         seen_lots_version_      = -1;
    int         seen_lot_types_version_ = -1;
    Json        lot_;
    std::string edit_name_;
    std::string edit_short_;
    std::string new_lot_type_;
    std::string new_lot_name_;
    std::string new_lot_short_;
    bool        print_new_ = true;
    // Types de lots
    std::string                   editing_lot_type_;
    std::string                   type_code_;
    std::string                   type_name_;
    std::string                   type_description_;
    std::vector< RequirementRow > requirements_;
};

} // namespace

std::unique_ptr< AppWindow > make_lot_admin_window() {
  return std::make_unique< LotAdminWindow >();
}

} // namespace qrprotec
