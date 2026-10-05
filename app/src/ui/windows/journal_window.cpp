/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          journal_window.cpp
        -\-    _|__
         |\___/  . \        Created on 05 Oct. 2026 at 08:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>

namespace qrprotec {

namespace {

// Libelles des champs du detail d'une operation (/api/operations/), dans l'ordre d'affichage
const std::pair< const char *, const char * > kDetailLabels[] = {
  { "reason", "Raison" },
  { "seal_number", "Numéro de scellé" },
  { "forced", "Scellage forcé" },
  { "signed_after_scan", "Ouverture anonyme signée après le scan" },
  { "complete", "Complète" },
  { "partial", "Vérif partielle" },
  { "partial_verif", "Pendant une vérif partielle" },
  { "present", "Présents" },
  { "missing", "Manquants" },
  { "expired", "Périmés" },
  { "replaced", "Remplacés" },
  { "count", "Nombre d'items" },
  { "origins", "Retirés de" },
  { "type", "Type" },
  { "peremption", "Péremption" },
  { "sealed_pack", "Paquet fermé" },
  { "pack", "Paquet" },
  { "matricule", "Matricule" },
  { "role", "Rôle" },
  { "fields", "Champs modifiés" },
  { "items", "Items" },
  { "missing_items", "Items manquants" },
  { "expired_items", "Items périmés" },
};

std::string join(const Json &list) {
  std::string text;
  for (const Json &value : list.items())
    text += (text.empty() ? "" : ", ") + (value.is_string() ? value.str() : value.dump());
  return text;
}

std::string detail_text(const Json &value) {
  if (value.is_bool())
    return value.boolean() ? "oui" : "non";
  if (value.is_array())
    return join(value);
  if (value.is_string())
    return value.str();
  return value.dump();
}

// Journal des operations : qui fait quoi et quand (verifs, scelles, reassorts, receptions, gestion).
// Roles gestion et admin ; filtres par personne, type d'operation, lot (avec ses sous-lots), dates et texte.
class JournalWindow final : public AppWindow {
  public:
    JournalWindow() : AppWindow("journal", "Journal des opérations", true, true) {}

    void on_open(App &app) override {
      app.refresh_lots();
      load(app, true);
    }

    void draw(App &app) override {
      // « Journal du lot » depuis la gestion des lots : filtre sur ce lot
      const std::string lot = app.take_journal_lot();
      if (!lot.empty()) {
        reset_filters();
        lot_ = lot;
        load(app, true);
      }
      draw_filters(app);
      ImGui::Separator();
      draw_table(app);
      draw_details();
    }

  private:
    void reset_filters() {
      by_.clear();
      kind_ = 0;
      lot_.clear();
      with_sub_lots_ = true;
      since_.clear();
      until_.clear();
      text_.clear();
    }

    std::string query() const {
      std::string path = "/api/operations/?limit=200";
      if (!by_.empty())
        path += "&by=" + url_encode(by_);
      if (kind_ > 0 && kind_ <= static_cast< int >(kinds_.size()))
        path += "&kind=" + url_encode(kinds_[kind_ - 1]["kind"].str());
      if (!lot_.empty())
        path += "&lot=" + url_encode(lot_) + (with_sub_lots_ ? "" : "&sub=0");
      if (const auto since = parse_user_date(since_))
        path += "&since=" + since->iso();
      if (const auto until = parse_user_date(until_))
        path += "&until=" + until->iso();
      if (!text_.empty())
        path += "&q=" + url_encode(text_);
      return path;
    }

    // first : nouvelle recherche (sinon page suivante, apres la derniere ligne affichee)
    void load(App &app, bool first) {
      std::string path = query();
      if (first)
        path += "&facets=1";
      else if (operations_.size() > 0)
        path += "&before=" + std::to_string(operations_[operations_.size() - 1]["id"].integer());
      const int request = ++request_;
      loading_          = true;
      app.api.get(path, [this, &app, first, request](const ApiResult &result) {
        if (request != request_)
          return; // reponse d'une recherche remplacee depuis
        loading_ = false;
        if (!result.ok) {
          app.notify("Journal : " + result.error, true);
          return;
        }
        if (first) {
          operations_ = result.data["operations"];
          selected_   = -1;
          kinds_      = result.data["kinds"];
          people_     = Json::array();
          for (Json person : result.data["people"].items()) {
            if (!person["verified"].boolean())
              person["name"] = person["name"].str() + " (nom déclaré)";
            people_.push_back(person);
          }
        } else {
          for (const Json &operation : result.data["operations"].items())
            operations_.push_back(operation);
        }
        more_ = result.data["more"].boolean();
      });
    }

    void draw_filters(App &app) {
      bool changed = false;
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Qui");
      ImGui::SameLine();
      changed |= search_select("journal_by", people_, "by", "name", by_, "Filtrer par personne…", "Tout le monde", 230.0f);
      ImGui::SameLine();
      ImGui::TextUnformatted("Quoi");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(220.0f);
      const std::string current =
        kind_ > 0 && kind_ <= static_cast< int >(kinds_.size()) ? kinds_[kind_ - 1]["label"].str() : "Toutes les opérations";
      if (ImGui::BeginCombo("##kind", current.c_str())) {
        if (ImGui::Selectable("Toutes les opérations", kind_ == 0)) {
          kind_   = 0;
          changed = true;
        }
        for (int index = 0; index < static_cast< int >(kinds_.size()); ++index)
          if (ImGui::Selectable(kinds_[index]["label"].str().c_str(), kind_ == index + 1)) {
            kind_   = index + 1;
            changed = true;
          }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("Lot");
      ImGui::SameLine();
      changed |= search_select("journal_lot", app.catalog.lots, "id", "name", lot_, "Filtrer par lot…", "Tous les lots",
                               230.0f);
      if (!lot_.empty()) {
        ImGui::SameLine();
        changed |= ImGui::Checkbox("avec ses sous-lots", &with_sub_lots_);
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Quand");
      ImGui::SameLine();
      changed |= date_field("##since", "du (ex: 01/10/26)", since_);
      ImGui::SameLine();
      changed |= date_field("##until", "au (inclus)", until_);
      ImGui::SameLine();
      const Date today = app.today();
      if (ImGui::Button("Aujourd'hui")) {
        since_ = until_ = today.display();
        changed         = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("7 jours")) {
        since_ = today.plus_days(-6).display();
        until_.clear();
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("30 jours")) {
        since_ = today.plus_days(-29).display();
        until_.clear();
        changed = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(220.0f);
      ImGui::InputTextWithHint("##text", "Texte (n° de scellé, item, nom…)", &text_);
      changed |= ImGui::IsItemDeactivatedAfterEdit();
      help_marker("Cherche dans le résumé de l'opération, le nom du lot et le nom ou le matricule de la personne. "
                  "Entrée pour lancer la recherche.");
      ImGui::SameLine();
      if (ImGui::Button("Effacer les filtres")) {
        reset_filters();
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Rafraîchir") || changed)
        load(app, true);
    }

    // Date saisie librement (meme regles que la peremption) ; retourne true quand la saisie est validee
    static bool date_field(const char *id, const char *hint, std::string &value) {
      ImGui::SetNextItemWidth(140.0f);
      ImGui::InputTextWithHint(id, hint, &value);
      if (!ImGui::IsItemDeactivatedAfterEdit())
        return false;
      if (const auto date = parse_user_date(value))
        value = date->display();
      else if (!value.empty())
        ImGui::SetTooltip("Date non reconnue");
      return true;
    }

    static ImVec4 kind_color(const Json &operation) {
      const std::string kind = operation["kind"].str();
      if (kind == "verif" || kind == "stock_verif")
        return operation["details"]["complete"].boolean(true) ? colors::green : colors::red;
      if (kind == "unseal" || kind == "item_delete")
        return colors::orange;
      if (kind == "seal")
        return colors::green;
      return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }

    void draw_table(App &app) {
      const float details_height = selected_ >= 0 ? ImGui::GetTextLineHeightWithSpacing() * 9.0f : 0.0f;
      const float footer         = ImGui::GetFrameHeightWithSpacing() + details_height;
      if (ImGui::BeginTable("journal", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Quand", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Qui", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Opération", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Lot", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Détail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int index = 0; index < static_cast< int >(operations_.size()); ++index) {
          const Json &operation = operations_[index];
          ImGui::TableNextRow();
          ImGui::PushID(index);
          ImGui::TableNextColumn();
          const std::string when = display_datetime(operation["at"]);
          if (ImGui::Selectable(when.c_str(), selected_ == index, ImGuiSelectableFlags_SpanAllColumns))
            selected_ = selected_ == index ? -1 : index;
          ImGui::TableNextColumn();
          if (operation["verified"].boolean())
            ImGui::TextUnformatted(operation["by_name"].str().c_str());
          else
            ImGui::TextColored(colors::orange, "%s", operation["by_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextColored(kind_color(operation), "%s", operation["kind_label"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(operation["lot_name"].str().c_str());
          ImGui::TableNextColumn();
          if (operation["reconstructed"].boolean()) {
            ImGui::TextDisabled("%s", operation["summary"].str().c_str());
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Reconstitué depuis l'historique enregistré avant le journal.");
          } else {
            ImGui::TextUnformatted(operation["summary"].str().c_str());
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (loading_)
        ImGui::TextDisabled("Chargement…");
      else if (operations_.size() == 0)
        ImGui::TextDisabled("Aucune opération ne correspond à ces filtres.");
      else
        ImGui::TextDisabled("%zu opération(s)%s", operations_.size(), more_ ? ", d'autres plus anciennes" : "");
      if (more_ && !loading_) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Afficher les plus anciennes"))
          load(app, false);
      }
    }

    void draw_details() {
      if (selected_ < 0 || selected_ >= static_cast< int >(operations_.size()))
        return;
      const Json &operation = operations_[selected_];
      ImGui::BeginChild("journal_details", ImVec2(0, 0), ImGuiChildFlags_Borders);
      ImGui::Text("%s — %s, %s", operation["kind_label"].str().c_str(), operation["by_name"].str().c_str(),
                  display_datetime(operation["at"]).c_str());
      if (!operation["verified"].boolean())
        ImGui::TextColored(colors::orange, "Identité non vérifiée par un badge (%s)", operation["by"].str().c_str());
      if (!operation["lot_name"].str().empty())
        ImGui::Text("Lot : %s (%s)", operation["lot_name"].str().c_str(), operation["lot"].str("supprimé").c_str());
      ImGui::TextWrapped("%s", operation["summary"].str().c_str());
      const Json &details = operation["details"];
      for (const Json &row : details["lacking"].items())
        ImGui::TextColored(colors::red, "Manque %d %s (%d/%d)", row["required"].integer() - row["present"].integer(),
                           row["type_name"].str().c_str(), row["present"].integer(), row["required"].integer());
      for (const auto &[key, label] : kDetailLabels) {
        const Json &value = details[key];
        if (value.is_null() || (value.is_array() && value.size() == 0))
          continue;
        ImGui::TextWrapped("%s : %s", label, detail_text(value).c_str());
      }
      ImGui::EndChild();
    }

    Json        operations_ = Json::array();
    Json        people_     = Json::array();
    Json        kinds_      = Json::array();
    int         selected_   = -1;
    bool        more_       = false;
    bool        loading_    = false;
    int         request_    = 0;
    std::string by_;
    int         kind_ = 0; // 0 : toutes, sinon index + 1 dans kinds_
    std::string lot_;
    bool        with_sub_lots_ = true;
    std::string since_;
    std::string until_;
    std::string text_;
};

} // namespace

std::unique_ptr< AppWindow > make_journal_window() { return std::make_unique< JournalWindow >(); }

} // namespace qrprotec
