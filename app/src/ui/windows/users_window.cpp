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
#include "../../core/codes.hpp"

#include <algorithm>
#include <chrono>
#include <functional>

namespace qrprotec {

namespace {

const char *const kRoles[] = { "normal", "chef", "gestion", "admin" };

int role_index(const std::string &role) {
  return role == "admin" ? 3 : role == "gestion" ? 2 : role == "chef" ? 1 : 0;
}

const char *role_label(const std::string &role) {
  return role == "admin" ? "Administrateur" : role == "gestion" ? "Gestion" : role == "chef" ? "Chef" : "Secouriste";
}

// Onglet « Ordinateurs connectes » (poste du serveur, API locale) : un admin cree la cle d'un autre ordinateur et
// son fichier de configuration chiffre, telechargeable pendant 5 minutes par le lien du QR code
// (POST /api/front-configs/), puis revoque, reactive ou supprime les ordinateurs (POST /api/front-configs/NOM/).
void draw_new_computer(App &app, const std::function< void() > &on_created) {
  struct State {
      std::string name;
      std::string password;
      bool        busy    = false;
      bool        exists  = false; // nom deja pris : proposer de remplacer sa cle
      std::string url;
      std::string shown_password; // genere par le serveur, ou celui saisi
      bool        ca      = false;
      std::string created_name;
      std::chrono::steady_clock::time_point expires;
  };
  static State state;
  if (!app.settings.api_endpoint().key.empty()) {
    ImGui::TextWrapped("Seulement depuis le poste du serveur : cet ordinateur est lui-même un front distant.");
    return;
  }
  ImGui::TextWrapped("Pour utiliser QRProtec sur un autre ordinateur (bureau, ordinateur portable) relié au même "
                     "serveur : donnez-lui un nom, puis récupérez le fichier de configuration sur cet ordinateur "
                     "grâce au QR code ou au lien affiché, valable 5 minutes.");
  ImGui::SetNextItemWidth(260.0f);
  if (ImGui::InputTextWithHint("Nom de l'ordinateur", "ex. accueil, portable-chef", &state.name))
    state.exists = false;
  help_marker("Lettres sans accents, chiffres, point, tiret ou souligné, sans espace. Il apparaît dans le journal "
              "et dans la liste des ordinateurs (sudo qrprotec-manage frontkey list).");
  ImGui::SetNextItemWidth(260.0f);
  ImGui::InputTextWithHint("Mot de passe du fichier", "vide = choisi automatiquement", &state.password,
                           ImGuiInputTextFlags_Password);
  help_marker("Demandé sur l'autre ordinateur à l'import du fichier. Laissé vide, un mot de passe facile à "
              "recopier est choisi et affiché à côté du QR code. 8 caractères au moins.");
  if (state.exists)
    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.0f, 1.0f),
                       "Ce nom existe déjà : « Remplacer » lui donne une nouvelle clé, l'ancien fichier ne marchera plus.");
  ImGui::BeginDisabled(state.busy || state.name.empty());
  const bool replace = state.exists && warning_button("Remplacer sa clé");
  if (state.exists)
    ImGui::SameLine();
  if (primary_button("Créer le fichier de configuration") || replace) {
    Json body;
    body["name"]     = state.name;
    body["password"] = state.password;
    body["replace"]  = replace;
    state.busy       = true;
    const std::string typed_password = state.password;
    app.api.post("/api/front-configs/", body, [&app, typed_password, on_created](const ApiResult &result) {
      state.busy = false;
      if (!result.ok) {
        state.exists = result.status == 409;
        if (!state.exists)
          app.notify("Création impossible : " + result.error, true);
        return;
      }
      state.exists         = false;
      state.url            = result.data["url"].str();
      state.shown_password = result.data["password"].str().empty() ? typed_password : result.data["password"].str();
      state.ca             = result.data["ca"].boolean();
      state.created_name   = result.data["name"].str();
      state.expires        = std::chrono::steady_clock::now() + std::chrono::seconds(result.data["seconds"].integer(300));
      state.password.clear();
      state.name.clear();
      app.notify("Ordinateur « " + state.created_name + " » ajouté : fichier téléchargeable pendant 5 minutes.");
      on_created();
    });
  }
  ImGui::EndDisabled();

  if (state.url.empty())
    return;
  const auto left = std::chrono::duration_cast< std::chrono::seconds >(state.expires - std::chrono::steady_clock::now());
  ImGui::Separator();
  if (left.count() <= 0) {
    ImGui::TextDisabled("Le lien du fichier de « %s » a expiré. L'ordinateur reste autorisé ; pour un nouveau fichier, "
                        "recréez-le avec le même nom (Remplacer sa clé).", state.created_name.c_str());
    if (button("Masquer"))
      state.url.clear();
    return;
  }
  const bool side_by_side = ImGui::GetContentRegionAvail().x > 560.0f;
  ImGui::BeginGroup();
  draw_qr(state.url, 220.0f);
  ImGui::EndGroup();
  if (side_by_side)
    ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Text("Fichier de « %s » : encore %d:%02d", state.created_name.c_str(), static_cast< int >(left.count() / 60),
              static_cast< int >(left.count() % 60));
  ImGui::Text("Mot de passe :");
  ImGui::SetWindowFontScale(1.6f);
  ImGui::TextUnformatted(state.shown_password.c_str());
  ImGui::SetWindowFontScale(1.0f);
  ImGui::TextWrapped("Sur l'autre ordinateur :\n"
                     "1. Téléchargez le fichier : tapez l'adresse ci-dessous dans son navigateur (ou scannez le QR "
                     "code, s'il a une caméra).\n"
                     "2. Ouvrez QRProtec, cliquez sur « API hors ligne » en haut à droite, puis « Choisir le fichier… » "
                     "(il est dans Téléchargements).\n"
                     "3. Tapez le mot de passe ci-dessus, puis « Importer ».");
  std::string link = state.url;
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##front_config_url", &link, ImGuiInputTextFlags_ReadOnly);
  if (state.ca)
    ImGui::TextDisabled("Le certificat du serveur est inclus dans le fichier.");
  if (button("Masquer le QR code"))
    state.url.clear();
  ImGui::EndGroup();
}

// Gestion des secouristes : creation, droits, renouvellement des badges (valables un an), et onglet des ordinateurs
// connectes au serveur (fronts distants).
class UsersWindow final : public AppWindow {
  public:
    UsersWindow() : AppWindow("users", "Utilisateurs", true, true) { admin_only = true; }

    void on_open(App &app) override {
      app.refresh_users();
      load_computers(app);
    }

    void reset_view(App &) override { select(Json()); }

    void draw(App &app) override {
      if (ImGui::BeginTabBar("users_tabs")) {
        if (tab_item("Secouristes")) {
          draw_users(app);
          ImGui::EndTabItem();
        }
        if (tab_item("Ordinateurs connectés")) {
          draw_computers(app);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
    }

  private:
    Json computers_;
    bool computers_loading_ = false;

    void load_computers(App &app) {
      if (!app.settings.api_endpoint().key.empty())
        return; // front distant : l'API refuse, rien a lister
      computers_loading_ = true;
      app.api.get("/api/front-configs/", [this, &app](const ApiResult &result) {
        computers_loading_ = false;
        if (!result.ok) {
          app.notify("Ordinateurs connectés : " + result.error, true);
          return;
        }
        computers_ = result.data["fronts"];
      });
    }

    void computer_action(App &app, const std::string &name, const char *action) {
      Json body;
      body["action"] = action;
      app.api.post("/api/front-configs/" + name + "/", body, [this, &app](const ApiResult &result) {
        if (!result.ok)
          app.notify("Impossible : " + result.error, true);
        load_computers(app);
      });
    }

    void draw_computers(App &app) {
      if (!app.settings.api_endpoint().key.empty()) {
        ImGui::TextWrapped("Les ordinateurs se gèrent depuis le poste du serveur : cet ordinateur est lui-même "
                           "connecté au serveur à distance.");
        return;
      }
      // liste en haut, creation en dessous : toute la largeur pour le QR code et ses explications
      ImGui::BeginChild("computers_list", ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y * 0.35f)),
                        ImGuiChildFlags_Borders);
      ImGui::BeginDisabled(computers_loading_);
      if (button("Rafraîchir"))
        load_computers(app);
      ImGui::EndDisabled();
      const Date today = app.today();
      if (computers_.size() == 0)
        ImGui::TextDisabled("Aucun autre ordinateur connecté au serveur.");
      else if (ImGui::BeginTable("computers", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ordinateur", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Dernière connexion", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableHeadersRow();
        for (const Json &front : computers_.items()) {
          const std::string name    = front["name"].str();
          const bool        revoked = front["revoked"].boolean();
          ImGui::PushID(name.c_str());
          ImGui::TableNextRow();
          if (revoked)
            row_color(colors::grey, 0.35f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(name.c_str());
          ImGui::TableNextColumn();
          if (revoked)
            ImGui::TextDisabled("Révoqué");
          else if (front["download_pending"].boolean())
            ImGui::TextUnformatted("Fichier à télécharger");
          else
            ImGui::TextUnformatted("Autorisé");
          ImGui::TableNextColumn();
          if (front["last_used"].is_null()) {
            ImGui::TextDisabled("jamais");
          } else {
            ImGui::TextUnformatted(relative_datetime(front["last_used"].str(), today).c_str());
            if (!front["last_address"].str().empty() && ImGui::IsItemHovered())
              ImGui::SetTooltip("Depuis %s", front["last_address"].str().c_str());
          }
          ImGui::TableNextColumn();
          if (revoked) {
            if (small_button("Réactiver"))
              computer_action(app, name, "restore");
            ImGui::SameLine();
            if (confirm_button("Supprimer", ("Supprimer « " + name + " » ? Il faudra lui refaire un fichier.").c_str(),
                               "delete_computer"))
              computer_action(app, name, "delete");
          } else if (confirm_button("Révoquer",
                                    ("Révoquer « " + name + " » ? Il n'aura plus accès au serveur, tout de suite.").c_str(),
                                    "revoke_computer")) {
            computer_action(app, name, "revoke");
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::EndChild();
      ImGui::BeginChild("computer_new", ImVec2(0, 0), ImGuiChildFlags_Borders);
      ImGui::SeparatorText("Connecter un nouvel ordinateur");
      draw_new_computer(app, [this, &app]() { load_computers(app); });
      ImGui::EndChild();
    }

    void draw_users(App &app) {
      if (seen_users_version_ != app.catalog.users_version) {
        seen_users_version_ = app.catalog.users_version;
        for (const Json &user : app.catalog.users.items())
          if (!selected_.empty() && user["matricule"].str() == selected_)
            select(user); // fiche mise a jour avec la liste
      }
      ImGui::BeginChild("users_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0), ImGuiChildFlags_Borders);
      if (button("Rafraîchir"))
        app.refresh_users();
      ImGui::SameLine();
      if (button("Nouvel utilisateur"))
        select(Json());
      const Date today = app.today();
      if (ImGui::BeginTable("users", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Matricule", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Rôle", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Droits", ImGuiTableColumnFlags_WidthFixed, 100.0f);
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
          ImGui::TextUnformatted(user["fonction"].str().c_str());
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
      fonction_   = user["fonction"].str(selected_.empty() ? "Secouriste" : "");
      role_       = role_index(user["role"].str(user["privileged"].boolean() ? "admin" : "normal"));
      active_     = user["active"].boolean(true);
      contact_    = user["pin_contact"].str();
      default_contact_ = user["default_contact"].boolean();
    }

    // Admin a prevenir si le PIN de cet utilisateur se bloque (affiche sur le telephone avec le lien de deblocage)
    // Types de notifications web que l'utilisateur peut recevoir (selon son role) : decocher en coupe un pour lui,
    // sur tous ses appareils. Il choisit ensuite les siens dans le front web (menu > Notifications).
    void draw_notifications(App &app) {
      ImGui::SeparatorText("Notifications web");
      const Json &types = user_["push_types"];
      if (types.size() == 0) {
        ImGui::TextDisabled("Réservées aux rôles gestion et admin.");
        return;
      }
      const int devices = user_["push_devices"].integer();
      if (devices == 0)
        ImGui::TextDisabled("Aucun appareil abonné.");
      else
        ImGui::Text("%d appareil%s abonné%s.", devices, devices > 1 ? "s" : "", devices > 1 ? "s" : "");
      help_marker("Types de notifications permis à cet utilisateur. Il choisit ensuite les siens, appareil par "
                  "appareil, dans le front web (menu > Notifications).");
      for (std::size_t i = 0; i < types.size(); ++i) {
        bool enabled = types[i]["enabled"].boolean();
        if (!ImGui::Checkbox(types[i]["label"].str().c_str(), &enabled))
          continue;
        Json disabled = Json::array();
        for (std::size_t j = 0; j < types.size(); ++j) {
          const bool on = j == i ? enabled : types[j]["enabled"].boolean();
          if (!on)
            disabled.push_back(Json(types[j]["type"].str()));
        }
        Json body;
        body["push_disabled"] = disabled;
        app.api.patch("/api/users/" + url_encode(selected_) + "/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          select(result.data);
          app.refresh_users();
        });
      }
    }

    // Reglages du front web de l'utilisateur (ligne d'info pendant la vérif, explication de la vérif masquee...),
    // qu'il change lui-meme dans les Reglages du web : un admin peut les changer ici (ex. reafficher l'explication)
    void draw_web_prefs(App &app) {
      const Json &prefs = user_["web_prefs_list"];
      if (prefs.size() == 0)
        return;
      ImGui::SeparatorText("Réglages du web");
      help_marker("Réglages que l'utilisateur choisit dans le front web (⚙ Réglages). Décocher « Explication de la "
                  "vérif masquée » lui réaffiche l'explication au début de sa prochaine vérif.");
      for (std::size_t i = 0; i < prefs.size(); ++i) {
        bool value = prefs[i]["value"].boolean();
        if (!ImGui::Checkbox(prefs[i]["label"].str().c_str(), &value))
          continue;
        Json changed = Json::object();
        changed[prefs[i]["name"].str()] = Json(value);
        Json body;
        body["web_prefs"] = changed;
        app.api.patch("/api/users/" + url_encode(selected_) + "/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          select(result.data);
          app.refresh_users();
        });
      }
    }

    void draw_contact(App &app) {
      // sans admin choisi : l'admin a contacter par defaut (case cochee dans la fiche d'un administrateur)
      std::string fallback = "(aucun : un administrateur)";
      for (const Json &user : app.catalog.users.items())
        if (user["default_contact"].boolean() && user["active"].boolean(true) && user["matricule"].str() != selected_)
          fallback = "(par défaut : " + user["prenom"].str() + " " + user["nom"].str() + ")";
      std::string preview = fallback;
      for (const Json &user : app.catalog.users.items())
        if (user["matricule"].str() == contact_)
          preview = user["prenom"].str() + " " + user["nom"].str();
      ImGui::SetNextItemWidth(260.0f);
      if (ImGui::BeginCombo("Admin à contacter", preview.c_str())) {
        if (ImGui::Selectable(fallback.c_str(), contact_.empty()))
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
                  "code) à envoyer à cet administrateur. Sans choix, c'est l'admin à contacter par défaut.");
      if (kRoles[role_] != std::string("admin"))
        return;
      ImGui::Checkbox("Admin à contacter par défaut", &default_contact_);
      help_marker("Proposé à tous les utilisateurs qui n'ont pas d'admin à contacter choisi, à la place de « un "
                  "administrateur ». Un seul admin par défaut : cocher cette case la décoche chez l'ancien.");
    }

    void draw_form(App &app) {
      const bool creating = selected_.empty();
      ImGui::SeparatorText(creating ? "Nouvel utilisateur" : "Utilisateur");
      ImGui::BeginDisabled(!creating);
      input_limited("Matricule", matricule_, 16, nullptr, ImGuiInputTextFlags_CharsNoBlank);
      ImGui::EndDisabled();
      input_limited("Prénom", prenom_, 32);
      input_limited("Nom", nom_, 32);
      input_limited("Rôle", fonction_, 32, "Secouriste");
      help_marker("Rôle dans l'équipe, par exemple l'autorité, l'OPÉ, le TECH. Purement indicatif : les droits "
                  "se choisissent juste en dessous. Laissé vide, c'est « Secouriste ».");
      ImGui::SetNextItemWidth(220.0f);
      ImGui::Combo("Droits", &role_, "Secouriste\0Chef\0Gestion\0Administrateur\0");
      help_marker("Secouriste : vérifs et scans, listes de matériel qui lui sont attribuées. Chef : crée les listes "
                  "de matériel, voit toutes les listes et les modèles, sans rien gérer dans la TECH. Gestion : mode privilégié (stocks, inventaire, lots, étiquettes) et "
                  "état des stocks sur le téléphone, sans les Réglages ni les Utilisateurs. Administrateur : tout.");
      if (!creating)
        ImGui::Checkbox("Compte actif", &active_);
      const ImGuiInputTextFlags pin_flags = ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal;
      ImGui::SetNextItemWidth(160.0f);
      input_limited(creating ? "Code PIN" : "Nouveau PIN", pin_, 8, "4 à 8 chiffres", pin_flags);
      help_marker("Demandé après le badge à chaque connexion. Obligatoire pour un administrateur (s'il n'en a pas, "
                  "il le choisit à sa prochaine connexion) ; pour les autres rôles, selon le réglage par défaut ou "
                  "le choix fait plus bas (obligatoire ou désactivé).");
      const bool pin_valid = pin_.empty() || (pin_.size() >= 4 && pin_.size() <= 8);
      if (!pin_valid)
        ImGui::TextColored(colors::orange, "Le PIN doit comporter 4 à 8 chiffres.");
      draw_contact(app);

      Json body;
      body["nom"]         = nom_;
      body["prenom"]      = prenom_;
      body["fonction"]    = fonction_;
      body["role"]        = kRoles[role_];
      body["pin_contact"] = contact_;
      body["default_contact"] = default_contact_ && kRoles[role_] == std::string("admin");
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
      const std::string pin_mode = user_["pin_mode"].str();
      const bool        admin    = user_["role"].str() == "admin";
      if (user_["pin_blocked"].boolean()) {
        if (user_["pin_forgotten"].boolean())
          ImGui::TextColored(colors::red, "Code PIN oublié, signalé %s.", display_on(user_["pin_blocked_since"], true).c_str());
        else
          ImGui::TextColored(colors::red, "PIN bloqué %s (%d essais faux).", display_on(user_["pin_blocked_since"], true).c_str(),
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
      else if (pin_mode == "disabled")
        ImGui::TextDisabled("PIN désactivé : jamais demandé.");
      else if (user_["has_pin"].boolean())
        ImGui::TextColored(colors::green, "PIN défini%s.", pin_mode == "required" ? " (obligatoire)" : "");
      else if (user_["pin_required"].boolean())
        ImGui::TextColored(colors::orange, "Aucun PIN : il sera choisi à la prochaine connexion (obligatoire).");
      else
        ImGui::TextDisabled("Aucun PIN (facultatif).");
      ImGui::BeginDisabled(pin_.empty() || !pin_valid);
      if (button("Définir le PIN"))
        update_pin(app, pin_);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!user_["has_pin"].boolean() || admin);
      if (button("Supprimer le PIN"))
        update_pin(app, "");
      ImGui::EndDisabled();
      // PIN obligatoire ou desactive pour ce secouriste, sinon le reglage par defaut (Reglages > Regles du serveur)
      if (!admin) {
        if (pin_mode != "required" && button("Rendre le PIN obligatoire"))
          update_pin_mode(app, "required");
        if (pin_mode != "required" && pin_mode != "disabled")
          ImGui::SameLine();
        if (pin_mode != "disabled"
            && confirm_button("Désactiver le PIN", "Le PIN de cet utilisateur sera supprimé et ne lui sera plus "
                                                   "demandé, même s'il est obligatoire par défaut. Continuer ?",
                              "pin_disable"))
          update_pin_mode(app, "disabled");
        if (!pin_mode.empty()) {
          ImGui::SameLine();
          if (button("Réglage par défaut"))
            update_pin_mode(app, "");
        }
        help_marker("Obligatoire : sans PIN, il le choisit à sa prochaine connexion. Désactivé : jamais de PIN, même "
                    "si le PIN est obligatoire par défaut (Réglages > Règles du serveur). Un utilisateur qui a un PIN "
                    "peut aussi se connecter sans badge, avec son nom, son prénom et son PIN.");
      }

      draw_notifications(app);
      draw_web_prefs(app);

      ImGui::SeparatorText("Badge");
      ImGui::Text("Valable jusqu'au : %s", display_date(user_["key_expires"]).c_str());
      // la cle du badge n'est connue qu'a sa creation ou son renouvellement (le serveur n'en garde que l'empreinte)
      if (!user_["badge_url"].str().empty()) {
        if (button("Aperçu et impression du badge"))
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

    void update_pin_mode(App &app, const std::string &mode) {
      Json body;
      body["pin_mode"] = mode;
      app.api.patch("/api/users/" + url_encode(selected_) + "/", body, [this, &app, mode](const ApiResult &result) {
        app.notify(!result.ok            ? result.error
                   : mode == "required" ? "PIN obligatoire : choisi à la prochaine connexion s'il n'existe pas."
                   : mode == "disabled" ? "PIN désactivé."
                                        : "PIN selon le réglage par défaut.",
                   !result.ok);
        if (result.ok)
          select(result.data);
        app.refresh_users();
      });
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
    bool        default_contact_ = false;
    std::string selected_;
    int         seen_users_version_ = -1;
    Json        user_;
    std::string matricule_;
    std::string nom_;
    std::string prenom_;
    std::string fonction_ = "Secouriste";
    int         role_       = 0; // index dans kRoles
    bool        active_     = true;
};

} // namespace

std::unique_ptr< AppWindow > make_users_window() {
  return std::make_unique< UsersWindow >();
}

} // namespace qrprotec
