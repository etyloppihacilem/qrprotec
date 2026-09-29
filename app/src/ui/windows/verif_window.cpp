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
      ImGui::Text("Vérif : %s", verif.lot["name"].str(verif.lot_id).c_str());
      ImGui::PopFont();
      if (verif.loading) {
        ImGui::TextDisabled("Chargement du contenu du lot...");
        return;
      }
      if (verif.lot["is_sealed"].boolean())
        status_banner("Lot scellé : valider cette vérif brisera le scellé", colors::orange, 1.0f);
      if (!verif.key.empty())
        ImGui::TextColored(colors::green, "Étiquette privée scannée : la vérif peut être validée.");
      else if (app.verif_key_ok())
        ImGui::TextColored(colors::green, "Mode gestion : validation autorisée sans étiquette privée.");
      else
        ImGui::TextColored(colors::orange, "Scannez l'étiquette privée du lot pour pouvoir valider.");
      ImGui::TextWrapped("Scannez chaque item du lot : il passe dans la pile et disparaît de la liste ci-dessous. "
                         "Scannez aussi les items périmés que vous retirez, puis leurs remplaçants.");

      // Attendus : d'abord la definition du type de lot (quantite par type d'item), puis les items deja
      // connus dans le lot. Un type peut etre attendu sans qu'aucun item du lot ne soit connu.
      std::map< std::string, int > fresh_by_type;
      const Date                   today = app.today();
      std::set< std::string >      scanned;
      for (const std::string &iid : app.stack.iids()) {
        scanned.insert(iid);
        const ParsedScan scan = parse_scan(iid);
        if (!is_expired(scan, today))
          ++fresh_by_type[scan.item_type];
      }
      std::map< std::string, std::vector< const Json * > > known_by_type; // items du lot pas encore scannes
      std::set< std::string >                               expected;
      for (const Json &item : verif.lot["items"].items()) {
        expected.insert(item["iid"].str());
        if (!scanned.count(item["iid"].str()))
          known_by_type[item["type"].str()].push_back(&item);
      }
      std::set< std::string > required_types;
      int                     remaining = 0;

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

      ImGui::SeparatorText("À scanner");
      const float footer = ImGui::GetFrameHeightWithSpacing() * 3.2f;
      if (ImGui::BeginTable("expected", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Scannés / attendus", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : verif.lot["requirements"].items()) {
          const std::string type     = row["type"].str();
          const int         required = row["required"].integer();
          const int         done     = fresh_by_type[type];
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
        ImGui::EndTable();
      }
      int added = 0;
      for (const std::string &iid : scanned)
        if (!expected.count(iid))
          ++added;
      if (remaining == 0)
        status_banner("✔ Tout est scanné : le lot sera complet", colors::green, 1.0f);
      else
        status_banner(std::to_string(remaining) + " item(s) encore attendu(s) – " + std::to_string(added)
                        + " nouvel(s) item(s) scanné(s)",
                      colors::red, 1.0f);

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
        status_banner("✔ VÉRIF ENREGISTRÉE : LOT COMPLET", colors::green, 1.4f);
      else
        status_banner("✘ VÉRIF ENREGISTRÉE : LOT INCOMPLET – voir ci-dessous", colors::red, 1.4f);
      if (report["unsealed"].boolean())
        ImGui::TextColored(colors::orange, "Le scellé du lot a été brisé : resceller le lot depuis la Gestion des lots.");
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
