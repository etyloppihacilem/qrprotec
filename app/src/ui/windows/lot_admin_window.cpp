/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lot_admin_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../../core/plural.hpp"
#include "../../core/search.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <set>

namespace qrprotec {

namespace {

struct RequirementRow {
    std::string type;
    int         quantity = 1;
    std::string location; // emplacement dans le lot (ex: pochette bleue)
};

// Ligne du contenu propre d'un lot : un attendu de son type (base > 0) avec ce que le lot a en plus et son
// emplacement dans ce lot, ou un item que seul ce lot contient (base 0).
struct LotContentRow {
    std::string type;
    int         base  = 0; // quantite attendue par le type de lot
    int         extra = 0; // en plus dans ce lot
    std::string location;
    std::string base_location; // emplacement du type de lot
};

// Emplacements donnes a des lignes, dans l'ordre enregistre (order) puis les nouveaux dans l'ordre des lignes ; deux
// orthographes du meme nom (casse, accents) n'en font qu'un.
std::vector< std::string > used_locations(const std::vector< std::string > &rows, const std::vector< std::string > &order) {
  std::set< std::string >    used;
  std::set< std::string >    seen;
  std::vector< std::string > result;
  for (const std::string &location : rows)
    if (!name_key(location).empty())
      used.insert(name_key(location));
  const auto add = [&](const std::string &location) {
    const std::string key = name_key(location);
    if (used.count(key) && seen.insert(key).second)
      result.push_back(location);
  };
  for (const std::string &location : order)
    add(location);
  for (const std::string &location : rows)
    add(location);
  return result;
}

// Emplacements a suggerer : ceux des lignes, puis les autres connus (inutilises gardes jusqu'a l'enregistrement)
std::vector< std::string > location_suggestions(std::vector< std::string > places, const std::vector< std::string > &known) {
  for (const std::string &location : known)
    if (!name_key(location).empty() && std::none_of(places.begin(), places.end(), [&](const std::string &place) {
          return name_key(place) == name_key(location);
        }))
      places.push_back(location);
  return places;
}

// Fleches ↑ ↓ de l'ordre des emplacements (celui de la verif, une pochette apres l'autre, et de la fiche du lot).
// Retourne true si `places` a change.
bool location_order_editor(std::vector< std::string > &places) {
  int move = -1, to = -1;
  for (std::size_t index = 0; index < places.size(); ++index) {
    ImGui::PushID(static_cast< int >(index));
    ImGui::BeginDisabled(index == 0);
    if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
      move = static_cast< int >(index);
      to   = move - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(index + 1 == places.size());
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
      move = static_cast< int >(index);
      to   = move + 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%zu. %s", index + 1, places[index].c_str());
    ImGui::PopID();
  }
  if (move < 0)
    return false;
  std::swap(places[static_cast< std::size_t >(move)], places[static_cast< std::size_t >(to)]);
  return true;
}

// Creation et agrement des lots, types de lots et edition des etiquettes publique / privee.
class LotAdminWindow final : public AppWindow {
  public:
    LotAdminWindow() : AppWindow("lot_admin", "Gestion des lots", true, true) {}

    void on_open(App &app) override {
      app.refresh_lots();
      app.refresh_lot_types();
      app.refresh_item_types();
    }

    void draw(App &app) override {
      // listes rechargees : on recharge aussi le lot et le type de lot affiches
      if (seen_lots_version_ != app.catalog.lots_version) {
        seen_lots_version_ = app.catalog.lots_version;
        if (!selected_lot_.empty())
          select_lot(app, selected_lot_);
        if (show_archived_lots_)
          load_all_lots(app);
      }
      if (seen_lot_types_version_ != app.catalog.lot_types_version) {
        seen_lot_types_version_ = app.catalog.lot_types_version;
        for (const Json &lot_type : app.catalog.lot_types.items())
          if (!editing_lot_type_.empty() && lot_type["type"].str() == editing_lot_type_)
            edit_lot_type(lot_type);
      }
      if (!ImGui::BeginTabBar("lot_tabs"))
        return;
      // « Voir le lot » depuis un type de lot unique : bascule sur l'onglet des lots
      const ImGuiTabItemFlags lots_flags = focus_lots_tab_ ? ImGuiTabItemFlags_SetSelected : 0;
      focus_lots_tab_                    = false;
      if (tab_item("Lots", lots_flags)) {
        draw_lots(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Types de lots")) {
        draw_lot_types(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // ---------------------------------------------------------------------------------------------------------------
    void draw_lots(App &app) {
      ImGui::BeginChild("lots_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0), ImGuiChildFlags_Borders);
      if (button("Rafraîchir")) {
        app.refresh_lots();
        app.refresh_lot_types();
      }
      ImGui::SameLine();
      if (button("Nouveau lot"))
        selected_lot_.clear();
      if (ImGui::Checkbox("Afficher les lots archivés", &show_archived_lots_) && show_archived_lots_)
        load_all_lots(app);
      ImGui::Checkbox("Afficher les rangements du stock", &show_storage_);
      // arborescence : chaque lot global suivi de ses sous-lots (archives compris si la case est cochee)
      const Json &lots = show_archived_lots_ ? all_lots_ : app.catalog.lots;
      for (const Json &lot : lots.items()) {
        if (!show_storage_ && lot["storage"].boolean() && lot["id"].str() != selected_lot_)
          continue;
        const std::string id       = lot["id"].str();
        const int         depth    = lot["depth"].integer();
        const bool        archived = !lot["active"].boolean(true);
        const std::string label    = (depth > 0 ? "└ " : "") + lot["name"].str() + "  (" + lot["lot_type_name"].str()
                                  + ")" + (archived ? " – archivé" : "") + "##" + id;
        ImGui::Indent(depth * 16.0f + 1.0f);
        if (archived)
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str(), selected_lot_ == id))
          select_lot(app, id);
        if (archived)
          ImGui::PopStyleColor();
        ImGui::Unindent(depth * 16.0f + 1.0f);
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("lot_details", ImVec2(0, 0), ImGuiChildFlags_Borders);
      if (selected_lot_.empty())
        draw_new_lot(app);
      else
        draw_lot_details(app);
      ImGui::EndChild();
    }

    void load_all_lots(App &app) {
      app.api.get("/api/lots/?all=1", [this, &app](const ApiResult &result) {
        if (result.ok)
          all_lots_ = result.data;
        else
          app.notify("Lots : " + result.error, true);
      });
    }

    // Types proposes a la creation d'un lot : ni archives, ni lots uniques (rangements compris) dont le lot existe deja
    static Json new_lot_types(const App &app) {
      Json result = Json::array();
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (!lot_type["archived"].boolean()
            && !((lot_type["unique"].boolean() || lot_type["storage"].boolean()) && lot_type["lot_count"].integer() > 0))
          result.push_back(lot_type);
      return result;
    }

    static bool type_seal_required(const App &app, const std::string &type) {
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["type"].str() == type)
          return lot_type["seal_required"].boolean();
      return false;
    }

    void draw_new_lot(App &app) {
      ImGui::SeparatorText("Nouveau lot");
      ImGui::TextUnformatted("Type de lot");
      search_select("new_lot_type", new_lot_types(app), "type", "name", new_lot_type_, "Tapez le nom du type de lot…");
      help_marker("Les types archivés et les lots uniques (créés avec leur type) ne sont pas proposés.");
      input_limited("Nom", new_lot_name_, 64);
      const bool name_taken = name_taken_warning(app.catalog.lots, "id", new_lot_name_, "", "Le lot");
      input_limited("Nom court (16 car.)", new_lot_short_, 16);
      ImGui::TextUnformatted("Dans le lot (sous-lot)");
      help_marker("Un sous-lot (ex : le sac d'O2 d'un B+, une armoire d'un VPS) a ses propres étiquettes et se vérifie "
                  "seul, ou avec les autres lots du même lot global. Le lot global est valide si tous ses lots le sont.");
      search_select("new_lot_parent", app.catalog.lots, "id", "name", new_lot_parent_, "Aucun : lot indépendant",
                    "Aucun : lot indépendant");
      input_limited("Emplacement de l'étiquette clé", new_lot_key_location_, 128);
      help_marker("Où trouver l'étiquette clé dans le lot (ex : « poche intérieure du couvercle »). Affiché quand "
                  "quelqu'un essaie de valider une vérif sans l'avoir scannée.");
      // scelle obligatoire : l'etiquette du scelle tient lieu d'etiquette publique
      ImGui::Checkbox(type_seal_required(app, new_lot_type_) ? "Voir l'étiquette clé après création (celle du scellé "
                                                              "sert d'étiquette publique)"
                                                            : "Voir les étiquettes publique et clé après création",
                      &print_new_);
      ImGui::BeginDisabled(new_lot_type_.empty() || new_lot_name_.empty() || name_taken);
      if (primary_button("Créer le lot")) {
        Json body;
        body["lot_type"]   = new_lot_type_;
        body["name"]       = new_lot_name_;
        body["name_short"] = new_lot_short_;
        body["parent"]       = new_lot_parent_;
        body["key_location"] = new_lot_key_location_;
        body["user"]         = app.user_ref();
        const bool print   = print_new_;
        app.api.post("/api/lots/", body, [this, &app, print](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          app.notify("Lot " + result.data["name"].str() + " créé.");
          if (print)
            preview_lot(app, result.data, !result.data["seal_required"].boolean());
          new_lot_name_.clear();
          new_lot_short_.clear();
          new_lot_key_location_.clear();
          selected_lot_ = result.data["id"].str();
          lot_          = result.data;
          app.refresh_lots();
        });
      }
      ImGui::EndDisabled();
      ImGui::Spacing();
      ImGui::TextWrapped("Pour remplir un lot : scannez les items puis l'étiquette clé du lot, et cliquez sur "
                         "\"Ajouter au lot\" dans la pile de scans.");
    }

    void select_lot(App &app, const std::string &id) {
      selected_lot_ = id;
      lot_          = Json();
      app.api.get("/api/lots/" + url_encode(id) + "/", [this, &app, id](const ApiResult &result) {
        if (selected_lot_ != id)
          return;
        if (result.ok) {
          lot_         = result.data;
          edit_name_   = lot_["name"].str();
          edit_short_  = lot_["name_short"].str();
          edit_parent_ = lot_["parent"].str();
          edit_key_location_ = lot_["key_location"].str();
        } else {
          app.notify(result.error, true);
        }
      });
    }

    // apercu des deux etiquettes du lot (publique puis privee), impression depuis la fenetre d'apercu. Un rangement
    // du stock n'a qu'une etiquette, celle de rangement (privee, sans expiration). Sans with_public (creation d'un
    // lot a scelle obligatoire), seulement l'etiquette cle : celle du scelle tient lieu d'etiquette publique.
    void preview_lot(App &app, const Json &lot, bool with_public = true) {
      const Parameters        parameters = lot_parameters(lot);
      std::vector< PrintJob > jobs;
      if (lot["storage"].boolean()) {
        app.build_label_jobs(TemplateCategory::LotStorage, { parameters }, "Rangement " + lot["name"].str(), jobs);
      } else {
        if (with_public)
          app.build_label_jobs(TemplateCategory::LotPublic, { parameters }, "Lot " + lot["name"].str() + " (publique)",
                               jobs);
        app.build_label_jobs(TemplateCategory::LotPrivate, { parameters }, "Lot " + lot["name"].str() + " (clé)", jobs);
      }
      app.preview_jobs(std::move(jobs), "Lot " + lot["name"].str());
    }

    void print_lot(App &app, const Json &lot, bool public_label, bool private_label) {
      const Parameters parameters = lot_parameters(lot);
      if (lot["storage"].boolean()) {
        app.print_labels(TemplateCategory::LotStorage, { parameters }, "Rangement " + lot["name"].str());
        return;
      }
      if (public_label)
        app.print_labels(TemplateCategory::LotPublic, { parameters }, "Lot " + lot["name"].str() + " (publique)");
      if (private_label)
        app.print_labels(TemplateCategory::LotPrivate, { parameters }, "Lot " + lot["name"].str() + " (clé)");
    }

    // Scelle : un lot scelle est valide sans verif ; une verif, un ajout ou un retrait d'items brise le scelle.
    void draw_seal(App &app) {
      ImGui::SeparatorText("Scellé");
      if (lot_["is_sealed"].boolean()) {
        ImGui::Text("Scellé%s %s par %s", lot_["seal_number"].str().empty() ? "" : (" n°" + lot_["seal_number"].str()).c_str(),
                    display_on(lot_["sealed"], true).c_str(), lot_["sealed_by"].str("-").c_str());
        if (button("Étiquettes du scellé"))
          app.preview_seal_labels(lot_);
        ImGui::SameLine();
        if (confirm_button("Briser le scellé", "Le lot devra être vérifié avant utilisation. Continuer ?", "unseal")) {
          Json body;
          body["user"]   = app.user_ref();
          body["reason"] = "ouverture";
          app.api.post("/api/lots/" + url_encode(selected_lot_) + "/unseal/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            lot_ = result.data;
            app.notify("Scellé brisé : le lot doit être vérifié.");
            app.refresh_lots();
          });
        }
        return;
      }
      if (!lot_["unsealed"].is_null())
        ImGui::TextDisabled("Dernier scellé brisé %s par %s", display_on(lot_["unsealed"], true).c_str(),
                            lot_["unsealed_by"].str("-").c_str());
      ImGui::TextWrapped("Un lot scellé est valide sans vérif tant que le scellé est intact. Faites une vérif "
                         "complète, rangez l'étiquette d'ouverture dans le lot, fermez-le avec un scellé et collez "
                         "l'étiquette du scellé dessus. Scanner l'étiquette d'ouverture ouvre le scellé.");
      ImGui::SetNextItemWidth(160.0f);
      input_limited("Numéro du scellé", seal_number_, 32, "facultatif");
      // un lot global se scelle avec ses sous-lots : ils doivent tous etre complets
      // complet et verifie : « Doit etre scelle » (scelle obligatoire, vérif complete faite) est pret a sceller
      const LotStatus status = lot_status(lot_);
      bool            ok     = lot_ok(status) || status == LotStatus::MustSeal;
      if (lot_is_group(lot_)) {
        ImVec4 color;
        group_banner(lot_, color, &ok);
      }
      if (!ok)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        ImGui::TextWrapped("Lot incomplet ou jamais vérifié : faites d'abord une vérif complète.");
        ImGui::PopStyleColor();
      }
      if (ok ? primary_button("Sceller le lot et imprimer les étiquettes") : button("Sceller quand même"))
        seal(app, !ok);
    }

    void seal(App &app, bool force) {
      Json body;
      body["user"]        = app.user_ref();
      body["seal_number"] = seal_number_;
      body["force"]       = force;
      app.api.post("/api/lots/" + url_encode(selected_lot_) + "/seal/", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        lot_ = result.data;
        seal_number_.clear();
        app.notify("Lot scellé : rangez l'étiquette d'ouverture dans le lot et collez l'autre sur le scellé.");
        app.preview_seal_labels(lot_);
        app.refresh_lots();
      });
    }

    void update_lot(App &app, const Json &body) {
      app.api.patch("/api/lots/" + url_encode(selected_lot_) + "/update/", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        lot_ = result.data;
        app.notify(lot_["active"].boolean(true) ? "Lot enregistré." : "Lot archivé.");
        app.refresh_lots();
        if (show_archived_lots_)
          load_all_lots(app);
      });
    }

    void draw_lot_details(App &app) {
      if (lot_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        return;
      }
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
      ImGui::TextUnformatted(lot_["name"].str().c_str());
      ImGui::PopFont();
      ImGui::TextDisabled("%s - %s - version %d%s", lot_["id"].str().c_str(), lot_["lot_type_name"].str().c_str(),
                          lot_["version"].integer(), lot_["storage"].boolean() ? " - rangement du stock" : "");
      if (lot_["path"].size() > 0) {
        std::string path;
        for (const Json &parent : lot_["path"].items())
          path += (path.empty() ? "" : " › ") + parent["name"].str();
        ImGui::Text("Dans : %s", path.c_str());
      }
      if (lot_["children"].size() > 0) {
        std::string children;
        for (const Json &child : lot_["children"].items())
          children += (children.empty() ? "" : ", ") + child["name"].str();
        ImGui::TextWrapped("Sous-lots : %s", children.c_str());
      }
      ImGui::Text("Dernière vérif : %s%s", display_datetime(lot_["last_verif"], true).c_str(),
                  lot_["last_verif_by"].str().empty() ? "" : (" par " + lot_["last_verif_by"].str()).c_str());
      ImGui::SameLine();
      if (small_button("Journal du lot"))
        app.show_journal(lot_["id"].str());
      const LotStatus status = lot_status(lot_);
      if (lot_is_group(lot_)) {
        ImVec4            color;
        const std::string banner = group_banner(lot_, color);
        status_banner(banner, color, 1.1f);
      } else {
        status_banner(lot_status_banner(lot_), lot_status_color(status), 1.1f);
      }
      draw_seal(app);

      ImGui::SeparatorText("Étiquettes");
      if (lot_["storage"].boolean()) {
        // rangement du stock : une seule etiquette, a coller a l'interieur
        if (primary_button("Aperçu de l'étiquette de rangement", ImVec2(-FLT_MIN, 0)))
          preview_lot(app, lot_);
        if (small_button("Impression directe"))
          print_lot(app, lot_, false, true);
      } else {
        if (primary_button("Aperçu des étiquettes publique et clé", ImVec2(-FLT_MIN, 0)))
          preview_lot(app, lot_);
        ImGui::TextDisabled("Impression directe :");
        ImGui::SameLine();
        if (small_button("publique"))
          print_lot(app, lot_, true, false);
        ImGui::SameLine();
        if (small_button("clé"))
          print_lot(app, lot_, false, true);
        ImGui::SameLine();
        if (small_button("les deux"))
          print_lot(app, lot_, true, true);
      }
      if (lot_["verif_key_expires"].is_null())
        ImGui::TextDisabled("Clé sans expiration (rangement du stock)");
      else
        ImGui::TextDisabled("Clé valable jusqu'au %s", display_date(lot_["verif_key_expires"]).c_str());
      if (confirm_button("Régénérer la clé",
                         "L'ancienne étiquette clé ne fonctionnera plus. Continuer ?", "rotate_key")) {
        app.api.post("/api/lots/" + url_encode(selected_lot_) + "/rotate-key/", Json::object(),
                     [this, &app](const ApiResult &result) {
                       if (!result.ok) {
                         app.notify(result.error, true);
                         return;
                       }
                       lot_ = result.data;
                       app.notify(lot_["storage"].boolean()
                                    ? "Nouvelle clé générée : imprimez la nouvelle étiquette de rangement."
                                    : "Nouvelle clé générée : imprimez la nouvelle étiquette clé.");
                     });
      }

      ImGui::SeparatorText("Informations");
      const bool active = lot_["active"].boolean(true);
      if (!active)
        ImGui::TextColored(colors::orange, "Lot archivé.");
      input_limited("Nom", edit_name_, 64);
      const bool name_taken = active && name_taken_warning(app.catalog.lots, "id", edit_name_, selected_lot_, "Le lot");
      input_limited("Nom court", edit_short_, 16);
      ImGui::TextUnformatted("Dans le lot (sous-lot)");
      search_select("edit_lot_parent", app.catalog.lots, "id", "name", edit_parent_, "Aucun : lot indépendant",
                    "Aucun : lot indépendant");
      input_limited("Emplacement de l'étiquette clé", edit_key_location_, 128);
      help_marker("Où trouver l'étiquette clé dans le lot (ex : « poche intérieure du couvercle »). Affiché quand "
                  "quelqu'un essaie de valider une vérif sans l'avoir scannée.");
      ImGui::BeginDisabled(edit_name_.empty() || name_taken);
      if (button("Enregistrer")) {
        Json body;
        body["name"]       = edit_name_;
        body["name_short"] = edit_short_;
        body["parent"]       = edit_parent_;
        body["key_location"] = edit_key_location_;
        update_lot(app, body);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (!active) {
        if (button("Désarchiver le lot")) {
          Json body;
          body["active"] = true;
          update_lot(app, body);
        }
      } else if (button("Archiver le lot…")) {
        ImGui::OpenPopup("archive_lot");
      }
      if (ImGui::BeginPopup("archive_lot")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::Text("Archiver le lot « %s » ?", lot_["name"].str().c_str());
        ImGui::TextWrapped("Il disparaît des listes et ne peut plus être vérifié. Ses items restent enregistrés "
                           "dans le lot. Ses sous-lots doivent d'abord être archivés ou retirés du lot.");
        ImGui::TextWrapped("Pour le désarchiver : cochez « Afficher les lots archivés » au-dessus de la liste, "
                           "sélectionnez-le puis cliquez sur « Désarchiver le lot ».");
        ImGui::PopTextWrapPos();
        if (danger_button("Archiver")) {
          Json body;
          body["active"] = false;
          show_archived_lots_ = true; // le lot reste visible et selectionne
          update_lot(app, body);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Annuler"))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::SameLine();
      if (button(lot_["is_sealed"].boolean() ? "Lancer une vérif (brise le scellé)" : "Lancer une vérif"))
        app.start_verif(selected_lot_, "", app.lot_label_scanned(selected_lot_)); // l'etiquette du lot reste a scanner

      ImGui::SeparatorText("Contenu");
      if (ImGui::BeginTable("lot_requirements", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Emplacement", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : lot_["requirements"].items()) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["type_name"].str().c_str());
          if (row["extra"].integer() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(dont %d propre à ce lot)", static_cast< int >(row["extra"].integer()));
          }
          ImGui::TableNextColumn();
          const std::string location = row["location"].str();
          ImGui::TextDisabled("%s", location.empty() ? "-" : location.c_str());
          ImGui::TableNextColumn();
          stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(-FLT_MIN, 0));
        }
        ImGui::EndTable();
      }
      if (!lot_["storage"].boolean())
        draw_lot_content(app);
      ImGui::Text("%s, dont %s.", nb(lot_["item_count"].integer(), "item").c_str(), nb(lot_["expired_count"].integer(), "périmé").c_str());
      if (ImGui::BeginTable("lot_items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const Json &item : lot_["items"].items()) {
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.4f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", item["iid"].str().c_str());
        }
        ImGui::EndTable();
      }
    }

    // Contenu propre au lot, par-dessus celui de son type : items en plus, emplacements et leur ordre dans ce lot
    // (ex : le sac de soin du VPS n'a pas les memes pochettes que celui du lot A).
    void draw_lot_content(App &app) {
      const Json       *lot_type = find_lot_type(app, lot_["lot_type"].str());
      const std::string source   = lot_["id"].str() + lot_["own_content"].dump() + (lot_type ? lot_type->dump() : "");
      if (content_source_ != source) {
        content_source_ = source;
        load_lot_content(lot_type);
      }
      if (!ImGui::TreeNode("Personnaliser le contenu de ce lot"))
        return;
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("Le contenu attendu du type « %s » vaut pour tous ses lots. Ajoutez ici ce que ce lot "
                          "contient en plus, et ses propres emplacements s'il n'est pas rangé comme les autres.",
                          lot_["lot_type_name"].str().c_str());
      ImGui::PopTextWrapPos();
      std::vector< std::string > row_locations;
      for (const LotContentRow &row : content_)
        row_locations.push_back(row.location);
      std::vector< std::string > places = used_locations(row_locations, content_locations_);
      std::vector< std::string > known  = content_locations_;
      if (lot_type)
        for (const Json &location : (*lot_type)["locations"].items())
          known.push_back(location.str());
      const std::vector< std::string > suggestions = location_suggestions(places, known);
      const Json item_types = without_archived(app.catalog.item_types);
      int        remove     = -1;
      for (std::size_t index = 0; index < content_.size(); ++index) {
        LotContentRow &row = content_[index];
        ImGui::PushID(static_cast< int >(index));
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginDisabled(row.base > 0); // un attendu du type ne se retire pas d'un lot
        search_select("type", row.base > 0 ? app.catalog.item_types : item_types, "type", "name", row.type,
                      "Tapez le nom du type d'item…", nullptr, width * 0.38f);
        ImGui::EndDisabled();
        ImGui::SameLine();
        input_suggest("location", row.location, suggestions, 64,
                      row.base > 0 && row.base_location.empty() ? "Sans emplacement" : "Emplacement", width * 0.26f);
        ImGui::SameLine();
        if (row.base > 0) {
          ImGui::AlignTextToFramePadding();
          ImGui::TextDisabled("%d du type +", row.base);
          ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("##extra", &row.extra);
        row.extra = std::max(row.base > 0 ? 0 : 1, row.extra);
        if (row.base == 0) {
          ImGui::SameLine();
          if (small_button("Retirer"))
            remove = static_cast< int >(index);
        } else if (name_key(row.location) != name_key(row.base_location)) {
          ImGui::SameLine();
          if (small_button("Emplacement du type"))
            row.location = row.base_location;
        }
        ImGui::PopID();
      }
      if (remove >= 0)
        content_.erase(content_.begin() + remove);
      if (button("+ Ajouter un item à ce lot"))
        content_.push_back({ "", 0, 1, "", "" });
      ImGui::SameLine();
      if (primary_button("Enregistrer le contenu du lot")) {
        Json body;
        body["locations"] = Json::array();
        for (const std::string &place : places)
          body["locations"].push_back(place);
        body["content"] = Json::array();
        for (const LotContentRow &row : content_) {
          if (row.type.empty() || (row.base > 0 && row.extra == 0 && name_key(row.location) == name_key(row.base_location)))
            continue; // rien de propre a ce lot
          Json entry;
          entry["type"]     = row.type;
          entry["extra"]    = row.extra;
          entry["location"] = row.location;
          body["content"].push_back(entry);
        }
        app.api.put("/api/lots/" + url_encode(selected_lot_) + "/content/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          lot_ = result.data;
          app.notify("Contenu du lot enregistré.");
          app.refresh_lots();
        });
      }
      if (places.size() > 1) {
        ImGui::SeparatorText("Ordre des emplacements de ce lot");
        ImGui::TextDisabled("Ordre dans lequel les secouristes vérifient ce lot, une pochette après l'autre.");
        if (location_order_editor(places)) {
          content_locations_ = location_suggestions(places, content_locations_);
          std::copy(places.begin(), places.end(), content_locations_.begin());
        }
        if (lot_type && small_button("Reprendre l'ordre du type")) {
          content_locations_.clear();
          for (const Json &location : (*lot_type)["locations"].items())
            content_locations_.push_back(location.str());
        }
        ImGui::TextDisabled("Enregistrez le contenu du lot pour garder l'ordre.");
      }
      ImGui::TreePop();
    }

    static const Json *find_lot_type(const App &app, const std::string &type) {
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["type"].str() == type)
          return &lot_type;
      return nullptr;
    }

    // Lignes de l'editeur : les attendus du type (avec ce que le lot y change) puis les items propres au lot
    void load_lot_content(const Json *lot_type) {
      content_.clear();
      content_locations_.clear();
      const Json &own = lot_["own_content"];
      for (const Json &location : own["locations"].items())
        content_locations_.push_back(location.str());
      if (lot_type) {
        if (content_locations_.empty())
          for (const Json &location : (*lot_type)["locations"].items())
            content_locations_.push_back(location.str());
        for (const Json &row : (*lot_type)["requirements"].items())
          content_.push_back({ row["type"].str(), static_cast< int >(row["quantity"].integer(1)), 0,
                               row["location"].str(), row["location"].str() });
      }
      for (const Json &row : own["rows"].items()) {
        const auto found = std::find_if(content_.begin(), content_.end(),
                                        [&](const LotContentRow &line) { return line.type == row["type"].str(); });
        const int extra = static_cast< int >(row["extra"].integer());
        if (found == content_.end()) {
          if (extra > 0)
            content_.push_back({ row["type"].str(), 0, extra, row["location"].str(), "" });
          continue;
        }
        found->extra = extra;
        if (!row["location"].is_null())
          found->location = row["location"].str();
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    void draw_lot_types(App &app) {
      ImGui::BeginChild("types_list", ImVec2(ImGui::GetContentRegionAvail().x * 0.35f, 0), ImGuiChildFlags_Borders);
      if (button("Nouveau type"))
        edit_lot_type(Json());
      ImGui::Checkbox("Afficher les types archivés", &show_archived_types_);
      for (const Json &lot_type : app.catalog.lot_types.items()) {
        const bool archived = lot_type["archived"].boolean();
        if (archived && !show_archived_types_)
          continue;
        const std::string code  = lot_type["type"].str();
        const std::string label = lot_type["name"].str() + " (" + code + ")" + (lot_type["unique"].boolean() ? " – unique" : "")
                                + (archived ? " – archivé" : "");
        if (archived)
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str(), editing_lot_type_ == code))
          edit_lot_type(lot_type);
        if (archived)
          ImGui::PopStyleColor();
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("type_form", ImVec2(0, 0), ImGuiChildFlags_Borders);
      ImGui::SeparatorText(editing_lot_type_.empty() ? "Nouveau type de lot" : "Type de lot");
      if (type_archived_)
        ImGui::TextColored(colors::orange, "Type archivé.");
      ImGui::BeginDisabled(!editing_lot_type_.empty());
      ImGui::SetNextItemWidth(120.0f);
      input_limited("Code (6 caractères)", type_code_, 6, nullptr, 0, true);
      ImGui::EndDisabled();
      bool code_taken = false;
      if (editing_lot_type_.empty())
        for (const Json &lot_type : app.catalog.lot_types.items())
          if (lot_type["type"].str() == type_code_) {
            ImGui::TextColored(colors::red, "Le code %s est déjà celui de « %s ».", type_code_.c_str(),
                               lot_type["name"].str().c_str());
            code_taken = true;
          }
      input_limited("Nom", type_name_, 64);
      bool name_taken = name_taken_warning(app.catalog.lot_types, "type", type_name_, editing_lot_type_, "Le type de lot");
      // lot unique : son lot porte le nom du type
      if (!name_taken && type_unique_ && editing_lot_type_.empty())
        name_taken = name_taken_warning(app.catalog.lots, "id", type_name_, "", "Le lot");
      ImGui::TextUnformatted("Description");
      ImGui::InputTextMultiline("##description", &type_description_, ImVec2(-FLT_MIN, 50));
      if (ImGui::Checkbox("Rangement du stock (armoire, tiroir...)", &type_storage_) && type_storage_)
        type_unique_ = true;
      help_marker("Les items rangés dans un lot de ce type restent comptés dans le stock. Un rangement est un lot "
                  "unique (un type par armoire, par tiroir), sans contenu attendu (on y range ce qu'on veut), et son "
                  "étiquette clé n'expire pas. Il se vérifie seul (un tiroir) ou avec les autres rangements du "
                  "même lot global (une armoire et ses tiroirs). Y ranger des items ne demande pas de vérif.");
      if (type_storage_ && !editing_lot_type_.empty() && !requirements_.empty())
        ImGui::TextColored(colors::orange, "Enregistrer supprimera le contenu attendu de ce type.");
      ImGui::BeginDisabled(type_storage_);
      ImGui::Checkbox("Scellé obligatoire (ex : kit accouchement)", &type_seal_required_);
      ImGui::EndDisabled();
      help_marker("Un lot de ce type doit toujours être scellé : une fois le scellé ouvert, il est rouge (et non "
                  "orange). Une vérif complète le marque « doit être scellé », toujours en rouge, jusqu'à la pose "
                  "d'un nouveau scellé.");
      ImGui::BeginDisabled(type_storage_);
      ImGui::Checkbox("Lot unique (le lot est créé avec le type)", &type_unique_);
      ImGui::EndDisabled();
      help_marker("Pour un lot qui n'existe qu'en un exemplaire (ex : le VPS, chacune de ses armoires) : le lot, du "
                  "même nom, est créé en même temps que le type, et aucun autre lot de ce type ne peut être créé. "
                  "Son nom court est aussi le nom du type. Renommer le type renomme son lot, l'archiver archive "
                  "son lot.");
      if (editing_lot_type_.empty()) {
        if (type_unique_) {
          ImGui::Indent();
          ImGui::TextDisabled("Nom court du lot : le nom du type (tronqué à 16 caractères)");
          ImGui::TextUnformatted("Dans le lot (sous-lot)");
          search_select("new_type_lot_parent", app.catalog.lots, "id", "name", type_lot_parent_, "Aucun : lot indépendant",
                        "Aucun : lot indépendant");
          ImGui::Checkbox("Voir les étiquettes du lot après création", &print_new_);
          ImGui::Unindent();
        }
        ImGui::BeginDisabled(type_code_.size() != 6 || type_name_.empty() || name_taken || code_taken);
        if (primary_button(type_unique_ ? "Créer le type et son lot" : "Créer le type de lot")) {
          Json body;
          body["type"]          = type_code_;
          body["name"]          = type_name_;
          body["description"]   = type_description_;
          body["storage"]       = type_storage_;
          body["unique"]        = type_unique_;
          body["seal_required"] = type_seal_required_ && !type_storage_;
          body["user"]          = app.user_ref();
          if (type_unique_) {
            body["parent"] = type_lot_parent_;
          }
          const bool print = type_unique_ && print_new_;
          app.api.post("/api/lot-types/", body, [this, &app, print](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            const bool with_lot = !result.data["created_lot"].is_null();
            if (result.data["storage"].boolean())
              app.notify("Rangement créé.");
            else
              app.notify(with_lot ? "Type et lot créés : ajoutez maintenant son contenu attendu."
                                  : "Type de lot créé : ajoutez maintenant son contenu attendu.");
            if (with_lot && print)
              preview_lot(app, result.data["created_lot"], !result.data["seal_required"].boolean());
            edit_lot_type(result.data);
            app.refresh_lot_types();
            if (with_lot)
              app.refresh_lots();
          });
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        return;
      }
      if (type_unique_ && !type_lot_.empty()) {
        ImGui::TextDisabled("Lot :");
        ImGui::SameLine();
        if (small_button("Voir le lot")) {
          select_lot(app, type_lot_);
          focus_lots_tab_ = true;
        }
      }
      ImGui::BeginDisabled(type_name_.empty() || name_taken);
      if (button("Enregistrer")) {
        Json body;
        body["name"]          = type_name_;
        body["description"]   = type_description_;
        body["storage"]       = type_storage_;
        body["unique"]        = type_unique_;
        body["seal_required"] = type_seal_required_ && !type_storage_;
        app.api.patch("/api/lot-types/" + url_encode(editing_lot_type_) + "/", body, [&app](const ApiResult &result) {
          app.notify(result.ok ? "Type de lot enregistré." : result.error, !result.ok);
          app.refresh_lot_types();
          app.refresh_lots(); // nom du lot d'un type unique
        });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      draw_archive_lot_type(app);

      ImGui::SeparatorText("Contenu attendu");
      if (type_storage_) {
        ImGui::TextDisabled("Un rangement du stock n'a pas de contenu attendu : on y range ce qu'on veut.");
        ImGui::EndChild();
        return;
      }
      ImGui::TextDisabled("Type d'item, emplacement dans le lot (facultatif : pendant la vérif, les attendus sont "
                          "regroupés par emplacement), quantité.");
      int        remove     = -1;
      const Json item_types = without_archived(app.catalog.item_types);
      // emplacements deja donnes dans ce type de lot, dans l'ordre de la verif : suggeres a la saisie (un nouveau
      // l'est aussitot tape), les inutilises seulement en suggestion jusqu'a l'enregistrement
      std::vector< std::string > row_locations;
      for (const RequirementRow &row : requirements_)
        row_locations.push_back(row.location);
      std::vector< std::string > places      = used_locations(row_locations, locations_);
      const std::vector< std::string > suggestions = location_suggestions(places, locations_);
      for (std::size_t index = 0; index < requirements_.size(); ++index) {
        RequirementRow &row = requirements_[index];
        ImGui::PushID(static_cast< int >(index));
        const float width = ImGui::GetContentRegionAvail().x;
        search_select("type", item_types, "type", "name", row.type, "Tapez le nom du type d'item…",
                      nullptr, width * 0.42f);
        ImGui::SameLine();
        input_suggest("location", row.location, suggestions, 64, "Emplacement (ex : pochette bleue)", width * 0.28f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("##qty", &row.quantity);
        row.quantity = std::max(1, row.quantity);
        ImGui::SameLine();
        if (small_button("Retirer"))
          remove = static_cast< int >(index);
        ImGui::PopID();
      }
      if (remove >= 0)
        requirements_.erase(requirements_.begin() + remove);
      if (button("+ Ajouter une ligne"))
        requirements_.push_back({});
      ImGui::SameLine();
      if (primary_button("Enregistrer le contenu")) {
        Json body;
        body["locations"] = Json::array();
        for (const std::string &place : places)
          body["locations"].push_back(place);
        body["requirements"] = Json::array();
        for (const RequirementRow &row : requirements_) {
          if (row.type.empty())
            continue;
          Json entry;
          entry["type"]     = row.type;
          entry["quantity"] = row.quantity;
          entry["location"] = row.location;
          body["requirements"].push_back(entry);
        }
        app.api.put("/api/lot-types/" + url_encode(editing_lot_type_) + "/requirements/", body,
                    [this, &app](const ApiResult &result) {
                      if (!result.ok) {
                        app.notify(result.error, true);
                        return;
                      }
                      app.notify("Contenu enregistre (version " + result.data["version"].str() + ").");
                      edit_lot_type(result.data);
                      app.refresh_lot_types();
                    });
      }
      // ordre des emplacements : celui de la verif (une pochette apres l'autre) et de la fiche du lot
      if (places.size() > 1) {
        ImGui::SeparatorText("Ordre des emplacements");
        ImGui::TextDisabled("Ordre dans lequel les secouristes les vérifient, une pochette après l'autre.");
        if (location_order_editor(places)) {
          locations_ = suggestions; // les inutilises gardes a la suite, toujours suggeres
          std::copy(places.begin(), places.end(), locations_.begin());
        }
        ImGui::TextDisabled("Enregistrez le contenu pour garder l'ordre.");
      }
      ImGui::EndChild();
    }

    // Archivage : le type n'est plus propose a la creation de lots. Un lot unique est archive avec son type.
    void draw_archive_lot_type(App &app) {
      if (type_archived_) {
        if (button("Désarchiver le type"))
          archive_lot_type(app, false);
        return;
      }
      if (button("Archiver le type…"))
        ImGui::OpenPopup("archive_lot_type");
      if (ImGui::BeginPopup("archive_lot_type")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::Text("Archiver le type « %s » ?", type_name_.c_str());
        ImGui::TextWrapped(type_unique_ ? "Il ne sera plus proposé, et son lot est archivé avec lui (ses sous-lots "
                                          "doivent d'abord être archivés ou retirés du lot)."
                                        : "Il ne sera plus proposé à la création de lots. Ses lots doivent d'abord "
                                          "être archivés.");
        ImGui::TextWrapped("Pour le désarchiver : cochez « Afficher les types archivés » au-dessus de la liste, "
                           "sélectionnez-le puis cliquez sur « Désarchiver le type ».");
        ImGui::PopTextWrapPos();
        if (danger_button("Archiver")) {
          archive_lot_type(app, true);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Annuler"))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }

    void archive_lot_type(App &app, bool archived) {
      Json body;
      body["archived"] = archived;
      app.api.patch("/api/lot-types/" + url_encode(editing_lot_type_) + "/", body,
                    [this, &app, archived](const ApiResult &result) {
                      if (!result.ok) {
                        app.notify(result.error, true);
                        return;
                      }
                      app.notify(archived ? "Type de lot archivé." : "Type de lot désarchivé.");
                      if (archived)
                        show_archived_types_ = true; // le type reste visible et selectionne
                      edit_lot_type(result.data);
                      app.refresh_lot_types();
                      app.refresh_lots();
                      if (show_archived_lots_)
                        load_all_lots(app);
                    });
    }

    void edit_lot_type(const Json &lot_type) {
      editing_lot_type_ = lot_type["type"].str();
      type_code_          = editing_lot_type_;
      type_name_          = lot_type["name"].str();
      type_description_   = lot_type["description"].str();
      type_storage_       = lot_type["storage"].boolean();
      type_unique_        = lot_type["unique"].boolean();
      type_seal_required_ = lot_type["seal_required"].boolean();
      type_archived_      = lot_type["archived"].boolean();
      type_lot_           = lot_type["lot"].str();
      type_lot_parent_.clear();
      requirements_.clear();
      locations_.clear();
      for (const Json &location : lot_type["locations"].items())
        locations_.push_back(location.str());
      for (const Json &row : lot_type["requirements"].items())
        requirements_.push_back({ row["type"].str(), row["quantity"].integer(1), row["location"].str() });
    }

    // Lots
    std::string selected_lot_;
    int         seen_lots_version_      = -1;
    int         seen_lot_types_version_ = -1;
    Json        lot_;
    std::string edit_name_;
    std::string edit_short_;
    std::string edit_parent_;
    std::string edit_key_location_;
    std::string new_lot_parent_;
    std::string new_lot_key_location_;
    std::string new_lot_type_;
    std::string new_lot_name_;
    std::string new_lot_short_;
    bool        print_new_ = true;
    std::string seal_number_;
    bool        show_archived_lots_ = false;
    bool        show_storage_       = false; // rangements du stock masques par defaut
    Json        all_lots_           = Json::array(); // archives compris (case « Afficher les lots archivés »)
    bool        focus_lots_tab_     = false;
    // Types de lots
    std::string                   editing_lot_type_;
    std::string                   type_code_;
    std::string                   type_name_;
    std::string                   type_description_;
    bool                          type_storage_ = false;
    bool                          type_unique_  = false;
    bool                          type_seal_required_ = false;
    bool                          type_archived_ = false;
    std::string                   type_lot_;        // lot d'un type unique
    std::string                   type_lot_parent_; // creation d'un type unique : parent de son lot
    bool                          show_archived_types_ = false;
    std::vector< RequirementRow > requirements_;
    std::vector< std::string >    locations_; // emplacements du type de lot, dans l'ordre de la verif
    // Contenu propre au lot affiche
    std::string                  content_source_; // lot et type d'ou viennent les lignes (rechargees s'ils changent)
    std::vector< LotContentRow > content_;
    std::vector< std::string >   content_locations_; // ordre des emplacements du lot
};

} // namespace

std::unique_ptr< AppWindow > make_lot_admin_window() {
  return std::make_unique< LotAdminWindow >();
}

} // namespace qrprotec
