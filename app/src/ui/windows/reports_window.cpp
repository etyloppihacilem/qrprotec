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

// Ce qu'il faut retenir de l'objet : en cours, utilisable ou non ; resolu, ce que la resolution en a fait (un
// signalement resolu ne dit plus « inutilisable » : l'objet est de nouveau utilisable, ou desactive / retire)
std::string report_state_text(const Json &report) {
  if (report["status"].str() != "resolved")
    return report["usable"].boolean() ? "toujours utilisable" : "inutilisable : à remplacer";
  const std::string resolution = report["resolution"].str();
  return resolution == "disabled"  ? "résolu : item désactivé, remplacé à la prochaine vérif"
         : resolution == "removed" ? "résolu : item retiré de l'inventaire"
                                   : "résolu : de nouveau utilisable";
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
            if (id != selected_)
              note_ = report["note"].str(); // la reponse deja donnee, a modifier avant le bouton suivant
            selected_ = id;
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

    enum class Kind { Plain, Primary, Warning, Disable, Remove };

    // Un choix : son bouton sur 1/columns de la largeur, et dessous une petite ligne grise qui dit ce qu'il fait.
    // last : dernier de la rangee (sinon le suivant se met a cote).
    bool choice_button(Kind kind, const char *label, const char *hint, int columns, bool last) {
      if (row_width_ < 0.0f)
        row_width_ = ImGui::GetContentRegionAvail().x;
      const float width   = (row_width_ - static_cast<float>(columns - 1) * ImGui::GetStyle().ItemSpacing.x) /
                            static_cast<float>(columns);
      const ImVec2 size(width, 0.0f);
      bool         pressed = false;
      ImGui::BeginGroup();
      switch (kind) {
      case Kind::Primary:
        pressed = primary_button(label, size);
        break;
      case Kind::Warning:
        pressed = warning_button(label, size);
        break;
      case Kind::Disable:
        pressed = confirm_button(label, "Désactiver l'item ? Il devra être remplacé comme un périmé.",
                                 "confirm_disable", size);
        break;
      case Kind::Remove:
        pressed = confirm_button(label, "Retirer l'item de l'inventaire (déjà jeté) ?", "confirm_remove", size);
        break;
      case Kind::Plain:
        pressed = button(label, size);
        break;
      }
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
      ImGui::TextDisabled("%s", hint);
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      if (last)
        row_width_ = -1.0f;
      else
        ImGui::SameLine();
      return pressed;
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
                          report_state_text(report).c_str());
      if (!report["comment"].str().empty())
        ImGui::TextWrapped("« %s »", report["comment"].str().c_str());
      draw_report_replies(report);
      if (!report["updated"].is_null())
        ImGui::TextDisabled("Mis à jour %s par %s", display_on(report["updated"], true).c_str(),
                            report["updated_by"].str("-").c_str());
      // reponse enregistree par le bouton choisi : « En cours » (remplace la precedente) ou « Résolu » (resolution)
      Json        changes;
      const char *text_key = nullptr;
      if (report["status"].str() != "resolved") {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##note", "Réponse visible de tous (facultatif), enregistrée avec le bouton choisi",
                                 &note_);
      }
      // deux blocs bien separes : « En cours » met seulement a jour, « Résolu » clot le signalement ; sous chaque
      // bouton, une petite ligne grise sur ce qu'il fait
      if (report["status"].str() == "resolved") {
        ImGui::SeparatorText("Signalement clos");
        if (choice_button(Kind::Plain, "Rouvrir", "Le signalement repasse en cours, à traiter de nouveau.", 1, true))
          changes["status"] = "progress";
      } else {
        ImGui::SeparatorText("Mettre à jour · le signalement reste ouvert");
        if (choice_button(Kind::Primary, "En cours · utilisable", "Pris en charge, l'objet reste disponible (vert).", 2,
                          false)) {
          changes["status"] = "progress";
          changes["usable"] = true;
          text_key          = "note";
        }
        if (choice_button(Kind::Warning, "En cours · inutilisable",
                          "Pris en charge, l'objet ne doit pas être utilisé (orange).", 2, true)) {
          changes["status"] = "progress";
          changes["usable"] = false;
          text_key          = "note";
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Clore le signalement");
        if (report["item"].str().empty()) {
          if (choice_button(Kind::Primary, "Résolu", "Problème réglé : l'objet redevient utilisable.", 1, true)) {
            changes["status"] = "resolved";
            text_key          = "resolution_note";
          }
        } else {
          // item non conforme : desactive (remplace a la prochaine verif comme un perime) ou deja retire (jete)
          if (choice_button(Kind::Primary, "Résolu · remis en service",
                            "L'item est conforme ou réparé : il redevient utilisable.", 3, false)) {
            changes["status"]     = "resolved";
            changes["resolution"] = "ok";
            text_key              = "resolution_note";
          }
          if (choice_button(Kind::Disable, "Résolu · item désactivé",
                            "Item non conforme : compté comme périmé, remplacé à la prochaine vérif.", 3,
                            false)) {
            changes["status"]     = "resolved";
            changes["resolution"] = "disabled";
            text_key              = "resolution_note";
          }
          if (choice_button(Kind::Remove, "Résolu · item retiré", "L'item a déjà été jeté : il sort de l'inventaire.",
                            3, true)) {
            changes["status"]     = "resolved";
            changes["resolution"] = "removed";
            text_key              = "resolution_note";
          }
        }
      }
      if (!changes.is_null()) {
        if (text_key)
          changes[text_key] = note_;
        update(app, report["id"].integer(), changes);
      }
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
    float       row_width_ = -1.0f; // largeur de la rangee de choix en cours (choice_button)
    bool        all_     = false;
    bool        loading_ = false;
    int         request_ = 0;
};

} // namespace

std::unique_ptr< AppWindow > make_reports_window() {
  return std::make_unique< ReportsWindow >();
}

} // namespace qrprotec
