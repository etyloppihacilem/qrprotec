/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          verif_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "windows.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace qrprotec {

namespace {

std::string iid_label(const App &app, const std::string &iid) {
  const ParsedScan scan = parse_scan(iid);
  std::string      text = app.catalog.item_type_name(scan.item_type);
  if (scan.peremption)
    text += " - exp. " + scan.peremption->display();
  return text;
}

class VerifWindow final : public AppWindow {
  public:
    VerifWindow() : AppWindow("verif", "Vérif", false, true) {}

    bool can_close(const App &app) const override { return !app.verif.active; }

    void draw(App &app) override {
      if (app.verif.active)
        draw_active(app);
      else if (!app.last_report.is_null())
        draw_report(app);
      else
        ImGui::TextWrapped("Aucune vérif en cours. Scannez l'étiquette d'un lot ou choisissez un lot dans la "
                           "fenêtre Lots pour commencer.");
    }

  private:
    void draw_active(App &app) {
      VerifSession &verif = app.verif;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      ImGui::Text("Vérif : %s", app.verif_title().c_str());
      ImGui::PopFont();
      if (verif.loading) {
        ImGui::TextDisabled("Chargement du contenu du lot...");
        return;
      }
      const std::vector< VerifPlanLot > plan  = app.plan_verif();
      const bool                        multi = plan.size() > 1;
      if (!verif.lot["global"].is_null()) {
        const Json &global = verif.lot["global"];
        ImGui::TextDisabled("Lot global %s : %s – vérif la plus ancienne : %s", global["name"].str().c_str(),
                            global["label"].str().c_str(), display_datetime(global["last_verif"]).c_str());
      }
      std::string sealed;
      for (const VerifPlanLot &row : plan)
        if ((*row.lot)["is_sealed"].boolean())
          sealed += (sealed.empty() ? "" : ", ") + (*row.lot)["name"].str();
      if (!sealed.empty())
        status_banner("Scellé (" + sealed + ") : valider cette vérif brisera le scellé", colors::orange, 1.0f);
      if (!verif.key.empty() || (!verif.extras.empty() && app.verif_key_ok()))
        ImGui::TextColored(colors::green, "Étiquette privée scannée : la vérif peut être validée.");
      else if (app.verif_key_ok())
        ImGui::TextColored(colors::green, "Mode gestion : validation autorisée sans étiquette privée.");
      else {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        ImGui::TextWrapped("Scannez l'étiquette privée du lot pour pouvoir valider. %s", key_hint(verif.lot).c_str());
        ImGui::PopStyleColor();
      }
      ImGui::TextWrapped("Scannez chaque item du lot : il passe dans la pile et disparaît de la liste ci-dessous. "
                         "Scannez aussi les items périmés que vous retirez, puis leurs remplaçants.%s",
                         multi ? "" : " L'étiquette privée d'un autre lot du même lot global l'ajoute à la vérif.");

      std::set< std::string > scanned;
      for (const std::string &iid : app.stack.iids())
        scanned.insert(iid);
      std::set< std::string > expected; // items connus des lots de la verif
      for (const VerifPlanLot &row : plan)
        for (const Json &item : (*row.lot)["items"].items())
          expected.insert(item["iid"].str());
      int remaining = 0;

      const auto item_row = [&](const Json &item) {
        ImGui::TableNextRow();
        if (item["expired"].boolean())
          row_color(colors::red, 0.45f);
        ImGui::TableNextColumn();
        ImGui::Indent();
        ImGui::TextUnformatted(item["type_name"].str().c_str());
        if (item["expired"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::red, "(périmé : à remplacer)");
        } else if (item["missed_verifs"].integer() > 0) {
          ImGui::SameLine();
          ImGui::TextDisabled("(non vu à la dernière vérif)");
        }
        ImGui::Unindent();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", item["iid"].str().c_str());
      };

      int added = 0, known_scanned = 0;
      for (const std::string &iid : scanned)
        if (!expected.count(iid))
          ++added;
        else
          ++known_scanned;
      // seulement des items qui ne sont pas dans le lot : probablement un reassort, pas une verif
      const bool                        restock = added > 0 && known_scanned == 0;
      const std::vector< const Json * > partial = app.partial_verif_lots();
      const bool                        orange  = restock || !partial.empty();

      ImGui::SeparatorText("À scanner");
      const float footer = ImGui::GetFrameHeightWithSpacing() * (orange ? 4.4f : 3.2f);
      if (ImGui::BeginTable("expected", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Scannés / attendus", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const VerifPlanLot &plan_row : plan) {
          const Json &lot = *plan_row.lot;
          // items du lot pas encore scannes, par type
          std::map< std::string, std::vector< const Json * > > known_by_type;
          for (const Json &item : lot["items"].items())
            if (!scanned.count(item["iid"].str()))
              known_by_type[item["type"].str()].push_back(&item);
          // verif groupee : titre de chaque lot (un lot qui ne contient rien et n'attend rien n'est qu'un regroupement)
          if (multi) {
            if (lot["requirements"].size() == 0 && lot["items"].size() == 0)
              continue;
            ImGui::TableNextRow();
            row_color(plan_row.complete ? colors::green : colors::grey, 0.55f);
            ImGui::TableNextColumn();
            ImGui::Indent(plan_row.depth * 16.0f + 1.0f);
            ImGui::Text("%s", lot["name"].str().c_str());
            ImGui::Unindent(plan_row.depth * 16.0f + 1.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plan_row.complete ? "complet" : "");
            ImGui::TableNextColumn();
          }
          std::set< std::string > required_types;
          for (const Json &row : lot["requirements"].items()) {
            const std::string type     = row["type"].str();
            const int         required = row["required"].integer();
            const auto        found    = plan_row.fresh.find(type);
            const int         done     = found == plan_row.fresh.end() ? 0 : found->second;
            const int         missing  = std::max(0, required - done);
            required_types.insert(type);
            remaining += missing;
            // ligne du type : progression coloree (rouge = rien, orange = partiel, vert = complet)
            ImGui::TableNextRow();
            row_color(missing == 0 ? colors::green : done == 0 ? colors::red : colors::orange, 0.25f);
            ImGui::TableNextColumn();
            ImGui::Text("%s", row["type_name"].str().c_str());
            if (!row["location"].str().empty()) {
              ImGui::SameLine();
              ImGui::TextColored(colors::grey, "– %s", row["location"].str().c_str());
            }
            ImGui::TableNextColumn();
            if (missing == 0)
              ImGui::TextColored(colors::green, "complet");
            else
              ImGui::TextColored(done == 0 ? colors::red : colors::orange, "encore %d", missing);
            ImGui::TableNextColumn();
            stock_bar(done, required, ImVec2(-1, 0));
            // items connus du lot pour ce type, puis ce qu'il faut ajouter depuis le stock
            int known_fresh = 0;
            for (const Json *item : known_by_type[type]) {
              item_row(*item);
              known_fresh += (*item)["expired"].boolean() ? 0 : 1;
            }
            if (missing > known_fresh) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::Indent();
              ImGui::TextColored(colors::orange, "+ %d à prendre dans le stock", missing - known_fresh);
              ImGui::Unindent();
            }
          }
          // items du lot dont le type n'est pas (ou plus) dans la definition
          bool header = false;
          for (const auto &[type, items] : known_by_type) {
            if (required_types.count(type))
              continue;
            if (!header) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextDisabled("Autres items du lot (hors définition du type de lot)");
              header = true;
            }
            for (const Json *item : items) {
              item_row(*item);
              ++remaining;
            }
          }
        }
        ImGui::EndTable();
      }
      if (remaining == 0)
        status_banner(multi ? "✔ Tout est scanné : les lots seront complets" : "✔ Tout est scanné : le lot sera complet",
                      colors::green, 1.0f);
      else
        status_banner(std::to_string(remaining) + " item(s) encore attendu(s) – " + std::to_string(added)
                        + " nouvel(s) item(s) scanné(s)",
                      colors::red, 1.0f);

      ImGui::BeginDisabled(!app.verif_key_ok() || verif.submitting);
      if (!partial.empty()) {
        std::string names;
        for (const Json *lot : partial)
          names += (names.empty() ? "" : ", ") + (*lot)["name"].str();
        const std::string label = "Vérif partielle : " + names + " – réassort pour le reste";
        if (warning_button(label.c_str(), ImVec2(-1, 0)))
          app.submit_verif(true);
        ImGui::SetItemTooltip("Seuls les lots complets avec les scans sont vérifiés (%s).\n"
                              "Les autres items scannés sont ajoutés à leur lot, qui sera signalé « vérif "
                              "recommandée » ;\nle reste de leur contenu n'est pas touché.",
                              names.c_str());
      } else if (restock) {
        const std::string label = "Ajouter " + std::to_string(added) + " item(s) au lot – réassort, sans vérif";
        if (warning_button(label.c_str(), ImVec2(-1, 0)))
          app.restock_verif();
        ImGui::SetItemTooltip("Aucun item déjà présent n'a été scanné : les items sont simplement ajoutés au lot.\n"
                              "Le lot sera signalé « vérif recommandée » (orange) pour que la personne suivante\n"
                              "fasse une vérif complète.");
      }
      const float width = ImGui::GetContentRegionAvail().x;
      if (primary_button(verif.submitting ? "Envoi..." : orange ? "Valider une vérif complète" : "Valider la vérif",
                         ImVec2(width * 0.45f, 0)))
        app.submit_verif();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(app.stack.empty() || verif.submitting);
      if (ImGui::Button("Annuler le dernier scan", ImVec2(width * 0.3f, 0)))
        app.stack.undo_last();
      ImGui::SetItemTooltip("Retire de la pile le dernier item scanné (erreur de scan).");
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (danger_button("Annuler la vérif", ImVec2(-1, 0)))
        app.cancel_verif();
    }

    void draw_list(const App &app, const char *title, const Json &iids, const ImVec4 &color) {
      if (iids.size() == 0)
        return;
      const std::string header = std::string(title) + " (" + std::to_string(iids.size()) + ")";
      ImGui::TextColored(color, "%s", header.c_str());
      ImGui::Indent();
      for (const Json &iid : iids.items())
        ImGui::BulletText("%s  [%s]", iid_label(app, iid.str()).c_str(), iid.str().c_str());
      ImGui::Unindent();
    }

    void draw_report(App &app) {
      const Json &report = app.last_report;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      ImGui::Text("Compte rendu : %s", app.last_report_lot.c_str());
      ImGui::PopFont();
      const bool several = report["lots"].size() > 1;
      if (report["complete"].boolean())
        status_banner(several ? "✔ VÉRIF ENREGISTRÉE : LOTS COMPLETS" : "✔ VÉRIF ENREGISTRÉE : LOT COMPLET", colors::green,
                      1.4f);
      else if (report["partial"].boolean())
        status_banner("↷ VÉRIF PARTIELLE ENREGISTRÉE – voir ci-dessous", colors::orange, 1.4f);
      else
        status_banner("✘ VÉRIF ENREGISTRÉE : INCOMPLET – voir ci-dessous", colors::red, 1.4f);
      if (report["unsealed"].boolean()) {
        std::string names;
        for (const Json &name : report["unsealed_lots"].items())
          names += (names.empty() ? "" : ", ") + name.str();
        ImGui::TextColored(colors::orange, "Scellé brisé (%s) : resceller depuis la Gestion des lots.",
                           names.empty() ? "lot" : names.c_str());
      }
      ImGui::Text("%zu item(s) présent(s).", report["present"].size());
      // verif groupee : resultat de chaque lot
      if (several)
        for (const Json &row : report["lots"].items()) {
          ImGui::Indent(row["depth"].integer() * 16.0f + 1.0f);
          if (!row["verified"].boolean())
            ImGui::TextColored(colors::orange, "↷ %s : non vérifié%s", row["name"].str().c_str(),
                               row["restocked"].integer() > 0
                                 ? (", " + row["restocked"].str() + " item(s) ajouté(s) en réassort").c_str()
                                 : "");
          else if (row["complete"].boolean())
            ImGui::TextColored(colors::green, "✔ %s : complet", row["name"].str().c_str());
          else
            ImGui::TextColored(colors::red, "✘ %s : incomplet", row["name"].str().c_str());
          ImGui::Unindent(row["depth"].integer() * 16.0f + 1.0f);
        }
      for (const Json &row : report["requirements"].items()) {
        const std::string label = row["lot_name"].str().empty() ? row["type_name"].str()
                                                                : row["lot_name"].str() + " · " + row["type_name"].str();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(320.0f);
        stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(200.0f, 0));
      }
      ImGui::Separator();
      ImGui::BeginChild("report", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
      draw_list(app, "Périmés toujours dans le lot : à remplacer", report["expired"], colors::red);
      draw_list(app, "Périmés considérés comme remplacés", report["replaced"], colors::green);
      // etiquette a dechirer absente : l'ensemble a ete entame, il compte comme utilise
      Json missing = Json::array();
      for (const Json &iid : report["missing"].items()) {
        bool torn = false;
        for (const Json &other : report["torn"].items())
          torn = torn || other.str() == iid.str();
        if (!torn)
          missing.push_back(iid);
      }
      draw_list(app, "Étiquette déchirée : utilisés", report["torn"], colors::orange);
      draw_list(app, "Attendus mais non scannés", missing, colors::orange);
      draw_list(app, "Retrouvés (étaient signalés disparus)", report["reactivated"], colors::green);
      draw_list(app, "Ajoutés à leur lot en réassort (lot non vérifié)", report["restocked"], colors::orange);
      draw_list(app, "Codes inconnus ignorés", report["unknown"], colors::red);
      ImGui::EndChild();
      if (ImGui::Button("Fermer le compte rendu", ImVec2(-1, 0))) {
        app.last_report = Json();
        open            = false;
      }
    }
};

} // namespace

std::unique_ptr< AppWindow > make_verif_window() {
  return std::make_unique< VerifWindow >();
}

} // namespace qrprotec
