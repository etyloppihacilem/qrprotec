/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          verif_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "windows.hpp"

#include <map>
#include <set>

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
      ImGui::Text("Vérif : %s", verif.lot["name"].str(verif.lot_id).c_str());
      ImGui::PopFont();
      if (verif.loading) {
        ImGui::TextDisabled("Chargement du contenu du lot...");
        return;
      }
      if (!verif.key.empty())
        ImGui::TextColored(colors::green, "Étiquette privée scannée : la vérif peut être validée.");
      else if (app.verif_key_ok())
        ImGui::TextColored(colors::green, "Mode responsable : validation autorisée sans étiquette privée.");
      else
        ImGui::TextColored(colors::orange, "Scannez l'étiquette privée du lot pour pouvoir valider.");
      ImGui::TextWrapped("Scannez chaque item du lot : il passe dans la pile et disparaît de la liste ci-dessous. "
                         "Scannez aussi les items périmés que vous retirez, puis leurs remplaçants.");

      // Progression par type : items frais scannes vs quantite exigee
      std::map< std::string, int > fresh_by_type;
      const Date                   today = app.today();
      for (const std::string &iid : app.stack.iids()) {
        const ParsedScan scan = parse_scan(iid);
        if (!is_expired(scan, today))
          ++fresh_by_type[scan.item_type];
      }
      ImGui::SeparatorText("Exigences du lot");
      if (ImGui::BeginTable("req", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Scannés / attendus", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : verif.lot["requirements"].items()) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["type_name"].str().c_str());
          ImGui::TableNextColumn();
          stock_bar(fresh_by_type[row["type"].str()], row["required"].integer(), ImVec2(-1, 0));
        }
        ImGui::EndTable();
      }

      // Items attendus (connus dans le lot) pas encore scannes
      std::set< std::string > expected;
      int                     remaining = 0;
      ImGui::SeparatorText("Items attendus");
      const float footer = ImGui::GetFrameHeightWithSpacing() * 2.5f;
      if (ImGui::BeginTable("expected", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const Json &item : verif.lot["items"].items()) {
          const std::string iid = item["iid"].str();
          expected.insert(iid);
          if (app.stack.contains_iid(iid))
            continue;
          ++remaining;
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.45f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          if (item["missed_verifs"].integer() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(non vu à la dernière vérif)");
          }
          if (item["expired"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "(périmé : à remplacer)");
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", iid.c_str());
        }
        ImGui::EndTable();
      }
      int added = 0;
      for (const std::string &iid : app.stack.iids())
        if (!expected.count(iid))
          ++added;
      ImGui::Text("%d attendu(s) restant(s), %d nouvel(s) item(s) scanné(s).", remaining, added);

      ImGui::BeginDisabled(!app.verif_key_ok() || verif.submitting);
      if (primary_button(verif.submitting ? "Envoi..." : "Valider la vérif", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0)))
        app.submit_verif();
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
      if (report["complete"].boolean())
        ImGui::TextColored(colors::green, "Lot complet et à jour.");
      else
        ImGui::TextColored(colors::red, "Lot incomplet ou contenant des périmés : voir ci-dessous.");
      ImGui::Text("%zu item(s) présent(s).", report["present"].size());
      for (const Json &row : report["requirements"].items()) {
        ImGui::TextUnformatted(row["type_name"].str().c_str());
        ImGui::SameLine(250.0f);
        stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(200.0f, 0));
      }
      ImGui::Separator();
      ImGui::BeginChild("report", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
      draw_list(app, "Périmés toujours dans le lot : à remplacer", report["expired"], colors::red);
      draw_list(app, "Périmés considérés comme remplacés", report["replaced"], colors::green);
      draw_list(app, "Attendus mais non scannés", report["missing"], colors::orange);
      draw_list(app, "Retrouvés (étaient signalés disparus)", report["reactivated"], colors::green);
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
