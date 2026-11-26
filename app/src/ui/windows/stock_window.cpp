/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          stock_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "../../core/plural.hpp"
#include "imgui_stdlib.h"
#include "implot.h"
#include "windows.hpp"

#include <utility>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <cmath>
#include <optional>
#include <cstdio>
#include <sstream>
#include <vector>

namespace qrprotec {

namespace {

// Jours depuis le 1er janvier 1970 (calendrier civil), pour placer une date sur l'axe des semaines
int civil_days(const Date &date) {
  const int y   = date.year - (date.month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (date.month + (date.month > 2 ? -3 : 9)) + 2) / 5 + date.day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

std::string rate_label(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), value >= 10 || value == static_cast< int >(value) ? "%.0f" : "%.1f", value);
  std::string text = buffer;
  std::replace(text.begin(), text.end(), '.', ',');
  return text;
}

const ImVec4 explain_stock(0.11f, 0.31f, 0.57f, 1.0f);
const ImVec4 explain_lots(0.42f, 0.61f, 0.85f, 1.0f);

// « Pourquoi cette commande ? » : ce qui a provoque la recommandation (phrases du serveur, les memes qu'au web),
// stock prevu semaine par semaine (minimum en stock, stock avec la commande, lots et besoin des lots), consommation
// mois par mois avec la consommation estimee.
void draw_order_explain(const Json &x) {
  if (!x.is_object())
    return;
  ImGui::Indent();
  const std::string trigger = x["trigger"].str();
  ImGui::TextColored(trigger == "below" ? colors::red : trigger == "expiry" ? colors::orange : explain_stock, "%s",
                     x["trigger_label"].str().c_str());
  for (const Json &line : x["why"].items()) {
    ImGui::Bullet();
    ImGui::TextWrapped("%s", line.str().c_str());
  }
  const Json &points = x["points"];
  const int   n      = static_cast< int >(points.size());
  const auto  first  = n > 0 ? Date::parse(points[0]["date"].str()) : std::nullopt;
  std::vector< double > xs, stock, with_order, lots;
  std::vector< double > ticks;
  std::vector< std::string > tick_labels;
  static const char *short_months[] = { "janv.", "févr.", "mars", "avr.", "mai", "juin",
                                        "juil.", "août", "sept.", "oct.", "nov.", "déc." };
  int month = -1;
  bool any_lots = x["lots_need"].integer() > 0;
  for (int index = 0; index < n; ++index) {
    const Json &at = points[index];
    xs.push_back(index);
    stock.push_back(at["stock"].integer());
    with_order.push_back(at["with_order"].integer());
    lots.push_back(at["lots"].integer());
    any_lots = any_lots || at["lots"].integer() > 0;
    const auto date = Date::parse(at["date"].str());
    if (date && date->month != month) {
      month = date->month;
      if (index > 0 || n < 3) {
        ticks.push_back(index);
        tick_labels.push_back(short_months[(date->month + 11) % 12]);
      }
    }
  }
  auto week_of = [&](const std::string &iso) -> std::optional< double > {
    const auto date = Date::parse(iso);
    if (!date || !first)
      return std::nullopt;
    return (civil_days(*date) - civil_days(*first)) / 7.0;
  };
  ImGui::Spacing();
  ImGui::TextUnformatted("Stock prévu");
  if (n > 0 && ImPlot::BeginPlot("##order_stock", ImVec2(-1, 220), ImPlotFlags_NoMouseText)) {
    const double minimum = x["min_quantity"].integer();
    const double need    = x["lots_need"].integer();
    double       top     = std::max({ 1.0, minimum, need });
    for (int index = 0; index < n; ++index)
      top = std::max({ top, stock[index], with_order[index], any_lots ? lots[index] : 0.0 });
    std::vector< const char * > label_ptrs;
    for (const std::string &label : tick_labels)
      label_ptrs.push_back(label.c_str());
    ImPlot::SetupAxes(nullptr, x["unit"].str().empty() ? "items" : x["unit"].str().c_str());
    ImPlot::SetupAxisLimits(ImAxis_X1, -0.3, std::max(1, n - 1) + 0.3, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
    if (!ticks.empty())
      ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), static_cast< int >(ticks.size()), label_ptrs.data());
    ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
    ImPlot::PlotLine("Stock", xs.data(), stock.data(), n,
                     ImPlotSpec(ImPlotProp_LineColor, explain_stock, ImPlotProp_LineWeight, 2.0f));
    ImPlot::PlotLine("Avec la commande", xs.data(), with_order.data(), n,
                     ImPlotSpec(ImPlotProp_LineColor, colors::green, ImPlotProp_LineWeight, 2.0f));
    if (minimum > 0)
      ImPlot::PlotInfLines("Minimum", &minimum, 1,
                           ImPlotSpec(ImPlotProp_LineColor, colors::red, ImPlotProp_LineWeight, 2.0f, ImPlotProp_Flags,
                                      ImPlotInfLinesFlags_Horizontal));
    if (any_lots) {
      ImPlot::PlotLine("Lots", xs.data(), lots.data(), n,
                       ImPlotSpec(ImPlotProp_LineColor, explain_lots, ImPlotProp_LineWeight, 1.5f));
      if (need > 0)
        ImPlot::PlotInfLines("Besoin des lots", &need, 1,
                             ImPlotSpec(ImPlotProp_LineColor, ImVec4(0.42f, 0.61f, 0.85f, 0.6f), ImPlotProp_LineWeight,
                                        3.0f, ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal));
    }
    // dates cles, etiquetees en haut : « commander » a gauche de sa ligne, « sous le min. » a droite
    if (const auto before = week_of(x["before"].str()); before && *before >= 0 && *before <= n - 1) {
      ImPlot::PlotInfLines("##before", &*before, 1,
                           ImPlotSpec(ImPlotProp_LineColor, colors::orange, ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
      ImPlot::Annotation(*before, top * 1.08, colors::orange, ImVec2(-4, 0), true, "commander");
    }
    if (const auto below = week_of(x["below_min"].str()); below && *below >= 0 && *below <= n - 1) {
      const ImVec4 faded(0.82f, 0.12f, 0.12f, 0.6f);
      ImPlot::PlotInfLines("##below", &*below, 1,
                           ImPlotSpec(ImPlotProp_LineColor, faded, ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
      ImPlot::Annotation(*below, top * 1.08, faded, ImVec2(4, 0), true, "sous le min.");
    }
    // survol : la semaine sous la souris
    if (ImPlot::IsPlotHovered()) {
      const int index = static_cast< int >(std::lround(ImPlot::GetPlotMousePos().x));
      if (index >= 0 && index < n) {
        const Json &at   = points[index];
        std::string text = "Semaine du " + Date::parse(at["date"].str()).value_or(Date{}).display() + " : stock "
                         + std::to_string(at["stock"].integer());
        if (at["with_order"].integer() != at["stock"].integer())
          text += ", " + std::to_string(at["with_order"].integer()) + " avec la commande";
        if (any_lots)
          text += "\nDans les lots : " + std::to_string(at["lots"].integer()) + " / "
                + std::to_string(x["lots_need"].integer());
        if (at["lost"].integer() > 0)
          text += "\n" + nb(at["lost"].integer(), "périmé", "périmés") + " sans servir depuis aujourd'hui";
        ImGui::SetTooltip("%s", text.c_str());
      }
    }
    ImPlot::EndPlot();
  }
  ImGui::TextUnformatted("Consommation par mois");
  const Json &months = x["monthly"];
  const int   m      = static_cast< int >(months.size());
  if (m > 0 && ImPlot::BeginPlot("##order_use", ImVec2(-1, 170), ImPlotFlags_NoMouseText)) {
    std::vector< double >      mx(m), used(m), total(m);
    std::vector< std::string > labels;
    double                     top  = std::max(1.0, x["per_month"].num());
    for (int index = 0; index < m; ++index) {
      mx[index]    = index;
      used[index]  = months[index]["used"].integer();
      total[index] = used[index] + months[index]["discarded"].integer();
      top          = std::max(top, total[index]);
      const auto date = Date::parse(months[index]["start"].str());
      labels.push_back(date ? short_months[(date->month + 11) % 12] : "?");
    }
    std::vector< const char * > label_ptrs;
    for (const std::string &label : labels)
      label_ptrs.push_back(label.c_str());
    ImPlot::SetupAxes(nullptr, x["unit"].str().empty() ? "items" : x["unit"].str().c_str());
    ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, m - 0.4, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.2 + 1, ImPlotCond_Always);
    ImPlot::SetupAxisTicks(ImAxis_X1, mx.data(), m, label_ptrs.data());
    ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
    // barres empilees : le total (jetes perimes) dessine d'abord, les utilises par-dessus
    ImPlot::PlotBars("Jetés périmés", mx.data(), total.data(), m, 0.6, ImPlotSpec(ImPlotProp_FillColor, colors::red));
    ImPlot::PlotBars("Utilisés", mx.data(), used.data(), m, 0.6, ImPlotSpec(ImPlotProp_FillColor, explain_stock));
    const double rate = x["per_month"].num();
    const std::string rate_name = "Consommation estimée (" + rate_label(rate) + "/mois)";
    if (rate > 0)
      ImPlot::PlotInfLines(rate_name.c_str(), &rate, 1,
                           ImPlotSpec(ImPlotProp_LineColor, colors::orange, ImPlotProp_LineWeight, 2.0f, ImPlotProp_Flags,
                                      ImPlotInfLinesFlags_Horizontal));
    ImPlot::EndPlot();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", x["quantity_text"].str().c_str());
  ImGui::PopStyleColor();
  ImGui::Unindent();
}


class StockWindow final : public AppWindow {
  public:
    StockWindow() : AppWindow("stock", "Gestion de la TECH", true, true) {}

    void on_open(App &app) override {
      app.refresh_tasks();
      app.refresh_events();
      app.refresh_stock();
      app.refresh_forecast(); // commandes suggerees : taches de la categorie « orders »
    }

    void reset_view(App &) override {
      select_        = "tasks";
      event_kind_.clear();
      explained_.clear();
    }

    void draw(App &app) override {
      if (button("Rafraîchir"))
        on_open(app);
      ImGui::SameLine();
      if (primary_button("Tableau de bord")) // scores, depenses, consommation, reassorts (comme le web)
        app.open_window("dashboard");
      if (app.catalog.loading_tasks || app.catalog.loading_events) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }
      if (std::string tab = std::exchange(app.tech_tab_to_show_, std::string()); !tab.empty())
        select_ = tab;
      const std::string tasks_tab = "À faire (" + std::to_string(app.catalog.tasks["count"].integer()) + ")###tasks";
      if (!ImGui::BeginTabBar("stock_tabs"))
        return;
      if (tab_item(tasks_tab.c_str(), select_ == "tasks" ? ImGuiTabItemFlags_SetSelected : 0)) {
        draw_tasks(app);
        ImGui::EndTabItem();
      }
      const int         late       = app.catalog.events["late"].integer();
      const std::string events_tab = "À venir" + (late > 0 ? " (" + std::to_string(late) + " en retard)" : std::string())
                                   + "###events";
      if (tab_item(events_tab.c_str(), select_ == "events" ? ImGuiTabItemFlags_SetSelected : 0)) {
        // tout le materiel suivi, avec son cycle, et les dates a modifier : Items > Entretien
        if (small_button("Tout le matériel suivi (Items › Entretien)"))
          app.show_stock_tab("equipment");
        ImGui::SameLine();
        help_marker("Items › Entretien : matériel non consommable et lots suivis (révision, désinfection), avec leur "
                    "cycle, pour réviser, désinfecter ou corriger une date. La périodicité se règle sur le type "
                    "d'item (Items › Types d'items) ou sur la fiche du lot.");
        draw_events(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
      select_.clear();
    }

  private:
    std::map< std::string, int > order_quantities_; // quantite de la case « Commandé », par type d'item
    std::set< std::string >      explained_;        // commandes dont « Pourquoi cette commande ? » est deplie

    // « Pourquoi cette commande ? » (deplie le detail sous la commande) et case « Commandé » (quantite modifiable)
    void draw_order_controls(App &app, const Json &row, const char *where) {
      const std::string type = row["type"].str();
      if (type.empty() || row["quantity"].integer() <= 0)
        return;
      const std::string key  = std::string(where) + type;
      const bool        open = explained_.count(key) > 0;
      if (row["explain"].is_object()) {
        if (small_button(open ? "Masquer le détail" : "Pourquoi cette commande ?")) {
          if (open)
            explained_.erase(key);
          else
            explained_.insert(key);
        }
        ImGui::SameLine();
      }
      auto found = order_quantities_.find(type);
      if (found == order_quantities_.end())
        found = order_quantities_.emplace(type, row["quantity"].integer()).first;
      bool ordered = false;
      if (ImGui::Checkbox("Commandé :", &ordered) && ordered && found->second > 0)
        app.place_order(type, row["name"].str(), found->second);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      if (ImGui::InputInt("##qty", &found->second))
        found->second = std::max(1, found->second);
      if (open)
        draw_order_explain(row["explain"]);
    }
    std::string event_kind_; // filtre des evenements a venir ('' : tout)
    std::string select_; // onglet a ouvrir (tasks : vue par defaut)

    // ---- À faire : liste de taches de la gestion, par priorite ----
    void draw_tasks(App &app) {
      const Json &data = app.catalog.tasks;
      if (!data.is_object() || data["categories"].size() == 0) {
        ImGui::TextDisabled(app.catalog.loading_tasks ? "Chargement..." : "Liste de tâches indisponible.");
        return;
      }
      ImGui::TextWrapped("Ce qu'il y a à faire pour tenir le stock, le plus important en premier.");
      if (data["count"].integer() == 0)
        status_banner("✔ Rien à faire pour l'instant", colors::green);
      ImGui::BeginChild("tasks_list");
      if (data["count"].integer() == 0)
        draw_placed_orders(app, data["orders_placed"]);
      int rank = 0;
      for (const Json &category : data["categories"].items()) {
        ++rank;
        const std::string key = category["key"].str();
        if (category["count"].integer() == 0) {
          if (key == "orders" && data["count"].integer() > 0)
            draw_placed_orders(app, data["orders_placed"]);
          continue;
        }
        const std::string title = std::to_string(rank) + ". " + category["title"].str() + " ("
                                + std::to_string(category["count"].integer()) + ")";
        ImGui::SeparatorText(title.c_str());
        ImGui::TextDisabled("%s", category["help"].str().c_str());
        int index = 0;
        for (const Json &row : data["tasks"].items()) {
          if (row["category"].str() != key)
            continue;
          ImGui::PushID((key + std::to_string(index++)).c_str());
          draw_task(app, row);
          ImGui::PopID();
        }
        if (key == "orders")
          draw_placed_orders(app, data["orders_placed"]);
      }
      ImGui::EndChild();
    }

    // Commandes passees en attente de livraison : sous les commandes, hors du compte des taches (sauf en retard).
    // Decocher : la commande ne viendra pas (ou erreur), la suggestion revient si elle tient toujours.
    void draw_placed_orders(App &app, const Json &rows) {
      if (!rows.is_array() || rows.size() == 0)
        return;
      ImGui::Spacing();
      ImGui::TextDisabled("Commandes passées, en attente de livraison (%d)", static_cast< int >(rows.size()));
      for (const Json &row : rows.items()) {
        const long long id   = static_cast< long long >(row["id"].num());
        const std::string name = row["name"].str();
        ImGui::PushID(("placed" + std::to_string(id)).c_str());
        bool checked = true;
        const std::string label = std::to_string(row["quantity"].integer()) + " × " + name;
        if (row["late"].boolean())
          ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        if (ImGui::Checkbox(label.c_str(), &checked) && !checked)
          app.cancel_order(id, label);
        if (row["late"].boolean())
          ImGui::PopStyleColor();
        const int days     = row["days"].integer();
        std::string detail = days == 0 ? "commandé aujourd'hui"
                                       : "commandé il y a " + std::to_string(days) + (days == 1 ? " jour" : " jours");
        if (!row["placed_by"].str().empty())
          detail += " par " + row["placed_by"].str();
        if (row["received"].integer() > 0)
          detail += " · reçu " + std::to_string(row["received"].integer()) + "/" + std::to_string(row["quantity"].integer());
        detail += " · se clôt toute seule à la réception";
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::PopID();
      }
    }

    // ---- À venir : evenements, a part des taches, dans l'ordre des dates (retard en tete) ----
    // Une carte encadree par evenement, un titre par mois. Revisions et desinfections : « Réviser » /
    // « Désinfecter » et « Saisir la date » depuis la carte.
    void draw_events(App &app) {
      const Json &data = app.catalog.events;
      static constexpr std::array< int, 3 >          horizons = { 30, 90, 365 };
      static constexpr std::array< const char *, 3 > labels   = { "30 jours", "3 mois", "1 an" };
      ImGui::TextWrapped("Ce qui arrive, dans l'ordre des dates : ce ne sont pas des tâches. Révisions et "
                         "désinfections se font ou se recalent directement depuis leur carte.");
      ImGui::TextUnformatted("Horizon :");
      for (std::size_t index = 0; index < horizons.size(); ++index) {
        ImGui::SameLine();
        const bool active = app.catalog.events_days == horizons[index];
        if (ImGui::RadioButton(labels[index], active) && !active) {
          app.catalog.events_days = horizons[index];
          app.refresh_events();
        }
      }
      if (!data.is_object() || !data["events"].is_array()) {
        ImGui::TextDisabled(app.catalog.loading_events ? "Chargement..." : "Évènements indisponibles.");
        return;
      }
      if (data["kinds"].size() > 1) {
        ImGui::TextUnformatted("Afficher :");
        ImGui::SameLine();
        if (ImGui::RadioButton(("Tout (" + std::to_string(data["count"].integer()) + ")").c_str(), event_kind_.empty()))
          event_kind_.clear();
        for (const Json &kind : data["kinds"].items()) {
          ImGui::SameLine();
          const std::string key   = kind["key"].str();
          const std::string label = kind["label"].str() + " (" + std::to_string(kind["count"].integer()) + ")";
          if (ImGui::RadioButton(label.c_str(), event_kind_ == key))
            event_kind_ = key;
        }
      }
      ImGui::BeginChild("events_list");
      std::string section;
      int         index = 0, shown = 0;
      for (const Json &row : data["events"].items()) {
        if (!event_kind_.empty() && row["kind"].str() != event_kind_)
          continue;
        ++shown;
        const auto        date = Date::parse(row["date"].str());
        const bool        late = row["late"].boolean();
        const std::string key  = late ? "late" : date ? std::to_string(date->year * 100 + date->month) : "";
        if (key != section) {
          section = key;
          static const char *months[] = { "Janvier", "Février", "Mars", "Avril", "Mai", "Juin", "Juillet",
                                          "Août", "Septembre", "Octobre", "Novembre", "Décembre" };
          const std::string title = late ? "En retard"
                                  : date ? std::string(months[(date->month + 11) % 12]) + " " + std::to_string(date->year)
                                         : "";
          ImGui::Spacing();
          if (late)
            ImGui::PushStyleColor(ImGuiCol_Text, colors::red);
          ImGui::SeparatorText(title.c_str());
          if (late)
            ImGui::PopStyleColor();
        }
        ImGui::PushID(("event" + std::to_string(index++)).c_str());
        draw_event(app, row, date);
        ImGui::PopID();
      }
      // a la fin, replies : peremptions par mois
      int group_index = 0;
      for (const Json &group : data["groups"].items()) {
        if (!event_kind_.empty() && group["kind"].str() != event_kind_)
          continue;
        ++shown;
        if (group_index == 0) {
          ImGui::Spacing();
          ImGui::Separator();
        }
        ImGui::PushID(("group" + std::to_string(group_index++)).c_str());
        draw_event_group(app, group);
        ImGui::PopID();
      }
      if (shown == 0)
        status_banner("✔ Rien de prévu sur cet horizon", colors::green);
      ImGui::EndChild();
    }

    // Groupe replie (« Péremptions de décembre : 34 ») : deplie, liste compacte triee par lot, puis type, puis date,
    // un intertitre par lot (bouton : ouvre le lot).
    void draw_event_group(App &app, const Json &group) {
      const std::string level = group["level"].str();
      const std::string title = group["title"].str() + " : " + std::to_string(group["count"].integer()) + "###"
                              + group["key"].str();
      if (level == "warn")
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
      const bool open = ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
      if (level == "warn")
        ImGui::PopStyleColor();
      if (!open)
        return;
      std::string section;
      int         index = 0;
      for (const Json &row : group["rows"].items()) {
        const std::string place = row["section"].str();
        if (!place.empty() && place != section) {
          section = place;
          const std::string lot = row["lot"].str();
          ImGui::PushID(("section" + std::to_string(index)).c_str());
          if (!lot.empty()) {
            if (ImGui::Selectable((place + " ›").c_str(), false, ImGuiSelectableFlags_None))
              app.show_lot(lot);
          } else {
            ImGui::TextColored(colors::grey, "%s", place.c_str());
          }
          ImGui::PopID();
        }
        const std::string row_level = row["level"].str();
        const ImVec4      color     = row_level == "warn"   ? colors::orange
                                    : row_level == "soon" ? colors::yellow
                                                          : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::Indent();
        ImGui::TextColored(color, "%s", row["text"].str().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", row["when"].str().c_str());
        ImGui::Unindent();
        ++index;
      }
      ImGui::TreePop();
    }

    void draw_event(App &app, const Json &row, const std::optional< Date > &date) {
      const std::string level = row["level"].str();
      const ImVec4      color = level == "bad"    ? colors::red
                              : level == "warn" ? colors::orange
                              : level == "soon" ? colors::yellow
                                                : ImGui::GetStyleColorVec4(ImGuiCol_Border);
      ImGui::PushStyleColor(ImGuiCol_Border, color);
      ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, level == "info" ? 1.0f : 2.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
      ImGui::BeginChild("card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
      // date en grand a gauche
      ImGui::BeginGroup();
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
      const std::string day = row["late"].boolean() ? "!" : date ? std::to_string(date->day) : "?";
      ImGui::TextColored(row["late"].boolean() ? colors::red : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", day.c_str());
      ImGui::PopFont();
      static const char *short_months[] = { "janv.", "févr.", "mars", "avr.", "mai", "juin",
                                            "juil.", "août", "sept.", "oct.", "nov.", "déc." };
      ImGui::TextDisabled("%s", row["late"].boolean() ? "retard" : date ? short_months[(date->month + 11) % 12] : "");
      ImGui::EndGroup();
      ImGui::SameLine(ImGui::GetFontSize() * 4.0f);
      ImGui::BeginGroup();
      ImGui::TextColored(level == "info" ? colors::grey : color, "%s",
                         row["kind_label"].str().c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("· %s", row["when"].str().c_str());
      ImGui::TextWrapped("%s", row["title"].str().c_str());
      if (!row["iid"].str().empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", row["iid"].str().c_str());
      }
      if (!row["detail"].str().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", row["detail"].str().c_str());
        ImGui::PopStyleColor();
      }
      const Json       &m   = row["maintenance"];
      const std::string lot = row["lot"].str();
      bool              any = false;
      if (m.is_object()) {
        if (small_button(m["verb"].str("Réviser").c_str()))
          app.open_revision(m, m["kind"].str());
        ImGui::SameLine();
        if (small_button("Saisir la date"))
          app.open_maintenance_date(m, m["kind"].str());
        any = true;
      } else if (row["lot_maintenance"].is_object()) { // desinfection du lot lui-meme
        if (small_button("Désinfecter"))
          app.open_lot_disinfection(row["lot_maintenance"]);
        ImGui::SameLine();
        if (small_button("Saisir la date"))
          app.open_lot_disinfection_date(row["lot_maintenance"]);
        any = true;
      }
      if (row["kind"].str() == "order") {
        if (small_button("Voir les commandes"))
          app.show_stock_tab("orders");
        ImGui::SameLine();
        draw_order_controls(app, row, "event");
      } else if (!lot.empty()) {
        if (any)
          ImGui::SameLine();
        if (small_button("Ouvrir le lot"))
          app.show_lot(lot);
      } else if (row["link"].str() == "lists") {
        if (small_button("Listes de matériel"))
          app.open_window("lists");
      } else if (row["link"].str() == "users" || row["link"].str() == "lot_admin") {
        ImGui::TextDisabled(row["link"].str() == "users" ? "Utilisateurs (menu Administration)" : "Lots (F2), fiche du lot");
      }
      ImGui::EndGroup();
      ImGui::EndChild();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor();
    }

    void draw_task(App &app, const Json &row) {
      const std::string level = row["level"].str();
      const ImVec4      color = level == "bad" ? colors::red : level == "warn" ? colors::orange : colors::grey;
      ImGui::TextColored(color, "●");
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextWrapped("%s", row["title"].str().c_str());
      if (!row["detail"].str().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", row["detail"].str().c_str());
        ImGui::PopStyleColor();
      }
      // ou traiter la tache : fiche du lot, fenetre ou onglet du poste
      const std::string link = row["link"].str();
      const std::string lot  = row["lot"].str();
      if ((link == "lot" || link == "lot_admin") && !lot.empty()) {
        if (small_button("Ouvrir le lot"))
          app.show_lot(lot);
      } else if (link == "item") {
        if (!lot.empty() && small_button("Ouvrir le lot"))
          app.show_lot(lot);
        else if (lot.empty() && small_button("Ouvrir l'entretien"))
          app.show_stock_tab("equipment");
      } else if (link == "consumables") {
        if (small_button("Voir les consommables")) {
          app.show_consumables = true;
          app.open_window("print_counts");
        }
      } else if (link == "forecast") {
        if (small_button(row["category"].str() == "orders" ? "Voir les commandes" : "Voir les transferts"))
          app.show_stock_tab(row["category"].str() == "orders" ? "orders" : "transfers");
        // case « Commandé » d'une commande suggeree (quantite modifiable) : la suggestion attend la livraison
        if (!row["type"].str().empty() && row["quantity"].integer() > 0) {
          ImGui::SameLine();
          draw_order_controls(app, row, "task");
        }
      } else if (link == "stock") {
        if (small_button("Voir l'état du stock"))
          app.show_stock_tab("stock");
      } else {
        static const std::pair< const char *, const char * > windows[] = {
          { "reports", "Signalements" }, { "lists", "Listes de matériel" }, { "users", "Utilisateurs" },
          { "items", "Items" }, { "lots", "Lots" },
        };
        for (const auto &[window_id, name] : windows) {
          // materiel perime, hors lot ou sans rangement dans le stock : recherche des items
          const std::string id = window_id;
          if (id != (link == "inventory" ? std::string("items") : link))
            continue;
          const AppWindow *window = app.window(id);
          if (window && app.can_open(*window)) {
            if (small_button((std::string("Ouvrir ") + name).c_str())) {
              if (id == "items")
                app.show_stock_tab("items");
              else
                app.open_window(id);
            }
          } else
            ImGui::TextDisabled("%s : à faire par un admin", name);
        }
      }
      ImGui::EndGroup();
      ImGui::Spacing();
    }


};

} // namespace

std::unique_ptr< AppWindow > make_stock_window() {
  return std::make_unique< StockWindow >();
}

} // namespace qrprotec
