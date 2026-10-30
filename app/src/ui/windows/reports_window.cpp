/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          reports_window.cpp
        -\-    _|__
         |\___/  . \        Created on 14 Oct. 2026 at 10:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "windows.hpp"

#include "imgui_stdlib.h"

namespace qrprotec {

namespace {

ImVec4 report_color(const Json &report) {
  const std::string color = report["color"].str();
  return color == "red" ? colors::red : color == "orange" ? colors::orange : colors::green;
}

std::string report_label(const Json &report) {
  const std::string status = report["status"].str();
  if (status == "new")
    return "Nouveau";
  if (status == "resolved") {
    const std::string resolution = report["resolution"].str();
    return resolution == "ok"         ? "Résolu · remis en service"
           : resolution == "disabled" ? "Résolu · item désactivé"
           : resolution == "removed"  ? "Résolu · item retiré"
                                      : "Résolu";
  }
  return report["usable"].boolean() ? "En cours · utilisable" : "En cours · inutilisable";
}

// Gestion des signalements de problemes (admins) : nouveau (rouge), en cours de resolution (orange si l'objet est
// inutilisable, vert s'il l'est toujours), resolu (vert). Meme page que « Signalements » sur le front web.
class ReportsWindow final : public AppWindow {
  public:
    ReportsWindow() : AppWindow("reports", "Signalements", true, true) { admin_only = true; }

    void on_open(App &app) override { load(app); }

    void reset_view(App &) override {
      selected_ = 0;
      all_      = false;
      note_.clear();
    }

    void draw(App &app) override {
      if (ImGui::RadioButton("En cours", !all_)) {
        all_ = false;
        load(app);
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Tous", all_)) {
        all_ = true;
        load(app);
      }
      ImGui::SameLine();
      if (button(loading_ ? "Chargement..." : "Actualiser"))
        load(app);
      ImGui::SameLine();
      ImGui::TextDisabled("%d nouveau(x), %d en cours", app.report_counts["new"].integer(),
                          app.report_counts["progress"].integer());
      ImGui::TextDisabled("Rouge : nouveau. Orange : en cours, objet inutilisable (à remplacer). Vert : en cours et "
                          "toujours utilisable, ou résolu.");

      const float detail = selected() ? ImGui::GetFrameHeightWithSpacing() * 10.0f : 0.0f;
      // fenetre etroite (a cote des Lots) : date et auteur seulement dans le detail
      const bool compact = ImGui::GetContentRegionAvail().x < 800.0f;
      if (ImGui::BeginTable(compact ? "reports_compact" : "reports", compact ? 3 : 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, -detail))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, compact ? 150.0f : 190.0f);
        if (!compact)
          ImGui::TableSetupColumn("Signalé", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Objet", ImGuiTableColumnFlags_WidthStretch);
        if (!compact)
          ImGui::TableSetupColumn("Par", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Commentaire", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const Json &report : reports_.items()) {
          const int id = report["id"].integer();
          ImGui::TableNextRow();
          row_color(report_color(report), id == selected_ ? 0.55f : 0.25f);
          ImGui::TableNextColumn();
          ImGui::PushID(id);
          if (ImGui::Selectable(report_label(report).c_str(), id == selected_,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
            selected_ = id;
            note_     = report["note"].str();
          }
          ImGui::PopID();
          if (!compact) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(display_datetime(report["created"]).c_str());
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(report["target"].str().c_str());
          if (!report["item"].str().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", report["item"].str().c_str());
          }
          if (!compact) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(report["by"].str().c_str());
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(first_line(report["comment"].str()).c_str());
        }
        ImGui::EndTable();
      }
      if (reports_.size() == 0 && !loading_)
        ImGui::TextDisabled(all_ ? "Aucun signalement." : "Aucun signalement en cours.");
      draw_detail(app);
    }

  private:
    static std::string first_line(const std::string &text) {
      const std::size_t end = text.find('\n');
      return end == std::string::npos ? text : text.substr(0, end) + "…";
    }

    const Json *selected() const {
      for (const Json &report : reports_.items())
        if (report["id"].integer() == selected_)
          return &report;
      return nullptr;
    }

    void draw_detail(App &app) {
      const Json *found = selected();
      if (!found)
        return;
      const Json report = *found; // copie : la liste peut etre rechargee pendant le dessin
      ImGui::SeparatorText(report["target"].str().c_str());
      ImGui::TextColored(report_color(report), "%s", report_label(report).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("signalé %s par %s – %s", display_on(report["created"], true).c_str(),
                          report["by"].str().c_str(),
                          report["usable"].boolean() ? "toujours utilisable" : "inutilisable : à remplacer");
      if (!report["comment"].str().empty())
        ImGui::TextWrapped("« %s »", report["comment"].str().c_str());
      if (!report["updated"].is_null())
        ImGui::TextDisabled("Mis à jour %s par %s", display_on(report["updated"], true).c_str(),
                            report["updated_by"].str("-").c_str());
      ImGui::SetNextItemWidth(-200.0f);
      ImGui::InputTextWithHint("##note", "Réponse visible de tous (facultatif)", &note_);
      ImGui::SameLine();
      Json changes;
      if (button("Enregistrer la réponse", ImVec2(-1, 0)))
        changes["note"] = note_;
      const float third = (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
      if (report["status"].str() == "resolved") {
        if (button("Rouvrir", ImVec2(-1, 0)))
          changes["status"] = "progress";
      } else {
        if (primary_button("En cours · utilisable", ImVec2(third, 0))) {
          changes["status"] = "progress";
          changes["usable"] = true;
        }
        ImGui::SetItemTooltip("Pris en charge, l'objet reste disponible (vert).");
        ImGui::SameLine();
        if (warning_button("En cours · inutilisable", ImVec2(third, 0))) {
          changes["status"] = "progress";
          changes["usable"] = false;
        }
        ImGui::SetItemTooltip("Pris en charge, l'objet n'est pas disponible (orange) : un item est à remplacer.");
        if (report["item"].str().empty()) {
          ImGui::SameLine();
          if (primary_button("Résolu", ImVec2(-1, 0)))
            changes["status"] = "resolved";
        } else {
          // item non conforme : desactive (remplace a la prochaine verif comme un perime) ou deja retire (jete)
          if (primary_button("Résolu · remis en service", ImVec2(third, 0))) {
            changes["status"]     = "resolved";
            changes["resolution"] = "ok";
          }
          ImGui::SetItemTooltip("L'item est conforme ou réparé : il redevient normal.");
          ImGui::SameLine();
          if (confirm_button("Résolu · item désactivé", "Désactiver l'item ? Il devra être remplacé comme un périmé.",
                             "confirm_disable", ImVec2(third, 0))) {
            changes["status"]     = "resolved";
            changes["resolution"] = "disabled";
          }
          ImGui::SetItemTooltip("Item non conforme : invalide comme un périmé, remplacé à la prochaine vérif.");
          ImGui::SameLine();
          if (confirm_button("Résolu · item retiré", "Retirer l'item de l'inventaire (déjà jeté) ?", "confirm_remove",
                             ImVec2(-1, 0))) {
            changes["status"]     = "resolved";
            changes["resolution"] = "removed";
          }
          ImGui::SetItemTooltip("L'item a déjà été jeté : il est marqué supprimé.");
        }
      }
      if (!changes.is_null())
        update(app, report["id"].integer(), changes);
    }

    void update(App &app, int id, Json body) {
      body["user"] = app.user_ref();
      app.api.post("/api/reports/" + std::to_string(id) + "/", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Signalement : " + result.error, true);
          return;
        }
        app.notify("Signalement mis à jour.");
        load(app);
      });
    }

    void load(App &app) {
      Json body;
      body["user"]  = app.user_ref();
      body["show"]  = all_ ? "all" : "open";
      loading_      = true;
      const int request = ++request_;
      app.api.post("/api/reports/list/", body, [this, &app, request](const ApiResult &result) {
        if (request != request_)
          return;
        loading_ = false;
        if (!result.ok) {
          app.notify("Signalements : " + result.error, true);
          return;
        }
        reports_          = result.data["reports"];
        app.report_counts = result.data["counts"];
      });
    }

    Json        reports_ = Json::array();
    int   selected_ = 0;
    std::string note_;
    bool        all_     = false;
    bool        loading_ = false;
    int         request_ = 0;
};

} // namespace

std::unique_ptr< AppWindow > make_reports_window() {
  return std::make_unique< ReportsWindow >();
}

} // namespace qrprotec
