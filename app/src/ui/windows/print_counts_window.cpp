/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_counts_window.cpp
        -\-    _|__
         |\___/  . \        Created on 12 Nov. 2026 at 17:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../core/paths.hpp"
#include "../../core/plural.hpp"
#include "../../core/template_io.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qrprotec {

namespace {

const char *const kMonths[12] = { "Janvier", "Février", "Mars",      "Avril",   "Mai",      "Juin",
                                  "Juillet", "Août",    "Septembre", "Octobre", "Novembre", "Décembre" };

std::string mm(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0) / 10.0);
  std::string result = text;
  for (char &c : result)
    if (c == '.')
      c = ',';
  return result;
}

// « 40 × 30 mm · direct » : en-tete de colonne (le libelle complet est dans l'infobulle)
std::string short_label(const Json &category) {
  return mm(category["width_mm"].num()) + " × " + mm(category["height_mm"].num()) + " mm · "
         + (category["support"].str() == "transfer" ? "transfert" : "direct");
}

std::string thousands(long long value) {
  std::string digits = std::to_string(value);
  for (int i = static_cast< int >(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast< std::size_t >(i), " "); // espace fine insecable
  return digits;
}

void right_aligned(const std::string &text, bool disabled = false) {
  const float width = ImGui::CalcTextSize(text.c_str()).x;
  const float space = ImGui::GetContentRegionAvail().x - width;
  if (space > 0.0f)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + space);
  if (disabled)
    ImGui::TextDisabled("%s", text.c_str());
  else
    ImGui::TextUnformatted(text.c_str());
}

// Impressions et consommables (Gestion, fronts seulement) :
// - Impressions : etiquettes envoyees a l'imprimante par tous les fronts, par mois calendaire et par categorie
//   d'etiquette (taille et support), total de l'annee ;
// - Consommables : stock estime des bobines et rubans (inventory/consumables.py), autonomie, couts, receptions et
//   inventaires ;
// - References : bobines et rubans achetes (boites de X bobines de Y etiquettes, rouleaux de ruban de N metres).
// En haut, le rouleau en place sur cet ordinateur, choisi sans passer par l'avertissement de changement de rouleau.
class PrintCountsWindow final : public AppWindow {
  public:
    PrintCountsWindow() : AppWindow("print_counts", "Impressions et consommables", true, true) {}

    void on_open(App &app) override {
      scan_label_types();
      load(app, year_);
      load_consumables(app);
    }

    void reset_view(App &) override {
      year_ = 0;
      edit_ = Json();
      move_ = MoveForm{};
    }

    void draw(App &app) override {
      draw_loaded_roll(app);
      if (!ImGui::BeginTabBar("print_counts_tabs"))
        return;
      const bool to_stock = app.show_consumables;
      app.show_consumables = false;
      if (tab_item("Impressions")) {
        draw_counts(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Consommables", to_stock ? ImGuiTabItemFlags_SetSelected : 0)) {
        draw_stock(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Références")) {
        draw_refs(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // Rouleau en place : choisi ici (gestion et admin) sans avertissement ; la file ne se met en pause que pour un
    // modele d'un autre type
    void draw_loaded_roll(App &app) {
      const std::string loaded = app.settings.print.loaded_label_type;
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Rouleau en place sur cet ordinateur :");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
      if (ImGui::BeginCombo("##loaded_roll", loaded.empty() ? "aucun (format libre)" : loaded.c_str())) {
        for (const LabelType &type : label_types_) {
          if (ImGui::Selectable(type.name.c_str(), type.name == loaded))
            app.set_loaded_label_type(type.name);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", type.summary().c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      help_marker("Le rouleau réellement chargé dans l'imprimante. Changez-le ici quand vous remettez un rouleau "
                  "(par exemple les étiquettes consommables) : pas d'avertissement. Une impression d'un autre type "
                  "s'arrête et demande de changer de rouleau.");
      ImGui::Separator();
    }

    void draw_counts(App &app) {
      const Json &years = data_["years"];
      const int   year  = data_["year"].integer();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
      if (ImGui::BeginCombo("Année", year > 0 ? std::to_string(year).c_str() : "")) {
        for (const Json &entry : years.items()) {
          const int value = entry.integer();
          if (ImGui::Selectable(std::to_string(value).c_str(), value == year))
            load(app, value);
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (button(loading_ ? "Chargement..." : "Actualiser"))
        load(app, year_);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("Une étiquette compte quand elle est envoyée à l'imprimante (pas en cas d'erreur ni "
                          "d'annulation), depuis tous les ordinateurs. Survolez une colonne pour voir ses types "
                          "d'étiquettes.");
      ImGui::PopTextWrapPos();
      if (const int waiting = app.print_counter.pending(); waiting > 0)
        ImGui::TextColored(colors::orange, "%s sur cet ordinateur, pas encore envoyée%s au serveur.",
                           nb(waiting, "étiquette imprimée", "étiquettes imprimées").c_str(), waiting > 1 ? "s" : "");
      if (!error_.empty())
        ImGui::TextColored(colors::red, "%s", error_.c_str());
      ImGui::Spacing();

      const Json &categories = data_["categories"];
      const int   count      = static_cast< int >(categories.size());
      if (count == 0) {
        if (!loading_ && error_.empty())
          ImGui::TextDisabled("Aucune étiquette imprimée en %d.", year > 0 ? year : 0);
        return;
      }
      const bool  with_total    = count > 1;
      const int   current_month = data_["current_month"].integer(); // 0 : annee passee
      const Json &totals        = data_["months_total"];
      // moyenne par mois : mois finis depuis le premier mois compte (le compteur a demarre en cours d'annee) ; le
      // mois en cours seulement s'il est le seul
      int first_month = 0;
      while (first_month < 11 && totals[first_month].integer() == 0)
        ++first_month;
      int last_month = current_month > 0 ? current_month - 1 : 12; // dernier mois fini (1 a 12)
      if (last_month <= first_month)
        last_month = current_month > 0 ? current_month : 12;
      const int months_counted = std::max(1, last_month - first_month);

      if (!ImGui::BeginTable("print_counts", 1 + count + (with_total ? 1 : 0),
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter
                               | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit))
        return;
      ImGui::TableSetupScrollFreeze(1, 1);
      ImGui::TableSetupColumn("Mois", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
      for (const Json &category : categories.items())
        ImGui::TableSetupColumn(short_label(category).c_str(), ImGuiTableColumnFlags_WidthStretch);
      if (with_total)
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch);
      // en-tetes : libelle complet et types d'etiquettes des modeles au survol
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      for (int column = 0; column < ImGui::TableGetColumnCount(); ++column) {
        ImGui::TableSetColumnIndex(column);
        ImGui::TableHeader(ImGui::TableGetColumnName(column));
        if (column >= 1 && column <= count && ImGui::IsItemHovered()) {
          const Json &category = categories[column - 1];
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(category["label"].str().c_str());
          const std::string names = type_names(category);
          if (!names.empty())
            ImGui::TextDisabled("Types d'étiquettes : %s", names.c_str());
          ImGui::EndTooltip();
        }
      }
      for (int month = 0; month < 12; ++month) {
        ImGui::TableNextRow();
        const bool future = current_month > 0 && month + 1 > current_month;
        if (month + 1 == current_month)
          row_color(colors::blue, 0.25f);
        ImGui::TableNextColumn();
        if (future)
          ImGui::TextDisabled("%s", kMonths[month]);
        else
          ImGui::TextUnformatted(month + 1 == current_month ? (std::string(kMonths[month]) + " (en cours)").c_str()
                                                             : kMonths[month]);
        for (int column = 0; column <= count; ++column) {
          if (column == count && !with_total)
            break;
          ImGui::TableNextColumn();
          if (future)
            continue;
          const int value = column < count ? categories[column]["months"][month].integer() : totals[month].integer();
          right_aligned(value > 0 ? thousands(value) : "–", value == 0);
        }
      }
      // total de l'annee
      ImGui::TableNextRow();
      row_color(colors::grey, 0.30f);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(("Total " + std::to_string(year)).c_str());
      for (const Json &category : categories.items()) {
        ImGui::TableNextColumn();
        right_aligned(thousands(category["total"].integer()));
      }
      if (with_total) {
        ImGui::TableNextColumn();
        right_aligned(thousands(data_["total"].integer()));
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("Moyenne par mois");
      if (ImGui::IsItemHovered())
      {
        if (months_counted > 1)
          ImGui::SetTooltip("Sur %d mois, de %s à %s (le mois en cours n'est pas fini).", months_counted,
                            kMonths[first_month], kMonths[last_month - 1]);
        else
          ImGui::SetTooltip("Sur 1 mois : %s.", kMonths[first_month]);
      }
      for (int column = 0; column <= count; ++column) {
        if (column == count && !with_total)
          break;
        ImGui::TableNextColumn();
        long long total = 0;
        for (int month = first_month; month < last_month; ++month)
          total += column < count ? categories[column]["months"][month].integer() : totals[month].integer();
        right_aligned(thousands(std::lround(static_cast< double >(total) / std::max(1, months_counted))), true);
      }
      ImGui::EndTable();
    }

    // -----------------------------------------------------------------------------------------------------------
    // Consommables : stock estime par categorie, receptions et inventaires

    struct MoveForm {
        int  ref       = 0;
        bool inventory = false;
        int  boxes     = 0;
        int  rolls     = 0;
        int  loose     = 0;
    };

    static ImVec4 level_color(const std::string &level) {
      return level == "bad" ? colors::red : level == "warn" ? colors::orange : level == "ok" ? colors::green : colors::grey;
    }

    static std::string euros(const Json &value) {
      if (value.is_null())
        return "–";
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f €", value.num());
      std::string result = text;
      for (char &c : result)
        if (c == '.')
          c = ',';
      return result;
    }

    const Json *find_ref(int id) const {
      for (const Json &ref : consumables_["refs"].items())
        if (ref["id"].integer() == id)
          return &ref;
      return nullptr;
    }

    void draw_stock(App &app) {
      if (button(consumables_loading_ ? "Chargement..." : "Actualiser##stock"))
        load_consumables(app);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Alerte « Commander » sous");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
      ImGui::InputInt("##alert_days", &alert_days_);
      alert_days_ = std::clamp(alert_days_, 1, 365);
      ImGui::SameLine();
      ImGui::TextUnformatted("jours de stock");
      if (alert_days_ != consumables_["alert_days"].integer() && consumables_.contains("alert_days")) {
        ImGui::SameLine();
        if (primary_button("Enregistrer##alert")) {
          Json body;
          body["alert_days"] = alert_days_;
          send(app, "PATCH", "/api/consumables/", body, "Seuil d'alerte enregistré.");
        }
      }
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("Stock estimé : dernier inventaire (ou première réception) + réceptions − étiquettes "
                          "imprimées depuis le lendemain. Le ruban s'use de la hauteur de chaque étiquette de "
                          "transfert + %g mm. Consommation : moyenne des %d derniers jours.",
                          consumables_["ribbon_gap_mm"].num(3.0), consumables_["rate_days"].integer(90));
      ImGui::PopTextWrapPos();
      if (!consumables_error_.empty())
        ImGui::TextColored(colors::red, "%s", consumables_error_.c_str());

      const Json &pools = consumables_["pools"];
      if (pools.size() == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Aucune référence : ajoutez vos bobines et rubans dans l'onglet Références.");
      } else if (ImGui::BeginTable("consumable_pools", 5,
                                   ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter)) {
        const std::string year = std::to_string(data_["year"].integer());
        ImGui::TableSetupColumn("Catégorie", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Stock estimé", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Par mois", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Coût / mois", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Depuis janvier", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (const Json &pool : pools.items()) {
          ImGui::TableNextRow();
          const std::string level = pool["level"].str();
          if (level != "ok")
            row_color(level_color(level), 0.30f);
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", pool["label"].str().c_str());
          ImGui::PushStyleColor(ImGuiCol_Text, level == "ok" ? colors::green : ImGui::GetStyleColorVec4(ImGuiCol_Text));
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextUnformatted(pool["state"].str().c_str());
          ImGui::PopTextWrapPos();
          ImGui::PopStyleColor();
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", pool["stock_text"].str().empty() ? "?" : pool["stock_text"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", pool["monthly_text"].str().empty() ? "–" : pool["monthly_text"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(euros(pool["monthly_cost"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", pool["year_used_text"].str().c_str());
          if (!pool["year_cost"].is_null())
            ImGui::TextDisabled("%s", euros(pool["year_cost"]).c_str());
        }
        ImGui::EndTable();
      }

      ImGui::Spacing();
      ImGui::SeparatorText("Réception ou inventaire");
      draw_move_form(app);

      ImGui::Spacing();
      ImGui::SeparatorText("Derniers mouvements");
      const Json &moves = consumables_["moves"];
      if (moves.size() == 0) {
        ImGui::TextDisabled("Aucune réception ni aucun inventaire.");
        return;
      }
      if (!ImGui::BeginTable("consumable_moves", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        return;
      ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
      ImGui::TableSetupColumn("Mouvement", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthStretch, 2.0f);
      ImGui::TableSetupColumn("Par", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
      ImGui::TableHeadersRow();
      for (const Json &move : moves.items()) {
        ImGui::TableNextRow();
        ImGui::PushID(move["id"].integer());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_datetime(move["created"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s : %s", move["kind_label"].str().c_str(), move["ref_name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s (%s)", move["what"].str().c_str(), move["units_text"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(move["by"].str().c_str());
        ImGui::TableNextColumn();
        if (confirm_button("Annuler", "Annuler ce mouvement ? Le stock estimé sera recalculé sans lui.",
                           "cancel_move"))
          send(app, "POST", "/api/consumables/moves/" + std::to_string(move["id"].integer()) + "/delete/",
               Json::object(), "Mouvement annulé.");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    void draw_move_form(App &app) {
      const Json *ref = find_ref(move_.ref);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      if (ImGui::BeginCombo("Référence", ref ? (*ref)["name"].str().c_str() : "choisir...")) {
        for (const Json &row : consumables_["refs"].items()) {
          if (row["archived"].boolean())
            continue;
          if (ImGui::Selectable((row["name"].str() + "##" + std::to_string(row["id"].integer())).c_str(),
                                row["id"].integer() == move_.ref))
            move_.ref = row["id"].integer();
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", row["pool_label"].str().c_str(), row["summary"].str().c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Réception", !move_.inventory))
        move_.inventory = false;
      ImGui::SameLine();
      if (ImGui::RadioButton("Inventaire", move_.inventory))
        move_.inventory = true;
      if (!ref)
        return;
      const bool  ribbon = (*ref)["kind"].str() == "ribbon";
      const float width  = ImGui::GetFontSize() * 7.0f;
      ImGui::SetNextItemWidth(width);
      ImGui::InputInt("boîtes", &move_.boxes);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(width);
      ImGui::InputInt(ribbon ? "rouleaux" : "bobines", &move_.rolls);
      if (move_.inventory) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        ImGui::InputInt(ribbon ? "mètres en plus (rouleau entamé)" : "étiquettes en plus (bobine entamée)",
                        &move_.loose);
      }
      move_.boxes = std::max(0, move_.boxes);
      move_.rolls = std::max(0, move_.rolls);
      move_.loose = std::max(0, move_.loose);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", (*ref)["summary"].str().c_str());
      if (move_.inventory)
        ImGui::TextDisabled("Inventaire : comptez tout ce qui reste de « %s » (toutes références de cette catégorie) "
                            "; le stock estimé repart de ce compte. Une bobine entamée : estimez à peu près.",
                            (*ref)["pool_label"].str().c_str());
      ImGui::PopTextWrapPos();
      const bool empty = move_.boxes == 0 && move_.rolls == 0 && move_.loose == 0;
      if (!move_.inventory && empty)
        ImGui::BeginDisabled();
      if (primary_button(move_.inventory ? "Enregistrer l'inventaire" : "Enregistrer la réception")) {
        Json body;
        body["ref"]   = move_.ref;
        body["kind"]  = move_.inventory ? "inventory" : "reception";
        body["boxes"] = move_.boxes;
        body["rolls"] = move_.rolls;
        body["loose"] = move_.inventory ? move_.loose : 0;
        send(app, "POST", "/api/consumables/moves/", body,
             move_.inventory ? "Inventaire enregistré." : "Réception enregistrée.");
        move_.boxes = move_.rolls = move_.loose = 0;
      }
      if (!move_.inventory && empty)
        ImGui::EndDisabled();
    }

    // -----------------------------------------------------------------------------------------------------------
    // References : bobines et rubans achetes

    void draw_refs(App &app) {
      ImGui::Checkbox("Voir les références archivées", &show_archived_);
      ImGui::SameLine();
      if (button("+ Nouvelle référence")) {
        edit_            = Json::object();
        edit_["kind"]    = "labels";
        edit_["support"] = "direct";
        edit_["per_box"] = 1;
        price_.clear();
      }
      const Json &refs = consumables_["refs"];
      if (ImGui::BeginTable("consumable_refs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH,
                            ImVec2(0, edit_.is_null() ? 0.0f : ImGui::GetContentRegionAvail().y * 0.4f))) {
        ImGui::TableSetupColumn("Référence", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Catégorie", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Conditionnement", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableHeadersRow();
        for (const Json &ref : refs.items()) {
          const bool archived = ref["archived"].boolean();
          if (archived && !show_archived_)
            continue;
          ImGui::TableNextRow();
          ImGui::PushID(ref["id"].integer());
          if (archived)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s%s", ref["name"].str().c_str(), archived ? " (archivée)" : "");
          if (!ref["supplier"].str().empty())
            ImGui::TextDisabled("%s", ref["supplier"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", ref["pool_label"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", ref["summary"].str().c_str());
          if (archived)
            ImGui::PopStyleColor();
          ImGui::TableNextColumn();
          if (small_button("Modifier")) {
            edit_  = ref;
            price_ = ref["box_price"].is_null() ? std::string() : euros(ref["box_price"]);
            if (!price_.empty())
              price_ = price_.substr(0, price_.find(' '));
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (refs.size() == 0)
        ImGui::TextDisabled("Aucune référence pour l'instant.");
      if (!edit_.is_null())
        draw_ref_form(app);
    }

    void draw_ref_form(App &app) {
      const bool creating = !edit_.contains("id");
      ImGui::SeparatorText(creating ? "Nouvelle référence" : "Modifier la référence");
      if (creating) {
        if (ImGui::RadioButton("Bobines d'étiquettes", edit_["kind"].str() == "labels"))
          edit_["kind"] = "labels";
        ImGui::SameLine();
        if (ImGui::RadioButton("Rubans de transfert", edit_["kind"].str() == "ribbon"))
          edit_["kind"] = "ribbon";
      }
      const bool  ribbon = edit_["kind"].str() == "ribbon";
      const float width  = ImGui::GetFontSize() * 7.0f;
      std::string name     = edit_["name"].str();
      std::string supplier = edit_["supplier"].str();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      if (input_limited("Nom", name, 80, ribbon ? "ex. Cire-résine 57 mm x 74 m" : "ex. Vélin 40 x 30"))
        edit_["name"] = name;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      if (input_limited("Fournisseur", supplier, 80))
        edit_["supplier"] = supplier;
      if (!ribbon) {
        // categorie : comme les types d'etiquettes des modeles (largeur = sens de la tete)
        if (!label_types_.empty()) {
          ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
          if (ImGui::BeginCombo("##from_type", "Reprendre un type d'étiquette...")) {
            for (const LabelType &type : label_types_)
              if (ImGui::Selectable(type.summary().c_str())) {
                edit_["width_mm"]  = type.width_mm;
                edit_["height_mm"] = type.height_mm;
                edit_["support"]   = label_support_id(type.support);
              }
            ImGui::EndCombo();
          }
        }
        float label_w = static_cast< float >(edit_["width_mm"].num(40.0));
        float label_h = static_cast< float >(edit_["height_mm"].num(30.0));
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputFloat("largeur (mm)", &label_w, 0.0f, 0.0f, "%g"))
          edit_["width_mm"] = static_cast< double >(label_w);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputFloat("hauteur (mm)", &label_h, 0.0f, 0.0f, "%g"))
          edit_["height_mm"] = static_cast< double >(label_h);
        if (!edit_.contains("width_mm"))
          edit_["width_mm"] = static_cast< double >(label_w);
        if (!edit_.contains("height_mm"))
          edit_["height_mm"] = static_cast< double >(label_h);
        ImGui::SameLine();
        if (ImGui::RadioButton("thermique direct", edit_["support"].str() != "transfer"))
          edit_["support"] = "direct";
        ImGui::SameLine();
        if (ImGui::RadioButton("transfert thermique", edit_["support"].str() == "transfer"))
          edit_["support"] = "transfer";
        int per_roll = edit_["per_roll"].integer(1000);
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputInt("étiquettes par bobine", &per_roll, 100))
          edit_["per_roll"] = std::max(1, per_roll);
        if (!edit_.contains("per_roll"))
          edit_["per_roll"] = per_roll;
      } else {
        float length = static_cast< float >(edit_["length_m"].num(74.0));
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputFloat("mètres par rouleau", &length, 0.0f, 0.0f, "%g"))
          edit_["length_m"] = static_cast< double >(length);
        if (!edit_.contains("length_m"))
          edit_["length_m"] = static_cast< double >(length);
      }
      int per_box = edit_["per_box"].integer(1);
      ImGui::SetNextItemWidth(width);
      if (ImGui::InputInt(ribbon ? "rouleaux par boîte" : "bobines par boîte", &per_box))
        edit_["per_box"] = std::max(1, per_box);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(width);
      input_limited("prix d'une boîte (€, facultatif)", price_, 12, "ex. 57,00");

      Json body = edit_;
      std::string price = price_;
      for (char &c : price)
        if (c == ',')
          c = '.';
      body["box_price"] = price;
      if (primary_button(creating ? "Ajouter la référence" : "Enregistrer")) {
        if (creating)
          send(app, "POST", "/api/consumables/refs/", body, "Référence ajoutée.", true);
        else
          send(app, "PATCH", "/api/consumables/refs/" + std::to_string(edit_["id"].integer()) + "/", body,
               "Référence enregistrée.", true);
      }
      ImGui::SameLine();
      if (!creating) {
        const bool archived = edit_["archived"].boolean();
        Json       change;
        change["archived"] = !archived;
        if (button(archived ? "Désarchiver" : "Archiver"))
          send(app, "PATCH", "/api/consumables/refs/" + std::to_string(edit_["id"].integer()) + "/", change,
               archived ? "Référence désarchivée." : "Référence archivée (plus proposée aux réceptions).", true);
        ImGui::SameLine();
      }
      if (button("Annuler##ref"))
        edit_ = Json();
    }

    void load_consumables(App &app) {
      consumables_loading_ = true;
      const int request    = ++consumables_request_;
      app.api.get("/api/consumables/", [this, request](const ApiResult &result) {
        if (request != consumables_request_)
          return;
        consumables_loading_ = false;
        if (!result.ok) {
          consumables_error_ = "Consommables : " + result.error;
          return;
        }
        apply_consumables(result.data);
      });
    }

    void apply_consumables(const Json &data) {
      consumables_error_.clear();
      consumables_ = data;
      alert_days_  = data["alert_days"].integer(60);
    }

    // POST / PATCH qui renvoie le resume des consommables
    void send(App &app, const std::string &method, const std::string &path, const Json &body,
              const std::string &done, bool close_form = false) {
      const int request = ++consumables_request_;
      auto callback = [this, &app, request, done, close_form](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        if (close_form)
          edit_ = Json();
        app.notify(done);
        if (request == consumables_request_)
          apply_consumables(result.data);
      };
      if (method == "PATCH")
        app.api.patch(path, body, callback);
      else
        app.api.post(path, body, callback);
    }

    void load(App &app, int year) {
      loading_          = true;
      const int request = ++request_;
      app.api.get("/api/print-counts/" + (year > 0 ? "?year=" + std::to_string(year) : std::string()),
                  [this, request, year](const ApiResult &result) {
                    if (request != request_)
                      return;
                    loading_ = false;
                    if (!result.ok) {
                      error_ = "Compteur d'impressions : " + result.error;
                      return;
                    }
                    error_.clear();
                    data_ = result.data;
                    year_ = year;
                  });
    }

    // types d'etiquettes des modeles (Editeur d'etiquettes), pour nommer les categories
    void scan_label_types() {
      std::vector< TemplateDocument > documents;
      for (const auto &path : glob_templates("*.qr")) {
        TemplateDocument document;
        std::string      error;
        if (load_template(document, path.string(), error))
          documents.push_back(std::move(document));
      }
      label_types_ = collect_label_types(documents);
    }

    std::string type_names(const Json &category) const {
      const auto        same = [](double a, double b) { return std::fabs(a - b) < 0.05; };
      const std::string support = category["support"].str();
      std::string       names;
      for (const LabelType &type : label_types_) {
        if (!same(type.width_mm, category["width_mm"].num()) || !same(type.height_mm, category["height_mm"].num())
            || support != label_support_id(type.support))
          continue;
        names += (names.empty() ? "" : ", ") + type.name;
      }
      return names;
    }

    Json                     data_ = Json::object();
    Json                     consumables_ = Json::object();
    std::string              consumables_error_;
    bool                     consumables_loading_ = false;
    int                      consumables_request_ = 0;
    int                      alert_days_          = 60;
    MoveForm                 move_;
    Json                     edit_; // reference en cours de creation ou de modification (null : aucune)
    std::string              price_;
    bool                     show_archived_ = false;
    std::vector< LabelType > label_types_;
    std::string              error_;
    int                      year_    = 0; // 0 : annee en cours
    bool                     loading_ = false;
    int                      request_ = 0;
};

} // namespace

std::unique_ptr< AppWindow > make_print_counts_window() {
  return std::make_unique< PrintCountsWindow >();
}

} // namespace qrprotec
