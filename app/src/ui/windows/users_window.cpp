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

const char *const kRoles[] = { "normal", "gestion", "admin" };

int role_index(const std::string &role) {
  return role == "admin" ? 2 : role == "gestion" ? 1 : 0;
}

const char *role_label(const std::string &role) {
  return role == "admin" ? "Administrateur" : role == "gestion" ? "Gestion" : "Secouriste";
}

// Gestion des secouristes : creation, droits, renouvellement des badges (valables un an).
class UsersWindow final : public AppWindow {
  public:
    UsersWindow() : AppWindow("users", "Utilisateurs", true, true) { admin_only = true; }

    void on_open(App &app) override { app.refresh_users(); }

    void draw(App &app) override {
      if (seen_users_version_ != app.catalog.users_version) {
        seen_users_version_ = app.catalog.users_version;
        for (const Json &user : app.catalog.users.items())
          if (!selected_.empty() && user["matricule"].str() == selected_)
            select(user); // fiche mise a jour avec la liste
      }
      ImGui::BeginChild("users_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0), ImGuiChildFlags_Borders);
      if (ImGui::Button("Rafraîchir"))
        app.refresh_users();
      ImGui::SameLine();
      if (ImGui::Button("Nouvel utilisateur"))
        select(Json());
      const Date today = app.today();
      if (ImGui::BeginTable("users", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Matricule", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Rôle", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Badge", ImGuiTableColumnFlags_WidthFixed, 95.0f);
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
          ImGui::TextUnformatted(role_label(user["role"].str(user["privileged"].boolean() ? "admin" : "normal")));
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
      pin_.clear();
      selected_   = user["matricule"].str();
      user_       = user;
      matricule_  = selected_;
      nom_        = user["nom"].str();
      prenom_     = user["prenom"].str();
      role_       = role_index(user["role"].str(user["privileged"].boolean() ? "admin" : "normal"));
      active_     = user["active"].boolean(true);
      contact_    = user["pin_contact"].str();
    }

    // Admin a prevenir si le PIN de cet utilisateur se bloque (affiche sur le telephone avec le lien de deblocage)
    void draw_contact(App &app) {
      std::string preview = "(aucun : un administrateur)";
      for (const Json &user : app.catalog.users.items())
        if (user["matricule"].str() == contact_)
          preview = user["prenom"].str() + " " + user["nom"].str();
      ImGui::SetNextItemWidth(260.0f);
      if (ImGui::BeginCombo("Admin à contacter", preview.c_str())) {
        if (ImGui::Selectable("(aucun : un administrateur)", contact_.empty()))
          contact_.clear();
        for (const Json &user : app.catalog.users.items()) {
          const std::string matricule = user["matricule"].str();
          if (user["role"].str() != "admin" || !user["active"].boolean(true) || matricule == selected_)
            continue;
          const std::string label = user["prenom"].str() + " " + user["nom"].str() + " (" + matricule + ")";
          if (ImGui::Selectable(label.c_str(), contact_ == matricule))
            contact_ = matricule;
        }
        ImGui::EndCombo();
      }
      help_marker("Si le PIN est bloqué après trop d'essais, le téléphone affiche un lien de déblocage (et son QR "
                  "code) à envoyer à cet administrateur.");
    }

    void draw_form(App &app) {
      const bool creating = selected_.empty();
      ImGui::SeparatorText(creating ? "Nouvel utilisateur" : "Utilisateur");
      ImGui::BeginDisabled(!creating);
      ImGui::InputText("Matricule", &matricule_, ImGuiInputTextFlags_CharsNoBlank);
      ImGui::EndDisabled();
      ImGui::InputText("Prénom", &prenom_);
      ImGui::InputText("Nom", &nom_);
      ImGui::SetNextItemWidth(220.0f);
      ImGui::Combo("Rôle", &role_, "Secouriste\0Gestion\0Administrateur\0");
      help_marker("Secouriste : vérifs et scans. Gestion : mode privilégié (stocks, inventaire, lots, étiquettes) et "
                  "état des stocks sur le téléphone, sans les Réglages ni les Utilisateurs. Administrateur : tout.");
      if (!creating)
        ImGui::Checkbox("Compte actif", &active_);
      const ImGuiInputTextFlags pin_flags = ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal;
      ImGui::SetNextItemWidth(160.0f);
      ImGui::InputTextWithHint(creating ? "Code PIN" : "Nouveau PIN", "4 à 8 chiffres", &pin_, pin_flags);
      help_marker("Demandé après le badge à chaque connexion. Obligatoire pour un administrateur (s'il n'en a pas, "
                  "il le choisit à sa prochaine connexion), facultatif pour les autres rôles.");
      const bool pin_valid = pin_.empty() || (pin_.size() >= 4 && pin_.size() <= 8);
      if (!pin_valid)
        ImGui::TextColored(colors::orange, "Le PIN doit comporter 4 à 8 chiffres.");
      draw_contact(app);

      Json body;
      body["nom"]         = nom_;
      body["prenom"]      = prenom_;
      body["role"]        = kRoles[role_];
      body["pin_contact"] = contact_;
      if (creating) {
        ImGui::BeginDisabled(matricule_.empty() || nom_.empty() || prenom_.empty() || !pin_valid);
        if (primary_button("Créer et voir le badge")) {
          body["matricule"] = matricule_;
          if (!pin_.empty())
            body["pin"] = pin_;
          pin_.clear();
          app.api.post("/api/users/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Utilisateur créé.");
            app.preview_labels(TemplateCategory::User, { user_parameters(result.data) },
                               "Badge de " + result.data["prenom"].str() + " " + result.data["nom"].str());
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
      ImGui::SeparatorText("Code PIN");
      if (user_["pin_blocked"].boolean()) {
        ImGui::TextColored(colors::red, "PIN bloqué le %s (%d essais faux).", display_datetime(user_["pin_blocked_since"]).c_str(),
                           user_["pin_failures"].integer());
        if (confirm_button("Réinitialiser le PIN", "L'utilisateur choisira un nouveau PIN à sa prochaine connexion. "
                                                   "Si son badge a pu être volé, renouvelez plutôt le badge. Continuer ?",
                           "pin_reset")) {
          Json reset;
          reset["pin_reset"] = true;
          app.api.patch("/api/users/" + url_encode(selected_) + "/", reset, [this, &app](const ApiResult &result) {
            app.notify(result.ok ? "PIN réinitialisé : nouveau PIN à la prochaine connexion." : result.error, !result.ok);
            if (result.ok)
              select(result.data);
            app.refresh_users();
          });
        }
      } else if (user_["pin_reset_required"].boolean())
        ImGui::TextColored(colors::orange, "PIN réinitialisé : il sera choisi à la prochaine connexion.");
      else if (user_["has_pin"].boolean())
        ImGui::TextColored(colors::green, "PIN défini.");
      else if (kRoles[role_] == std::string("admin"))
        ImGui::TextColored(colors::orange, "Aucun PIN : il sera choisi à la prochaine connexion (obligatoire).");
      else
        ImGui::TextDisabled("Aucun PIN (facultatif).");
      ImGui::BeginDisabled(pin_.empty() || !pin_valid);
      if (ImGui::Button("Définir le PIN"))
        update_pin(app, pin_);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!user_["has_pin"].boolean() || user_["role"].str() == "admin");
      if (ImGui::Button("Supprimer le PIN"))
        update_pin(app, "");
      ImGui::EndDisabled();

      ImGui::SeparatorText("Badge");
      ImGui::Text("Valable jusqu'au : %s", display_date(user_["key_expires"]).c_str());
      // la cle du badge n'est connue qu'a sa creation ou son renouvellement (le serveur n'en garde que l'empreinte)
      if (!user_["badge_url"].str().empty()) {
        if (ImGui::Button("Aperçu et impression du badge"))
          app.preview_labels(TemplateCategory::User, { user_parameters(user_) },
                             "Badge de " + user_["prenom"].str() + " " + user_["nom"].str());
        ImGui::SameLine();
      }
      if (confirm_button("Renouveler (1 an)", "L'ancien badge ne fonctionnera plus. Continuer ?", "renew_key")) {
        app.api.post("/api/users/" + url_encode(selected_) + "/renew-key/", Json::object(),
                     [this, &app](const ApiResult &result) {
                       if (!result.ok) {
                         app.notify(result.error, true);
                         return;
                       }
                       select(result.data);
                       app.notify("Badge renouvelé.");
                       app.preview_labels(TemplateCategory::User, { user_parameters(result.data) }, "Nouveau badge");
                       app.refresh_users();
                     });
      }
      if (user_["badge_url"].str().empty())
        ImGui::TextDisabled("Pour réimprimer un badge, renouvelez-le : sa clé n'est affichée qu'à sa création.");
    }

    void update_pin(App &app, const std::string &pin) {
      Json body;
      body["pin"] = pin;
      app.api.patch("/api/users/" + url_encode(selected_) + "/", body, [this, &app, pin](const ApiResult &result) {
        app.notify(result.ok ? (pin.empty() ? "PIN supprimé." : "PIN enregistré.") : result.error, !result.ok);
        if (result.ok)
          select(result.data);
        app.refresh_users();
      });
      pin_.clear();
    }

    std::string pin_;
    std::string contact_;
    std::string selected_;
    int         seen_users_version_ = -1;
    Json        user_;
    std::string matricule_;
    std::string nom_;
    std::string prenom_;
    int         role_       = 0; // index dans kRoles
    bool        active_     = true;
};

} // namespace

std::unique_ptr< AppWindow > make_users_window() {
  return std::make_unique< UsersWindow >();
}

} // namespace qrprotec
