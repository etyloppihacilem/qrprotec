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
#include <sstream>
#include <vector>

namespace qrprotec {

namespace {


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
    }

    void draw(App &app) override {
      if (button("Rafraîchir"))
        on_open(app);
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
      // a la fin, replies : peremptions par mois et cartes d'invites a activer apres l'horizon
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
      if (group["kind"].str() == "guest_card")
        ImGui::TextDisabled("Utilisateurs (menu Administration) : à remettre à une équipe, ou à désactiver.");
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
      if (!lot.empty()) {
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
        const std::string type = row["type"].str();
        if (!type.empty() && row["quantity"].integer() > 0) {
          auto found = order_quantities_.find(type);
          if (found == order_quantities_.end())
            found = order_quantities_.emplace(type, row["quantity"].integer()).first;
          ImGui::SameLine();
          bool ordered = false;
          if (ImGui::Checkbox("Commandé :", &ordered) && ordered && found->second > 0)
            app.place_order(type, row["name"].str(), found->second);
          ImGui::SameLine();
          ImGui::SetNextItemWidth(110.0f);
          if (ImGui::InputInt("##qty", &found->second))
            found->second = std::max(1, found->second);
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
