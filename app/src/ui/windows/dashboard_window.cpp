/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          dashboard_window.cpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 21:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../core/plural.hpp"
#include "../widgets.hpp"
#include "implot.h"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace qrprotec {

namespace {

// Tableau de bord de la TECH (roles gestion et admin, menu TECH) : le meme que sur le web (bouton en haut de la
// Gestion de la TECH). Tout est calcule et ecrit par le serveur (database/inventory/dashboard.py, GET /api/dashboard/) :
// scores de fonctionnement de chaque activite, ce qui merite l'attention, puis une section par sujet (chiffres,
// phrases, graphes par mois, tableaux). Ici seulement le rendu generique.

ImVec4 level_color(const std::string &level) {
  if (level == "bad")
    return colors::red;
  if (level == "warn")
    return colors::orange;
  if (level == "ok")
    return colors::green;
  return colors::grey;
}

ImVec4 hex_color(const std::string &hex) {
  if (hex.size() != 7 || hex[0] != '#')
    return colors::blue;
  const long value = std::strtol(hex.c_str() + 1, nullptr, 16);
  return ImVec4(((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, 1.0f);
}

std::string cell_text(const Json &cell) {
  if (cell.is_string())
    return cell.str();
  if (cell.is_number()) {
    const double value = cell.num();
    char         text[32];
    if (std::floor(value) == value)
      std::snprintf(text, sizeof(text), "%lld", static_cast< long long >(value));
    else
      std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
  }
  return cell.is_null() ? std::string("–") : cell.dump();
}

// Score sur 100 dans un anneau de la couleur de son niveau (gris et « – » : pas encore assez de donnees)
void score_ring(const Json &value, const std::string &level, float radius) {
  ImDrawList  *draw   = ImGui::GetWindowDrawList();
  const ImVec2 pos    = ImGui::GetCursorScreenPos();
  const ImVec2 center(pos.x + radius + 2.0f, pos.y + radius + 2.0f);
  const float  thick  = std::max(4.0f, radius * 0.18f);
  draw->AddCircle(center, radius - thick / 2, ImGui::GetColorU32(ImGuiCol_FrameBg), 48, thick);
  if (!value.is_null()) {
    const float pi    = 3.14159265f;
    const float start = -pi / 2;
    const float end   = start + 2 * pi * static_cast< float >(std::clamp(value.num(), 0.0, 100.0) / 100.0);
    draw->PathArcTo(center, radius - thick / 2, start, end, 48);
    draw->PathStroke(ImGui::GetColorU32(level_color(level)), 0, thick);
  }
  const std::string text = value.is_null() ? std::string("–") : std::to_string(value.integer());
  ImGui::SetWindowFontScale(radius > 30 ? 1.6f : 1.2f);
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(center.x - size.x / 2, center.y - size.y / 2),
                ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Dummy(ImVec2(radius * 2 + 4, radius * 2 + 4));
}

class DashboardWindow final : public AppWindow {
  public:
    DashboardWindow() : AppWindow("dashboard", "Tableau de bord de la TECH", true, true) {}

    void on_open(App &app) override { load(app); }

    void reset_view(App &) override { scroll_to_.clear(); }

    void draw(App &app) override {
      if (button("Rafraîchir"))
        load(app);
      ImGui::SameLine();
      if (loading_)
        ImGui::TextDisabled("calcul…");
      else if (data_.contains("start"))
        ImGui::TextDisabled("Sur les 12 derniers mois. Scores sur 100 : vert à partir de 75, orange de 50 à 75, rouge "
                            "en dessous.");
      if (!error_.empty())
        ImGui::TextColored(colors::red, "%s", error_.c_str());
      if (!data_.contains("sections"))
        return;
      ImGui::BeginChild("dashboard", ImVec2(0, 0));
      draw_scores();
      draw_alerts();
      for (const Json &part : data_["sections"].items())
        draw_section(app, part);
      ImGui::EndChild();
    }

  private:
    void load(App &app) {
      loading_ = true;
      app.api.get("/api/dashboard/", [this](const ApiResult &result) {
        loading_ = false;
        if (result.ok)
          data_ = result.data;
        error_ = result.ok ? std::string() : result.error;
      });
    }

    void draw_scores() {
      const Json &scores = data_["scores"];
      if (scores.size() == 0)
        return;
      const Json &overall = data_["overall"];
      score_ring(overall["value"], overall["level"].str(), 38.0f);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::Dummy(ImVec2(0, 18));
      ImGui::SetWindowFontScale(1.3f);
      ImGui::TextUnformatted("Fonctionnement global");
      ImGui::SetWindowFontScale(1.0f);
      ImGui::TextDisabled("%s", overall["detail"].str().c_str());
      ImGui::EndGroup();
      const int columns = std::clamp(static_cast< int >(ImGui::GetContentRegionAvail().x / 230.0f), 1,
                                     static_cast< int >(scores.size()));
      if (!ImGui::BeginTable("scores", columns, ImGuiTableFlags_SizingStretchSame))
        return;
      for (const Json &row : scores.items()) {
        ImGui::TableNextColumn();
        ImGui::PushID(row["key"].str().c_str());
        score_ring(row["value"], row["level"].str(), 28.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        // pas d'emoji au poste : la police n'en a pas
        if (ImGui::Selectable(row["title"].str().c_str()))
          scroll_to_ = row["key"].str();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        for (const Json &text : row["parts"].items())
          ImGui::TextWrapped("%s", text.str().c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", row["detail"].str().c_str());
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    void draw_alerts() {
      const Json &alerts = data_["alerts"];
      if (alerts.size() == 0)
        return;
      ImGui::SeparatorText("À surveiller");
      for (const Json &row : alerts.items()) {
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(row["level"].str()));
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", row["text"].str().c_str());
      }
    }

    void draw_section(App &app, const Json &part) {
      const std::string key = part["key"].str();
      ImGui::PushID(key.c_str());
      ImGui::Spacing();
      if (scroll_to_ == key) {
        ImGui::SetScrollHereY(0.0f);
        ImGui::SetNextItemOpen(true);
        scroll_to_.clear();
      }
      std::string header = part["title"].str();
      const Json &score  = part["score"];
      if (score.is_object())
        header += score["value"].is_null() ? "   (score : –)" : "   (score : " + std::to_string(score["value"].integer()) + ")";
      if (score.is_object())
        ImGui::PushStyleColor(ImGuiCol_Text, level_color(score["level"].str()));
      const bool open = ImGui::CollapsingHeader((header + "###section").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
      if (score.is_object())
        ImGui::PopStyleColor();
      if (open) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", part["help"].str().c_str());
        ImGui::PopStyleColor();
        draw_tiles(part["tiles"]);
        for (const Json &line : part["lines"].items()) {
          const std::string level = line["level"].str();
          if (level.empty()) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line["text"].str().c_str());
          } else {
            ImGui::PushStyleColor(ImGuiCol_Text, level_color(level));
            ImGui::Bullet();
            ImGui::TextWrapped("%s", line["text"].str().c_str());
            ImGui::PopStyleColor();
          }
        }
        int index = 0;
        for (const Json &chart : part["charts"].items())
          draw_chart(chart, index++);
        index = 0;
        for (const Json &table : part["tables"].items())
          draw_table(app, table, index++);
        draw_link(app, part["link"].str());
      }
      ImGui::PopID();
    }

    static void draw_tiles(const Json &tiles) {
      if (tiles.size() == 0)
        return;
      const int columns = std::clamp(static_cast< int >(ImGui::GetContentRegionAvail().x / 170.0f), 1,
                                     static_cast< int >(tiles.size()));
      if (!ImGui::BeginTable("tiles", columns, ImGuiTableFlags_SizingStretchSame))
        return;
      for (const Json &tile : tiles.items()) {
        ImGui::TableNextColumn();
        const std::string level = tile["level"].str();
        ImGui::SetWindowFontScale(1.4f);
        if (level.empty() || level == "ok")
          ImGui::TextUnformatted(tile["value"].str().c_str());
        else
          ImGui::TextColored(level_color(level), "%s", tile["value"].str().c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextWrapped("%s", tile["label"].str().c_str());
        if (!tile["detail"].str().empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TextWrapped("%s", tile["detail"].str().c_str());
          ImGui::PopStyleColor();
        }
      }
      ImGui::EndTable();
    }

    static void draw_chart(const Json &chart, int index) {
      const Json &labels = chart["labels"];
      const Json &series = chart["series"];
      const int   n      = static_cast< int >(labels.size());
      if (n == 0 || series.size() == 0)
        return;
      const bool                        stacked = chart["stacked"].boolean();
      std::vector< std::vector< double > > values;
      double                            top = 0.0;
      std::vector< double >             base(n, 0.0);
      for (const Json &row : series.items()) {
        std::vector< double > column;
        for (int i = 0; i < n; ++i) {
          const double value = row["values"][i].num();
          column.push_back(stacked ? base[i] + value : value);
          if (stacked)
            base[i] += value;
          top = std::max(top, column.back());
        }
        values.push_back(column);
      }
      if (top <= 0.0)
        return;
      std::vector< double >      xs;
      std::vector< std::string > names;
      std::vector< const char * > ticks;
      for (int i = 0; i < n; ++i) {
        xs.push_back(i);
        names.push_back(labels[i].str());
      }
      for (const std::string &name : names)
        ticks.push_back(name.c_str());
      const std::string id = chart["title"].str() + "##chart" + std::to_string(index);
      if (!ImPlot::BeginPlot(id.c_str(), ImVec2(-1, 200), ImPlotFlags_NoMouseText))
        return;
      ImPlot::SetupAxes(nullptr, chart["unit"].str().empty() ? nullptr : chart["unit"].str().c_str());
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, n - 0.4, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + (top < 5 ? 0.5 : 1), ImPlotCond_Always);
      ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, ticks.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      const int count = static_cast< int >(series.size());
      if (stacked) {
        // barres empilees : la plus haute (cumul) d'abord, les suivantes par-dessus
        for (int k = count - 1; k >= 0; --k)
          ImPlot::PlotBars(series[k]["name"].str().c_str(), xs.data(), values[k].data(), n, 0.6,
                           ImPlotSpec(ImPlotProp_FillColor, hex_color(series[k]["color"].str())));
      } else {
        const double width = 0.7 / count;
        for (int k = 0; k < count; ++k) {
          std::vector< double > shifted;
          for (int i = 0; i < n; ++i)
            shifted.push_back(i - 0.35 + width * (k + 0.5));
          ImPlot::PlotBars(series[k]["name"].str().c_str(), shifted.data(), values[k].data(), n, width,
                           ImPlotSpec(ImPlotProp_FillColor, hex_color(series[k]["color"].str())));
        }
      }
      if (ImPlot::IsPlotHovered()) {
        const int i = static_cast< int >(std::lround(ImPlot::GetPlotMousePos().x));
        if (i >= 0 && i < n) {
          std::string text = names[i] + " :";
          for (const Json &row : series.items())
            text += "\n" + row["name"].str() + " : " + cell_text(row["values"][i]) +
                    (chart["unit"].str().empty() ? std::string() : " " + chart["unit"].str());
          ImGui::SetTooltip("%s", text.c_str());
        }
      }
      ImPlot::EndPlot();
    }

    static void draw_table(App &app, const Json &table, int index) {
      const Json &columns = table["columns"];
      const int   count   = static_cast< int >(columns.size());
      if (count == 0)
        return;
      ImGui::Spacing();
      ImGui::TextUnformatted(table["title"].str().c_str());
      const std::string id = "table" + std::to_string(index);
      if (!ImGui::BeginTable(id.c_str(), count + 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                         ImGuiTableFlags_SizingStretchProp))
        return;
      for (int k = 0; k < count; ++k)
        ImGui::TableSetupColumn(columns[k].str().c_str(), k == 0 ? ImGuiTableColumnFlags_WidthStretch
                                                                 : ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      int row_index = 0;
      for (const Json &row : table["rows"].items()) {
        ImGui::TableNextRow();
        const std::string level = row["level"].str();
        if (!level.empty())
          row_color(level_color(level), 0.18f);
        for (int k = 0; k < count; ++k) {
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(cell_text(row["cells"][k]).c_str());
        }
        ImGui::TableNextColumn();
        const std::string lot = row["lot"].str();
        if (!lot.empty()) {
          ImGui::PushID(row_index);
          if (small_button("Ouvrir le lot"))
            app.show_lot(lot);
          ImGui::PopID();
        }
        ++row_index;
      }
      ImGui::EndTable();
    }

    static void draw_link(App &app, const std::string &link) {
      if (link.empty() || link == "stats")
        return;
      ImGui::Spacing();
      if (link == "lots" && small_button("Ouvrir l'état des lots"))
        app.open_window("lots");
      else if (link == "reports" && small_button("Ouvrir les signalements"))
        app.open_window("reports");
      else if (link == "lists" && small_button("Ouvrir les listes de matériel"))
        app.open_window("lists");
      else if (link == "forecast" && small_button("Ouvrir la projection du stock"))
        app.show_stock_tab("projection");
      else if (link == "purchases" && small_button("Ouvrir les statistiques des achats"))
        app.show_stock_tab("purchase_stats");
      else if (link == "consumables" && small_button("Ouvrir les consommables")) {
        app.show_consumables = true;
        app.open_window("print_counts");
      }
    }

    Json        data_ = Json::object();
    std::string error_;
    std::string scroll_to_; // section a montrer (score clique)
    bool        loading_ = false;
};

} // namespace

std::unique_ptr< AppWindow > make_dashboard_window() {
  return std::make_unique< DashboardWindow >();
}

} // namespace qrprotec
