/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          stock_window.cpp
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
#include <vector>

namespace qrprotec {

namespace {

class StockWindow final : public AppWindow {
  public:
    StockWindow() : AppWindow("stock", "Etat des stocks", true, true) {}

    void on_open(App &app) override { app.refresh_stock(); }

    void draw(App &app) override {
      if (ImGui::Button("Rafraichir"))
        app.refresh_stock();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("##filter", "Filtrer", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("Sous le minimum uniquement", &only_low_);
      ImGui::SameLine();
      ImGui::Checkbox("Compter les lots", &include_lots_);
      help_marker("La barre compare les items non perimes au minimum du type. Par defaut seul le stock (hors lots) "
                  "est compte.");
      if (app.catalog.loading_stock) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }

      std::vector< const Json * > rows;
      for (const Json &row : app.catalog.stock.items())
        rows.push_back(&row);
      // les plus critiques en premier
      std::stable_sort(rows.begin(), rows.end(), [this](const Json *a, const Json *b) {
        return ratio(*a) < ratio(*b);
      });

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      if (!ImGui::BeginTable("stock", 6, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 220.0f);
      ImGui::TableSetupColumn("Quantite / minimum", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Dans les lots", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Perimes", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Bientot perimes", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Disparus", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableHeadersRow();
      std::string needle = filter_;
      std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast< char >(std::tolower(c)); });
      for (const Json *row_ptr : rows) {
        const Json &row      = *row_ptr;
        const int   quantity = count(row);
        const int   minimum  = row["min_quantity"].integer();
        std::string haystack = row["name"].str() + " " + row["type"].str();
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast< char >(std::tolower(c)); });
        if (!needle.empty() && haystack.find(needle) == std::string::npos)
          continue;
        if (only_low_ && quantity >= minimum && quantity > 0)
          continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row["name"].str().c_str());
        ImGui::TextDisabled("%s", row["type"].str().c_str());
        ImGui::TableNextColumn();
        stock_bar(quantity, minimum, ImVec2(-1, ImGui::GetFrameHeight() * 1.3f));
        ImGui::TableNextColumn();
        ImGui::Text("%d", row["lots_fresh"].integer());
        ImGui::TableNextColumn();
        const int expired = row["stock_expired"].integer() + row["lots_expired"].integer();
        if (expired > 0)
          ImGui::TextColored(colors::red, "%d", expired);
        else
          ImGui::TextUnformatted("0");
        ImGui::TableNextColumn();
        const int soon = row["expiring_soon"].integer();
        if (soon > 0)
          ImGui::TextColored(colors::orange, "%d", soon);
        else
          ImGui::TextUnformatted("0");
        ImGui::TableNextColumn();
        ImGui::Text("%d", row["missing"].integer());
      }
      ImGui::EndTable();
    }

  private:
    int count(const Json &row) const {
      return row["stock_fresh"].integer() + (include_lots_ ? row["lots_fresh"].integer() : 0);
    }

    float ratio(const Json &row) const {
      const int minimum = row["min_quantity"].integer();
      return minimum > 0 ? static_cast< float >(count(row)) / static_cast< float >(minimum) : 1e6f;
    }

    std::string filter_;
    bool        only_low_     = false;
    bool        include_lots_ = false;
};

} // namespace

std::unique_ptr< AppWindow > make_stock_window() {
  return std::make_unique< StockWindow >();
}

} // namespace qrprotec
