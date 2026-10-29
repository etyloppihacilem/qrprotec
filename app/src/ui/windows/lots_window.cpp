/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lots_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "../../core/plural.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace qrprotec {

namespace {

// Couleur d'un etat calcule par le serveur (lot global) : "ok", "warn" ou "bad"
ImVec4 kind_color(const std::string &kind) {
  return kind == "ok" ? colors::green : kind == "warn" ? colors::orange : colors::red;
}

// Libelle court pour une cellule (sans le symbole ni le detail apres la virgule)
std::string short_label(const std::string &label) {
  std::string text = label.substr(0, label.find(','));
  for (const char *symbol : { "✔ ", "✘ ", "⚠ " })
    if (text.rfind(symbol, 0) == 0)
      text = text.substr(std::string(symbol).size());
  return text;
}

// Vue publique des lots : ce qui est perime, ce qui manque, et lancement d'une verif.
class LotsWindow final : public AppWindow {
  public:
    LotsWindow() : AppWindow("lots", "Lots", false, true) {}

    void on_open(App &app) override { app.refresh_lots(); }

    void draw(App &app) override {
      // liste rechargee (Rafraichir, fin de verif...) : la fiche affichee est rechargee aussi
      if (seen_lots_version_ != app.catalog.lots_version) {
        seen_lots_version_ = app.catalog.lots_version;
        if (!selected_.empty())
          select(app, selected_);
      }
      if (const std::string id = app.take_lot_to_show(); !id.empty())
        select(app, id, app.take_seal_to_show()); // etiquette publique ou scelle scanne
      // rangements du stock : affiches par defaut en gestion et admin, masques sinon (a chaque changement de role)
      if (const int privileged = app.privileged() ? 1 : 0; privileged != seen_privileged_) {
        seen_privileged_ = privileged;
        show_storage_    = privileged == 1;
      }
      if (app.take_lots_to_treat()) { // voyant de la barre de menu
        only_problems_ = true;
        show_storage_  = false; // comme le voyant, qui ne compte pas les rangements du stock
        filter_.clear();
      }
      if (button("Rafraîchir"))
        app.refresh_lots();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(250.0f);
      ImGui::InputTextWithHint("##filter", "Filtrer", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("Seulement les lots à traiter", &only_problems_);
      ImGui::SameLine();
      ImGui::Checkbox("Afficher les rangements du stock", &show_storage_);
      if (app.catalog.loading_lots) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }

      const float details_width = selected_.empty() ? 0.0f : ImGui::GetContentRegionAvail().x * 0.45f;
      ImGui::BeginChild("list", ImVec2(selected_.empty() ? 0.0f : -details_width, 0));
      // rangements du stock dans leur propre tableau, sous celui des lots, seulement si la case est cochee
      if (show_storage_) {
        draw_table(app, false, ImGui::GetContentRegionAvail().y * 0.6f);
        ImGui::SeparatorText("Rangements du stock");
        draw_table(app, true, 0.0f);
      }
      else
        draw_table(app, false, 0.0f);
      ImGui::EndChild();
      if (!selected_.empty()) {
        ImGui::SameLine();
        ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
        draw_details(app);
        ImGui::EndChild();
      }
    }

  private:
    // storage : tableau des rangements du stock, sinon celui des lots ; height 0 = toute la place restante
    void draw_table(App &app, bool storage, float height) {
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      // fiche ouverte ou fenetre etroite (a cote des Signalements) : on masque les colonnes secondaires
      const bool compact = !selected_.empty() || ImGui::GetContentRegionAvail().x < 700.0f;
      const std::string table_id = std::string(storage ? "storage" : "lots") + (compact ? "_compact" : "");
      if (!ImGui::BeginTable(table_id.c_str(), compact ? 3 : 5, flags, ImVec2(0.0f, height)))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn(storage ? "Rangement" : "Lot", ImGuiTableColumnFlags_WidthStretch);
      if (!compact)
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
      ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, compact ? 70.0f : 80.0f);
      ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, compact ? 150.0f : 160.0f);
      if (!compact)
        ImGui::TableSetupColumn("Dernière vérif", ImGuiTableColumnFlags_WidthFixed, 180.0f);
      ImGui::TableHeadersRow();
      // lots a traiter en haut (rouges, orange, puis peremption ou revision proche), chacun suivi de ses sous-lots :
      // ordre calcule par le serveur (serializers.attention_order)
      std::vector< const Json * > lots;
      for (const Json &lot : app.catalog.lots.items())
        lots.push_back(&lot);
      std::stable_sort(lots.begin(), lots.end(), [](const Json *a, const Json *b) {
        return (*a)["attention_order"].integer() < (*b)["attention_order"].integer();
      });
      for (const Json *lot_row : lots) {
        const Json       &lot  = *lot_row;
        const std::string id   = lot["id"].str();
        const std::string name = lot["name"].str();
        const int         expired = lot["expired_count"].integer();
        const int         soon    = lot["expiring_soon_count"].integer();
        const int         revisions = lot["revision_late_count"].integer() + lot["revision_soon_count"].integer();
        const LotStatus   status   = lot_status(lot);
        const int         depth    = lot["depth"].integer();
        // lot global : son etat est celui de l'ensemble de ses lots (tous valides, verif la plus ancienne)
        const bool        top      = depth == 0 && !lot["global"].is_null();
        const ImVec4      color    = top ? kind_color(lot["global"]["kind"].str()) : lot_status_color(status);
        const bool        ok       = top ? lot["global"]["kind"].str() == "ok" : lot_ok(status);
        if (!search_matches(filter_, name + " " + id + " " + lot["lot_type_name"].str()))
          continue;
        // rangements dans leur tableau ; case decochee : le rangement ouvert (ex : etiquette scannee) reste ici
        if (storage ? !lot["storage"].boolean()
                    : lot["storage"].boolean() && (show_storage_ || id != selected_))
          continue;
        if (only_problems_ && ok && soon == 0 && revisions == 0)
          continue;
        ImGui::TableNextRow();
        // vert = verifie et complet, rouge sinon (incomplet, perimes, jamais verifie)
        row_color(color, ok ? 0.22f : 0.30f);
        ImGui::TableNextColumn();
        ImGui::Indent(depth * 18.0f + 1.0f);
        const std::string label = (depth > 0 ? "└ " : "") + name + "##" + id;
        if (ImGui::Selectable(label.c_str(), selected_ == id, ImGuiSelectableFlags_SpanAllColumns))
          select(app, id);
        ImGui::Unindent(depth * 18.0f + 1.0f);
        if (!compact) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s%s", lot["lot_type_name"].str().c_str(),
                              top ? " · lot global" : lot["storage"].boolean() ? " · rangement" : "");
        }
        if (revisions > 0) {
          ImGui::SameLine();
          ImGui::TextColored(colors::orange, "· %d à réviser", revisions);
          ImGui::SetItemTooltip("Matériel dont la révision est en retard ou dans les 30 jours");
        }
        if (!compact) {
          ImGui::TableNextColumn();
          ImGui::Text("%d", lot["item_count"].integer());
        }
        ImGui::TableNextColumn();
        if (expired > 0)
          ImGui::TextColored(colors::red, "%d", expired);
        else if (soon > 0)
          ImGui::TextColored(colors::orange, "%d bientôt", soon);
        else
          ImGui::TextUnformatted("0");

        ImGui::TableNextColumn();
        // cellule d'etat en couleur pleine
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImGui::GetColorU32(color));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (top)
          ImGui::TextUnformatted(short_label(lot["global"]["label"].str()).c_str());
        else
          ImGui::TextUnformatted(lot_status_label(status));
        ImGui::PopStyleColor();
        if (top)
          ImGui::SetItemTooltip("Lot global : valide si tous ses lots le sont.\nPropre état : %s",
                                lot_status_label(status));
        if (!compact) {
          ImGui::TableNextColumn();
          // lot global : la plus ancienne des dernieres verifs de ses lots
          ImGui::TextUnformatted(display_datetime(top ? lot["global"]["last_verif"] : lot["last_verif"]).c_str());
        }
      }
      ImGui::EndTable();
    }

    void select(App &app, const std::string &id, const std::string &seal = {}) {
      selected_   = id;
      details_    = Json();
      confirm_used_ = false;
      seal_check_.clear();
      const std::string query = seal.empty() ? "" : "?seal=" + url_encode(seal);
      app.api.get("/api/lots/" + url_encode(id) + "/" + query, [this, &app, id](const ApiResult &result) {
        if (selected_ != id)
          return;
        if (result.ok) {
          details_    = result.data;
          seal_check_ = result.data["seal_check"].str();
        }
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
      ImGui::TextDisabled("%s - %s%s", details_["lot_type_name"].str().c_str(), details_["id"].str().c_str(),
                          details_["storage"].boolean() ? " - rangement du stock" : "");
      if (details_["path"].size() > 0) {
        std::string path;
        for (const Json &parent : details_["path"].items())
          path += (path.empty() ? "" : " › ") + parent["name"].str();
        ImGui::Text("Dans : %s", path.c_str());
      }
      ImGui::Text("Dernière vérif : %s par %s", display_datetime(details_["last_verif"], true).c_str(),
                  details_["last_verif_by"].str("-").c_str());
      if (!details_["next_revision"].is_null()) {
        const int  late = details_["revision_late_count"].integer();
        const bool warn = late > 0 || details_["revision_soon_count"].integer() > 0;
        const std::string text = "Prochaine révision : " + display_date(details_["next_revision"])
                               + (late > 0 ? " · " + nb(late, "révision") + " en retard" : "");
        if (warn)
          ImGui::TextColored(colors::orange, "⚠ %s", text.c_str());
        else
          ImGui::TextUnformatted(text.c_str());
      }
      const LotStatus status = lot_status(details_);
      // QR code de scelle scanne : resultat du controle
      if (!seal_check_.empty()) {
        if (seal_check_ == "valid")
          status_banner("✔ SCELLÉ INTACT : pas de vérif nécessaire", colors::green);
        else if (seal_check_ == "wrong")
          status_banner("✘ ÉTIQUETTE D'UN ANCIEN SCELLÉ", colors::red);
        else
          status_banner("✘ SCELLÉ BRISÉ : vérif nécessaire", colors::red);
      }
      if (lot_is_group(details_)) {
        ImVec4            color;
        const std::string banner = group_banner(details_, color);
        status_banner(banner, color);
      } else {
        status_banner(lot_status_banner(details_), lot_status_color(status));
      }
      if (details_["is_sealed"].boolean()) {
        ImGui::TextWrapped("Scellé %s par %s. Tant que le scellé est intact, le lot n'a pas besoin de vérif.",
                           display_on(details_["sealed"], true).c_str(), details_["sealed_by"].str("-").c_str());
      } else if (!details_["unsealed"].is_null()) {
        ImGui::TextDisabled("Dernier scellé brisé %s par %s", display_on(details_["unsealed"], true).c_str(),
                            details_["unsealed_by"].str("-").c_str());
      }
      const std::size_t sub_lots = details_["descendants"].size();
      const std::string with     = sub_lots == 1 ? " avec son sous-lot"
                                 : sub_lots > 1 ? " avec ses " + nb(sub_lots, "sous-lot") : "";
      if (details_["is_sealed"].boolean()) {
        if (button(("Lancer une vérif" + with + " (brise le scellé)").c_str(), ImVec2(-1, 0)))
          app.start_verif(details_["id"].str(), "", app.lot_label_scanned(details_["id"].str()));
      } else if (primary_button(("Lancer une vérif" + with).c_str(), ImVec2(-1, 0)))
        app.start_verif(details_["id"].str(), "", app.lot_label_scanned(details_["id"].str()));
      if (report_button("⚠ Signaler un problème sur ce lot", ImVec2(-1, 0)))
        app.report_problem({ details_["id"].str(), details_["name"].str(), "", "", details_["name"].str() });
      // tout secouriste : vérif recommandee pour les suivants (pas pour un lot scelle : etiquette d'ouverture)
      if (!details_["is_sealed"].boolean() && !details_["storage"].boolean()) {
        if (!confirm_used_) {
          if (button("Marquer ce lot comme utilisé (demander une vérif)", ImVec2(-1, 0)))
            confirm_used_ = true;
        } else {
          ImGui::TextWrapped("Les suivants verront « vérif recommandée » jusqu'à la prochaine vérif complète.");
          if (warning_button("Confirmer : lot utilisé", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
            app.mark_lot_used(details_["id"].str(), details_["name"].str());
            confirm_used_ = false;
          }
          ImGui::SameLine();
          if (button("Annuler", ImVec2(-1, 0)))
            confirm_used_ = false;
        }
      }
      if (button("Fermer", ImVec2(-1, 0)))
        selected_.clear();
      draw_global(app);

      ImGui::SeparatorText("Contenu attendu");
      if (ImGui::BeginTable("req", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();
        // types incomplets en haut, puis ceux qui ont des perimes, puis 0/0, puis les complets (ordre de la definition a
        // egalite)
        std::vector< const Json * > requirements;
        for (const Json &row : details_["requirements"].items())
          requirements.push_back(&row);
        const auto requirement_rank = [](const Json &row) {
          const int present = row["present"].integer(), required = row["required"].integer();
          return present < required ? 0 : row["expired"].integer() > 0 ? 1 : (present <= 0 && required <= 0) ? 2 : 3;
        };
        // emplacements (pochettes) dans l'ordre du type de lot, « sans emplacement » en dernier : un titre chacun
        std::vector< std::string > places;
        for (const Json *requirement : requirements)
          if (std::find(places.begin(), places.end(), (*requirement)["location"].str()) == places.end())
            places.push_back((*requirement)["location"].str());
        std::stable_partition(places.begin(), places.end(), [](const std::string &place) { return !place.empty(); });
        const bool grouped = !places.empty() && !places.front().empty();
        if (!grouped)
          places = { "" };
        std::stable_sort(requirements.begin(), requirements.end(), [&](const Json *a, const Json *b) {
          return requirement_rank(*a) < requirement_rank(*b);
        });
        // emplacements a traiter en haut (rang de leur type le plus a traiter), l'ordre de la TECH a egalite
        const auto place_rank = [&](const std::string &place) {
          for (const Json *requirement : requirements) // deja tries : le premier de l'emplacement donne son rang
            if ((*requirement)["location"].str() == place)
              return requirement_rank(*requirement);
          return 3;
        };
        std::stable_sort(places.begin(), places.end(),
                         [&](const std::string &a, const std::string &b) { return place_rank(a) < place_rank(b); });
        for (const std::string &place : places) {
          if (grouped) {
            int present = 0, required = 0;
            for (const Json *requirement : requirements)
              if ((*requirement)["location"].str() == place) {
                present += std::min((*requirement)["present"].integer(), (*requirement)["required"].integer());
                required += (*requirement)["required"].integer();
              }
            ImGui::TableNextRow();
            row_color(present < required ? colors::orange : colors::green, 0.15f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(place.empty() ? "Sans emplacement" : place.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d / %d", present, required);
            ImGui::TableNextColumn();
          }
          for (const Json *requirement : requirements) {
            const Json &row = *requirement;
            if (grouped && row["location"].str() != place)
              continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (grouped)
              ImGui::Indent(12.0f);
            ImGui::TextUnformatted(row["type_name"].str().c_str());
            if (grouped)
              ImGui::Unindent(12.0f);
            ImGui::TableNextColumn();
            stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(-1, 0));
            ImGui::TableNextColumn();
            const int expired = row["expired"].integer();
            if (expired > 0)
              ImGui::TextColored(colors::red, "%d", expired);
            else
              ImGui::TextUnformatted("-");
          }
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
        ProblemTarget report;
        // items a traiter en haut : a remplacer (perime, desactive, inutilisable), disparus, signales ou revision en
        // retard, bientot perimes, puis le reste
        struct ItemRow {
            const Json *item;
            bool        missing;
            int         rank;
        };
        std::vector< ItemRow > rows;
        for (const Json &item : details_["items"].items()) {
          const auto date = Date::parse(item["peremption"].str());
          const int  rank = item["expired"].boolean()                                     ? 0
                          : item["reported"].boolean() || item["revision_late"].boolean() ? 2
                          : date && *date <= soon                                         ? 3
                                                                                          : 4;
          rows.push_back({ &item, false, rank });
        }
        for (const Json &item : details_["missing_items"].items())
          rows.push_back({ &item, true, 1 });
        std::stable_sort(rows.begin(), rows.end(), [](const ItemRow &a, const ItemRow &b) { return a.rank < b.rank; });
        for (const ItemRow &entry : rows) {
          const Json &item = *entry.item;
          if (entry.missing) {
            ImGui::TableNextRow();
            row_color(colors::grey, 0.3f);
            ImGui::TableNextColumn();
            ImGui::Text("%s (disparu)", item["type_name"].str().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", item["iid"].str().c_str());
            continue;
          }
          const auto date = Date::parse(item["peremption"].str());
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.4f);
          else if (date && *date <= soon)
            row_color(colors::orange, 0.3f);
          ImGui::TableNextColumn();
          ImGui::PushID(item["iid"].str().c_str());
          if (report_mark("##report"))
            report = { details_["id"].str(), details_["name"].str(), item["iid"].str(), "",
                       item["type_name"].str() + " (" + item["iid"].str() + ")" };
          ImGui::PopID();
          ImGui::SameLine();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          if (item["disabled"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "(désactivé)");
          } else if (item["unusable"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "(signalé inutilisable)");
          } else if (item["revision_late"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(item["expired"].boolean() ? colors::red : colors::orange, "(révision en retard)");
          } else if (item["reported"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::orange, "(problème signalé)");
          }
          if (item["missed_verifs"].integer() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(non vu à la dernière vérif)");
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(date ? date->display().c_str() : "-");
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", item["iid"].str().c_str());
        }
        ImGui::EndTable();
        if (!report.label.empty())
          app.report_problem(report);
      }
    }

    // Lot global (depuis n'importe lequel de ses lots) : etat de l'ensemble et de chaque lot
    void draw_global(App &app) {
      const Json &global = details_["global"];
      if (global.is_null())
        return;
      ImGui::SeparatorText(("Lot global : " + global["name"].str()).c_str());
      status_banner(global["label"].str(), kind_color(global["kind"].str()), 1.0f);
      ImGui::TextWrapped("Vérif la plus ancienne : %s", global["never_verified"].boolean()
                                                          ? "jamais (un lot n'a jamais été vérifié)"
                                                          : display_datetime(global["last_verif"], true).c_str());
      std::string open_id;
      if (ImGui::BeginTable("global", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Lot", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (const Json &row : global["lots"].items()) {
          const std::string id      = row["id"].str();
          const bool        counted = row["counted"].boolean();
          const Json       &state   = row["effective"];
          ImGui::TableNextRow();
          if (id == details_["id"].str())
            row_color(colors::grey, 0.25f);
          ImGui::TableNextColumn();
          ImGui::Indent(row["depth"].integer() * 16.0f + 1.0f);
          const std::string label = (row["depth"].integer() > 0 ? "└ " : "") + row["name"].str() + "##g" + id;
          if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
            open_id = id;
          ImGui::Unindent(row["depth"].integer() * 16.0f + 1.0f);
          ImGui::TableNextColumn();
          if (counted) {
            ImGui::TextColored(kind_color(state["kind"].str()), "%s", state["label"].str().c_str());
            ImGui::TextDisabled("vérif : %s", display_datetime(row["last_verif"]).c_str());
          } else {
            ImGui::TextDisabled("regroupement");
          }
        }
        ImGui::EndTable();
      }
      if (!open_id.empty() && open_id != details_["id"].str())
        select(app, open_id);
    }

    std::string filter_;
    bool        only_problems_ = false;
    bool        show_storage_  = false;
    int         seen_privileged_ = -1;
    std::string selected_;
    int         seen_lots_version_ = -1;
    bool        confirm_used_      = false; // « Marquer ce lot comme utilisé » : confirmation affichee
    Json        details_;
    std::string seal_check_; // resultat du QR de scelle scanne ("valid", "wrong", "unsealed")
};

} // namespace

std::unique_ptr< AppWindow > make_lots_window() {
  return std::make_unique< LotsWindow >();
}

} // namespace qrprotec
