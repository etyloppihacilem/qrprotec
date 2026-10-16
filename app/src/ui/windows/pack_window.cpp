/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          pack_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../../core/plural.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

namespace qrprotec {

namespace {

std::vector< Parameters > item_labels(const Json &items) {
  std::vector< Parameters > labels;
  const int                 count = static_cast< int >(items.size());
  for (int index = 0; index < count; ++index)
    labels.push_back(item_parameters(items[index], index + 1, count));
  return labels;
}

// Fiche d'un paquet ferme : ouverte en scannant l'etiquette du paquet (mode privilegie), depuis la reception
// ou la liste des paquets. L'ouverture imprime (apres apercu) les etiquettes individuelles des items.
class PackWindow final : public AppWindow {
  public:
    PackWindow() : AppWindow("pack", "Paquet fermé", true, true) {}

    void draw(App &app) override {
      if (const std::string id = app.take_pack_to_show(); !id.empty()) {
        pack_id_ = id;
        load(app);
      }
      ImGui::SetNextItemWidth(160.0f);
      const bool enter = ImGui::InputTextWithHint("##pack_id", "Code du paquet", &input_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((button("Afficher") || enter) && !input_.empty()) {
        pack_id_ = input_;
        input_.clear();
        load(app);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(pack_id_.empty());
      if (button("Rafraîchir"))
        load(app);
      ImGui::EndDisabled();
      ImGui::Separator();

      if (pack_id_.empty()) {
        ImGui::TextWrapped("Scannez l'étiquette d'un paquet fermé pour afficher sa fiche et l'ouvrir.");
        return;
      }
      if (loading_) {
        ImGui::TextDisabled("Chargement du paquet %s...", pack_id_.c_str());
        return;
      }
      if (!error_.empty()) {
        ImGui::TextColored(colors::red, "%s", error_.c_str());
        return;
      }
      if (pack_.is_null())
        return;

      const bool opened = !pack_["opened"].is_null();
      const int  count  = pack_["count"].integer();
      const auto date   = Date::parse(pack_["peremption"].str());
      const bool expired = date && *date < app.today();
      if (opened)
        status_banner("OUVERT " + display_on(pack_["opened"], true), colors::grey);
      else
        status_banner("FERMÉ", colors::orange);

      ImGui::Text("%d x %s", count, pack_["type_name"].str().c_str());
      ImGui::TextDisabled("Paquet %s - type %s", pack_["id"].str().c_str(), pack_["type"].str().c_str());
      if (pack_["peremption"].is_null())
        ImGui::TextUnformatted("Non périssable");
      else if (expired)
        ImGui::TextColored(colors::red, "PÉRIMÉ depuis le %s", display_date(pack_["peremption"]).c_str());
      else
        ImGui::Text("Péremption : %s", display_date(pack_["peremption"]).c_str());
      ImGui::Text("Reçu %s par %s", display_on(pack_["created"], true).c_str(), pack_["created_by"].str().c_str());
      if (opened && !pack_["opened_by"].str().empty())
        ImGui::Text("Ouvert par %s", pack_["opened_by"].str().c_str());
      ImGui::Spacing();

      ImGui::BeginDisabled(busy_);
      const std::string label = opened ? "Réimprimer les " + std::to_string(count) + " étiquettes des items"
                                       : "Ouvrir le paquet et imprimer les " + std::to_string(count) + " étiquettes";
      if (opened ? button(label.c_str(), ImVec2(-1, 0))
                 : primary_button(label.c_str(), ImVec2(-1, ImGui::GetFrameHeight() * 1.5f)))
        open_pack(app);
      if (!opened)
        ImGui::TextDisabled("Un aperçu des étiquettes s'affiche avant l'impression.");
      if (button("Étiquette du paquet"))
        app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack_) }, "Paquet");
      if (opened) {
        ImGui::SameLine();
        if (confirm_button("Refermer le paquet (ouvert par erreur)",
                           "Le paquet redevient fermé. Les étiquettes déjà imprimées restent valables "
                           "(mêmes items). Continuer ?",
                           "close_pack"))
          close_pack(app);
      }
      ImGui::EndDisabled();

      // signalement du paquet (vaut pour tous ses items) et desactivation (rappel produit, admins)
      ImGui::Spacing();
      if (pack_["reported"].integer() > 0)
        ImGui::TextColored(colors::orange, "%s en cours sur ce paquet.", nb(pack_["reported"].integer(), "signalement").c_str());
      if (report_button("⚠ Signaler un problème sur ce paquet", ImVec2(-1, 0)))
        app.report_problem({ "", "", "", pack_["type"].str(),
                             "Paquet de " + pack_["type_name"].str() + " (" + pack_["id"].str() + ")",
                             pack_["id"].str() });
      const int disabled = pack_["disabled"].integer();
      if (disabled > 0)
        ImGui::TextColored(colors::red, "%s sur %d %s : à remplacer comme des périmés.", nb(disabled, "item").c_str(),
                           count, pl(disabled, "désactivé").c_str());
      if (app.admin()) {
        ImGui::BeginDisabled(busy_);
        if (disabled > 0) {
          if (button("Réactiver le paquet"))
            set_disabled(app, false);
          ImGui::SameLine();
        }
        if (disabled < count) {
          ImGui::SetNextItemWidth(280.0f);
          input_limited("##disable_reason", reason_, 128, "Raison (obligatoire), ex : rappel produit");
          ImGui::SameLine();
          ImGui::BeginDisabled(reason_.empty());
          if (confirm_button("Désactiver le paquet", "Désactiver tous les items du paquet ?", "disable_pack"))
            set_disabled(app, true);
          ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
      }

      if (ImGui::CollapsingHeader("Items du paquet")) {
        for (const Json &iid : pack_["items"].items())
          ImGui::BulletText("%s", iid.str().c_str());
      }
    }

  private:
    void load(App &app) {
      loading_ = true;
      error_.clear();
      const std::string id = pack_id_;
      app.api.get("/api/packs/" + url_encode(id) + "/", [this, id](const ApiResult &result) {
        if (id != pack_id_)
          return;
        loading_ = false;
        if (!result.ok) {
          pack_  = Json();
          error_ = result.status == 404 ? "Paquet inconnu : " + id : result.error;
          return;
        }
        pack_ = result.data;
      });
    }

    void open_pack(App &app) {
      busy_ = true;
      Json body;
      body["user"]         = app.user_ref();
      const std::string id = pack_id_;
      app.api.post("/api/packs/" + url_encode(id) + "/open/", body, [this, &app, id](const ApiResult &result) {
        busy_ = false;
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.preview_labels(TemplateCategory::Item, item_labels(result.data["items"]), "Paquet ouvert");
        app.pack_opened(id);
        if (id == pack_id_)
          load(app);
      });
    }

    void set_disabled(App &app, bool disabled) {
      busy_ = true;
      Json body;
      body["user"]         = app.user_ref();
      body["pack"]         = pack_id_;
      body["disabled"]     = disabled;
      body["reason"]       = reason_;
      const std::string id = pack_id_;
      app.api.post("/api/items/disable/", body, [this, &app, id, disabled](const ApiResult &result) {
        busy_ = false;
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify(std::string(disabled ? "Paquet désactivé" : "Paquet réactivé") + " ("
                   + nb(result.data["count"].integer(), "item") + ").");
        reason_.clear();
        if (id == pack_id_)
          load(app);
      });
    }

    void close_pack(App &app) {
      busy_ = true;
      Json body;
      body["user"]         = app.user_ref();
      const std::string id = pack_id_;
      app.api.post("/api/packs/" + url_encode(id) + "/close/", body, [this, &app, id](const ApiResult &result) {
        busy_ = false;
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify("Paquet " + id + " refermé.");
        app.pack_opened(id); // met a jour la pile et la liste des paquets
        if (id == pack_id_)
          load(app);
      });
    }

    std::string pack_id_;
    std::string input_;
    std::string reason_; // raison de la desactivation
    Json        pack_;
    std::string error_;
    bool        loading_ = false;
    bool        busy_    = false;
};

} // namespace

std::unique_ptr< AppWindow > make_pack_window() {
  return std::make_unique< PackWindow >();
}

} // namespace qrprotec
