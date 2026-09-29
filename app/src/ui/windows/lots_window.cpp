/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lots_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <cctype>

namespace qrprotec {

namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast< char >(std::tolower(c)); });
  return value;
}

// Vue publique des lots : ce qui est perime, ce qui manque, et lancement d'une verif.
class LotsWindow final : public AppWindow {
  public:
    LotsWindow() : AppWindow("lots", "Lots", false, true) {}

    void on_open(App &app) override { app.refresh_lots(); }

    void draw(App &app) override {
      if (ImGui::Button("Rafraîchir"))
        app.refresh_lots();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(250.0f);
      ImGui::InputTextWithHint("##filter", "Filtrer", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("Seulement les lots à traiter", &only_problems_);
      if (app.catalog.loading_lots) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }

      const float details_width = selected_.empty() ? 0.0f : ImGui::GetContentRegionAvail().x * 0.45f;
      ImGui::BeginChild("list", ImVec2(selected_.empty() ? 0.0f : -details_width, 0));
      draw_table(app);
      ImGui::EndChild();
      if (!selected_.empty()) {
        ImGui::SameLine();
        ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
        draw_details(app);
        ImGui::EndChild();
      }
    }

  private:
    void draw_table(App &app) {
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      if (!ImGui::BeginTable("lots", 5, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Lot", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
      ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Complet", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Dernière vérif", ImGuiTableColumnFlags_WidthFixed, 150.0f);
      ImGui::TableHeadersRow();
      const std::string needle = lower(filter_);
      for (const Json &lot : app.catalog.lots.items()) {
        const std::string id   = lot["id"].str();
        const std::string name = lot["name"].str();
        const int         expired = lot["expired_count"].integer();
        const int         soon    = lot["expiring_soon_count"].integer();
        const bool        complete = lot["complete"].boolean();
        if (!needle.empty() && lower(name + " " + id + " " + lot["lot_type_name"].str()).find(needle) == std::string::npos)
          continue;
        if (only_problems_ && complete && soon == 0)
          continue;
        ImGui::TableNextRow();
        if (expired > 0)
          row_color(colors::red, 0.35f);
        else if (!complete)
          row_color(colors::orange, 0.30f);
        ImGui::TableNextColumn();
        if (ImGui::Selectable((name + "##" + id).c_str(), selected_ == id, ImGuiSelectableFlags_SpanAllColumns))
          select(app, id);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lot["lot_type_name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", lot["item_count"].integer());
        ImGui::TableNextColumn();
        if (expired > 0)
          ImGui::TextColored(colors::red, "%d", expired);
        else if (soon > 0)
          ImGui::TextColored(colors::orange, "%d bientôt", soon);
        else
          ImGui::TextUnformatted("0");
        ImGui::TableNextColumn();
        ImGui::TextColored(complete ? colors::green : colors::orange, complete ? "oui" : "non");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_datetime(lot["last_verif"]).c_str());
      }
      ImGui::EndTable();
    }

    void select(App &app, const std::string &id) {
      selected_ = id;
      details_  = Json();
      app.api.get("/api/lots/" + url_encode(id) + "/", [this, &app, id](const ApiResult &result) {
        if (selected_ != id)
          return;
        if (result.ok)
          details_ = result.data;
        else
          app.notify(result.error, true);
      });
    }

    void draw_details(App &app) {
      if (details_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        return;
      }
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
      ImGui::TextUnformatted(details_["name"].str().c_str());
      ImGui::PopFont();
      ImGui::TextDisabled("%s - %s", details_["lot_type_name"].str().c_str(), details_["id"].str().c_str());
      ImGui::Text("Dernière vérif : %s par %s", display_datetime(details_["last_verif"]).c_str(),
                  details_["last_verif_by"].str("-").c_str());
      if (primary_button("Lancer une vérif", ImVec2(-1, 0)))
        app.start_verif(details_["id"].str(), "");
      if (ImGui::Button("Fermer", ImVec2(-1, 0)))
        selected_.clear();

      ImGui::SeparatorText("Contenu attendu");
      if (ImGui::BeginTable("req", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : details_["requirements"].items()) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["type_name"].str().c_str());
          ImGui::TableNextColumn();
          stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(-1, 0));
          ImGui::TableNextColumn();
          const int expired = row["expired"].integer();
          if (expired > 0)
            ImGui::TextColored(colors::red, "%d", expired);
          else
            ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
      }

      ImGui::SeparatorText("Items");
      const Date soon = app.today().plus_days(app.settings.expiring_soon_days);
      if (ImGui::BeginTable("items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const Json &item : details_["items"].items()) {
          const auto date = Date::parse(item["peremption"].str());
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.4f);
          else if (date && *date <= soon)
            row_color(colors::orange, 0.3f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          if (item["missed_verifs"].integer() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(non vu à la dernière vérif)");
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(date ? date->display().c_str() : "-");
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", item["iid"].str().c_str());
        }
        for (const Json &item : details_["missing_items"].items()) {
          ImGui::TableNextRow();
          row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          ImGui::Text("%s (disparu)", item["type_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", item["iid"].str().c_str());
        }
        ImGui::EndTable();
      }
    }

    std::string filter_;
    bool        only_problems_ = false;
    std::string selected_;
    Json        details_;
};

} // namespace

std::unique_ptr< AppWindow > make_lots_window() {
  return std::make_unique< LotsWindow >();
}

} // namespace qrprotec
