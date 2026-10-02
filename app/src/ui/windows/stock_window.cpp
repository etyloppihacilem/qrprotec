/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          stock_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "implot.h"
#include "windows.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

namespace qrprotec {

namespace {

// Horizons proposes : index dans "points" des previsions (un point par mois)
constexpr std::array< int, 4 >          horizon_months = { 0, 1, 3, 6 };
constexpr std::array< const char *, 4 > horizon_labels = { "Aujourd'hui", "M+1", "M+3", "M+6" };

const ImVec4 stock_color(0.11f, 0.31f, 0.57f, 1.0f);
const ImVec4 lots_color(0.42f, 0.61f, 0.85f, 1.0f);
const ImVec4 sealed_color(0.78f, 0.35f, 0.0f, 1.0f);

std::string date_text(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : std::string("-");
}

std::string short_date(const Json &value) {
  const auto date = Date::parse(value.str());
  if (!date)
    return "-";
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02d/%02d", date->day, date->month);
  return buffer;
}

std::string month_label(const Json &value) {
  static const char *names[] = { "janv.", "févr.", "mars", "avr.", "mai", "juin",
                                 "juil.", "août", "sept.", "oct.", "nov.", "déc." };
  const auto date = Date::parse(value.str());
  return date && date->month >= 1 && date->month <= 12 ? names[date->month - 1] : "?";
}

// Nombre avec decimales seulement si utile (consommation par mois)
std::string rate_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), value >= 10 || value == static_cast< int >(value) ? "%.0f" : "%.1f", value);
  return buffer;
}

void colored_count(int value, const ImVec4 &color) {
  if (value > 0)
    ImGui::TextColored(color, "%d", value);
  else
    ImGui::TextUnformatted("0");
}

class StockWindow final : public AppWindow {
  public:
    StockWindow() : AppWindow("stock", "État des stocks", true, true) {}

    void on_open(App &app) override {
      app.refresh_stock();
      app.refresh_forecast();
    }

    void draw(App &app) override {
      if (ImGui::Button("Rafraîchir"))
        on_open(app);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("##filter", "Filtrer", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("Sous le minimum uniquement", &only_low_);
      ImGui::SameLine();
      ImGui::Checkbox("Compter les lots", &include_lots_);
      help_marker("La barre compare les items non périmés au minimum du type. Par défaut seul le stock est compté : "
                  "items hors lots et items rangés dans un rangement du stock (armoire, tiroir : lots dont le type "
                  "est un rangement).");
      if (app.catalog.loading_stock || app.catalog.loading_forecast) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }

      const Json &forecast = app.catalog.forecast;
      int         orders   = 0;
      for (const Json &row : forecast["types"].items())
        orders += row["order"].is_object() ? 1 : 0;
      const std::string orders_tab    = "Commandes (" + std::to_string(orders) + ")###orders";
      const std::string transfers_tab = "Transferts (" + std::to_string(forecast["transfers"].size()) + ")###transfers";

      if (!ImGui::BeginTabBar("stock_tabs"))
        return;
      if (ImGui::BeginTabItem("Actuel")) {
        draw_current(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Projection")) {
        draw_projection(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(orders_tab.c_str())) {
        draw_orders(app);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(transfers_tab.c_str())) {
        draw_transfers(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // ---- Actuel : etat du stock aujourd'hui ----
    void draw_current(App &app) {
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
      ImGui::TableSetupColumn("Quantité / minimum", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Dans les lots", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Bientôt périmés", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Disparus", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableHeadersRow();
      for (const Json *row_ptr : rows) {
        const Json &row      = *row_ptr;
        const int   quantity = count(row);
        const int   minimum  = row["min_quantity"].integer();
        if (!search_matches(filter_, row["name"].str() + " " + row["type"].str()))
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
        colored_count(row["stock_expired"].integer() + row["lots_expired"].integer(), colors::red);
        ImGui::TableNextColumn();
        colored_count(row["expiring_soon"].integer(), colors::orange);
        ImGui::TableNextColumn();
        ImGui::Text("%d", row["missing"].integer());
      }
      ImGui::EndTable();
    }

    int count(const Json &row) const {
      return row["stock_fresh"].integer() + (include_lots_ ? row["lots_fresh"].integer() : 0);
    }

    float ratio(const Json &row) const {
      const int minimum = row["min_quantity"].integer();
      return minimum > 0 ? static_cast< float >(count(row)) / static_cast< float >(minimum) : 1e6f;
    }

    // ---- Projection : stock a M+k, graphes du type selectionne ----
    const Json &point(const Json &row) const {
      const Json &points = row["points"];
      const int   index  = std::min(horizon_months[horizon_], static_cast< int >(points.size()) - 1);
      return points[std::max(0, index)];
    }

    int projected(const Json &row) const {
      const Json &at = point(row);
      if (with_consumption_)
        return at["stock"].integer() + (include_lots_ ? at["lots"].integer() : 0);
      return at["stock_static"].integer() + (include_lots_ ? at["lots_static"].integer() : 0);
    }

    void draw_projection(App &app) {
      const Json &forecast = app.catalog.forecast;
      if (!forecast["types"].is_array()) {
        ImGui::TextDisabled(app.catalog.loading_forecast ? "Chargement des prévisions..." : "Prévisions non chargées.");
        return;
      }
      ImGui::TextUnformatted("Horizon :");
      for (std::size_t index = 0; index < horizon_labels.size(); ++index) {
        ImGui::SameLine();
        if (ImGui::RadioButton(horizon_labels[index], horizon_ == static_cast< int >(index)))
          horizon_ = static_cast< int >(index);
      }
      const Json *any = forecast["types"].size() ? &forecast["types"][0] : nullptr;
      if (any) {
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", date_text(point(*any)["date"]).c_str());
      }
      ImGui::SameLine();
      ImGui::Checkbox("Avec consommation", &with_consumption_);
      help_marker("Avec consommation : chaque lieu utilise ses items les plus anciens en premier, au rythme mesuré "
                  "sur l'historique (items absents aux vérifs). Sans : seuls les items encore valides à la date.\n"
                  "Les items manqués à la dernière vérif ne comptent pas ; ils reviennent s'ils sont retrouvés.");
      ImGui::SameLine();
      ImGui::TextDisabled("(historique : %d j)", forecast["history_days"].integer());

      std::vector< const Json * > rows;
      for (const Json &row : forecast["types"].items()) {
        if (!search_matches(filter_, row["name"].str() + " " + row["type"].str()))
          continue;
        const int minimum = row["min_quantity"].integer();
        if (only_low_ && projected(row) >= minimum && projected(row) > 0 && row["below_min"].is_null())
          continue;
        rows.push_back(&row);
      }
      std::stable_sort(rows.begin(), rows.end(), [this](const Json *a, const Json *b) {
        const bool a_below = !(*a)["below_min"].is_null(), b_below = !(*b)["below_min"].is_null();
        if (a_below != b_below)
          return a_below;
        if (a_below)
          return (*a)["below_min"].str() < (*b)["below_min"].str();
        return projected_ratio(*a) < projected_ratio(*b);
      });
      if (selected_.empty() && !rows.empty())
        selected_ = (*rows.front())["type"].str();

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      const float left = ImGui::GetContentRegionAvail().x * 0.55f;
      if (ImGui::BeginChild("projection_table", ImVec2(left, 0), ImGuiChildFlags_ResizeX)) {
        const std::string valid = "Valide au " + (any ? short_date(point(*any)["date"]) : std::string()) + " / minimum";
        if (ImGui::BeginTable("projection", 6, flags)) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 180.0f);
          ImGui::TableSetupColumn(valid.c_str(), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Périment", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Perdus", ImGuiTableColumnFlags_WidthFixed, 60.0f);
          ImGui::TableSetupColumn("Conso / mois", ImGuiTableColumnFlags_WidthFixed, 90.0f);
          ImGui::TableSetupColumn("Sous le min.", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableHeadersRow();
          for (const Json *row_ptr : rows) {
            const Json       &row  = *row_ptr;
            const Json       &at   = point(row);
            const std::string type = row["type"].str();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(type.c_str());
            if (ImGui::Selectable(row["name"].str().c_str(), selected_ == type, ImGuiSelectableFlags_SpanAllColumns))
              selected_ = type;
            ImGui::PopID();
            ImGui::TextDisabled("%s", type.c_str());
            ImGui::TableNextColumn();
            stock_bar(projected(row), row["min_quantity"].integer(), ImVec2(-1, ImGui::GetFrameHeight() * 1.3f));
            ImGui::TableNextColumn();
            colored_count(at["expiring"].integer(), colors::orange);
            ImGui::TableNextColumn();
            colored_count(at["lost"].integer(), colors::red);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rate_text(row["per_month"].num()).c_str());
            ImGui::TableNextColumn();
            if (row["below_min"].is_null())
              ImGui::TextColored(colors::green, "non");
            else
              ImGui::TextColored(colors::orange, "%s", date_text(row["below_min"]).c_str());
          }
          ImGui::EndTable();
        }
      }
      ImGui::EndChild();
      ImGui::SameLine();
      if (ImGui::BeginChild("projection_detail", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const Json &row : forecast["types"].items())
          if (row["type"].str() == selected_)
            draw_type_detail(app, row);
      }
      ImGui::EndChild();
    }

    float projected_ratio(const Json &row) const {
      const int minimum = row["min_quantity"].integer();
      return minimum > 0 ? static_cast< float >(projected(row)) / static_cast< float >(minimum) : 1e6f;
    }

    void draw_type_detail(App &app, const Json &row) {
      ImGui::TextUnformatted(row["name"].str().c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("· conso %s / mois", rate_text(row["per_month"].num()).c_str());
      if (row["unconfirmed"].integer() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %d non confirmé(s)", row["unconfirmed"].integer());
        help_marker("Items manqués à la dernière vérif : ils ne comptent pas dans les prévisions. S'ils sont "
                    "retrouvés (vérif, ajout à un lot), ils reviennent et ne comptent plus comme utilisés.");
      }
      const Json &order = row["order"];
      if (order.is_object()) {
        const ImVec4 color = order["urgent"].boolean() ? colors::red : colors::orange;
        ImGui::TextColored(color, "Commander %d avant le %s (sous le minimum le %s)", order["quantity"].integer(),
                           date_text(order["before"]).c_str(), date_text(order["below_min"]).c_str());
      }
      if (row["expired_now"].integer() > 0)
        ImGui::TextColored(colors::red, "%d périmé(s) aujourd'hui (stock et lots)", row["expired_now"].integer());

      draw_projection_plot(row);
      draw_expiry_plot(row);

      if (ImGui::CollapsingHeader("Dates de péremption et emplacements")) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("batches", 5, flags)) {
          ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableSetupColumn("Où", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("Utilisés", ImGuiTableColumnFlags_WidthFixed, 65.0f);
          ImGui::TableSetupColumn("Perdus", ImGuiTableColumnFlags_WidthFixed, 55.0f);
          ImGui::TableHeadersRow();
          for (const Json &batch : row["batches"].items()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(batch["peremption"].is_null() ? "aucune" : date_text(batch["peremption"]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(batch["path"].str().c_str());
            if (batch["sealed"].boolean()) {
              ImGui::SameLine();
              ImGui::TextDisabled("(scellé)");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", batch["count"].integer());
            ImGui::TableNextColumn();
            ImGui::Text("%d", batch["used"].integer());
            ImGui::TableNextColumn();
            colored_count(batch["lost"].integer(), colors::red);
          }
          ImGui::EndTable();
        }
      }
      if (ImGui::CollapsingHeader("Consommation par lot")) {
        ImGui::BulletText("Stock (sorties directes) : %s / mois", rate_text(row["stock_per_month"].num()).c_str());
        for (const Json &lot : row["lots_consumption"].items()) {
          ImGui::BulletText("%s : %s / mois", lot["path"].str().c_str(), rate_text(lot["per_month"].num()).c_str());
          if (!lot["id"].is_null()) {
            ImGui::SameLine();
            ImGui::PushID(lot["id"].str().c_str());
            if (ImGui::SmallButton("Afficher"))
              app.show_lot(lot["id"].str());
            ImGui::PopID();
          }
        }
      }
    }

    void draw_projection_plot(const Json &row) {
      const Json         &points = row["points"];
      const int           n      = static_cast< int >(points.size());
      std::vector< double > xs, stock, still_valid;
      std::vector< std::string > labels;
      for (int index = 0; index < n; ++index) {
        const Json &at = points[index];
        xs.push_back(index);
        stock.push_back(at["stock"].integer() + (include_lots_ ? at["lots"].integer() : 0));
        still_valid.push_back(at["stock_static"].integer() + (include_lots_ ? at["lots_static"].integer() : 0));
        labels.push_back(index == 0 ? "auj." : short_date(at["date"]));
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      if (!ImPlot::BeginPlot("##projection", ImVec2(-1, 220), ImPlotFlags_NoMouseText))
        return;
      const double minimum = row["min_quantity"].integer();
      double       top     = minimum;
      for (int index = 0; index < n; ++index)
        top = std::max({ top, stock[index], still_valid[index] });
      ImPlot::SetupAxes(nullptr, include_lots_ ? "stock + lots" : "stock");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.2, std::max(1, n - 1) + 0.2, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
      if (n > 0)
        ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      if (n > 0) {
        ImPlot::PlotShaded("##stock_fill", xs.data(), stock.data(), n, 0.0,
                           ImPlotSpec(ImPlotProp_FillColor, stock_color, ImPlotProp_FillAlpha, 0.15f,
                                      ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
        ImPlot::PlotLine("Stock projeté", xs.data(), stock.data(), n,
                         ImPlotSpec(ImPlotProp_LineColor, stock_color, ImPlotProp_LineWeight, 2.0f, ImPlotProp_Marker,
                                    ImPlotMarker_Circle));
        ImPlot::PlotLine("Sans consommation", xs.data(), still_valid.data(), n,
                         ImPlotSpec(ImPlotProp_LineColor, lots_color, ImPlotProp_LineWeight, 1.0f));
        if (minimum > 0)
          ImPlot::PlotInfLines("Minimum", &minimum, 1,
                               ImPlotSpec(ImPlotProp_LineColor, colors::red, ImPlotProp_Flags,
                                          ImPlotInfLinesFlags_Horizontal));
        const double horizon = std::min(horizon_months[horizon_], n - 1);
        if (horizon > 0)
          ImPlot::PlotInfLines("##horizon", &horizon, 1,
                               ImPlotSpec(ImPlotProp_LineColor, colors::grey, ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
      }
      ImPlot::EndPlot();
    }

    void draw_expiry_plot(const Json &row) {
      const Json &months = row["calendar"];
      const int   n      = static_cast< int >(months.size());
      std::vector< double >      xs, stock, lots, sealed, lost;
      std::vector< std::string > labels;
      for (int index = 0; index < n; ++index) {
        const Json &month    = months[index];
        const Json &expiring = month["expiring"];
        xs.push_back(index);
        stock.push_back(expiring["stock"].integer());
        lots.push_back(stock.back() + expiring["lots"].integer());
        sealed.push_back(lots.back() + expiring["sealed"].integer());
        const Json &gone = month["lost"];
        lost.push_back(gone["stock"].integer() + gone["lots"].integer() + gone["sealed"].integer());
        labels.push_back(month_label(month["start"]));
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      if (!ImPlot::BeginPlot("Péremptions par mois", ImVec2(-1, 200), ImPlotFlags_NoMouseText))
        return;
      double top = 1;
      for (int index = 0; index < n; ++index)
        top = std::max(top, sealed[index]);
      ImPlot::SetupAxes(nullptr, "items");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, std::max(1, n) - 0.4, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
      if (n > 0)
        ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      if (n > 0) {
        // barres empilees : chaque serie est dessinee avec le cumul, de la plus haute a la plus basse
        ImPlot::PlotBars("Scellés", xs.data(), sealed.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, sealed_color));
        ImPlot::PlotBars("Lots", xs.data(), lots.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, lots_color));
        ImPlot::PlotBars("Stock", xs.data(), stock.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, stock_color));
        ImPlot::PlotBars("Perdus (pas utilisés à temps)", xs.data(), lost.data(), n, 0.25,
                         ImPlotSpec(ImPlotProp_FillColor, colors::red, ImPlotProp_FillAlpha, 0.85f));
      }
      ImPlot::EndPlot();
    }

    // ---- Commandes ----
    void draw_orders(App &app) {
      const Json &forecast = app.catalog.forecast;
      std::vector< const Json * > rows;
      for (const Json &row : forecast["types"].items())
        if (row["order"].is_object() && search_matches(filter_, row["name"].str() + " " + row["type"].str()))
          rows.push_back(&row);
      std::stable_sort(rows.begin(), rows.end(), [](const Json *a, const Json *b) {
        return (*a)["order"]["before"].str() < (*b)["order"]["before"].str();
      });
      ImGui::TextDisabled("Types qui passeront sous leur minimum dans les 6 mois. Délai de livraison : %d j "
                          "(Réglages).",
                          forecast["lead_days"].integer());
      if (!rows.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Copier la liste")) {
          std::ostringstream text;
          for (const Json *row : rows)
            text << (*row)["name"].str() << " : " << (*row)["order"]["quantity"].integer() << " (avant le "
                 << date_text((*row)["order"]["before"]) << ")\n";
          ImGui::SetClipboardText(text.str().c_str());
          app.notify("Liste des commandes copiée.");
        }
      }
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
      if (!ImGui::BeginTable("orders", 6, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("À commander", ImGuiTableColumnFlags_WidthFixed, 170.0f);
      ImGui::TableSetupColumn("Avant le", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Sous le minimum le", ImGuiTableColumnFlags_WidthFixed, 140.0f);
      ImGui::TableSetupColumn("Stock / minimum", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Conso / mois", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableHeadersRow();
      for (const Json *row_ptr : rows) {
        const Json &row   = *row_ptr;
        const Json &order = row["order"];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row["name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", order["quantity"].integer());
        if (row["pack_size"].integer() > 1) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%d paquets)", order["quantity"].integer() / row["pack_size"].integer());
        }
        ImGui::TableNextColumn();
        if (order["urgent"].boolean())
          ImGui::TextColored(colors::red, "%s", date_text(order["before"]).c_str());
        else
          ImGui::TextColored(colors::orange, "%s", date_text(order["before"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(date_text(order["below_min"]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", row["stock_now"].integer(), row["min_quantity"].integer());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(rate_text(row["per_month"].num()).c_str());
      }
      ImGui::EndTable();
      if (rows.empty())
        ImGui::TextDisabled("Rien à commander.");
    }

    // ---- Transferts : echanges entre lots pour utiliser les items avant leur peremption ----
    void draw_transfers(App &app) {
      const Json &transfers = app.catalog.forecast["transfers"];
      ImGui::TextWrapped("Items qui périmeront là où ils sont, avec le lot où ils seront utilisés à temps. Chaque "
                         "échange garde les deux lots complets : sortez les items indiqués, mettez-les dans le lot "
                         "d'arrivée, et reprenez en échange ceux de la colonne « Reprendre ». Les déplacements se "
                         "font comme d'habitude (scan puis « Ajouter au lot ») ; un échange fait disparaît de la liste.");
      if (transfers.size() == 0) {
        ImGui::TextDisabled("Aucun échange à faire.");
        return;
      }
      if (!ImGui::BeginChild("transfers"))
        return ImGui::EndChild();
      int group_index = 0;
      for (const Json &group : transfers.items()) {
        ImGui::PushID(group_index++);
        const Json &source = group["source"];
        ImGui::Separator();
        ImGui::TextUnformatted(source["path"].str().c_str());
        ImGui::SameLine();
        if (source["sealed"].boolean())
          ImGui::TextColored(colors::red, "· scellé, ouvrir avant le %s", date_text(group["before"]).c_str());
        else
          ImGui::TextColored(colors::orange, "· avant le %s", date_text(group["before"]).c_str());
        if (!source["id"].is_null()) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Afficher le lot"))
            app.show_lot(source["id"].str());
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("moves", 5, flags)) {
          ImGui::TableSetupColumn("Sortir", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Périme", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableSetupColumn("Vers", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Reprendre", ImGuiTableColumnFlags_WidthFixed, 160.0f);
          ImGui::TableSetupColumn("Avant le", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableHeadersRow();
          int move_index = 0;
          for (const Json &move : group["moves"].items()) {
            ImGui::PushID(move_index++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string what = move["type_name"].str() + " ×" + std::to_string(move["take"].size());
            if (ImGui::TreeNodeEx(what.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
              for (const Json &iid : move["take"].items())
                ImGui::TextDisabled("%s", iid.str().c_str());
              ImGui::TreePop();
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(colors::red, "%s", date_text(move["take_peremption"]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(move["target"]["path"].str().c_str());
            ImGui::TableNextColumn();
            ImGui::Text("×%d (%s)", static_cast< int >(move["back"].size()),
                        date_text(move["back_peremption"]).c_str());
            if (ImGui::IsItemHovered()) {
              std::string iids;
              for (const Json &iid : move["back"].items())
                iids += iid.str() + "\n";
              ImGui::SetTooltip("%s", iids.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(date_text(move["before"]).c_str());
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    std::string filter_;
    std::string selected_;
    bool        only_low_         = false;
    bool        include_lots_     = false;
    bool        with_consumption_ = true;
    int         horizon_          = 1;
};

} // namespace

std::unique_ptr< AppWindow > make_stock_window() {
  return std::make_unique< StockWindow >();
}

} // namespace qrprotec
