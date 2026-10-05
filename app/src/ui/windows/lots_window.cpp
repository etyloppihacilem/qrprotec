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
      if (ImGui::Button("Rafraîchir"))
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
      // fiche ouverte : la liste est etroite, on masque les colonnes secondaires
      const bool compact = !selected_.empty();
      if (!ImGui::BeginTable(compact ? "lots_compact" : "lots", compact ? 3 : 5, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Lot", ImGuiTableColumnFlags_WidthStretch);
      if (!compact)
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
      ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, compact ? 70.0f : 80.0f);
      ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, compact ? 150.0f : 160.0f);
      if (!compact)
        ImGui::TableSetupColumn("Dernière vérif", ImGuiTableColumnFlags_WidthFixed, 180.0f);
      ImGui::TableHeadersRow();
      for (const Json &lot : app.catalog.lots.items()) {
        const std::string id   = lot["id"].str();
        const std::string name = lot["name"].str();
        const int         expired = lot["expired_count"].integer();
        const int         soon    = lot["expiring_soon_count"].integer();
        const LotStatus   status   = lot_status(lot);
        const int         depth    = lot["depth"].integer();
        // lot global : son etat est celui de l'ensemble de ses lots (tous valides, verif la plus ancienne)
        const bool        top      = depth == 0 && !lot["global"].is_null();
        const ImVec4      color    = top ? kind_color(lot["global"]["kind"].str()) : lot_status_color(status);
        const bool        ok       = top ? lot["global"]["kind"].str() == "ok" : lot_ok(status);
        if (!search_matches(filter_, name + " " + id + " " + lot["lot_type_name"].str()))
          continue;
        // rangements du stock masques par defaut (sauf le lot ouvert, ex : etiquette scannee)
        if (!show_storage_ && lot["storage"].boolean() && id != selected_)
          continue;
        if (only_problems_ && ok && soon == 0)
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
      ImGui::Text("Dernière vérif : %s par %s", display_datetime(details_["last_verif"]).c_str(),
                  details_["last_verif_by"].str("-").c_str());
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
        ImGui::TextWrapped("Scellé le %s par %s. Tant que le scellé est intact, le lot n'a pas besoin de vérif.",
                           display_datetime(details_["sealed"]).c_str(), details_["sealed_by"].str("-").c_str());
      } else if (!details_["unsealed"].is_null()) {
        ImGui::TextDisabled("Dernier scellé brisé le %s par %s", display_datetime(details_["unsealed"]).c_str(),
                            details_["unsealed_by"].str("-").c_str());
      }
      const std::size_t sub_lots = details_["descendants"].size();
      const std::string with     = sub_lots > 0 ? " avec ses " + std::to_string(sub_lots) + " sous-lot(s)" : "";
      if (details_["is_sealed"].boolean()) {
        if (ImGui::Button(("Lancer une vérif" + with + " (brise le scellé)").c_str(), ImVec2(-1, 0)))
          app.start_verif(details_["id"].str(), "");
      } else if (primary_button(("Lancer une vérif" + with).c_str(), ImVec2(-1, 0)))
        app.start_verif(details_["id"].str(), "");
      if (ImGui::Button("Fermer", ImVec2(-1, 0)))
        selected_.clear();
      draw_global(app);

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

    // Lot global (depuis n'importe lequel de ses lots) : etat de l'ensemble et de chaque lot
    void draw_global(App &app) {
      const Json &global = details_["global"];
      if (global.is_null())
        return;
      ImGui::SeparatorText(("Lot global : " + global["name"].str()).c_str());
      status_banner(global["label"].str(), kind_color(global["kind"].str()), 1.0f);
      ImGui::TextWrapped("Vérif la plus ancienne : %s", global["never_verified"].boolean()
                                                          ? "jamais (un lot n'a jamais été vérifié)"
                                                          : display_datetime(global["last_verif"]).c_str());
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
    Json        details_;
    std::string seal_check_; // resultat du QR de scelle scanne ("valid", "wrong", "unsealed")
};

} // namespace

std::unique_ptr< AppWindow > make_lots_window() {
  return std::make_unique< LotsWindow >();
}

} // namespace qrprotec
