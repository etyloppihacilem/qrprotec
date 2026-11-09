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
#include <map>
#include <set>

namespace qrprotec {

namespace {

struct RequirementRow {
    std::string type;
    int         quantity = 1;
    std::string location; // emplacement dans le lot (ex: pochette bleue)
    bool        optional = false; // facultatif a la verif (seulement du non consommable sans taches)
};

// Un type d'item peut etre facultatif dans un lot : non consommable et sans taches (ItemType.can_be_optional)
bool can_be_optional(const Json &item_types, const std::string &type) {
  for (const Json &item_type : item_types.items())
    if (item_type["type"].str() == type)
      return !item_type["consumable"].boolean(true) && item_type["tasks"].size() == 0;
  return false;
}

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

    void reset_view(App &) override {
      selected_lot_.clear();
      lot_ = Json();
      seal_number_.clear();
      show_archived_lots_  = false;
      show_storage_        = false;
      show_archived_types_ = false;
      focus_lots_tab_      = true;
      edit_lot_type(Json()); // formulaire de type de lot vide (nouveau type)
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
      if (primary_button("Nouveau lot"))
        selected_lot_.clear();
      ImGui::SameLine();
      if (button("Rafraîchir")) {
        app.refresh_lots();
        app.refresh_lot_types();
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##lot_search", lot_search_, 64, "Rechercher un lot (nom, type)…");
      if (ImGui::Checkbox("Lots archivés", &show_archived_lots_) && show_archived_lots_)
        load_all_lots(app);
      ImGui::SameLine();
      ImGui::Checkbox("Rangements du stock", &show_storage_);
      ImGui::Separator();
      // Trois groupes : les lots (arborescence, chaque lot global suivi de ses sous-lots), les rangements du stock,
      // puis les lots archives. Avec une recherche : seulement les lots trouves, a plat, avec leur lot global.
      const Json &lots      = show_archived_lots_ ? all_lots_ : app.catalog.lots;
      const bool  searching = !lot_search_.empty();
      ImGui::BeginChild("lots_scroll");
      const auto matches = [&](const Json &lot) {
        return !searching || search_matches(lot_search_, lot["name"].str())
            || search_matches(lot_search_, lot["lot_type_name"].str()) || search_matches(lot_search_, lot["id"].str());
      };
      // attention d'abord (serializers.attention_order) : chaque lot global suivi de ses sous-lots
      std::vector< const Json * > ordered;
      for (const Json &lot : lots.items())
        ordered.push_back(&lot);
      std::stable_sort(ordered.begin(), ordered.end(), [](const Json *a, const Json *b) {
        return (*a)["attention_order"].integer() < (*b)["attention_order"].integer();
      });
      int shown = 0;
      const auto group = [&](const char *title, auto keep) {
        bool header = false;
        for (const Json *row : ordered) {
          const Json &lot = *row;
          if (!keep(lot) || !matches(lot))
            continue;
          if (!header) {
            header = true;
            if (title)
              ImGui::SeparatorText(title);
          }
          draw_lot_row(app, lot, searching);
          ++shown;
        }
      };
      group(nullptr, [](const Json &lot) { return lot["active"].boolean(true) && !lot["storage"].boolean(); });
      if (show_storage_ || searching)
        group("Rangements du stock", [](const Json &lot) { return lot["active"].boolean(true) && lot["storage"].boolean(); });
      else if (!selected_lot_.empty())
        group("Rangements du stock", [this](const Json &lot) {  // le rangement choisi reste visible
          return lot["storage"].boolean() && lot["id"].str() == selected_lot_ && lot["active"].boolean(true);
        });
      if (show_archived_lots_)
        group("Archivés", [](const Json &lot) { return !lot["active"].boolean(true); });
      if (shown == 0)
        ImGui::TextDisabled(searching ? "Aucun lot trouvé." : "Aucun lot.");
      ImGui::EndChild();
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginChild("lot_details", ImVec2(0, 0), ImGuiChildFlags_Borders);
      if (selected_lot_.empty())
        draw_new_lot(app);
      else
        draw_lot_details(app);
      ImGui::EndChild();
    }

    // Une ligne de la liste des lots : pastille de l'etat, nom, type en gris. Les sous-lots sont decales sous leur
    // lot global ; a plat (recherche), le lot global est rappele en gris.
    void draw_lot_row(App &app, const Json &lot, bool flat) {
      const std::string id       = lot["id"].str();
      const bool        archived = !lot["active"].boolean(true);
      const float       indent   = flat ? 0.0f : lot["depth"].integer() * 18.0f;
      ImGui::PushID(id.c_str());
      if (indent > 0)
        ImGui::Indent(indent);
      const float  radius = ImGui::GetFontSize() * 0.28f;
      const ImVec2 cursor = ImGui::GetCursorScreenPos();
      // lot global : couleur de l'ensemble de ses lots, comme la fenetre Lots
      const bool        top   = lot["depth"].integer() == 0 && !lot["global"].is_null();
      const std::string kind  = lot["global"]["kind"].str();
      const ImVec4      color = archived ? colors::grey
                              : top      ? (kind == "ok" ? colors::green : kind == "warn" ? colors::orange : colors::red)
                                         : lot_status_color(lot_status(lot));
      // ligne entiere cliquable, nom puis type en gris par-dessus
      if (ImGui::Selectable("##lot", selected_lot_ == id, ImGuiSelectableFlags_AllowOverlap))
        select_lot(app, id);
      if (!top && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", archived ? "Archivé" : lot_status_label(lot_status(lot)));
      // pastille de l'etat (gris si archive), par-dessus la ligne (meme selectionnee)
      ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + radius + 2.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f), radius,
        ImGui::GetColorU32(color));
      ImGui::SameLine();
      ImGui::SetCursorScreenPos(ImVec2(cursor.x + radius * 2.0f + 8.0f, cursor.y));
      if (archived)
        ImGui::TextDisabled("%s", lot["name"].str().c_str());
      else
        ImGui::TextUnformatted(lot["name"].str().c_str());
      ImGui::SameLine();
      std::string detail = lot["lot_type_name"].str();
      if (lot["spare"].boolean() && lot["parent"].str().empty())
        detail += " - rechange";
      if (flat && !lot["parent"].str().empty())
        for (const Json &parent : (show_archived_lots_ ? all_lots_ : app.catalog.lots).items())
          if (parent["id"].str() == lot["parent"].str()) {
            detail += " - dans " + parent["name"].str();
            break;
          }
      ImGui::TextDisabled("%s", detail.c_str());
      if (indent > 0)
        ImGui::Unindent(indent);
      ImGui::PopID();
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

    // Referentiel dont ce type est un rangement (ex : « Lot B+ » pour un sac de soin), vide sinon
    static std::string rangement_of(const App &app, const std::string &type) {
      std::string parent;
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["type"].str() == type)
          parent = lot_type["part_of"].str();
      if (!parent.empty())
        for (const Json &lot_type : app.catalog.lot_types.items())
          if (lot_type["type"].str() == parent)
            return lot_type["name"].str();
      return parent;
    }

    // Lot d'un rangement sans lot parent : case « Sac de rechange », sinon creation refusee (comme le serveur).
    // Retourne vrai si le lot peut etre cree / enregistre.
    static bool spare_choice(const App &app, const std::string &type, const std::string &parent, bool &spare) {
      const std::string referentiel = rangement_of(app, type);
      if (referentiel.empty())
        return true;
      if (!parent.empty())
        return true;
      ImGui::Checkbox("Sac de rechange (libre, hors de tout lot)", &spare);
      ImGui::SameLine();
      help_marker("Un rangement (ex : un sac de soin) se range dans un lot. Seul un sac de rechange reste libre : il "
                  "sert à l'assemblage ou à remplacer un sac en panne.");
      if (!spare) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        ImGui::TextWrapped("Rangement de %s : choisissez le lot où il est rangé, ou cochez « Sac de rechange ».",
                           referentiel.c_str());
        ImGui::PopStyleColor();
      }
      return spare;
    }

    static bool type_seal_required(const App &app, const std::string &type) {
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["type"].str() == type)
          return lot_type["seal_required"].boolean();
      return false;
    }

    void draw_new_lot(App &app) {
      ImGui::SeparatorText("Nouveaux lots");
      ImGui::TextUnformatted("Type de lot");
      search_select("new_lot_type", new_lot_types(app), "type", "name", new_lot_type_, "Tapez le nom du type de lot…");
      help_marker("Les types archivés et les lots uniques (créés avec leur type) ne sont pas proposés.");
      if (new_lot_type_ != batch_type_) {
        batch_type_ = new_lot_type_;
        batch_base_.clear();
        batch_base_short_.clear();
        batch_subs_.clear();
        batch_rows_.clear();
        if (!new_lot_type_.empty())
          load_batch(app, true);
      }
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
      if (ImGui::InputInt("Nombre de lots", &new_lot_count_, 1, 1)) {
        new_lot_count_ = std::clamp(new_lot_count_, 1, 30);
        if (!new_lot_type_.empty())
          load_batch(app, false);
      }
      if (!batch_subs_.empty()) {
        ImGui::TextUnformatted("Sous-lots de chaque lot");
        help_marker("Proposés d'après les lots déjà créés de ce type et les rangements du référentiel. Mettez 0 pour "
                    "un rangement qui n'en fait pas partie (le sac de soin du VPS n'est pas un sac des lots B+).");
        for (BatchSub &sub : batch_subs_) {
          ImGui::PushID(sub.type.c_str());
          ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
          if (ImGui::InputInt((sub.name + (sub.rangement ? "  (rangement)" : "")).c_str(), &sub.count, 1, 1)) {
            sub.count = std::clamp(sub.count, 0, 9);
            load_batch(app, false);
          }
          ImGui::PopID();
        }
      }
      if (batch_mode())
        draw_batch(app);
      else
        draw_single_lot(app);
    }

    bool batch_mode() const {
      if (new_lot_count_ > 1)
        return true;
      for (const BatchSub &sub : batch_subs_)
        if (sub.count > 0)
          return true;
      return false;
    }

    // Noms proposes par le serveur (numerotes apres l'existant), sous-lots par defaut au changement de type
    void load_batch(App &app, bool default_subs) {
      Json body;
      body["lot_type"]   = new_lot_type_;
      body["count"]      = new_lot_count_;
      body["preview"]    = true;
      body["name"]       = batch_base_;
      body["name_short"] = batch_base_short_;
      if (!default_subs) {
        Json subs = Json::array();
        for (const BatchSub &sub : batch_subs_) {
          Json row;
          row["lot_type"] = sub.type;
          row["count"]    = sub.count;
          subs.push_back(row);
        }
        body["sub_lots"] = subs;
      }
      const std::string type = new_lot_type_;
      app.api.post("/api/lots/batch/", body, [this, &app, type, default_subs](const ApiResult &result) {
        if (type != new_lot_type_)
          return;  // type change entre-temps
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        if (default_subs) {
          batch_subs_.clear();
          for (const Json &row : result.data["sub_types"].items())
            batch_subs_.push_back({ row["lot_type"].str(), row["name"].str(), static_cast< int >(row["count"].integer()),
                                    row["rangement"].boolean() });
        }
        batch_rows_.clear();
        for (const Json &lot : result.data["lots"].items()) {
          batch_rows_.push_back({ type, "", lot["name"].str(), lot["name_short"].str(), lot["key_location"].str(), 0 });
          for (const Json &sub : lot["sub_lots"].items())
            batch_rows_.push_back({ sub["lot_type"].str(), sub["lot_type_name"].str(), sub["name"].str(),
                                    sub["name_short"].str(), sub["key_location"].str(), 1 });
        }
      });
    }

    void draw_batch(App &app) {
      if (input_limited("Nom de base", batch_base_, 56, "Le nom du type : « B+ » donne B+ 1, B+ 2…"))
        load_batch(app, false);
      if (input_limited("Nom court de base", batch_base_short_, 12, "Le nom de base"))
        load_batch(app, false);
      ImGui::TextUnformatted("Dans le lot");
      search_select("batch_parent", app.catalog.lots, "id", "name", new_lot_parent_, "Aucun : lots indépendants",
                    "Aucun : lots indépendants");
      const bool spare_ok = spare_choice(app, new_lot_type_, new_lot_parent_, new_lot_spare_);
      ImGui::SeparatorText("Lots prévus");
      ImGui::TextDisabled("Noms modifiables. Changer le nombre ou le nom de base les recalcule.");
      bool taken = false;
      std::set< std::string > seen;
      // colonnes elargissables a la souris (bord de l'en-tete), largeurs gardees dans imgui.ini
      if (ImGui::BeginTable("batch_rows", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
                                                 | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Nom court", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Emplacement de l'étiquette clé", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (size_t index = 0; index < batch_rows_.size(); ++index) {
          BatchRow &row = batch_rows_[index];
          ImGui::PushID(static_cast< int >(index));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (row.depth > 0)
            ImGui::TextDisabled("└");
          if (row.depth > 0)
            ImGui::SameLine();
          ImGui::SetNextItemWidth(-FLT_MIN);
          input_limited("##name", row.name, 64);
          const std::string key = normalize_search(row.name);
          const bool        dup = !seen.insert(key).second || !name_taken(app.catalog.lots, "id", row.name, "").empty();
          if (dup || row.name.empty()) {
            taken = true;
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "%s", row.name.empty() ? "vide" : "déjà pris");
          }
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-FLT_MIN);
          input_limited("##short", row.short_name, 16);
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-FLT_MIN);
          input_limited("##key", row.key_location, 128);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::Checkbox("Voir toutes les étiquettes après création (publiques puis clés, groupées)", &print_new_);
      size_t lots = 0;
      for (const BatchRow &row : batch_rows_)
        lots += row.depth == 0;
      const std::string label = "Créer " + std::to_string(batch_rows_.size()) + (batch_rows_.size() > 1 ? " lots" : " lot")
                              + (batch_rows_.size() > lots ? " (sous-lots compris)" : "");
      ImGui::BeginDisabled(batch_rows_.empty() || taken || !spare_ok);
      if (primary_button(label.c_str())) {
        Json list = Json::array();
        Json current;
        bool has_current = false;
        auto row_json    = [](const BatchRow &row) {
          Json item;
          item["lot_type"]     = row.type;
          item["name"]         = row.name;
          item["name_short"]   = row.short_name;
          item["key_location"] = row.key_location;
          return item;
        };
        Json subs = Json::array();
        for (const BatchRow &row : batch_rows_) {
          if (row.depth == 0) {
            if (has_current) {
              current["sub_lots"] = subs;
              list.push_back(current);
            }
            current     = row_json(row);
            subs        = Json::array();
            has_current = true;
          } else {
            subs.push_back(row_json(row));
          }
        }
        if (has_current) {
          current["sub_lots"] = subs;
          list.push_back(current);
        }
        Json body;
        body["lot_type"] = new_lot_type_;
        body["lots"]     = list;
        body["parent"]   = new_lot_parent_;
        body["spare"]    = new_lot_parent_.empty() && new_lot_spare_;
        body["user"]     = app.user_ref();
        const bool print = print_new_;
        app.api.post("/api/lots/batch/", body, [this, &app, print](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          const Json &created = result.data["lots"];
          app.notify(std::to_string(created.size()) + (created.size() > 1 ? " lots créés." : " lot créé."));
          if (print)
            preview_lots(app, created);
          batch_rows_.clear();
          batch_type_.clear();  // recharge les noms suivants
          app.refresh_lots();
        });
      }
      ImGui::EndDisabled();
      if (taken)
        ImGui::TextDisabled("Un nom est vide, donné deux fois ou déjà pris par un lot.");
    }

    // Etiquettes de plusieurs lots en un seul apercu : toutes les publiques (sauf scelle obligatoire), puis les cles
    void preview_lots(App &app, const Json &lots) {
      std::vector< Parameters > publics, keys;
      for (const Json &lot : lots.items()) {
        if (lot["storage"].boolean())
          continue;
        if (!lot["seal_required"].boolean())
          publics.push_back(lot_parameters(lot));
        keys.push_back(lot_parameters(lot));
      }
      std::vector< PrintJob > jobs;
      const std::string       what = std::to_string(lots.size()) + " lots";
      if (!publics.empty())
        app.build_label_jobs(TemplateCategory::LotPublic, publics, what + " (publiques)", jobs);
      if (app.declared_identity && !keys.empty())
        app.build_label_jobs(TemplateCategory::LotPrivate, keys, what + " (clés)", jobs);
      app.preview_jobs(std::move(jobs), what);
    }

    void draw_single_lot(App &app) {
      input_limited("Nom", new_lot_name_, 64);
      const bool name_taken = name_taken_warning(app.catalog.lots, "id", new_lot_name_, "", "Le lot");
      input_limited("Nom court (16 car.)", new_lot_short_, 16);
      ImGui::TextUnformatted("Dans le lot (sous-lot)");
      help_marker("Un sous-lot (ex : le sac d'O2 d'un B+, une armoire d'un VPS) a ses propres étiquettes et se vérifie "
                  "seul, ou avec les autres lots du même lot global. Le lot global est valide si tous ses lots le sont.");
      search_select("new_lot_parent", app.catalog.lots, "id", "name", new_lot_parent_, "Aucun : lot indépendant",
                    "Aucun : lot indépendant");
      const bool spare_ok = spare_choice(app, new_lot_type_, new_lot_parent_, new_lot_spare_);
      input_limited("Emplacement de l'étiquette clé", new_lot_key_location_, 128);
      help_marker("Où trouver l'étiquette clé dans le lot (ex : « poche intérieure du couvercle »). Affiché quand "
                  "quelqu'un essaie de valider une vérif sans l'avoir scannée.");
      // scelle obligatoire : l'etiquette du scelle tient lieu d'etiquette publique
      ImGui::Checkbox(type_seal_required(app, new_lot_type_) ? "Voir l'étiquette clé après création (celle du scellé "
                                                              "sert d'étiquette publique)"
                                                            : "Voir les étiquettes publique et clé après création",
                      &print_new_);
      ImGui::BeginDisabled(new_lot_type_.empty() || new_lot_name_.empty() || name_taken || !spare_ok);
      if (primary_button("Créer le lot")) {
        Json body;
        body["lot_type"]   = new_lot_type_;
        body["name"]       = new_lot_name_;
        body["name_short"] = new_lot_short_;
        body["parent"]       = new_lot_parent_;
        body["spare"]        = new_lot_parent_.empty() && new_lot_spare_;
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
          edit_optional_     = lot_["optional"].boolean();
          edit_spare_        = lot_["spare"].boolean();
        } else {
          app.notify(result.error, true);
        }
      });
    }

    // apercu des deux etiquettes du lot (publique puis privee), impression depuis la fenetre d'apercu. Un rangement
    // du stock n'a qu'une etiquette, celle de rangement (privee, sans expiration). Sans with_public (creation d'un
    // lot a scelle obligatoire), seulement l'etiquette cle : celle du scelle tient lieu d'etiquette publique. Badge
    // obligatoire (nom declare desactive) : pas d'etiquette cle, elle n'autorise rien de plus que la publique
    // (impression directe « clé » toujours possible).
    void preview_lot(App &app, const Json &lot, bool with_public = true) {
      const Parameters        parameters = lot_parameters(lot);
      std::vector< PrintJob > jobs;
      if (lot["storage"].boolean()) {
        app.build_label_jobs(TemplateCategory::LotStorage, { parameters }, "Rangement " + lot["name"].str(), jobs);
      } else {
        if (with_public)
          app.build_label_jobs(TemplateCategory::LotPublic, { parameters }, "Lot " + lot["name"].str() + " (publique)",
                               jobs);
        if (app.declared_identity)
          app.build_label_jobs(TemplateCategory::LotPrivate, { parameters }, "Lot " + lot["name"].str() + " (clé)",
                               jobs);
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
      if (lot_["is_sealed"].boolean()) {
        ImGui::TextColored(colors::green, "Scellé%s %s par %s",
                           lot_["seal_number"].str().empty() ? "" : (" n°" + lot_["seal_number"].str()).c_str(),
                           display_on(lot_["sealed"], true).c_str(), lot_["sealed_by"].str("-").c_str());
        ImGui::TextDisabled("Valide sans vérif tant que le scellé est intact.");
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
      ImGui::TextUnformatted("Lot non scellé.");
      if (!lot_["unsealed"].is_null()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Dernier scellé brisé %s par %s", display_on(lot_["unsealed"], true).c_str(),
                            lot_["unsealed_by"].str("-").c_str());
      }
      // un lot global se scelle avec ses sous-lots : ils doivent tous etre complets
      // complet et verifie : « Doit etre scelle » (scelle obligatoire, vérif complete faite) est pret a sceller
      const LotStatus status = lot_status(lot_);
      bool            ok     = lot_ok(status) || status == LotStatus::MustSeal;
      if (lot_is_group(lot_)) {
        ImVec4 color;
        group_banner(lot_, color, &ok);
      }
      if (!ok)
        ImGui::TextColored(colors::orange, "Lot incomplet ou jamais vérifié : faites d'abord une vérif complète.");
      ImGui::SetNextItemWidth(160.0f);
      input_limited("Numéro du scellé", seal_number_, 32, "facultatif");
      if (ok ? primary_button("Sceller le lot et imprimer les étiquettes") : button("Sceller quand même"))
        seal(app, !ok);
      ImGui::SeparatorText("Comment sceller");
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("1. Faites une vérif complète du lot.");
      ImGui::TextDisabled("2. Cliquez sur « Sceller le lot » : deux étiquettes s'impriment.");
      ImGui::TextDisabled("3. Rangez l'étiquette d'ouverture dans le lot, fermez-le avec un scellé et collez "
                          "l'étiquette du scellé dessus.");
      ImGui::TextDisabled("Un lot scellé est valide sans vérif tant que le scellé est intact. Scanner l'étiquette "
                          "d'ouverture ouvre le scellé.");
      ImGui::PopTextWrapPos();
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

    // Fiche d'un lot : en tete (nom, etat, derniere verif, action principale) puis un onglet par sujet. L'onglet
    // « État et contenu » vient en premier : ce qui manque (lots exiges, attendus) avant le reste.
    void draw_lot_details(App &app) {
      if (lot_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        return;
      }
      const bool active  = lot_["active"].boolean(true);
      const bool storage = lot_["storage"].boolean();
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
      ImGui::TextUnformatted(lot_["name"].str().c_str());
      ImGui::PopFont();
      if (!active) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors::orange, "(archivé)");
      }
      if (storage)
        ImGui::TextDisabled("%s - rangement du stock - %s", lot_["lot_type_name"].str().c_str(), lot_["id"].str().c_str());
      else
        ImGui::TextDisabled("%s - version %d - %s", lot_["lot_type_name"].str().c_str(), lot_["version"].integer(),
                            lot_["id"].str().c_str());
      if (lot_["path"].size() > 0) {
        std::string path;
        for (const Json &parent : lot_["path"].items())
          path += (path.empty() ? "" : " › ") + parent["name"].str();
        ImGui::TextWrapped("Rangé dans : %s", path.c_str());
      }
      if (lot_["spare"].boolean() && lot_["path"].size() == 0)
        ImGui::TextUnformatted("Sac de rechange : libre, hors de tout lot.");
      if (lot_["children"].size() > 0) {
        std::string children;
        for (const Json &child : lot_["children"].items())
          children += (children.empty() ? "" : ", ") + child["name"].str();
        ImGui::TextWrapped("Contient les lots : %s", children.c_str());
      }
      const LotStatus status = lot_status(lot_);
      if (lot_is_group(lot_)) {
        ImVec4            color;
        const std::string banner = group_banner(lot_, color);
        status_banner(banner, color, 1.1f);
      } else {
        status_banner(lot_status_banner(lot_), lot_status_color(status), 1.1f);
      }
      // lot global : sa derniere verif est celle de ses sous-lots (bandeau), pas la sienne
      if (!lot_["last_verif"].is_null() || !lot_is_group(lot_))
        ImGui::Text("Dernière vérif : %s%s", display_datetime(lot_["last_verif"], true).c_str(),
                    lot_["last_verif_by"].str().empty() ? "" : (" par " + lot_["last_verif_by"].str()).c_str());
      if (!storage && lot_["is_sealed"].boolean()) {
        ImGui::SameLine();
        ImGui::TextColored(colors::green, "- scellé%s",
                           lot_["seal_number"].str().empty() ? "" : (" n°" + lot_["seal_number"].str()).c_str());
      }

      // actions de tous les jours, toujours au meme endroit
      if (active) {
        if (primary_button(lot_["is_sealed"].boolean() ? "Lancer une vérif (brise le scellé)" : "Lancer une vérif"))
          app.start_verif(selected_lot_, "", app.lot_label_scanned(selected_lot_)); // l'etiquette du lot reste a scanner
      } else {
        ImGui::TextColored(colors::orange, "Lot archivé : il n'apparaît plus dans les listes et ne se vérifie plus.");
        if (primary_button("Désarchiver le lot")) {
          Json body;
          body["active"] = true;
          update_lot(app, body);
        }
      }
      ImGui::SameLine();
      if (button("Journal du lot"))
        app.show_journal(lot_["id"].str());
      ImGui::Spacing();

      if (!ImGui::BeginTabBar("lot_detail_tabs"))
        return;
      if (tab_item("État et contenu")) {
        ImGui::BeginChild("lot_state_tab");
        draw_lot_state(app);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (!storage && tab_item("Scellé")) {
        ImGui::BeginChild("lot_seal_tab");
        draw_seal(app);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (tab_item("Étiquettes")) {
        ImGui::BeginChild("lot_labels_tab");
        draw_lot_labels(app);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      // lot global dont les sacs sont des rangements d'un referentiel (le B+) : ou va chaque ligne de la liste
      if (lot_["distribution"].size() > 0 && tab_item("Répartition")) {
        ImGui::BeginChild("lot_distribution_tab");
        draw_distribution(app);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      if (tab_item("Modifier le lot")) {
        ImGui::BeginChild("lot_edit_tab");
        draw_lot_info(app);
        if (!storage) {
          ImGui::Spacing();
          draw_lot_content(app);
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    // Onglet « État et contenu » : lots exiges (ce qui manque d'abord), attendus, puis items ranges dans le lot
    void draw_lot_state(App &app) {
      if (lot_["slots"].size() > 0)
        draw_lot_slots(app);
      ImGui::SeparatorText("Contenu attendu");
      if (lot_["requirements"].size() == 0) {
        if (lot_["slots"].size() > 0 || lot_["children"].size() > 0)
          ImGui::TextDisabled("Aucun item attendu directement : ce lot regroupe ses sous-lots.");
        else
          ImGui::TextDisabled("Aucun item attendu : le type « %s » n'a pas de contenu (onglet Types de lots).",
                              lot_["lot_type_name"].str().c_str());
      } else {
        draw_requirements_by_location();
      }

      const int items   = static_cast< int >(lot_["item_count"].integer());
      const int expired = static_cast< int >(lot_["expired_count"].integer());
      const std::string title = "Items rangés dans le lot : " + std::to_string(items)
                              + (expired > 0 ? ", dont " + nb(expired, "périmé") : std::string()) + "###lot_items";
      ImGui::Spacing();
      if (expired > 0)
        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing); // des perimes a jeter : la liste s'ouvre
      if (!ImGui::CollapsingHeader(title.c_str()))
        return;
      if (items == 0) {
        ImGui::TextDisabled("Aucun item dans ce lot.");
        return;
      }
      if (ImGui::BeginTable("lot_items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        // perimes en premier (a jeter), ordre du serveur ensuite
        for (const bool expired_pass : { true, false })
          for (const Json &item : lot_["items"].items()) {
            if (item["expired"].boolean() != expired_pass)
              continue;
            ImGui::TableNextRow();
            if (expired_pass)
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

    // Attendus groupes par emplacement (pochettes), ce qui manque d'abord : les emplacements ou il manque quelque
    // chose passent devant (ordre de la TECH a egalite), et dans chacun les lignes incompletes en haut.
    void draw_requirements_by_location() {
      const auto missing = [](const Json &row) {
        return !row["optional"].boolean() && row["present"].integer() < row["required"].integer();
      };
      std::vector< std::string >                          locations; // ordre du serveur (ordre de la TECH)
      std::map< std::string, std::vector< const Json * > > rows;
      for (const Json &row : lot_["requirements"].items()) {
        const std::string location = row["location"].str();
        if (rows.find(location) == rows.end())
          locations.push_back(location);
        rows[location].push_back(&row);
      }
      const auto location_missing = [&](const std::string &location) {
        for (const Json *row : rows[location])
          if (missing(*row))
            return true;
        return false;
      };
      std::stable_sort(locations.begin(), locations.end(), [&](const std::string &a, const std::string &b) {
        return location_missing(a) && !location_missing(b);
      });
      const bool titles = locations.size() > 1 || !locations.front().empty();
      if (!ImGui::BeginTable("lot_requirements", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        return;
      ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Présents", ImGuiTableColumnFlags_WidthFixed, 130.0f);
      ImGui::TableHeadersRow();
      for (const std::string &location : locations) {
        std::vector< const Json * > &group = rows[location];
        std::stable_sort(group.begin(), group.end(),
                         [&](const Json *a, const Json *b) { return missing(*a) && !missing(*b); });
        if (titles) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Spacing();
          ImGui::TextDisabled("%s", location.empty() ? "Sans emplacement" : location.c_str());
        }
        for (const Json *row : group) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (titles)
            ImGui::Indent(14.0f);
          ImGui::TextUnformatted((*row)["type_name"].str().c_str());
          if ((*row)["extra"].integer() > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(dont %d propre à ce lot)", static_cast< int >((*row)["extra"].integer()));
          }
          if (titles)
            ImGui::Unindent(14.0f);
          ImGui::TableNextColumn();
          requirement_bar(*row, (*row)["present"].integer(), ImVec2(-FLT_MIN, 0));
        }
      }
      ImGui::EndTable();
    }

    // Onglet « Répartition » : la liste du referentiel (ex : B+) repartie entre les sacs de ce lot, lot par lot.
    // Le type de chaque sac donne la repartition par defaut ; une ligne peut changer de sac ou etre coupee en deux.
    void draw_distribution(App &app) {
      const std::string source = lot_["id"].str() + lot_["distribution"].dump();
      if (distribution_source_ != source) {
        distribution_source_ = source;
        distribution_.clear();
        for (const Json &group : lot_["distribution"].items())
          for (const Json &line : group["lines"].items())
            for (const Json &place : line["places"].items())
              distribution_[group["referentiel"].str() + "|" + line["type"].str() + "|" + place["lot"].str()] = {
                static_cast< int >(place["quantity"].integer()), place["location"].str()
              };
      }
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("La liste du référentiel répartie entre les sacs de ce lot. Le type de chaque sac donne la "
                          "répartition par défaut ; ici, elle ne vaut que pour ce lot (ex : la cardio-pompe dans le "
                          "sac d'O2 de ce lot, dans le sac de soin d'un autre).");
      ImGui::PopTextWrapPos();
      for (const Json &group : lot_["distribution"].items()) {
        const std::string referentiel = group["referentiel"].str();
        ImGui::PushID(referentiel.c_str());
        ImGui::SeparatorText(("Liste " + group["referentiel_name"].str()).c_str());
        const int sacs = static_cast< int >(group["sacs"].size());
        // lignes mal placees (le serveur en compte moins que la liste) en haut ; ordre fixe pendant la saisie
        std::vector< const Json * > lines;
        for (const Json &line : group["lines"].items())
          lines.push_back(&line);
        std::stable_sort(lines.begin(), lines.end(), [](const Json *a, const Json *b) {
          return (*a)["placed"].integer() < (*a)["required"].integer()
              && (*b)["placed"].integer() >= (*b)["required"].integer();
        });
        int missing = 0;
        if (ImGui::BeginTable("distribution", sacs + 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
          ImGui::TableSetupColumn("Liste", ImGuiTableColumnFlags_WidthStretch, 1.4f);
          ImGui::TableSetupColumn("Nb", ImGuiTableColumnFlags_WidthFixed, 30.0f);
          for (const Json &sac : group["sacs"].items())
            ImGui::TableSetupColumn(sac["lot_type_name"].str().c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
          ImGui::TableSetupColumn("Placés", ImGuiTableColumnFlags_WidthFixed, 55.0f);
          ImGui::TableHeadersRow();
          for (const Json *row : lines) {
            const Json       &line = *row;
            const std::string type = line["type"].str();
            ImGui::PushID(type.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", line["type_name"].str().c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", static_cast< int >(line["required"].integer()));
            int placed = 0;
            for (const Json &sac : group["sacs"].items()) {
              ImGui::TableNextColumn();
              ImGui::PushID(sac["id"].str().c_str());
              DistributionCell &cell = distribution_[referentiel + "|" + type + "|" + sac["id"].str()];
              ImGui::SetNextItemWidth(-FLT_MIN);
              if (ImGui::InputInt("##quantity", &cell.quantity, 1, 1))
                cell.quantity = std::clamp(cell.quantity, 0, 999);
              if (cell.quantity > 0) {
                std::vector< std::string > places;
                for (const Json &location : sac["locations"].items())
                  places.push_back(location.str());
                input_suggest("location", cell.location, places, 64, "Sans emplacement");
              }
              placed += cell.quantity;
              ImGui::PopID();
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const int required = static_cast< int >(line["required"].integer());
            if (placed < required) {
              ++missing;
              ImGui::TextColored(colors::red, "%d/%d", placed, required);
            } else if (placed > required) {
              ImGui::TextColored(colors::blue, "%d + %d", required, placed - required);
            } else {
              ImGui::TextColored(colors::green, "%d", placed);
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        if (missing > 0)
          ImGui::TextColored(colors::red, "%s de la liste dans aucun sac : elle%s manquera%s à la vérif.",
                             nb(missing, "ligne").c_str(), missing > 1 ? "s" : "", missing > 1 ? "nt" : "");
        if (primary_button("Enregistrer la répartition")) {
          Json body;
          body["referentiel"] = referentiel;
          body["rows"]        = Json::array();
          for (const Json &line : group["lines"].items())
            for (const Json &sac : group["sacs"].items()) {
              const DistributionCell &cell = distribution_[referentiel + "|" + line["type"].str() + "|" + sac["id"].str()];
              Json entry;
              entry["lot"]      = sac["id"].str();
              entry["type"]     = line["type"].str();
              entry["quantity"] = cell.quantity;
              entry["location"] = cell.location;
              body["rows"].push_back(entry);
            }
          app.api.put("/api/lots/" + url_encode(selected_lot_) + "/distribution/", body,
                      [this, &app](const ApiResult &result) {
                        if (!result.ok) {
                          app.notify(result.error, true);
                          return;
                        }
                        lot_ = result.data;
                        app.notify("Répartition enregistrée.");
                        app.refresh_lots();
                      });
        }
        ImGui::SameLine();
        if (button("Annuler les changements"))
          distribution_source_.clear();
        ImGui::PopID();
      }
    }

    void draw_lot_labels(App &app) {
      if (lot_["storage"].boolean()) {
        // rangement du stock : une seule etiquette, a coller a l'interieur
        ImGui::TextWrapped("Une seule étiquette, à coller à l'intérieur du rangement.");
        if (primary_button("Aperçu de l'étiquette de rangement"))
          preview_lot(app, lot_);
        ImGui::SameLine();
        if (button("Imprimer directement"))
          print_lot(app, lot_, false, true);
      } else {
        ImGui::TextWrapped(app.declared_identity
                             ? "Étiquette publique : à coller dehors, tout le monde peut la scanner. Étiquette clé : "
                               "rangée dans le lot, elle valide une vérif."
                             : "Étiquette publique : à coller dehors, tout le monde peut la scanner.");
        if (primary_button(app.declared_identity ? "Aperçu des étiquettes publique et clé" : "Aperçu de l'étiquette publique"))
          preview_lot(app, lot_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Imprimer directement :");
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

      ImGui::SeparatorText("Clé du lot");
      ImGui::PushTextWrapPos(0.0f);
      if (lot_["verif_key_expires"].is_null())
        ImGui::TextDisabled("Clé sans expiration (rangement du stock)");
      else if (!app.declared_identity)  // badge obligatoire : la date n'est plus appliquee (models.lot_keys_expire)
        ImGui::TextDisabled("Clé sans expiration tant que le badge est obligatoire (date : %s)",
                            display_date(lot_["verif_key_expires"]).c_str());
      else
        ImGui::TextDisabled("Clé valable jusqu'au %s", display_date(lot_["verif_key_expires"]).c_str());
      ImGui::PopTextWrapPos();
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
      ImGui::SameLine();
      help_marker("À faire si l'étiquette clé est perdue ou a pu être copiée : l'ancienne ne marche plus.");
    }

    // Onglet « Modifier le lot » : nom, rangement dans un autre lot, archivage
    void draw_lot_info(App &app) {
      const bool active = lot_["active"].boolean(true);
      ImGui::SeparatorText("Informations");
      // libelle au-dessus de chaque champ, champs pleine largeur
      ImGui::TextUnformatted("Nom");
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##name", edit_name_, 64);
      const bool name_taken = active && name_taken_warning(app.catalog.lots, "id", edit_name_, selected_lot_, "Le lot");
      ImGui::TextUnformatted("Nom court (16 caractères, sur les petites étiquettes)");
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##short", edit_short_, 16);
      ImGui::TextUnformatted("Rangé dans le lot");
      ImGui::SameLine();
      help_marker("Un sous-lot (ex : le sac d'O2 d'un B+, une armoire d'un VPS) a ses propres étiquettes et se vérifie "
                  "seul, ou avec les autres lots du même lot global. Le lot global est valide si tous ses lots le sont.");
      search_select("edit_lot_parent", app.catalog.lots, "id", "name", edit_parent_, "Aucun : lot indépendant",
                    "Aucun : lot indépendant");
      ImGui::TextUnformatted("Emplacement de l'étiquette clé");
      ImGui::SameLine();
      help_marker("Où trouver l'étiquette clé dans le lot (ex : « poche intérieure du couvercle »). Affiché quand "
                  "quelqu'un essaie de valider une vérif sans l'avoir scannée.");
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##key_location", edit_key_location_, 128, "ex : poche intérieure du couvercle");
      const bool spare_ok = spare_choice(app, lot_["lot_type"].str(), edit_parent_, edit_spare_);
      if (!lot_["storage"].boolean() && !edit_parent_.empty()) {
        ImGui::Checkbox("Sous-lot facultatif à la vérif", &edit_optional_);
        ImGui::SameLine();
        help_marker("Le lot qui le contient se valide sans lui (ex : la caisse des lampes du VPS) : il est affiché en "
                    "gris pendant la vérif, on peut y entrer pour scanner ce qu'il contient et savoir où c'est. "
                    "Tout son contenu est facultatif, sauf quand un admin rend la vérif complète (front web).");
      }
      ImGui::BeginDisabled(edit_name_.empty() || name_taken || !spare_ok);
      if (primary_button("Enregistrer")) {
        Json body;
        body["name"]       = edit_name_;
        body["name_short"] = edit_short_;
        body["parent"]       = edit_parent_;
        body["key_location"] = edit_key_location_;
        if (!lot_["storage"].boolean())
          body["optional"] = edit_optional_ && !edit_parent_.empty();
        if (!rangement_of(app, lot_["lot_type"].str()).empty())
          body["spare"] = edit_parent_.empty() && edit_spare_;
        update_lot(app, body);
      }
      ImGui::EndDisabled();
      if (!active)
        return; // desarchiver : bouton sous l'etat du lot
      ImGui::SameLine(0.0f, 24.0f);
      if (button("Archiver le lot…"))
        ImGui::OpenPopup("archive_lot");
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
    }

    // Places des lots exiges : sous-lots ranges, places vides, assembler / liberer, remplacer un lot fixe
    void draw_lot_slots(App &app) {
      ImGui::SeparatorText("Lots exigés");
      for (const Json &slot : lot_["slots"].items()) {
        const std::string slot_id  = slot["id"].str();
        const bool        assembly = slot["mode"].str() == "assembly";
        ImGui::PushID(slot_id.c_str());
        std::string accepted;
        for (const Json &row : slot["accepted"].items())
          accepted += (accepted.empty() ? "" : " ou ") + row["name"].str();
        ImGui::Text("%s", slot["name"].str().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s%s%s)", assembly ? "à l'assemblage" : "fixe",
                            slot["quantity"].integer() > 1 ? (", " + std::to_string(slot["quantity"].integer())).c_str() : "",
                            slot["optional"].boolean() ? ", facultatif" : "");
        if (accepted != slot["name"].str())
          ImGui::TextDisabled("  accepte : %s", accepted.c_str());
        ImGui::Indent();
        for (const Json &child : slot["lots"].items()) {
          ImGui::PushID(child["id"].str().c_str());
          ImGui::TextColored(state_color(child["state"]), "%s", child["name"].str().c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", child["state"]["label"].str().c_str());
          if (!child["replaces_name"].str().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::orange, "remplace %s", child["replaces_name"].str().c_str());
            ImGui::SameLine();
            if (small_button("Remettre l'original"))
              slot_action(app, "restore", "", child["id"].str(), "");
          } else if (assembly) {
            ImGui::SameLine();
            if (small_button("Libérer"))
              slot_action(app, "release", "", child["id"].str(), "");
          } else if (slot["candidates"].size() > 0) {
            ImGui::SameLine();
            if (small_button("Remplacer…")) {
              slot_pick_.clear();
              ImGui::OpenPopup("replace");
            }
            if (ImGui::BeginPopup("replace")) {
              ImGui::Text("Remplacer %s par :", child["name"].str().c_str());
              ImGui::TextDisabled("Un lot non fixe d'un type accepté. L'original sort du lot jusqu'à son retour.");
              search_select("replace_by", slot["candidates"], "id", "name", slot_pick_, "Tapez le nom du lot…",
                            nullptr, 320.0f);
              ImGui::BeginDisabled(slot_pick_.empty());
              if (primary_button("Remplacer")) {
                slot_action(app, "replace", slot_id, slot_pick_, child["id"].str());
                ImGui::CloseCurrentPopup();
              }
              ImGui::EndDisabled();
              ImGui::EndPopup();
            }
          }
          ImGui::PopID();
        }
        const int missing = static_cast< int >(slot["missing"].integer());
        if (missing > 0) {
          if (assembly) {
            ImGui::TextColored(colors::grey, "%d à assembler : %s disponibles, dont %s valides", missing,
                               std::to_string(slot["available"].size()).c_str(),
                               std::to_string(slot["available_ok"].integer()).c_str());
            if (slot["available"].size() > 0) {
              ImGui::SameLine();
              search_select("assemble", slot["available"], "id", "name", slot_pick_, "Lot à assembler…", nullptr,
                            260.0f);
              ImGui::SameLine();
              ImGui::BeginDisabled(slot_pick_.empty());
              if (small_button("Assembler"))
                slot_action(app, "assemble", slot_id, slot_pick_, "");
              ImGui::EndDisabled();
            }
          } else {
            ImGui::TextColored(slot["optional"].boolean() ? colors::grey : colors::red, "%s : %d manquant%s%s",
                               slot["name"].str().c_str(), missing, missing > 1 ? "s" : "",
                               slot["optional"].boolean() ? " (facultatif)" : " : rangez un lot dans celui-ci (Dans le lot)");
          }
        }
        ImGui::Unindent();
        ImGui::PopID();
      }
    }

    static ImVec4 state_color(const Json &state) {
      const std::string kind = state["kind"].str();
      return kind == "ok" ? colors::green : kind == "warn" ? colors::orange : colors::red;
    }

    void slot_action(App &app, const std::string &action, const std::string &slot, const std::string &lot,
                     const std::string &original) {
      Json body;
      if (!slot.empty())
        body["slot"] = slot;
      body["lot"] = lot;
      if (!original.empty())
        body["original"] = original;
      body["user"]         = app.user_ref();
      const std::string id = selected_lot_;
      app.api.post("/api/lots/" + url_encode(id) + "/slots/" + action + "/", body,
                   [this, &app, id](const ApiResult &result) {
                     if (!result.ok) {
                       app.notify(result.error, true);
                       return;
                     }
                     slot_pick_.clear();
                     if (selected_lot_ == id)
                       lot_ = result.data;
                     app.refresh_lots();
                   });
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
      ImGui::SeparatorText("Contenu propre à ce lot");
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
                      "Tapez le nom du type d'item…", nullptr, width * 0.34f);
        ImGui::EndDisabled();
        ImGui::SameLine();
        input_suggest("location", row.location, suggestions, 64,
                      row.base > 0 && row.base_location.empty() ? "Sans emplacement" : "Emplacement", width * 0.22f);
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
        if (found != content_.end() && row["removed"].integer() > 0) {
          // reparti dans un autre sac (onglet Repartition du lot global) : le serveur garde ce retrait
          found->base -= static_cast< int >(row["removed"].integer());
          if (found->base <= 0) {
            content_.erase(found);
            continue;
          }
        }
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
      if (primary_button("Nouveau type"))
        edit_lot_type(Json());
      ImGui::SameLine();
      ImGui::Checkbox("Types archivés", &show_archived_types_);
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##lot_type_search", lot_type_search_, 64, "Rechercher un type (nom, code)…");
      ImGui::Separator();
      ImGui::BeginChild("types_scroll");
      // par nom, en groupes : lots, lots uniques (un seul lot de ce type), rangements du stock, archives
      std::vector< const Json * > types;
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type_search_.empty() || search_matches(lot_type_search_, lot_type["name"].str())
            || search_matches(lot_type_search_, lot_type["type"].str()))
          types.push_back(&lot_type);
      std::stable_sort(types.begin(), types.end(), [](const Json *a, const Json *b) {
        return name_key((*a)["name"].str()) < name_key((*b)["name"].str());
      });
      std::map< std::string, int > lot_counts; // lots actifs par type
      for (const Json &lot : app.catalog.lots.items())
        ++lot_counts[lot["lot_type"].str()];
      enum Group { Lots, Unique, Storage, Archived };
      const auto group_of = [](const Json &lot_type) {
        return lot_type["archived"].boolean() ? Archived
             : lot_type["storage"].boolean()  ? Storage
             : lot_type["unique"].boolean()   ? Unique
                                              : Lots;
      };
      // rangements d'un referentiel (part_of, ex : Sac B+ soin du Lot B+) : sous leur referentiel, en arborescence
      // (a plat pendant une recherche, avec le nom du referentiel en gris)
      std::map< std::string, const Json * > by_code;
      for (const Json *row : types)
        if (!(*row)["archived"].boolean() || show_archived_types_)
          by_code[(*row)["type"].str()] = row;
      const bool searching = !lot_type_search_.empty();
      const auto nested    = [&](const Json &lot_type) {
        const std::string parent = lot_type["part_of"].str();
        return !searching && !parent.empty() && by_code.count(parent) > 0;
      };
      int        shown    = 0;
      const auto draw_row = [&](const Json &lot_type, int depth) {
        ++shown;
        const bool        archived = lot_type["archived"].boolean();
        const std::string code     = lot_type["type"].str();
        ImGui::PushID(code.c_str());
        if (depth > 0)
          ImGui::Indent(18.0f);
        // ligne entiere cliquable, nom puis code en gris par-dessus
        const float start = ImGui::GetCursorPosX();
        if (ImGui::Selectable("##type", editing_lot_type_ == code, ImGuiSelectableFlags_AllowOverlap))
          edit_lot_type(lot_type);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start);
        const std::string name = (depth > 0 ? "└ " : "") + lot_type["name"].str();
        if (archived)
          ImGui::TextDisabled("%s", name.c_str());
        else
          ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        std::string detail = code;
        if (lot_type["referentiel"].boolean())
          detail += " - référentiel";
        if (searching && !lot_type["part_of"].str().empty())
          for (const Json &parent : app.catalog.lot_types.items())
            if (parent["type"].str() == lot_type["part_of"].str())
              detail += " - rangement de " + parent["name"].str();
        if (!lot_type["storage"].boolean() && !lot_type["unique"].boolean())
          detail += " - " + nb(lot_counts[code], "lot");
        ImGui::TextDisabled("%s", detail.c_str());
        if (depth > 0)
          ImGui::Unindent(18.0f);
        ImGui::PopID();
      };
      for (const Group group : { Lots, Unique, Storage, Archived }) {
        if (group == Archived && !show_archived_types_)
          continue;
        bool header = false;
        for (const Json *row : types) {
          const Json &lot_type = *row;
          if (group_of(lot_type) != group || nested(lot_type))
            continue;
          if (!header) {
            header = true;
            ImGui::SeparatorText(group == Lots     ? "Types de lots"
                                 : group == Unique  ? "Lots uniques"
                                 : group == Storage ? "Rangements du stock"
                                                    : "Archivés");
          }
          draw_row(lot_type, 0);
          if (searching)
            continue;
          for (const Json *child : types)
            if (nested(*child) && (*child)["part_of"].str() == lot_type["type"].str()
                && (show_archived_types_ || !(*child)["archived"].boolean()))
              draw_row(*child, 1);
        }
      }
      if (shown == 0)
        ImGui::TextDisabled(lot_type_search_.empty() ? "Aucun type de lot." : "Aucun type trouvé.");
      ImGui::EndChild();
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
      draw_referentiel_fields(app);
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
          body["referentiel"]   = type_referentiel_ && !type_storage_;
          body["part_of"]       = type_storage_ || type_referentiel_ ? std::string() : type_part_of_;
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
        body["referentiel"]   = type_referentiel_ && !type_storage_;
        body["part_of"]       = type_storage_ || type_referentiel_ ? std::string() : type_part_of_;
        app.api.patch("/api/lot-types/" + url_encode(editing_lot_type_) + "/", body, [this, &app](const ApiResult &result) {
          app.notify(result.ok ? "Type de lot enregistré." : result.error, !result.ok);
          if (result.ok)
            edit_lot_type(result.data); // emplacements effaces d'un nouveau referentiel
          app.refresh_lot_types();
          app.refresh_lots(); // nom du lot d'un type unique
        });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      draw_archive_lot_type(app);

      if (!type_storage_ && !editing_lot_type_.empty())
        draw_slots_editor(app);
      ImGui::SeparatorText("Contenu attendu");
      if (type_storage_) {
        ImGui::TextDisabled("Un rangement du stock n'a pas de contenu attendu : on y range ce qu'on veut.");
        ImGui::EndChild();
        return;
      }
      if (type_referentiel_) {
        ImGui::TextWrapped("Liste obligatoire (d'après les recommandations du ministère), sans emplacements : ils sont "
                           "donnés par les rangements de ce référentiel.");
        if (!type_rangements_.empty())
          ImGui::TextColored(colors::grey, "Répartie entre : %s. Un lot de ce type n'attend rien lui-même : son contenu "
                             "est porté par ses sous-lots.", type_rangements_.c_str());
      } else
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
                      nullptr, width * (type_referentiel_ ? 0.6f : 0.36f));
        ImGui::SameLine();
        if (!type_referentiel_) {
          input_suggest("location", row.location, suggestions, 64, "Emplacement (ex : pochette bleue)", width * 0.24f);
          ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputInt("##qty", &row.quantity);
        row.quantity = std::max(1, row.quantity);
        ImGui::SameLine();
        // facultatif : seulement du materiel non consommable sans taches (ex : les casques du VPS)
        const bool optional_ok = can_be_optional(item_types, row.type);
        row.optional           = row.optional && optional_ok;
        ImGui::BeginDisabled(!optional_ok);
        ImGui::Checkbox("Facultatif", &row.optional);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(optional_ok ? "Le lot se valide sans : affiché en gris pendant la vérif, on peut "
                                            "quand même les scanner pour savoir où ils sont."
                                          : "Seul le matériel non consommable sans tâches peut être facultatif.");
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
          entry["location"] = type_referentiel_ ? std::string() : row.location;
          entry["optional"] = row.optional;
          body["requirements"].push_back(entry);
        }
        app.api.put("/api/lot-types/" + url_encode(editing_lot_type_) + "/requirements/", body,
                    [this, &app](const ApiResult &result) {
                      if (!result.ok) {
                        app.notify(result.error, true);
                        return;
                      }
                      // nouvelle version seulement si le contenu a change et qu'une verif a ete faite sur l'ancienne
                      app.notify(std::string(result.data["new_version"].boolean() ? "Contenu enregistre (nouvelle version "
                                                                                  : "Contenu enregistre (version ")
                                 + result.data["version"].str() + ").");
                      edit_lot_type(result.data);
                      app.refresh_lot_types();
                    });
      }
      // ordre des emplacements : celui de la verif (une pochette apres l'autre) et de la fiche du lot
      if (places.size() > 1 && !type_referentiel_) {
        ImGui::SeparatorText("Ordre des emplacements");
        ImGui::TextDisabled("Ordre dans lequel les secouristes les vérifient, une pochette après l'autre.");
        if (location_order_editor(places)) {
          locations_ = suggestions; // les inutilises gardes a la suite, toujours suggeres
          std::copy(places.begin(), places.end(), locations_.begin());
        }
        ImGui::TextDisabled("Enregistrez le contenu pour garder l'ordre.");
      }
      if (type_referentiel_)
        draw_coverage(app);
      ImGui::EndChild();
    }

    // Lots exiges par le type (« 1 lot B+ » du VPS, « Lot V ou B+ ou VPS » du CAI) : fixes ou a l'assemblage
    void draw_slots_editor(App &app) {
      ImGui::SeparatorText("Lots exigés");
      ImGui::TextDisabled("Fixe : le sous-lot y reste (le B+ du VPS), une place vide rend le lot incomplet. À "
                          "l'assemblage : n'importe quel lot libre d'un type accepté, choisi au moment d'assembler.");
      Json types = Json::array();
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (!lot_type["archived"].boolean() && !lot_type["storage"].boolean() && lot_type["type"].str() != editing_lot_type_)
          types.push_back(lot_type);
      int remove = -1;
      for (std::size_t index = 0; index < slots_.size(); ++index) {
        SlotRow &row = slots_[index];
        ImGui::PushID(static_cast< int >(index) + 5000);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.3f);
        input_limited("##slot_name", row.name, 64, "Nom (par défaut : les types acceptés)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("##slot_qty", &row.quantity);
        row.quantity = std::clamp(row.quantity, 1, 99);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        const char *modes[] = { "Fixe", "À l'assemblage" };
        ImGui::Combo("##slot_mode", &row.mode, modes, 2);
        ImGui::SameLine();
        ImGui::Checkbox("Facultatif", &row.optional);
        ImGui::SameLine();
        if (small_button("Retirer"))
          remove = static_cast< int >(index);
        ImGui::Indent();
        ImGui::TextDisabled("Types acceptés :");
        int drop = -1;
        for (std::size_t accepted = 0; accepted < row.accepted.size(); ++accepted) {
          ImGui::SameLine();
          const Json *lot_type = find_lot_type(app, row.accepted[accepted]);
          const std::string label = (lot_type ? (*lot_type)["name"].str() : row.accepted[accepted]) + "  ✕##acc"
                                  + std::to_string(accepted);
          if (small_button(label.c_str()))
            drop = static_cast< int >(accepted);
          if (accepted + 1 < row.accepted.size()) {
            ImGui::SameLine();
            ImGui::TextDisabled("ou");
          }
        }
        if (drop >= 0)
          row.accepted.erase(row.accepted.begin() + drop);
        if (search_select("slot_add", types, "type", "name", row.adding, "+ Ajouter un type accepté…", nullptr,
                          ImGui::GetContentRegionAvail().x * 0.5f)
            && !row.adding.empty()) {
          if (std::find(row.accepted.begin(), row.accepted.end(), row.adding) == row.accepted.end())
            row.accepted.push_back(row.adding);
          row.adding.clear();
        }
        ImGui::Unindent();
        ImGui::PopID();
      }
      if (remove >= 0)
        slots_.erase(slots_.begin() + remove);
      if (button("+ Ajouter un lot exigé"))
        slots_.push_back({});
      ImGui::SameLine();
      if (primary_button("Enregistrer les lots exigés")) {
        Json body;
        body["slots"] = Json::array();
        for (const SlotRow &row : slots_) {
          if (row.accepted.empty())
            continue;
          Json entry;
          entry["name"]     = row.name;
          entry["quantity"] = row.quantity;
          entry["mode"]     = std::string(row.mode == 1 ? "assembly" : "fixed");
          entry["optional"] = row.optional;
          entry["accepted"] = Json::array();
          for (const std::string &code : row.accepted)
            entry["accepted"].push_back(code);
          body["slots"].push_back(entry);
        }
        app.api.put("/api/lot-types/" + url_encode(editing_lot_type_) + "/slots/", body,
                    [this, &app](const ApiResult &result) {
                      app.notify(result.ok ? "Lots exigés enregistrés." : result.error, !result.ok);
                      if (result.ok) {
                        edit_lot_type(result.data);
                        app.refresh_lot_types();
                        app.refresh_lots();
                      }
                    });
      }
    }

    // Referentiel (liste du ministere, sans emplacements) et « Rangement de » (sa repartition entre des sacs)
    void draw_referentiel_fields(App &app) {
      ImGui::BeginDisabled(type_storage_ || !type_part_of_.empty());
      ImGui::Checkbox("Référentiel (liste obligatoire, sans emplacements)", &type_referentiel_);
      ImGui::EndDisabled();
      help_marker("Le contenu obligatoire d'après les recommandations du ministère (ex : la liste du B+), sans "
                  "emplacements. Ses rangements (ex : « Sac de soin » et « Sac O2 ») se répartissent la liste, rangée "
                  "par pochettes, avec ce qu'ils ont en plus. Un lot du référentiel n'attend alors rien lui-même : "
                  "ses sous-lots portent son contenu, et la couverture montre ce qui manque dans ses sacs. Sans "
                  "rangement (ex : un kit d'un seul sac), un lot du référentiel attend la liste lui-même.");
      if (type_storage_ || type_referentiel_)
        return;
      Json referentiels = Json::array();
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["referentiel"].boolean() && lot_type["type"].str() != editing_lot_type_)
          referentiels.push_back(lot_type);
      ImGui::TextUnformatted("Rangement de");
      ImGui::SameLine();
      help_marker("Ce type range une partie de la liste d'un référentiel (ex : le sac de soin du B+), par "
                  "emplacements, avec ce qu'il a en plus. Deux sacs du même rangement sont interchangeables ; un sac "
                  "rangé autrement (ex : le sac de soin du VPS) est un autre rangement du même référentiel.");
      search_select("type_part_of", referentiels, "type", "name", type_part_of_, "Aucun référentiel",
                    "Aucun : type ordinaire");
      if (!type_part_of_.empty() || type_suggested_part_of_.empty())
        return;
      // transition : tous les lots de ce type sont des sous-lots de lots d'un meme type
      std::string name = type_suggested_part_of_;
      bool        ready = false;
      for (const Json &lot_type : app.catalog.lot_types.items())
        if (lot_type["type"].str() == type_suggested_part_of_) {
          name  = lot_type["name"].str();
          ready = lot_type["referentiel"].boolean();
        }
      ImGui::TextColored(colors::grey, "Suggestion : rangement de %s (tous ses lots sont dans des lots %s).", name.c_str(),
                         name.c_str());
      if (ready) {
        ImGui::SameLine();
        if (small_button("Choisir"))
          type_part_of_ = type_suggested_part_of_;
      } else
        ImGui::TextColored(colors::grey, "Cochez d'abord « Référentiel » sur le type %s.", name.c_str());
    }

    // Couverture d'un referentiel : pour chaque lot, la liste face au contenu attendu de ses sacs
    void draw_coverage(App &app) {
      ImGui::SeparatorText("Couverture");
      ImGui::TextDisabled("Pour chaque lot, ce que ses sacs placent de la liste obligatoire.");
      if (coverage_type_ != editing_lot_type_ && !coverage_loading_)
        load_coverage(app);
      ImGui::SameLine();
      if (small_button("Actualiser"))
        load_coverage(app);
      if (coverage_loading_) {
        ImGui::TextDisabled("Chargement…");
        return;
      }
      if (coverage_["lots"].size() == 0) {
        ImGui::TextDisabled("Aucun lot de ce référentiel pour l'instant.");
        return;
      }
      for (const Json &lot : coverage_["lots"].items()) {
        ImGui::PushID(lot["id"].str().c_str());
        const long        missing = lot["missing"].integer();
        const std::string title   = lot["name"].str() + (lot["complete"].boolean()
                                                             ? std::string(" — liste couverte")
                                                             : " — " + std::to_string(missing)
                                                                   + (missing > 1 ? " lignes manquent" : " ligne manque"))
                                + "###coverage";
        ImGui::PushStyleColor(ImGuiCol_Text, lot["complete"].boolean() ? colors::green : colors::red);
        const bool open = ImGui::CollapsingHeader(title.c_str());
        ImGui::PopStyleColor();
        if (open) {
          if (ImGui::BeginTable("coverage", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Liste", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Placé", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Où", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableHeadersRow();
            for (const char *group : { "lines", "extra" })
              for (const Json &line : lot[group].items()) {
                const long   required = line["required"].integer();
                const long   placed   = line["placed"].integer();
                const bool   extra    = std::string(group) == "extra" || placed > required;
                const ImVec4 color    = placed < required ? colors::red : (extra ? colors::blue : colors::green);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%s", line["type_name"].str().c_str());
                ImGui::TableNextColumn();
                if (required > 0)
                  ImGui::Text("%ld", required);
                else
                  ImGui::TextDisabled("en plus");
                ImGui::TableNextColumn();
                ImGui::TextColored(color, "%ld", placed);
                ImGui::TableNextColumn();
                std::string where;
                for (const Json &place : line["places"].items()) {
                  if (!where.empty())
                    where += ", ";
                  where += place["lot_name"].str();
                  if (!place["location"].str().empty())
                    where += " (" + place["location"].str() + ")";
                  where += " : " + place["quantity"].str();
                }
                if (where.empty())
                  ImGui::TextColored(colors::red, "à placer dans un rangement");
                else
                  ImGui::TextWrapped("%s", where.c_str());
              }
            ImGui::EndTable();
          }
          if (small_button("Reprendre ce contenu comme liste")) {
            // transition : la liste part de ce qui est rangé dans ce lot, la TECH la corrige d'après le PDF
            requirements_.clear();
            for (const char *group : { "lines", "extra" })
              for (const Json &line : lot[group].items())
                if (line["placed"].integer() > 0)
                  requirements_.push_back({ line["type"].str(), static_cast< int >(line["placed"].integer()), "", false });
            app.notify("Liste reprise de " + lot["name"].str() + " : corrigez-la puis « Enregistrer le contenu ».");
          }
          ImGui::SetItemTooltip("Remplace les lignes ci-dessus par tout ce que ce lot et ses sacs contiennent "
                                "(rien n'est enregistré avant « Enregistrer le contenu »).");
        }
        ImGui::PopID();
      }
    }

    void load_coverage(App &app) {
      coverage_type_    = editing_lot_type_;
      coverage_loading_ = true;
      const std::string type = editing_lot_type_;
      app.api.get("/api/lot-types/" + url_encode(type) + "/coverage/", [this, &app, type](const ApiResult &result) {
        if (type != coverage_type_)
          return; // un autre type a ete choisi entre-temps
        coverage_loading_ = false;
        coverage_         = result.ok ? result.data : Json();
        if (!result.ok)
          app.notify(result.error, true);
      });
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
      type_referentiel_   = lot_type["referentiel"].boolean();
      type_part_of_       = lot_type["part_of"].str();
      type_suggested_part_of_ = lot_type["suggested_part_of"].str();
      type_rangements_.clear();
      for (const Json &row : lot_type["rangements"].items())
        type_rangements_ += (type_rangements_.empty() ? "" : ", ") + row["name"].str();
      coverage_type_.clear(); // couverture rechargee a l'affichage
      type_lot_parent_.clear();
      requirements_.clear();
      locations_.clear();
      for (const Json &location : lot_type["locations"].items())
        locations_.push_back(location.str());
      for (const Json &row : lot_type["requirements"].items())
        requirements_.push_back({ row["type"].str(), static_cast< int >(row["quantity"].integer(1)),
                                  row["location"].str(), row["optional"].boolean() });
      slots_.clear();
      for (const Json &row : lot_type["slots"].items()) {
        SlotRow slot;
        slot.name     = row["name"].str();
        slot.quantity = static_cast< int >(row["quantity"].integer(1));
        slot.mode     = row["mode"].str() == "assembly" ? 1 : 0;
        slot.optional = row["optional"].boolean();
        for (const Json &code : row["accepted"].items())
          slot.accepted.push_back(code.str());
        slots_.push_back(slot);
      }
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
    bool        edit_optional_ = false;
    bool        edit_spare_    = false; // sac de rechange (rangement libre)
    std::string new_lot_parent_;
    bool        new_lot_spare_ = false; // rangement cree libre (sac de rechange)
    std::string new_lot_key_location_;
    std::string new_lot_type_;
    std::string new_lot_name_;
    std::string new_lot_short_;
    bool        print_new_ = true;
    // creation de plusieurs lots (et de leurs sous-lots) d'un coup
    struct BatchSub {
      std::string type, name;
      int         count;
      bool        rangement;
    };
    struct BatchRow {
      std::string type, type_name, name, short_name, key_location;
      int         depth;
    };
    struct SlotRow {
      std::string                name;
      std::vector< std::string > accepted;
      int                        quantity = 1;
      int                        mode     = 0; // 0 fixe, 1 a l'assemblage
      bool                       optional = false;
      std::string                adding;
    };
    std::vector< SlotRow > slots_;
    std::string            slot_pick_; // lot choisi pour assembler / remplacer (fiche du lot)
    int                    new_lot_count_ = 1;
    std::string             batch_type_;
    std::string             batch_base_;
    std::string             batch_base_short_;
    std::vector< BatchSub > batch_subs_;
    std::vector< BatchRow > batch_rows_;
    std::string seal_number_;
    bool        show_archived_lots_ = false;
    bool        show_storage_       = false; // rangements du stock masques par defaut
    std::string lot_search_;
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
    bool                          type_referentiel_ = false;
    std::string                   type_part_of_;           // referentiel dont ce type est un rangement
    std::string                   type_suggested_part_of_; // transition : referentiel de tous ses lots
    std::string                   type_rangements_;        // rangements d'un referentiel, pour l'affichage
    std::string                   coverage_type_;          // type dont la couverture est chargee
    bool                          coverage_loading_ = false;
    Json                          coverage_;
    std::string                   type_lot_;        // lot d'un type unique
    std::string                   type_lot_parent_; // creation d'un type unique : parent de son lot
    bool                          show_archived_types_ = false;
    std::string                   lot_type_search_;
    std::vector< RequirementRow > requirements_;
    std::vector< std::string >    locations_; // emplacements du type de lot, dans l'ordre de la verif
    // Contenu propre au lot affiche
    struct DistributionCell {
      int         quantity = 0;
      std::string location;
    };
    std::map< std::string, DistributionCell > distribution_; // referentiel|type|sac -> quantite et emplacement
    std::string                               distribution_source_;
    std::string                  content_source_; // lot et type d'ou viennent les lignes (rechargees s'ils changent)
    std::vector< LotContentRow > content_;
    std::vector< std::string >   content_locations_; // ordre des emplacements du lot
};

} // namespace

std::unique_ptr< AppWindow > make_lot_admin_window() {
  return std::make_unique< LotAdminWindow >();
}

} // namespace qrprotec
