/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          users_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

namespace qrprotec {

namespace {

// Gestion des secouristes : creation, droits, renouvellement des badges (valables un an).
class UsersWindow final : public AppWindow {
  public:
    UsersWindow() : AppWindow("users", "Utilisateurs", true, true) {}

    void on_open(App &app) override { app.refresh_users(); }

    void draw(App &app) override {
      ImGui::BeginChild("users_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0), ImGuiChildFlags_Borders);
      if (ImGui::Button("Rafraichir"))
        app.refresh_users();
      ImGui::SameLine();
      if (ImGui::Button("Nouvel utilisateur"))
        select(Json());
      const Date today = app.today();
      if (ImGui::BeginTable("users", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Matricule", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Badge", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();
        for (const Json &user : app.catalog.users.items()) {
          const std::string matricule = user["matricule"].str();
          const auto        expires   = Date::parse(user["key_expires"].str());
          ImGui::TableNextRow();
          if (!user["active"].boolean(true))
            row_color(colors::grey, 0.35f);
          else if (!expires || *expires < today)
            row_color(colors::red, 0.35f);
          else if (*expires < today.plus_days(30))
            row_color(colors::orange, 0.3f);
          ImGui::TableNextColumn();
          if (ImGui::Selectable(matricule.c_str(), selected_ == matricule, ImGuiSelectableFlags_SpanAllColumns))
            select(user);
          ImGui::TableNextColumn();
          ImGui::Text("%s %s", user["prenom"].str().c_str(), user["nom"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(user["privileged"].boolean() ? "Responsable" : "Secouriste");
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(expires ? expires->display().c_str() : "-");
        }
        ImGui::EndTable();
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("user_form", ImVec2(0, 0), ImGuiChildFlags_Borders);
      draw_form(app);
      ImGui::EndChild();
    }

  private:
    void select(const Json &user) {
      selected_   = user["matricule"].str();
      user_       = user;
      matricule_  = selected_;
      nom_        = user["nom"].str();
      prenom_     = user["prenom"].str();
      privileged_ = user["privileged"].boolean();
      active_     = user["active"].boolean(true);
    }

    void draw_form(App &app) {
      const bool creating = selected_.empty();
      ImGui::SeparatorText(creating ? "Nouvel utilisateur" : "Utilisateur");
      ImGui::BeginDisabled(!creating);
      ImGui::InputText("Matricule", &matricule_, ImGuiInputTextFlags_CharsNoBlank);
      ImGui::EndDisabled();
      ImGui::InputText("Prenom", &prenom_);
      ImGui::InputText("Nom", &nom_);
      ImGui::Checkbox("Responsable (mode privilegie)", &privileged_);
      if (!creating)
        ImGui::Checkbox("Compte actif", &active_);

      Json body;
      body["nom"]        = nom_;
      body["prenom"]     = prenom_;
      body["privileged"] = privileged_;
      if (creating) {
        ImGui::BeginDisabled(matricule_.empty() || nom_.empty() || prenom_.empty());
        if (primary_button("Creer et imprimer le badge")) {
          body["matricule"] = matricule_;
          app.api.post("/api/users/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Utilisateur cree.");
            app.print_labels(TemplateCategory::User, { user_parameters(result.data) }, "Badge");
            select(result.data);
            app.refresh_users();
          });
        }
        ImGui::EndDisabled();
        return;
      }
      body["active"] = active_;
      if (primary_button("Enregistrer")) {
        app.api.patch("/api/users/" + url_encode(selected_) + "/", body, [this, &app](const ApiResult &result) {
          app.notify(result.ok ? "Utilisateur enregistre." : result.error, !result.ok);
          if (result.ok)
            select(result.data);
          app.refresh_users();
        });
      }
      ImGui::SeparatorText("Badge");
      ImGui::Text("Valable jusqu'au : %s", display_date(user_["key_expires"]).c_str());
      if (ImGui::Button("Imprimer le badge"))
        app.print_labels(TemplateCategory::User, { user_parameters(user_) }, "Badge");
      ImGui::SameLine();
      if (confirm_button("Renouveler (1 an)", "L'ancien badge ne fonctionnera plus. Continuer ?", "renew_key")) {
        app.api.post("/api/users/" + url_encode(selected_) + "/renew-key/", Json::object(),
                     [this, &app](const ApiResult &result) {
                       if (!result.ok) {
                         app.notify(result.error, true);
                         return;
                       }
                       select(result.data);
                       app.notify("Badge renouvele.");
                       app.print_labels(TemplateCategory::User, { user_parameters(result.data) }, "Badge");
                       app.refresh_users();
                     });
      }
    }

    std::string selected_;
    Json        user_;
    std::string matricule_;
    std::string nom_;
    std::string prenom_;
    bool        privileged_ = false;
    bool        active_     = true;
};

} // namespace

std::unique_ptr< AppWindow > make_users_window() {
  return std::make_unique< UsersWindow >();
}

} // namespace qrprotec
