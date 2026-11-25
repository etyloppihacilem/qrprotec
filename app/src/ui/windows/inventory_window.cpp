/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inventory_window.cpp
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
#include "implot.h"
#include "windows.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <sstream>
#include <utility>

namespace qrprotec {

namespace {

const Json *find_type(const App &app, const std::string &type) {
  for (const Json &item_type : app.catalog.item_types.items())
    if (item_type["type"].str() == type)
      return &item_type;
  return nullptr;
}

// Rangements du stock actifs ou ranger une reception, nommes par leur chemin sous le Stock (« Armoire › Tiroir 2 »).
// Le Stock lui-meme n'en est pas un : sans rangement, les items restent dans le stock en vrac.
Json storage_places(const App &app) {
  const Json &lots = app.catalog.lots;
  auto        find = [&lots](const std::string &id) -> const Json * {
    for (const Json &lot : lots.items())
      if (lot["id"].str() == id)
        return &lot;
    return nullptr;
  };
  Json places = Json::array();
  for (const Json &lot : lots.items()) {
    if (!lot["storage"].boolean() || !lot["active"].boolean(true) || lot["lot_type"].str() == "stock0")
      continue;
    std::string label  = lot["name"].str();
    const Json *parent = find(lot["parent"].str());
    for (int depth = 0; parent && (*parent)["lot_type"].str() != "stock0" && depth < 16; ++depth) {
      label  = (*parent)["name"].str() + " › " + label;
      parent = find((*parent)["parent"].str());
    }
    Json place     = lot;
    place["label"] = label;
    places.push_back(std::move(place));
  }
  return places;
}

std::string place_label(const App &app, const std::string &id) {
  const Json places = storage_places(app); // la boucle ne prolonge pas la vie d'un temporaire dont on prend items()
  for (const Json &place : places.items())
    if (place["id"].str() == id)
      return place["label"].str();
  return id;
}

// Item fictif pour l'apercu d'un type d'item (avant toute reception)
Json sample_item(const std::string &type, const std::string &name, bool perishable, const std::string &peremption_iso,
                 bool tear_off = false, const std::vector< std::string > &tasks = {},
                 const std::string &label_style = "qr") {
  const auto  date = Date::parse(peremption_iso);
  const Date  when = date ? *date : Date::today().plus_days(365);
  std::string code = type;
  code.resize(6, 'x');
  Json item;
  item["iid"]        = code + (perishable ? when.iso().substr(0, 4) + when.iso().substr(5, 2) + when.iso().substr(8, 2) : "00000000") + "00000001";
  item["url"]        = "https://example.com/item?id=" + item_code(item["iid"].str()); // taille du QR proche du reel
  item["type"]       = type;
  item["type_name"]  = name;
  item["peremption"] = perishable ? Json(when.iso()) : Json();
  item["tear_off"]   = tear_off;
  item["label_style"] = label_style;
  item["task_url"]   = "https://example.com/task?id=" + item_code(item["iid"].str());
  item["tasks"]      = Json::array();
  for (const std::string &task : tasks)
    item["tasks"].push_back(task);
  return item;
}

// Styles d'etiquette d'un type d'item (ItemType.label_style) : chacun a sa version liste de taches et a dechirer
const char *const kLabelStyles[]     = { "qr", "small", "barcode", "sheet" };
const char *const kLabelStyleNames[] = { "QR code", "QR code réduit", "Code-barres", "Planche (plusieurs par étiquette)" };

int label_style_index(const std::string &style) {
  return style == "small" ? 1 : style == "barcode" ? 2 : style == "sheet" ? 3 : 0;
}

std::vector< std::string > tasks_of(const Json &item_type) {
  std::vector< std::string > tasks;
  for (const Json &task : item_type["tasks"].items())
    tasks.push_back(task.str());
  return tasks;
}

std::vector< Parameters > item_labels(const Json &items) {
  std::vector< Parameters > labels;
  const int                 count = static_cast< int >(items.size());
  for (int index = 0; index < count; ++index)
    labels.push_back(item_parameters(items[index], index + 1, count));
  return labels;
}

// ---- Etat du stock (onglets venus de la Gestion de la TECH) ----
// Horizons proposes : index dans "points" des previsions (un point par mois)
constexpr std::array< int, 4 >          horizon_months = { 0, 1, 3, 6 };
constexpr std::array< const char *, 4 > horizon_labels = { "Aujourd'hui", "M+1", "M+3", "M+6" };

const ImVec4 stock_color(0.11f, 0.31f, 0.57f, 1.0f);
const ImVec4 lots_color(0.42f, 0.61f, 0.85f, 1.0f);
const ImVec4 sealed_color(0.78f, 0.35f, 0.0f, 1.0f);

std::string date_text(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : std::string("-");
}

std::string short_date(const Json &value) {
  const auto date = Date::parse(value.str());
  if (!date)
    return "-";
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02d/%02d", date->day, date->month);
  return buffer;
}

std::string month_label(const Json &value) {
  static const char *names[] = { "janv.", "févr.", "mars", "avr.", "mai", "juin",
                                 "juil.", "août", "sept.", "oct.", "nov.", "déc." };
  const auto date = Date::parse(value.str());
  return date && date->month >= 1 && date->month <= 12 ? names[date->month - 1] : "?";
}

// Nombre avec decimales seulement si utile (consommation par mois)
std::string rate_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), value >= 10 || value == static_cast< int >(value) ? "%.0f" : "%.1f", value);
  return buffer;
}

void colored_count(int value, const ImVec4 &color) {
  if (value > 0)
    ImGui::TextColored(color, "%d", value);
  else
    ImGui::TextUnformatted("0");
}

// Trois fenetres de gestion faites du meme code (l'ancien Inventaire), une par domaine :
// - Stock (F6) : qu'est-ce que j'ai en reserve ? Etat du stock (aujourd'hui, projection), commandes, transferts.
// - Items (F7) : comment le materiel est defini et suivi. Types d'items, items, entretien (revisions et
//   desinfections), paquets fermes.
// - Reception (F9) : une commande arrive. Nouvelle reception, receptions passees.
// Chacune peut etre ouverte a cote d'une autre (Lots d'un cote, Types d'items de l'autre).
class InventoryWindow final : public AppWindow {
  public:
    enum class View { Stock, Items, Reception };

    InventoryWindow(View view, const char *id, const char *title) : AppWindow(id, title, true, true), view_(view) {}

    void on_open(App &app) override {
      app.refresh_item_types();
      switch (view_) {
        case View::Stock:
          app.refresh_stock();
          app.refresh_forecast();
          break;
        case View::Items:
          search(app);
          load_packs(app);
          break;
        case View::Reception:
          app.refresh_lots(true); // rangements du stock ou ranger la reception
          load_receptions(app);
          break;
      }
    }

    void reset_view(App &) override {
      filter_.clear();
      selected_.clear();
      only_low_          = false;
      zero_open_         = false;
      include_lots_      = false;
      with_consumption_  = true;
      horizon_           = 1;
      stock_projection_  = false;
      clear_type_form();
      show_archived_ = false;
      search_text_.clear();
      search_type_.clear();
      search_status_     = 0;
      search_stock_only_ = false;
      selected_item_     = Json();
      equipment_item_    = Json();
      equipment_search_.clear();
      equipment_soon_only_ = false;
      first_tab_         = true;
      reload_pending     = true; // recherche refaite sans filtre au prochain affichage
    }

    void draw(App &app) override {
      // onglet demande d'ailleurs (menu, bouton d'une autre fenetre) : seulement s'il est dans cette fenetre
      static const char *const kStockTabs[]     = { "stock", "projection", "orders", "transfers" };
      static const char *const kItemsTabs[]     = { "types", "items", "equipment", "packs" };
      static const char *const kReceptionTabs[] = { "reception", "past_receptions" };
      const auto mine = [this](const std::string &tab) {
        const auto in = [&tab](const auto &list) {
          return std::find(std::begin(list), std::end(list), tab) != std::end(list);
        };
        return view_ == View::Stock ? in(kStockTabs) : view_ == View::Items ? in(kItemsTabs) : in(kReceptionTabs);
      };
      if (!app.stock_tab_to_show_.empty() && mine(app.stock_tab_to_show_)) {
        stock_select_ = std::exchange(app.stock_tab_to_show_, std::string());
        if (stock_select_ == "stock" || stock_select_ == "projection")
          stock_projection_ = stock_select_ == "projection";
        if (stock_select_ == "past_receptions")
          load_receptions(app);
      }
      if (view_ == View::Reception) {
        if (std::string type = std::exchange(app.receptions_type_to_show_, std::string()); !type.empty()) {
          receptions_type_ = type;
          load_receptions(app);
        }
      }
      if (view_ == View::Items) {
        if (seen_packs_version_ != app.catalog.packs_version) {
          seen_packs_version_ = app.catalog.packs_version;
          load_packs(app);
        }
        if (seen_types_version_ != app.catalog.item_types_version) {
          seen_types_version_ = app.catalog.item_types_version;
          for (const Json &item_type : app.catalog.item_types.items())
            if (!editing_type_.empty() && item_type["type"].str() == editing_type_)
              edit_type(item_type);
        }
        if (app.type_to_show_.rfind("item:", 0) == 0) {
          const std::string code = std::exchange(app.type_to_show_, std::string()).substr(5);
          for (const Json &item_type : app.catalog.item_types.items())
            if (item_type["type"].str() == code)
              edit_type(item_type);
        }
        if (std::string iid = std::exchange(app.item_to_show_, std::string()); !iid.empty()) {
          search_text_ = iid;
          search_type_.clear();
          search_status_         = 0;
          search_stock_only_     = false;
          selected_item_         = Json::object();
          selected_item_["iid"] = iid; // choisi des que la recherche revient
          search(app);
        }
      }
      const auto selected = [this](const char *key) {
        return stock_select_ == key ? ImGuiTabItemFlags_SetSelected : 0;
      };
      const bool first = std::exchange(first_tab_, false);
      if (ImGui::BeginTabBar("inventory_tabs")) {
        switch (view_) {
          case View::Stock: draw_stock_tabs(app, selected); break;
          case View::Items:
            if (tab_item("Types d'items", first ? ImGuiTabItemFlags_SetSelected : selected("types"))) {
              draw_types(app);
              ImGui::EndTabItem();
            }
            if (tab_item("Items", selected("items"))) {
              draw_items(app);
              ImGui::EndTabItem();
            }
            if (tab_item("Entretien", selected("equipment"))) {
              draw_equipment(app);
              ImGui::EndTabItem();
            }
            if (tab_item("Paquets fermés", selected("packs"))) {
              draw_packs(app);
              ImGui::EndTabItem();
            }
            break;
          case View::Reception:
            if (tab_item("Nouvelle réception", first ? ImGuiTabItemFlags_SetSelected : selected("reception"))) {
              draw_reception(app);
              ImGui::EndTabItem();
            }
            if (tab_item("Réceptions passées", selected("past_receptions"))) {
              draw_receptions(app);
              ImGui::EndTabItem();
            }
            break;
        }
        ImGui::EndTabBar();
      }
      stock_select_.clear();
    }

  private:
    template < typename Selected > void draw_stock_tabs(App &app, const Selected &selected) {
      const Json &forecast = app.catalog.forecast;
      int         orders   = 0;
      for (const Json &row : forecast["types"].items())
        orders += row["order"].is_object() ? 1 : 0;
      const std::string orders_tab    = "Commandes (" + std::to_string(orders) + ")###orders";
      const std::string transfers_tab = "Transferts (" + std::to_string(forecast["transfers"].size()) + ")###transfers";
      const ImGuiTabItemFlags stock_flags = selected("stock") | selected("projection");
      if (tab_item("État du stock", stock_flags)) {
        draw_stock_toolbar(app);
        if (ImGui::RadioButton("Aujourd'hui", !stock_projection_))
          stock_projection_ = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Projection", stock_projection_))
          stock_projection_ = true;
        if (stock_projection_)
          draw_projection(app);
        else
          draw_current(app);
        ImGui::EndTabItem();
      }
      if (tab_item(orders_tab.c_str(), selected("orders"))) {
        draw_stock_toolbar(app);
        draw_orders(app);
        ImGui::EndTabItem();
      }
      if (tab_item(transfers_tab.c_str(), selected("transfers"))) {
        draw_stock_toolbar(app);
        draw_transfers(app);
        ImGui::EndTabItem();
      }
    }

    View view_;

    // ---------------------------------------------------------------------------------------------------------------
    // Reception : creation des items et impression des etiquettes en serie
    void draw_reception(App &app) {
      ImGui::TextWrapped("Réception d'une commande : choisissez le type, la date de péremption et la quantité. "
                         "Chaque item recoit un identifiant unique et sa propre étiquette.");
      ImGui::TextUnformatted("Type d'item");
      // un type parent (serum phy en mL, bandes) n'a pas d'etiquette : on receptionne ses sous-types
      Json receivable = Json::array();
      const Json types = without_archived(app.catalog.item_types); // temporaire detruit avant la boucle sinon
      for (const Json &item_type : types.items())
        if (!item_type["is_group"].boolean())
          receivable.push_back(item_type);
      if (search_select("reception_type", receivable, "type", "name", reception_type_,
                        "Tapez le nom ou le code du type…")) {
        if (const Json *type = find_type(app, reception_type_))
          quantity_ = std::max(1, (*type)["default_pack_size"].integer(1));
        suggest_place(app);
      }
      const Json *type       = find_type(app, reception_type_);
      const bool  perishable = type && (*type)["perissable"].boolean();
      ImGui::BeginDisabled(!perishable);
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("Péremption", "ex: 09/2026, 020926", &peremption_);
      // saisie libre : la date comprise est affichee, et remise en forme en quittant le champ
      const auto typed = parse_user_date(peremption_);
      if (ImGui::IsItemDeactivatedAfterEdit() && typed)
        peremption_ = typed->display();
      ImGui::EndDisabled();
      help_marker("Tapez la date comme elle vient : 02/09/2026, 2/9/26, 020926, ou seulement le mois "
                  "(09/2026, 09/26, 0926 = dernier jour du mois).");
      if (type && !perishable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(non périssable)");
      } else if (!peremption_.empty()) {
        ImGui::SameLine();
        if (typed)
          ImGui::TextColored(colors::green, "→ %s", typed->display().c_str());
        else
          ImGui::TextColored(colors::red, "date non reconnue");
      }
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputInt(sealed_ ? "Items par paquet" : "Quantité", &quantity_);
      quantity_ = std::clamp(quantity_, 1, 10000);
      ImGui::TextUnformatted("Rangement du stock");
      search_select("reception_place", storage_places(app), "id", "label", reception_place_,
                    "Tapez le nom du rangement…", "Stock (sans rangement)");
      if (!reception_place_.empty() && reception_place_ == suggested_place_) {
        ImGui::SameLine();
        ImGui::TextDisabled("(dernier rangement de ce type)");
      }
      help_marker("Toute la réception est rangée d'un coup dans ce rangement du stock (ex : 50 compresses dans "
                  "l'armoire), sans scanner les items un par un. Sans rangement, les items restent dans le stock.");
      ImGui::Checkbox("Paquet fermé : seulement une étiquette de paquet", &sealed_);
      help_marker("Pour un paquet que l'on n'ouvre pas tout de suite (ex: boîte de compresses). Les étiquettes "
                  "individuelles seront imprimées à l'ouverture : scannez l'étiquette du paquet puis « Ouvrir ».");
      if (sealed_) {
        // plusieurs paquets identiques recus ensemble : une etiquette par paquet, une seule reception
        ImGui::Indent();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputInt("Nombre de paquets", &pack_count_);
        pack_count_ = std::clamp(pack_count_, 1, std::max(1, std::min(500, 10000 / quantity_)));
        ImGui::TextDisabled("%s : à l'ouverture %s.",
                            quantity_ > 1 ? ("Étiquettes des " + nb(quantity_, "item")).c_str() : "Étiquette de l'item",
                            pack_count_ > 1 ? "de chaque paquet" : "du paquet");
        if (pack_count_ > 1)
          ImGui::TextDisabled("Total : %s.", nb(pack_count_ * quantity_, "item").c_str());
        ImGui::Unindent();
      } else
        ImGui::Checkbox("Imprimer les étiquettes des items (aperçu puis impression)", &print_items_);

      const auto date = parse_user_date(peremption_);
      std::string problem;
      if (!type)
        problem = "Choisissez un type d'item.";
      else if (perishable && !date)
        problem = "Date de péremption invalide.";
      else if (perishable && date && *date < app.today())
        problem = "Attention : cette date est déjà passée.";
      if (!problem.empty())
        ImGui::TextColored(colors::orange, "%s", problem.c_str());

      ImGui::BeginDisabled(!type || (perishable && !date) || creating_);
      const std::string label = sealed_ && pack_count_ > 1
                                  ? "Créer " + nb(pack_count_, "paquet") + " de " + nb(quantity_, "item") + " et leurs étiquettes"
                              : sealed_ ? "Créer le paquet de " + nb(quantity_, "item") + " et son étiquette"
                              : print_items_ ? "Créer " + nb(quantity_, "item") + " et "
                                                 + pl(quantity_, "voir l'étiquette", "voir les étiquettes")
                                             : "Créer " + nb(quantity_, "item");
      if (primary_button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFrameHeight() * 1.5f)))
        create_batch(app, perishable && date ? date->iso() : "");
      ImGui::SameLine();
      const bool preview = button("Aperçu", ImVec2(-1, ImGui::GetFrameHeight() * 1.5f));
      if (preview && sealed_) {
        Json pack   = sample_item(reception_type_, (*type)["name"].str(), perishable, date ? date->iso() : "");
        pack["id"]    = "00000000";
        pack["url"]   = "https://example.com/pack?id=" + code_prefix() + "00000000";
        pack["count"] = quantity_;
        app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) },
                           "Exemple d'étiquette de paquet (avant création)");
      } else if (preview) {
        app.preview_labels(TemplateCategory::Item,
                           { item_parameters(sample_item(reception_type_, (*type)["name"].str(), perishable,
                                                         date ? date->iso() : "", (*type)["tear_off"].boolean(),
                                                         tasks_of(*type), (*type)["label_style"].str("qr"))) },
                           "Exemple d'étiquette (avant création)");
      }
      ImGui::EndDisabled();

      if (last_batch_.is_null())
        return;
      ImGui::SeparatorText("Dernière réception");
      const Json &items = last_batch_["items"];
      ImGui::Text("%s : %s", nb(items.size(), "item créé", "items créés").c_str(), items[0]["type_name"].str().c_str());
      if (!items[0]["location"].is_null() && !items[0]["location"].str().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(colors::green, "rangés dans %s", place_label(app, items[0]["location"].str()).c_str());
      }
      const Json batch_packs = batch_sealed_packs();
      if (batch_packs.size() > 1) {
        // plusieurs paquets fermes : leurs etiquettes ensemble, la fiche de chacun dans la liste dessous
        if (button("Étiquettes des paquets"))
          app.preview_labels(TemplateCategory::ItemPack, sealed_packs_parameters(batch_packs), "Paquets");
      } else if (batch_packs.size() == 1) {
        // paquet ferme : etiquette du paquet, et fiche du paquet pour l'ouvrir plus tard
        if (button("Étiquette du paquet"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(batch_packs[0]) }, "Paquet");
        ImGui::SameLine();
        if (button("Fiche du paquet (ouverture)"))
          app.show_pack(batch_packs[0]["id"].str());
      } else if (button("Aperçu et impression des étiquettes")) {
        app.preview_labels(TemplateCategory::Item, item_labels(items), "Réception");
      }
      const bool sealed_batch = !last_batch_["sealed_pack"].is_null();
      ImGui::BeginChild("batch", ImVec2(0, 0), ImGuiChildFlags_Borders);
      for (std::size_t index = 0; index < items.size(); ++index) {
        const Json &item = items[index];
        ImGui::PushID(static_cast< int >(index));
        if (sealed_batch && batch_packs.size() > 1) {
          // items crees paquet par paquet : premier item de chaque paquet = bouton vers sa fiche (ouverture)
          const std::size_t per_pack = static_cast< std::size_t >(std::max(1, batch_packs[0]["count"].integer(1)));
          const std::size_t pack     = std::min(index / per_pack, batch_packs.size() - 1);
          const std::string pack_id  = batch_packs[pack]["id"].str();
          if (index % per_pack == 0) {
            if (small_button(("Paquet " + pack_id).c_str()))
              app.show_pack(pack_id);
          } else {
            ImGui::TextDisabled("paquet %s", pack_id.c_str());
          }
        } else if (sealed_batch)
          ImGui::TextDisabled("dans le paquet");
        else if (small_button("Imprimer"))
          app.print_labels(TemplateCategory::Item,
                           { item_parameters(item, static_cast< int >(index) + 1, static_cast< int >(items.size())) },
                           "Item");
        ImGui::SameLine();
        ImGui::Text("%s", item["iid"].str().c_str());
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    // Rangement propose : le dernier ou ce type a ete range (vide s'il n'a jamais ete range)
    void suggest_place(App &app) {
      reception_place_.clear();
      suggested_place_.clear();
      if (reception_type_.empty())
        return;
      const std::string type = reception_type_;
      app.api.get("/api/receptions/storage/?type=" + url_encode(type), [this, type](const ApiResult &result) {
        if (!result.ok || reception_type_ != type || result.data["location"].is_null())
          return;
        suggested_place_ = result.data["location"].str();
        reception_place_ = suggested_place_;
      });
    }

    // Paquets fermes de la derniere reception (serveur plus ancien : seulement « sealed_pack »)
    Json batch_sealed_packs() const {
      if (last_batch_["sealed_packs"].is_array())
        return last_batch_["sealed_packs"];
      Json packs = Json::array();
      if (!last_batch_["sealed_pack"].is_null())
        packs.push_back(last_batch_["sealed_pack"]);
      return packs;
    }

    static std::vector< Parameters > sealed_packs_parameters(const Json &packs) {
      std::vector< Parameters > parameters;
      for (const Json &pack : packs.items())
        parameters.push_back(sealed_pack_parameters(pack));
      return parameters;
    }

    void create_batch(App &app, const std::string &peremption) {
      Json body;
      body["type"]        = reception_type_;
      body["peremption"]  = peremption.empty() ? Json() : Json(peremption);
      body["count"]       = quantity_;
      body["sealed_pack"] = sealed_;
      if (sealed_)
        body["packs"] = pack_count_;
      body["location"]    = reception_place_.empty() ? Json() : Json(reception_place_);
      body["user"]        = app.user_ref();
      creating_           = true;
      // paquet ferme : seule l'etiquette du paquet, les items seront etiquetes a l'ouverture
      const bool sealed = sealed_, print_items = print_items_ && !sealed_;
      app.api.post("/api/items/batch/", body, [this, &app, sealed, print_items](const ApiResult &result) {
        creating_ = false;
        if (!result.ok) {
          app.notify("Création refusée : " + result.error, true);
          return;
        }
        last_batch_ = result.data;
        if (sealed)
          ++app.catalog.packs_version;
        app.notify(nb(result.data["items"].size(), "item créé", "items créés") + ".");
        // apercu avant impression : etiquette du paquet puis etiquettes individuelles
        std::vector< PrintJob > jobs;
        if (sealed)
          app.build_label_jobs(TemplateCategory::ItemPack, sealed_packs_parameters(batch_sealed_packs()), "Paquet", jobs);
        if (print_items)
          app.build_label_jobs(TemplateCategory::Item, item_labels(result.data["items"]), "Réception", jobs);
        app.preview_jobs(std::move(jobs), "Réception");
        load_packs(app);
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Types d'items
    void draw_types(App &app) {
      if (primary_button("Nouveau type d'item"))
        clear_type_form();
      ImGui::SameLine();
      if (button("Rafraîchir"))
        app.refresh_item_types();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x * 0.5f));
      input_limited("##item_type_search", type_search_, 64, "Rechercher un type d'item (nom, code)…");
      ImGui::SameLine();
      ImGui::Checkbox("Types archivés", &show_archived_);
      // par nom, archives en dernier
      std::vector< const Json * > types;
      for (const Json &item_type : app.catalog.item_types.items())
        if ((show_archived_ || !item_type["archived"].boolean())
            && (type_search_.empty() || search_matches(type_search_, item_type["name"].str())
                || search_matches(type_search_, item_type["type"].str())))
          types.push_back(&item_type);
      std::stable_sort(types.begin(), types.end(), [](const Json *a, const Json *b) {
        const bool archived_a = (*a)["archived"].boolean(), archived_b = (*b)["archived"].boolean();
        if (archived_a != archived_b)
          return archived_b;
        return name_key((*a)["name"].str()) < name_key((*b)["name"].str());
      });
      if (types.empty())
        ImGui::TextDisabled(type_search_.empty() ? "Aucun type d'item." : "Aucun type d'item trouvé.");
      if (ImGui::BeginTable("types", 9,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // nom en premier (code en gris), colonnes courtes ensuite : le nom garde la place
        ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Min.", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Périssable", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Paquet", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("À déchirer", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Étiquette", ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Consomm.", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Tâches", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Entretien", ImGuiTableColumnFlags_WidthFixed, 110.0f); // revision, desinfection
        ImGui::TableHeadersRow();
        for (const Json *row : types) {
          const Json       &item_type = *row;
          const bool        archived  = item_type["archived"].boolean();
          const std::string code = item_type["type"].str();
          ImGui::TableNextRow();
          if (archived)
            row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          // ligne entiere cliquable, nom puis code en gris par-dessus
          const float start = ImGui::GetCursorPosX();
          if (ImGui::Selectable(("##" + code).c_str(), editing_type_ == code,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            edit_type(item_type);
          ImGui::SameLine();
          ImGui::SetCursorPosX(start);
          ImGui::TextUnformatted(item_type["name"].str().c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s%s%s", code.c_str(), archived ? " - archivé" : "", unit_summary(app, item_type).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["min_quantity"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["perissable"].boolean() ? "oui" : "non");
          ImGui::TableNextColumn();
          ImGui::Text("%d", item_type["default_pack_size"].integer(1));
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["tear_off"].boolean() ? "oui" : "non");
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(kLabelStyleNames[label_style_index(item_type["label_style"].str())]);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item_type["consumable"].boolean(true) ? "oui" : "non");
          ImGui::TableNextColumn();
          if (item_type["tasks"].size() > 0)
            ImGui::Text("%zu", item_type["tasks"].size());
          else
            ImGui::TextDisabled("-");
          ImGui::TableNextColumn();
          {
            const bool  tracked      = !item_type["consumable"].boolean(true);
            const int   revision     = tracked ? item_type["revision_months"].integer() : 0;
            const int   disinfection = tracked ? item_type["disinfection_months"].integer() : 0;
            std::string text;
            if (revision > 0)
              text = "Rév. " + std::to_string(revision) + " m";
            if (disinfection > 0)
              text += (text.empty() ? "" : " · ") + std::string("Dés. ") + std::to_string(disinfection) + " m";
            if (text.empty())
              ImGui::TextDisabled("-");
            else
              ImGui::TextUnformatted(text.c_str());
            ImGui::SetItemTooltip("Révision et désinfection : périodicité en mois");
          }
        }
        ImGui::EndTable();
      }
      ImGui::SeparatorText(editing_type_.empty() ? "Nouveau type d'item" : "Modifier le type");
      ImGui::BeginDisabled(!editing_type_.empty());
      ImGui::SetNextItemWidth(120.0f);
      input_limited("Code (6 caractères)", form_code_, 6, nullptr, 0, true);
      ImGui::EndDisabled();
      help_marker("Début de l'identifiant de chaque item, ex: serphy pour du sérum phy. Non modifiable ensuite.");
      bool              code_taken = false;
      const std::string code_name  = editing_type_.empty() ? name_taken_code(app, form_code_) : std::string();
      if (!code_name.empty()) {
        ImGui::TextColored(colors::red, "Le code %s est déjà celui de « %s ».", form_code_.c_str(), code_name.c_str());
        code_taken = true;
      }
      input_limited("Nom", form_name_, 64);
      const bool name_taken = name_taken_warning(app.catalog.item_types, "type", form_name_, editing_type_,
                                                 "Le type d'item");
      ImGui::TextUnformatted("Description");
      ImGui::InputTextMultiline("##description", &form_description_, ImVec2(-FLT_MIN, 60));
      ImGui::Checkbox("Item à cocher (sans étiquette : gobelets, sucres…)", &form_to_check_);
      help_marker("Pour ce qu'on ne va pas étiqueter un par un (ex : 5 gobelets, 15 sucres) ou une simple "
                  "vérification (ex : « Piles de la lampe vérifiées ») : pas d'étiquette, pas de stock, pas de "
                  "péremption. Dans un lot, la vérif affiche une case à cocher : on compte à la main, puis on coche. "
                  "Non coché, il manque jusqu'à la vérif suivante. Un type qui a déjà des étiquettes ne peut pas "
                  "le devenir.");
      draw_unit_fields(app);
      // item a cocher : rien de ce qui suit ne le concerne (sauf les taches, affichees sous la case) ; type a unites :
      // seulement son minimum (en unites)
      ImGui::BeginDisabled(form_to_check_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt(form_unit_on_ && !form_unit_.empty() ? ("Quantité minimale en stock (" + form_unit_ + ")").c_str()
                                    : "Quantité minimale en stock",
                      &form_min_);
      ImGui::EndDisabled();
      ImGui::BeginDisabled(form_to_check_ || form_unit_on_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Items par paquet (réception)", &form_pack_size_);
      ImGui::Checkbox("Périssable (date de péremption obligatoire)", &form_perishable_);
      ImGui::SetNextItemWidth(200.0f);
      ImGui::Combo("Étiquette", &form_label_style_, kLabelStyleNames, IM_ARRAYSIZE(kLabelStyleNames));
      help_marker("QR code : contient l'URL de l'item, l'appareil photo d'un téléphone ouvre sa fiche web.\n"
                  "QR code réduit : petit QR de l'identifiant seul, moins déformé sur un objet rond.\n"
                  "Code-barres : identifiant seul, pour les objets ronds (flacon, tube) : collez-le avec les barres "
                  "qui font le tour de l'objet.\n"
                  "Planche : plusieurs petits items sur une même étiquette qui se découpe (ex : 5 dosettes de sérum "
                  "phy), chacun avec le petit QR de son identifiant (modèle « Item - planche » des Réglages).\n\n"
                  "Chaque style a sa version liste de tâches et à déchirer (modèles « Item - ... » des Réglages).");
      ImGui::Checkbox("Étiquette à déchirer avant utilisation", &form_tear_off_);
      help_marker("Pour un ensemble étiqueté une seule fois (ex : sachet de plusieurs sérums phy). L'étiquette porte "
                  "« Déchirer avant utilisation » : on l'arrache dès qu'on entame l'ensemble. À la vérif suivante, "
                  "l'étiquette manquante compte l'ensemble comme utilisé tout de suite : il faut remettre un "
                  "ensemble complet, les restes de l'ancien sont considérés comme perdus.");
      ImGui::EndDisabled();
      ImGui::BeginDisabled(form_to_check_ || form_unit_on_);
      ImGui::Checkbox("Non consommable (matériel réutilisable)", &form_non_consumable_);
      help_marker("Ex : lecteur de glycémie, oxymètre. Un item non consommable ne s'use pas : s'il manque à la vérif "
                  "d'un lot, il a été retiré du lot : il passe « hors lot » au lieu d'être compté comme utilisé ou "
                  "perdu, et rejoint le stock quand il y est scanné ou remis en stock à la main. Sorti du stock sans lot, il reste « sorti » jusqu'à son retour. Dans un rangement du "
                  "stock, il manque comme les autres items.");
      ImGui::EndDisabled();
      draw_lost_object(app);
      ImGui::TextUnformatted("Tâches à la vérif (une par ligne)");
      help_marker("Ex : DAE « Voyant vert allumé », « Électrodes dans leur date ». Les items de ce type ont une "
                  "étiquette de tâches : la scanner pendant une vérif ouvre la liste à cocher, et l'item n'est "
                  "compté qu'une fois toutes ses tâches cochées et validées. Un administrateur peut scanner deux "
                  "fois de suite l'étiquette pour toutes les valider. Vide : item ordinaire.");
      ImGui::BeginDisabled(form_unit_on_);
      ImGui::InputTextMultiline("##tasks", &form_tasks_, ImVec2(-FLT_MIN, 70));
      ImGui::EndDisabled();
      // l'etiquette a dechirer disparait a l'ouverture : elle ne peut pas porter la liste des taches
      const bool tear_and_tasks = form_tear_off_ && !form_task_list().empty();
      if (tear_and_tasks)
        ImGui::TextColored(colors::red, "Une étiquette à déchirer ne peut pas avoir de tâches : décochez-la ou videz "
                                        "les tâches.");
      // revision periodique : materiel non consommable seulement
      ImGui::BeginDisabled(!form_non_consumable_ || form_to_check_);
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Révision tous les (mois)", &form_revision_months_);
      help_marker("Ex : aspirateur de mucosités révisé tous les 3 mois. 0 : pas de révision. À la révision (bouton "
                  "« Réviser » de l'item scanné, ici ou sur sa fiche web), la prochaine date vaut par défaut la date "
                  "de la révision + ce nombre de mois, et peut être changée. Les lots affichent leurs prochaines "
                  "révisions comme les péremptions.");
      if (form_revision_months_ > 0) {
        ImGui::Checkbox("Utilisable même si la révision est en retard", &form_revision_usable_);
        help_marker("Coché : en retard, l'item reste dans son lot avec un avertissement. Décoché : il compte comme "
                    "à remplacer, comme un périmé, jusqu'à sa révision.");
        ImGui::TextUnformatted("Tâches de la révision (une par ligne)");
        ImGui::InputTextMultiline("##revision_tasks", &form_revision_tasks_, ImVec2(-FLT_MIN, 70));
      }
      // desinfection periodique : distincte de la revision (un item peut avoir les deux)
      ImGui::SetNextItemWidth(150.0f);
      ImGui::InputInt("Désinfection tous les (mois)", &form_disinfection_months_);
      help_marker("Ex : BAVU ou attelles désinfectés tous les mois. 0 : pas de désinfection. Suivie à part de la "
                  "révision, avec sa propre date, ses tâches et le bouton « Désinfecter ».");
      if (form_disinfection_months_ > 0) {
        ImGui::Checkbox("Utilisable même si la désinfection est en retard", &form_disinfection_usable_);
        ImGui::TextUnformatted("Tâches de la désinfection (une par ligne)");
        ImGui::InputTextMultiline("##disinfection_tasks", &form_disinfection_tasks_, ImVec2(-FLT_MIN, 70));
      }
      ImGui::EndDisabled();
      form_revision_months_     = std::clamp(form_revision_months_, 0, 120);
      form_disinfection_months_ = std::clamp(form_disinfection_months_, 0, 120);
      form_min_       = std::max(0, form_min_);
      form_pack_size_ = std::max(1, form_pack_size_);

      Json body;
      body["name"]              = form_name_;
      body["description"]       = form_description_;
      body["min_quantity"]      = form_min_;
      body["perissable"]        = form_perishable_ && !form_to_check_;
      body["default_pack_size"] = form_pack_size_;
      body["tear_off"]          = form_tear_off_ && !form_to_check_;
      body["physical"]          = !form_to_check_;
      body["label_style"]       = kLabelStyles[form_label_style_];
      body["consumable"]        = !form_non_consumable_;
      body["lost_object"]       = form_lost_object_ && !form_to_check_;
      body["tasks"]             = Json::array();
      for (const std::string &task : form_task_list())
        body["tasks"].push_back(task);
      body["revision_months"] = form_non_consumable_ && !form_to_check_ ? form_revision_months_ : 0;
      body["revision_usable"] = form_revision_usable_;
      body["revision_tasks"]  = Json::array();
      for (const std::string &task : task_lines(form_revision_tasks_))
        body["revision_tasks"].push_back(task);
      body["disinfection_months"] = form_non_consumable_ && !form_to_check_ ? form_disinfection_months_ : 0;
      body["disinfection_usable"] = form_disinfection_usable_;
      body["disinfection_tasks"]  = Json::array();
      for (const std::string &task : task_lines(form_disinfection_tasks_))
        body["disinfection_tasks"].push_back(task);
      // type parent (a unites ou non) : ni etiquette ni item (pas de peremption, de taches, d'etiquette a dechirer)
      const bool unit_type = form_unit_on_ && !form_to_check_;
      body["is_group"]     = unit_type;
      body["unit"]         = unit_type ? form_unit_ : std::string();
      body["group"]        = unit_type || form_to_check_ ? std::string() : form_group_;
      body["fills"]        = form_fills_;
      if (!editing_type_.empty())
        body["default_subtype"] = unit_type ? form_default_subtype_ : std::string();
      if (unit_type) {
        body["perissable"] = false;
        body["tear_off"]   = false;
        body["tasks"]      = Json::array();
      }
      if (editing_type_.empty()) {
        // type parent : l'unite est facultative (sans unite, chaque item d'un sous-type compte pour 1)
        ImGui::BeginDisabled(form_code_.size() != 6 || form_name_.empty() || name_taken || code_taken || tear_and_tasks);
        if (primary_button("Créer le type")) {
          body["type"] = form_code_;
          app.api.post("/api/item-types/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify(result.error, true);
              return;
            }
            app.notify("Type " + result.data["type"].str() + " créé.");
            app.preview_labels(TemplateCategory::Item,
                               { item_parameters(sample_item(result.data["type"].str(), result.data["name"].str(),
                                                             result.data["perissable"].boolean(), "",
                                                             result.data["tear_off"].boolean(), form_task_list(),
                                                             result.data["label_style"].str("qr"))) },
                               "Étiquette d'un item " + result.data["name"].str() + " (exemple)");
            clear_type_form();
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
      } else {
        ImGui::BeginDisabled(form_name_.empty() || name_taken || tear_and_tasks);
        if (primary_button("Enregistrer")) {
          app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [&app](const ApiResult &result) {
            app.notify(result.ok ? "Type enregistré." : result.error, !result.ok);
            app.refresh_item_types();
          });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (button("Nouveau type"))
          clear_type_form();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(form_code_.empty());
      if (button("Aperçu de l'étiquette"))
        app.preview_labels(TemplateCategory::Item,
                           { item_parameters(sample_item(form_code_, form_name_, form_perishable_, "", form_tear_off_,
                                                         form_task_list(), kLabelStyles[form_label_style_])) },
                           "Étiquette d'un item " + form_name_ + " (exemple)");
      ImGui::EndDisabled();
      if (!editing_type_.empty())
        draw_archive_type(app);
    }

    // Objet qu'on peut perdre (ex : ordinateur) : la fiche ouverte par le scan de son etiquette affiche le message
    // « objet perdu », sans etat vert, sans lot ni verifs. Le message est commun a tous les types coches : il se
    // modifie ici, sous la case, et s'enregistre a part du type.
    void draw_lost_object(App &app) {
      ImGui::BeginDisabled(form_to_check_);
      ImGui::Checkbox("Message objet perdu sur sa fiche", &form_lost_object_);
      ImGui::EndDisabled();
      help_marker("Ex : ordinateur, tablette. Scanner l'étiquette d'un objet de ce type (appareil photo d'un "
                  "téléphone) ouvre une fiche avec le message « objet perdu » ci-dessous, pour la personne qui l'a "
                  "trouvé : pas d'état vert, pas de lot, pas d'informations sur les vérifs. Le message est le même "
                  "pour tous les objets.");
      if (!form_lost_object_ || form_to_check_)
        return;
      if (!lost_message_loaded_ && !lost_message_loading_) {
        lost_message_loading_ = true;
        app.api.get("/api/lost-object-message/", [this](const ApiResult &result) {
          lost_message_loading_ = false;
          if (!result.ok)
            return;
          lost_message_loaded_ = true;
          lost_message_       = result.data["message"].str();
          lost_message_saved_ = lost_message_;
        });
      }
      // limite du serveur en caracteres, pas en octets (accents)
      const size_t length = static_cast<size_t>(std::count_if(lost_message_.begin(), lost_message_.end(),
                                                              [](char c) { return (c & 0xC0) != 0x80; }));
      ImGui::Indent();
      ImGui::TextUnformatted("Message objet perdu (commun à tous les objets)");
      ImGui::BeginDisabled(!lost_message_loaded_);
      ImGui::InputTextMultiline("##lost_message", &lost_message_, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.5f));
      ImGui::BeginDisabled(lost_message_ == lost_message_saved_ || length > 2000);
      if (button("Enregistrer le message")) {
        Json body;
        body["message"] = lost_message_;
        app.api.patch("/api/lost-object-message/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          lost_message_       = result.data["message"].str();
          lost_message_saved_ = lost_message_;
          app.notify("Message objet perdu enregistré.");
        });
      }
      ImGui::EndDisabled();
      ImGui::EndDisabled();
      if (length > 2000)
        ImGui::TextColored(colors::red, "2000 caractères au plus.");
      else if (lost_message_loaded_ && lost_message_ != lost_message_saved_)
        ImGui::TextColored(colors::orange, "Message pas encore enregistré.");
      ImGui::Unindent();
    }

    // Attentes en quantite (« 200 mL de sérum phy » des recommandations, « 4 bandes, L ou S ») : type parent, a
    // unites ou non, ou sous-type d'un type parent qui en remplit un nombre d'unites (sachet de 5 dosettes de 5 mL :
    // 25 ; sans unite, un item compte pour 1)
    void draw_unit_fields(App &app) {
      ImGui::BeginDisabled(form_to_check_);
      ImGui::Checkbox("Type parent (rempli par ses sous-types, sans étiquette)", &form_unit_on_);
      help_marker("Ce type ne s'imprime pas et n'a pas d'items : ce sont ses sous-types qu'on réceptionne et qu'on "
                  "scanne, et dans un lot n'importe lequel d'entre eux le remplit. Sans unité, chaque item d'un "
                  "sous-type compte pour 1, ex : « Bandes » rempli par « Bandes L » et « Bandes S ». Avec une unité, "
                  "pour une attente en quantité des recommandations, ex : « 200 mL de sérum phy » rempli par des "
                  "sachets de 25 mL ou des flacons de 50 mL, chacun pour ses unités. Ajoutez aussi un sous-type dans "
                  "le lot pour en exiger un minimum compris dedans (« 4 bandes, dont 1 L »).");
      if (form_unit_on_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        input_limited("Unité (facultative)", form_unit_, 16);
        ImGui::SetItemTooltip("Vide : chaque item d'un sous-type compte pour 1 (bandes L ou S). Une unité (mL) : "
                              "chaque sous-type en remplit un nombre (flacon de 50 mL).");
        // sous-type par defaut : propose a la reception, a la commande et au reassort
        Json subtypes = Json::array();
        for (const Json &other : app.catalog.item_types.items())
          if (!editing_type_.empty() && other["group"].str() == editing_type_ && !other["archived"].boolean())
            subtypes.push_back(other);
        if (subtypes.size() > 0) {
          ImGui::TextUnformatted("Sous-type par défaut");
          search_select("default_subtype", subtypes, "type", "name", form_default_subtype_, "Tapez le nom…",
                        "Aucun (celui qui en remplit le plus)", 320.0f);
          help_marker("Proposé pour compléter un lot (« il manque 50 mL : 2 sachets ») et pour la commande quand "
                      "le stock passe sous le minimum.");
          std::string list;
          for (const Json &sub : subtypes.items())
            list += (list.empty() ? "" : ", ") + sub["name"].str()
                    + (form_unit_.empty() ? std::string()
                                          : " (" + std::to_string(sub["fills"].integer(1)) + " " + form_unit_ + ")");
          ImGui::TextDisabled("Sous-types : %s", list.c_str());
        } else if (!editing_type_.empty())
          ImGui::TextDisabled("Aucun sous-type : choisissez ce type comme « Sous-type de » sur ses conditionnements.");
      } else {
        Json groups = Json::array();
        for (const Json &other : app.catalog.item_types.items())
          if (other["is_group"].boolean() && !other["archived"].boolean() && other["type"].str() != editing_type_)
            groups.push_back(other);
        if (groups.size() > 0 || !form_group_.empty()) {
          ImGui::TextUnformatted("Sous-type de");
          search_select("group", groups, "type", "name", form_group_, "Tapez le nom du type parent…", "Aucun",
                        320.0f);
          std::string unit;
          for (const Json &group : groups.items())
            if (group["type"].str() == form_group_)
              unit = group["unit"].str();
          if (!form_group_.empty() && !unit.empty()) { // sans unite, un item compte pour 1
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::InputInt(("Remplit (" + unit + ")").c_str(), &form_fills_);
            form_fills_ = std::clamp(form_fills_, 1, 1000000);
            help_marker("Unités qu'un item de ce type remplit dans les attentes de son type à unités, ex : 25 pour un "
                        "sachet de 5 dosettes de 5 mL, 50 pour un flacon de 50 mL. Il s'imprime et se réceptionne "
                        "comme un type ordinaire.");
          }
        }
      }
      ImGui::EndDisabled();
    }

    // Archivage : le type n'est plus propose (reception, contenu des lots, stock), ses items restent en base
    void draw_archive_type(App &app) {
      ImGui::SameLine();
      const bool archived = editing_type_archived_;
      if (archived ? button("Désarchiver le type") : button("Archiver le type…")) {
        if (archived)
          archive_type(app, false);
        else
          ImGui::OpenPopup("archive_item_type");
      }
      if (ImGui::BeginPopup("archive_item_type")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::Text("Archiver le type « %s » ?", form_name_.c_str());
        ImGui::TextWrapped("Il ne sera plus proposé à la réception ni dans le contenu des lots, et disparaît de "
                           "l'état des stocks. Les items existants restent en base. Il doit d'abord être retiré du "
                           "contenu attendu des types de lots.");
        ImGui::TextWrapped("Pour le désarchiver : cochez « Afficher les types archivés » au-dessus de la liste, "
                           "sélectionnez-le puis cliquez sur « Désarchiver le type ».");
        ImGui::PopTextWrapPos();
        if (danger_button("Archiver") || enter_pressed()) {
          archive_type(app, true);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Annuler"))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }

    void archive_type(App &app, bool archived) {
      Json body;
      body["archived"] = archived;
      app.api.patch("/api/item-types/" + url_encode(editing_type_) + "/", body, [this, &app, archived](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify(archived ? "Type archivé." : "Type désarchivé.");
        if (archived)
          show_archived_ = true; // le type reste visible et selectionne
        edit_type(result.data);
        app.refresh_item_types();
      });
    }

    // « - en mL » pour un type a unites, « - 25 mL de Sérum phy » pour un de ses sous-types
    static std::string unit_summary(const App &app, const Json &item_type) {
      if (!item_type["unit"].str().empty())
        return " - en " + item_type["unit"].str() + ", sans étiquette";
      if (item_type["is_group"].boolean())
        return " - type parent, sans étiquette";
      if (const Json *group = item_type["group"].str().empty() ? nullptr : find_type(app, item_type["group"].str()))
        return (*group)["unit"].str().empty()
                 ? " - sous-type de " + (*group)["name"].str()
                 : " - " + std::to_string(item_type["fills"].integer(1)) + " " + (*group)["unit"].str() + " de "
                     + (*group)["name"].str();
      return "";
    }

    // Nom du type qui a deja ce code (vide si libre)
    static std::string name_taken_code(const App &app, const std::string &code) {
      const Json *type = find_type(app, code);
      return type ? (*type)["name"].str() : std::string();
    }

    void edit_type(const Json &item_type) {
      editing_type_     = item_type["type"].str();
      form_code_        = editing_type_;
      form_name_        = item_type["name"].str();
      form_description_ = item_type["description"].str();
      form_min_         = item_type["min_quantity"].integer();
      form_pack_size_   = item_type["default_pack_size"].integer(1);
      form_perishable_  = item_type["perissable"].boolean();
      form_tear_off_    = item_type["tear_off"].boolean();
      form_label_style_ = label_style_index(item_type["label_style"].str());
      form_non_consumable_ = !item_type["consumable"].boolean(true);
      form_lost_object_    = item_type["lost_object"].boolean();
      form_to_check_       = !item_type["physical"].boolean(true);
      form_tasks_.clear();
      for (const std::string &task : tasks_of(item_type))
        form_tasks_ += (form_tasks_.empty() ? "" : "\n") + task;
      form_revision_months_ = item_type["revision_months"].integer();
      form_revision_usable_ = item_type["revision_usable"].boolean(true);
      form_revision_tasks_.clear();
      for (const Json &task : item_type["revision_tasks"].items())
        form_revision_tasks_ += (form_revision_tasks_.empty() ? "" : "\n") + task.str();
      form_disinfection_months_ = item_type["disinfection_months"].integer();
      form_disinfection_usable_ = item_type["disinfection_usable"].boolean(true);
      form_disinfection_tasks_.clear();
      for (const Json &task : item_type["disinfection_tasks"].items())
        form_disinfection_tasks_ += (form_disinfection_tasks_.empty() ? "" : "\n") + task.str();
      editing_type_archived_ = item_type["archived"].boolean();
      form_unit_             = item_type["unit"].str();
      form_unit_on_          = item_type["is_group"].boolean() || !form_unit_.empty();
      form_group_            = item_type["group"].str();
      form_fills_            = std::max(1, item_type["fills"].integer(1));
      form_default_subtype_  = item_type["default_subtype"].str();
    }

    void clear_type_form() {
      editing_type_.clear();
      form_code_.clear();
      form_name_.clear();
      form_description_.clear();
      form_min_        = 0;
      form_pack_size_  = 1;
      form_perishable_ = true;
      form_tear_off_   = false;
      form_label_style_ = 0;
      form_non_consumable_ = false;
      form_lost_object_    = false;
      form_to_check_       = false;
      form_tasks_.clear();
      form_revision_months_ = 0;
      form_revision_usable_ = true;
      form_revision_tasks_.clear();
      form_disinfection_months_ = 0;
      form_disinfection_usable_ = true;
      form_disinfection_tasks_.clear();
      editing_type_archived_ = false;
      form_unit_on_          = false;
      form_unit_.clear();
      form_group_.clear();
      form_fills_ = 1;
      form_default_subtype_.clear();
    }

    // Taches saisies, une par ligne (lignes vides ignorees)
    std::vector< std::string > form_task_list() const { return task_lines(form_tasks_); }

    static std::vector< std::string > task_lines(const std::string &text) {
      std::vector< std::string > tasks;
      std::size_t                start = 0;
      while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
          end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && std::isspace(static_cast< unsigned char >(line.back())))
          line.pop_back();
        while (!line.empty() && std::isspace(static_cast< unsigned char >(line.front())))
          line.erase(line.begin());
        if (!line.empty())
          tasks.push_back(line);
        start = end + 1;
      }
      return tasks;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Items : recherche, reimpression unitaire, suppression manuelle
    void search(App &app) {
      std::string query = "/api/items/?limit=500";
      if (!search_text_.empty())
        query += "&q=" + url_encode(search_text_);
      if (!search_type_.empty())
        query += "&type=" + url_encode(search_type_);
      static const char *statuses[] = { "", "active", "missing", "replaced", "deleted", "out", "loose" };
      if (search_status_ > 0)
        query += std::string("&status=") + statuses[search_status_];
      if (search_stock_only_)
        query += "&location=stock";
      app.api.get(query, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Items : " + result.error, true);
          return;
        }
        items_ = result.data;
        // l'item selectionne est remplace par sa version a jour
        const std::string selected = selected_item_["iid"].str();
        selected_item_             = Json();
        for (const Json &item : items_.items())
          if (!selected.empty() && item["iid"].str() == selected)
            selected_item_ = item;
      });
    }

    void draw_items(App &app) {
      bool changed = false;
      ImGui::SetNextItemWidth(220.0f);
      changed |= ImGui::InputTextWithHint("##q", "Rechercher un iid", &search_text_, ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      changed |= search_select("search_type", app.catalog.item_types, "type", "name", search_type_,
                               "Filtrer par type…", "Tous les types", 240.0f);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(150.0f);
      changed |= ImGui::Combo("##status", &search_status_, "Tous statuts\0Presents\0Disparus\0Remplaces\0Supprimes\0Sortis\0Hors lot\0");
      ImGui::SameLine();
      changed |= ImGui::Checkbox("En stock", &search_stock_only_);
      ImGui::SameLine();
      if (button("Rechercher") || changed)
        search(app);

      // la liste prend la hauteur restante moins celle du bas (item choisi, actions avancees) mesuree a l'image
      // precedente : deplier les actions avancees retrecit la liste au lieu de faire defiler toute la fenetre.
      // Au moins quelques lignes de liste : sur un ecran trop petit, c'est la fenetre qui defile.
      const float min_list = ImGui::GetTextLineHeightWithSpacing() * 6;
      const float list_h   = std::max(min_list, ImGui::GetContentRegionAvail().y - items_bottom_h_);
      if (ImGui::BeginTable("items", 6,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, list_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Emplacement", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Statut", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("AperçuRéimprimer").x + ImGui::GetStyle().FramePadding.x * 4
                                  + ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableHeadersRow();
        for (const Json &item : items_.items()) {
          const std::string iid = item["iid"].str();
          ImGui::TableNextRow();
          if (item["expired"].boolean())
            row_color(colors::red, 0.35f);
          else if (item["status"].str() != "active")
            row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          if (ImGui::Selectable(iid.c_str(), selected_item_["iid"].str() == iid))
            selected_item_ = item;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["type_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["status"].str() == "loose" ? "Hors lot"
                                 : item["location"].is_null() ? "Sans rangement" : item["location_name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item["status"].str().c_str());
          if (item["disabled"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "désactivé");
            ImGui::SetItemTooltip("%s", item["disabled_reason"].str().c_str());
          }
          ImGui::TableNextColumn();
          ImGui::PushID(iid.c_str());
          if (small_button("Aperçu"))
            app.preview_labels(TemplateCategory::Item, { item_parameters(item) }, "Item " + iid);
          ImGui::SameLine();
          if (small_button("Réimprimer"))
            app.print_labels(TemplateCategory::Item, { item_parameters(item) }, "Remplacement");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      const float bottom_start = ImGui::GetCursorPosY();
      draw_item_actions(app);
      items_bottom_h_ = ImGui::GetCursorPosY() - bottom_start;
    }

    // Sous la liste des items : item selectionne et actions avancees
    void draw_item_actions(App &app) {
      if (selected_item_.is_null()) {
        ImGui::TextDisabled("Sélectionnez un item pour les actions avancées.");
        return;
      }
      const std::string iid = selected_item_["iid"].str();
      ImGui::Text("%s - %s - vu %s par %s", iid.c_str(), selected_item_["type_name"].str().c_str(),
                  display_on(selected_item_["last_seen"], true).c_str(), selected_item_["last_seen_by"].str().c_str());
      draw_split(app, iid);
      if (ImGui::CollapsingHeader("Actions avancées")) {
        ImGui::TextWrapped("Les items ne sont normalement pas supprimés : ils sont détectés comme disparus par les "
                           "vérifs. N'utilisez la suppression que pour un cas exceptionnel (casse, erreur de saisie).");
        if (selected_item_["status"].str() == "loose") {
          // non consommable retire de son lot : il rejoint le stock a la main (ou en le scannant a une verif du stock)
          if (button("Remettre en stock")) {
            Json body;
            body["user"]  = app.user_ref();
            body["items"] = Json::array();
            body["items"].push_back(iid);
            app.api.post("/api/items/to-stock/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item remis en stock." : result.error, !result.ok);
              selected_item_ = Json();
              search(app);
            });
          }
          ImGui::SameLine();
        }
        if (selected_item_["status"].str() == "deleted") {
          if (button("Restaurer l'item")) {
            Json body;
            body["user"] = app.user_ref();
            app.api.post("/api/items/" + url_encode(iid) + "/restore/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item restauré." : result.error, !result.ok);
              selected_item_ = Json();
              search(app);
            });
          }
        } else {
          ImGui::SetNextItemWidth(300.0f);
          input_limited("##reason", delete_reason_, 128, "Raison (obligatoire)");
          ImGui::SameLine();
          ImGui::BeginDisabled(delete_reason_.empty());
          if (confirm_button("Marquer comme supprimé", "Marquer cet item comme supprimé ?", "confirm_delete")) {
            Json body;
            body["user"]   = app.user_ref();
            body["reason"] = delete_reason_;
            app.api.post("/api/items/" + url_encode(iid) + "/delete/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "Item marqué comme supprimé." : result.error, !result.ok);
              delete_reason_.clear();
              selected_item_ = Json();
              search(app);
            });
          }
          ImGui::EndDisabled();
        }
        if (selected_item_["status"].str() != "deleted" && selected_item_["status"].str() != "replaced")
          draw_disable(app, iid);
      }
    }

    // Decoupage d'un item en plus petits sous-types du meme type a unites (sachet de 5 serums phy → 5 dosettes) :
    // les nouveaux items prennent sa place (lot ou stock) et leurs etiquettes s'ouvrent en apercu, a imprimer.
    void draw_split(App &app, const std::string &iid) {
      if (selected_item_["status"].str() != "active")
        return;
      if (split_iid_ != iid) {
        split_iid_     = iid;
        split_targets_ = Json();
        split_choice_  = 0;
        app.api.get("/api/items/" + url_encode(iid) + "/split/", [this, iid](const ApiResult &result) {
          if (result.ok && split_iid_ == iid)
            split_targets_ = result.data["targets"];
        });
      }
      if (split_targets_.size() == 0)
        return;
      split_choice_ = std::clamp(split_choice_, 0, static_cast< int >(split_targets_.size()) - 1);
      const Json &target = split_targets_[split_choice_];
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Découper en");
      ImGui::SameLine();
      if (split_targets_.size() > 1) {
        ImGui::SetNextItemWidth(260.0f);
        const std::string current = target["count"].str() + " × " + target["name"].str();
        if (ImGui::BeginCombo("##split", current.c_str())) {
          for (int index = 0; index < static_cast< int >(split_targets_.size()); ++index) {
            const Json       &option = split_targets_[index];
            const std::string label  = option["count"].str() + " × " + option["name"].str();
            if (ImGui::Selectable(label.c_str(), index == split_choice_))
              split_choice_ = index;
          }
          ImGui::EndCombo();
        }
      } else
        ImGui::Text("%s × %s", target["count"].str().c_str(), target["name"].str().c_str());
      ImGui::SameLine();
      const std::string question = "Découper " + selected_item_["type_name"].str() + " en " + target["count"].str()
                                   + " × " + target["name"].str() + " ?\nSon étiquette est à jeter : les nouveaux "
                                   "items ont chacun la leur, à imprimer et coller.";
      if (confirm_button("Découper", question.c_str(), "confirm_split")) {
        Json body;
        body["user"] = app.user_ref();
        body["type"] = target["type"].str();
        app.api.post("/api/items/" + url_encode(iid) + "/split/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify(result.error, true);
            return;
          }
          const Json &items = result.data["items"];
          app.notify(nb(static_cast< long long >(items.size()), "item créé", "items créés") + " : imprimez leurs étiquettes.");
          app.preview_labels(TemplateCategory::Item, item_labels(items), "Découpage");
          selected_item_ = Json();
          split_iid_.clear();
          search(app);
        });
      }
      help_marker("Le conditionnement est ouvert pour être rangé à l'unité (ex : un sachet de 5 dosettes de sérum phy "
                  "en 5 dosettes étiquetées une par une) : même péremption, même lot ou rangement. L'item découpé "
                  "est retiré.");
    }

    // Desactivation (rappel produit, non-conformite), par un admin : l'item est invalide comme un perime et doit etre
    // remplace. « Toute sa reception » : tous les items recus avec lui (paquet ouvert a la reception, rappel d'un lot).
    void send_disable(App &app, Json body, const std::string &done) {
      body["user"] = app.user_ref();
      app.api.post("/api/items/disable/", body, [this, &app, done](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        app.notify(done + " (" + nb(result.data["count"].integer(), "item modifié", "items modifiés") + ").");
        disable_reason_.clear();
        disable_reception_ = false;
        search(app);
        load_receptions(app);
      });
    }

    void draw_disable(App &app, const std::string &iid) {
      ImGui::SeparatorText("Rappel produit, non-conformité");
      if (item_reception_iid_ != iid) {
        item_reception_iid_ = iid;
        item_reception_     = Json();
        app.api.get("/api/receptions/?item=" + url_encode(iid), [this, iid](const ApiResult &result) {
          if (result.ok && result.data.size() > 0 && item_reception_iid_ == iid)
            item_reception_ = result.data[0];
        });
      }
      if (!item_reception_.is_null()) {
        ImGui::TextDisabled("Reçu %s par %s, avec %s%s.",
                            display_on(item_reception_["added"], true).c_str(),
                            item_reception_["added_by"].str("-").c_str(),
                            nb(item_reception_["count"].integer() - 1, "autre item", "autres items").c_str(),
                            item_reception_["sealed_pack"].is_null() ? ""
                            : item_reception_["sealed_packs"].integer(1) > 1
                              ? (" (" + nb(item_reception_["sealed_packs"].integer(), "paquet fermé", "paquets fermés")
                                 + ")").c_str()
                              : (" (paquet fermé " + item_reception_["sealed_pack"].str() + ")").c_str());
        ImGui::SameLine();
        if (small_button("Voir les réceptions de ce type"))
          app.show_receptions(selected_item_["type"].str());
      }
      if (!app.admin()) {
        ImGui::TextDisabled("La désactivation d'un item est réservée aux administrateurs.");
        return;
      }
      if (selected_item_["disabled"].boolean()) {
        ImGui::TextColored(colors::red, "Désactivé %s par %s : %s",
                           display_on(selected_item_["disabled_at"], true).c_str(),
                           selected_item_["disabled_by"].str("-").c_str(),
                           selected_item_["disabled_reason"].str().c_str());
        if (button("Réactiver l'item")) {
          Json body;
          body["disabled"] = false;
          body["items"]    = Json::array();
          body["items"].push_back(iid);
          send_disable(app, body, "Item réactivé");
        }
        return;
      }
      ImGui::TextWrapped("Un item désactivé est invalide comme s'il était périmé : il ne compte plus dans son lot et "
                         "doit être remplacé (le remplaçant le sort du lot à la vérif).");
      ImGui::SetNextItemWidth(300.0f);
      input_limited("##disable_reason", disable_reason_, 128, "Raison (obligatoire), ex : rappel produit");
      ImGui::SameLine();
      const int count = item_reception_.is_null() ? 1 : item_reception_["count"].integer();
      ImGui::BeginDisabled(count <= 1);
      const std::string whole = "Toute sa réception (" + std::to_string(count) + " items)";
      ImGui::Checkbox(whole.c_str(), &disable_reception_);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(disable_reason_.empty());
      const bool all = disable_reception_ && count > 1;
      if (confirm_button(all ? "Désactiver la réception" : "Désactiver l'item",
                         all ? "Désactiver tous les items reçus avec celui-ci ?" : "Désactiver cet item ?",
                         "confirm_disable")) {
        Json body;
        body["disabled"] = true;
        body["reason"]   = disable_reason_;
        if (all)
          body["reception"] = iid;
        else {
          body["items"] = Json::array();
          body["items"].push_back(iid);
        }
        send_disable(app, body, all ? "Réception désactivée" : "Item désactivé");
      }
      ImGui::EndDisabled();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Receptions passees : items recus ensemble (meme type, meme date, meme instant), pour ranger d'un coup ce qui a
    // ete recu ensemble dans un rangement du stock, ou le desactiver (paquet ouvert des la reception, rappel produit).
    void load_receptions(App &app) {
      std::string query = "/api/receptions/?limit=200";
      if (!receptions_type_.empty())
        query += "&type=" + url_encode(receptions_type_);
      app.api.get(query, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Réceptions : " + result.error, true);
          return;
        }
        receptions_ = result.data;
      });
    }

    void draw_receptions(App &app) {
      if (search_select("receptions_type", app.catalog.item_types, "type", "name", receptions_type_,
                        "Filtrer par type…", "Tous les types", 240.0f))
        load_receptions(app);
      ImGui::SameLine();
      if (button("Rafraîchir"))
        load_receptions(app);
      const float detail =
        selected_reception_.empty() ? 0 : ImGui::GetFrameHeightWithSpacing() * (app.admin() ? 6.0f : 3.5f);
      if (ImGui::BeginTable("receptions", 7,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, -detail))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reçu le", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00/00/0000 à 00:00").x);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Présents", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Désactivés", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Par", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const Json &row : receptions_.items()) {
          const std::string first = row["item"].str();
          ImGui::TableNextRow();
          if (row["disabled"].integer() > 0)
            row_color(colors::red, row["disabled"].integer() == row["count"].integer() ? 0.35f : 0.18f);
          ImGui::TableNextColumn();
          ImGui::PushID(first.c_str());
          if (ImGui::Selectable(display_datetime(row["added"]).c_str(), selected_reception_ == first,
                                ImGuiSelectableFlags_SpanAllColumns))
            selected_reception_ = first;
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["type_name"].str().c_str());
          if (!row["sealed_pack"].is_null()) {
            ImGui::SameLine();
            if (row["sealed_packs"].integer(1) > 1)
              ImGui::TextDisabled("%s", nb(row["sealed_packs"].integer(), "paquet", "paquets").c_str());
            else
              ImGui::TextDisabled("paquet %s", row["sealed_pack"].str().c_str());
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["peremption"].is_null() ? "-" : display_date(row["peremption"]).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["count"].integer());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["active"].integer());
          ImGui::TableNextColumn();
          ImGui::Text("%d", row["disabled"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["added_by"].str().c_str());
        }
        ImGui::EndTable();
      }
      if (receptions_.size() == 0)
        ImGui::TextDisabled("Aucune réception.");
      if (selected_reception_.empty())
        return;
      const Json *selected = nullptr;
      for (const Json &row : receptions_.items())
        if (row["item"].str() == selected_reception_)
          selected = &row;
      if (!selected)
        return;
      const Json row = *selected;
      ImGui::SeparatorText((row["type_name"].str() + " reçus " + display_on(row["added"], true)).c_str());
      // toute la reception rangee d'un coup dans un rangement du stock (ceux deja mis dans un lot restent)
      const int in_stock = row["in_stock"].integer();
      search_select("store_place", storage_places(app), "id", "label", store_place_, "Tapez le nom du rangement…",
                    "Stock (sans rangement)", 300.0f);
      ImGui::SameLine();
      ImGui::BeginDisabled(in_stock == 0 || storing_);
      const std::string store_label = "Ranger " + pl(in_stock, "l'item", "les " + std::to_string(in_stock) + " items")
                                    + (store_place_.empty() ? " en vrac dans le stock" : " dans " + place_label(app, store_place_));
      if (button(store_label.c_str())) {
        Json body;
        body["reception"] = selected_reception_;
        body["location"]  = store_place_.empty() ? Json() : Json(store_place_);
        body["user"]      = app.user_ref();
        storing_          = true;
        app.api.post("/api/receptions/store/", body, [this, &app](const ApiResult &result) {
          storing_ = false;
          if (!result.ok) {
            app.notify("Rangement refusé : " + result.error, true);
            return;
          }
          app.notify(nb(result.data["count"].integer(), "item rangé", "items rangés") + ".");
          load_receptions(app);
          app.refresh_lots(true);
        });
      }
      ImGui::EndDisabled();
      help_marker("Range d'un coup tous les items de cette réception encore dans le stock. Ceux déjà mis dans un lot, "
                  "utilisés ou supprimés ne bougent pas.");
      if (in_stock < row["count"].integer())
        ImGui::TextDisabled("%s sur %d encore dans le stock.", nb(in_stock, "item").c_str(), row["count"].integer());
      if (!app.admin())
        return;
      ImGui::SetNextItemWidth(300.0f);
      input_limited("##reception_reason", disable_reason_, 128, "Raison (obligatoire), ex : rappel produit");
      ImGui::SameLine();
      ImGui::BeginDisabled(disable_reason_.empty());
      const std::string label = "Désactiver les " + row["count"].str() + " items";
      if (confirm_button(label.c_str(), "Désactiver tous les items de cette réception ?", "confirm_reception")) {
        Json body;
        body["disabled"]  = true;
        body["reason"]    = disable_reason_;
        body["reception"] = selected_reception_;
        send_disable(app, body, "Réception désactivée");
      }
      ImGui::EndDisabled();
      if (row["disabled"].integer() > 0) {
        ImGui::SameLine();
        if (button("Réactiver la réception")) {
          Json body;
          body["disabled"]  = false;
          body["reception"] = selected_reception_;
          send_disable(app, body, "Réception réactivée");
        }
      }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Paquets fermes : ouverture et impression des etiquettes individuelles
    void load_packs(App &app) {
      app.api.get(std::string("/api/packs/") + (show_opened_ ? "" : "?opened=0"), [this](const ApiResult &result) {
        if (result.ok)
          packs_ = result.data;
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Materiel : items non consommables ou suivis (revision, desinfection), dates et cycle, et desinfection des
    // lots eux-memes (cellule du VPS...). Gestion et admins (fenetre privilegiee).
    void load_equipment(App &app) {
      equipment_loading_ = true;
      seen_maintenance_version_ = app.catalog.maintenance_version;
      app.api.get("/api/maintenance/", [this](const ApiResult &result) {
        equipment_loading_ = false;
        if (result.ok) {
          equipment_ = result.data;
          for (const Json &item : equipment_["items"].items()) // selection relue (nouvelles dates)
            if (equipment_item_.is_object() && item["iid"].str() == std::as_const(equipment_item_)["iid"].str())
              equipment_item_ = item;
        }
      });
    }

    // Revision ou desinfection d'un item, objet du serveur (item["maintenance"]), Json() si le type n'en a pas
    static Json maintenance_kind(const Json &item, const std::string &kind) {
      for (const Json &row : item["maintenance"].items())
        if (row["kind"].str() == kind)
          return row;
      return Json();
    }

    // Echeance : date, rouge en retard, orange dans les 30 jours, et ou on en est dans le cycle
    static void draw_due(const Json &m, const Date &today) {
      if (!m.is_object()) {
        ImGui::TextDisabled("—");
        return;
      }
      const auto due = Date::parse(m["due"].str());
      if (!due) {
        ImGui::TextDisabled("pas prévue");
      } else {
        const ImVec4 color = m["late"].boolean() ? colors::red
                           : *due <= today.plus_days(30) ? colors::orange : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TextColored(color, "%s%s", due->display().c_str(), m["late"].boolean() ? " (en retard)" : "");
      }
      const int months = m["months"].integer();
      std::string tip = months == 1 ? "Tous les mois" : "Tous les " + std::to_string(months) + " mois";
      if (m["cycle_month"].integer() > 0 && months > 1)
        tip += ", mois " + std::to_string(m["cycle_month"].integer()) + " sur " + std::to_string(months);
      if (!m["done_at"].str().empty())
        tip += "\nDernière le " + display_date(Json(m["done_at"].str().substr(0, 10)))
             + (m["done_by"].str().empty() ? "" : " par " + m["done_by"].str());
      ImGui::SetItemTooltip("%s", tip.c_str());
    }

    void draw_equipment(App &app) {
      if (!equipment_loading_ && (equipment_.is_null() || seen_maintenance_version_ != app.catalog.maintenance_version))
        load_equipment(app);
      const Date today = Date::today();
      ImGui::TextWrapped("Matériel non consommable (lecteurs de glycémie, attelles, bouteilles d'O2...) : prochaines "
                         "révisions et désinfections, la plus proche en haut. Survolez une date pour le cycle. La "
                         "périodicité se règle sur le type d'item (Items › Types d'items).");
      ImGui::SetNextItemWidth(220.0f);
      ImGui::InputTextWithHint("##equipment_q", "Rechercher (type, iid, lieu)", &equipment_search_);
      ImGui::SameLine();
      ImGui::Checkbox("En retard ou dans les 30 jours", &equipment_soon_only_);
      ImGui::SameLine();
      if (button(equipment_loading_ ? "Chargement..." : "Rafraîchir"))
        load_equipment(app);

      const float min_list = ImGui::GetTextLineHeightWithSpacing() * 6;
      const float list_h   = std::max(min_list, ImGui::GetContentRegionAvail().y - equipment_bottom_h_);
      if (ImGui::BeginTable("equipment", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                              | ImGuiTableFlags_Resizable,
                            ImVec2(0, list_h))) {
        const float date_w = ImGui::CalcTextSize("00/00/0000 (en retard)").x;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("iid", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Emplacement", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Révision", ImGuiTableColumnFlags_WidthFixed, date_w);
        ImGui::TableSetupColumn("Désinfection", ImGuiTableColumnFlags_WidthFixed, date_w);
        ImGui::TableHeadersRow();
        const std::string needle = normalize_search(equipment_search_);
        int shown = 0;
        for (const Json &item : equipment_["items"].items()) {
          const std::string iid   = item["iid"].str();
          const std::string place = item["status"].str() == "loose" ? "Hors lot"
                                  : item["location"].is_null()      ? "Sans rangement"
                                                                    : item["location_name"].str();
          if (!needle.empty() && normalize_search(item["type_name"].str() + " " + iid + " " + place).find(needle)
                                   == std::string::npos)
            continue;
          const Json revision     = maintenance_kind(item, "revision");
          const Json disinfection = maintenance_kind(item, "disinfection");
          bool late = false, soon = false;
          for (const Json *m : { &revision, &disinfection }) {
            if (!m->is_object())
              continue;
            late |= (*m)["late"].boolean();
            if (const auto due = Date::parse((*m)["due"].str()))
              soon |= *due <= today.plus_days(30);
          }
          if (equipment_soon_only_ && !late && !soon)
            continue;
          ++shown;
          ImGui::TableNextRow();
          if (late)
            row_color(colors::red, 0.2f);
          else if (item["status"].str() != "active")
            row_color(colors::grey, 0.3f);
          ImGui::TableNextColumn();
          ImGui::PushID(iid.c_str());
          if (ImGui::Selectable(item["type_name"].str().c_str(), std::as_const(equipment_item_)["iid"].str() == iid,
                                ImGuiSelectableFlags_SpanAllColumns))
            equipment_item_ = item;
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(iid.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(place.c_str());
          if (item["disabled"].boolean()) {
            ImGui::SameLine();
            ImGui::TextColored(colors::red, "désactivé");
          }
          ImGui::TableNextColumn();
          draw_due(revision, today);
          ImGui::TableNextColumn();
          draw_due(disinfection, today);
        }
        ImGui::EndTable();
        if (shown == 0 && !equipment_loading_)
          ImGui::TextDisabled(equipment_soon_only_ || !needle.empty() ? "Rien ne correspond."
                                                                       : "Aucun matériel non consommable.");
      }
      const float bottom_start = ImGui::GetCursorPosY();
      draw_equipment_actions(app);
      draw_lot_disinfections(app, today);
      equipment_bottom_h_ = ImGui::GetCursorPosY() - bottom_start;
    }

    void draw_equipment_actions(App &app) {
      if (!equipment_item_.is_object()) {
        ImGui::TextDisabled("Choisissez un matériel pour le réviser, le désinfecter ou saisir une date.");
        return;
      }
      ImGui::Text("%s · %s", equipment_item_["type_name"].str().c_str(), equipment_item_["iid"].str().c_str());
      bool first = true;
      for (const char *kind : { "revision", "disinfection" }) {
        const Json m = maintenance_kind(equipment_item_, kind);
        if (!m.is_object())
          continue;
        ImGui::PushID(kind);
        if (!std::exchange(first, false))
          ImGui::SameLine();
        if (button(m["verb"].str(std::string(kind) == "revision" ? "Réviser" : "Désinfecter").c_str()))
          app.open_revision(equipment_item_, kind);
        ImGui::SameLine();
        const std::string date_label = "Date de " + m["noun"].str() + "...";
        if (button(date_label.c_str()))
          app.open_maintenance_date(equipment_item_, kind);
        ImGui::PopID();
      }
      if (first)
        ImGui::TextDisabled("Ni révision ni désinfection pour ce type : réglez-les dans Items › Types d'items.");
    }

    // Desinfection des lots eux-memes : periodicite par lot (0 : aucune), faite, date saisie a la main
    void draw_lot_disinfections(App &app, const Date &today) {
      ImGui::Spacing();
      ImGui::SeparatorText("Désinfection des lots");
      ImGui::TextWrapped("Pour un lot désinfecté lui-même (cellule du VPS, caisson...) : choisissez le lot et sa "
                         "périodicité. Ses désinfections arrivent dans « À venir » et les tâches de la Gestion de la "
                         "TECH.");
      if (equipment_["lots"].size() > 0
          && ImGui::BeginTable("lot_disinfections", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Lot", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Prochaine", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00/00/0000 (en retard)").x);
        ImGui::TableSetupColumn("Tous les", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("DésinfecterDate...FicheRetirer").x
                                  + ImGui::GetStyle().FramePadding.x * 8 + ImGui::GetStyle().ItemSpacing.x * 3);
        ImGui::TableHeadersRow();
        for (const Json &lot : equipment_["lots"].items()) {
          const std::string id = lot["id"].str();
          const Json       &m  = lot["disinfection"];
          ImGui::PushID(id.c_str());
          ImGui::TableNextRow();
          if (m["late"].boolean())
            row_color(colors::red, 0.2f);
          ImGui::TableNextColumn();
          ImGui::Text("%s", lot["name"].str().c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", lot["lot_type_name"].str().c_str());
          ImGui::TableNextColumn();
          draw_due(m, today);
          ImGui::TableNextColumn();
          int months = m["months"].integer(1);
          ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
          ImGui::InputInt("mois", &months, 0);
          if (ImGui::IsItemDeactivatedAfterEdit() && months >= 1 && months <= 120 && months != m["months"].integer())
            app.set_lot_disinfection_months(id, months);
          ImGui::TableNextColumn();
          if (small_button("Désinfecter"))
            app.open_lot_disinfection(m);
          ImGui::SameLine();
          if (small_button("Date..."))
            app.open_lot_disinfection_date(m);
          ImGui::SameLine();
          if (small_button("Fiche"))
            app.show_lot(id);
          ImGui::SameLine();
          if (small_button("Retirer"))
            app.set_lot_disinfection_months(id, 0);
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      search_select("lot_disinfection_add", app.catalog.lots, "id", "name", add_disinfection_lot_, "Ajouter un lot…",
                    nullptr, 240.0f);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
      ImGui::InputInt("mois##add", &add_disinfection_months_, 0);
      add_disinfection_months_ = std::clamp(add_disinfection_months_, 1, 120);
      ImGui::SameLine();
      ImGui::BeginDisabled(add_disinfection_lot_.empty());
      if (button("Ajouter la désinfection")) {
        app.set_lot_disinfection_months(add_disinfection_lot_, add_disinfection_months_);
        add_disinfection_lot_.clear();
      }
      ImGui::EndDisabled();
    }

    void draw_packs(App &app) {
      ImGui::TextWrapped("Paquets fermés reçus en stock. À l'ouverture, les étiquettes individuelles des items "
                         "sont imprimées. Scanner l'étiquette d'un paquet ouvre directement sa fiche.");
      if (button("Rafraîchir"))
        load_packs(app);
      ImGui::SameLine();
      if (ImGui::Checkbox("Afficher les paquets déjà ouverts", &show_opened_))
        load_packs(app);
      if (!ImGui::BeginTable("packs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                             | ImGuiTableFlags_Resizable))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      // largeurs calculees sur le texte : la date d'ouverture reste lisible, le contenu prend le reste
      const float digits = ImGui::CalcTextSize("00/00/0000").x;
      ImGui::TableSetupColumn("Paquet", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000000").x);
      ImGui::TableSetupColumn("Contenu", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, digits);
      ImGui::TableSetupColumn("Ouvert le", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00/00/0000 00:00").x);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::CalcTextSize("Ouvrir...Étiquette").x + ImGui::GetStyle().FramePadding.x * 4
                                + ImGui::GetStyle().ItemSpacing.x);
      ImGui::TableHeadersRow();
      for (const Json &pack : packs_.items()) {
        const std::string id = pack["id"].str();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(id.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d x %s", pack["count"].integer(), pack["type_name"].str().c_str());
        ImGui::SetItemTooltip("%d x %s", pack["count"].integer(), pack["type_name"].str().c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(pack["peremption"]).c_str());
        ImGui::TableNextColumn();
        std::string opened = display_datetime(pack["opened"]); // "JJ/MM/AAAA à HH:MM", "aujourd'hui à HH:MM"...
        if (const std::size_t at = opened.find(" à "); at != std::string::npos)
          opened.replace(at, 4, " ");
        ImGui::TextUnformatted(opened.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(id.c_str());
        if (pack["opened"].is_null() ? primary_button("Ouvrir...") : button("Fiche"))
          app.show_pack(id);
        ImGui::SameLine();
        if (button("Étiquette"))
          app.preview_labels(TemplateCategory::ItemPack, { sealed_pack_parameters(pack) }, "Paquet");
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Materiel
    Json        equipment_;                   // /api/maintenance/ : {"items", "lots"}
    Json        equipment_item_;              // item choisi (actions sous la liste)
    bool        equipment_loading_ = false;
    int         seen_maintenance_version_ = -1;
    std::string equipment_search_;
    bool        equipment_soon_only_ = false;
    float       equipment_bottom_h_  = 0.0f; // hauteur du bas mesuree (voir items_bottom_h_)
    std::string add_disinfection_lot_;
    int         add_disinfection_months_ = 12;

    // Reception
    std::string reception_type_;
    std::string reception_place_; // rangement du stock de la reception (vide : le stock en vrac)
    std::string suggested_place_; // dernier rangement du type choisi, propose par defaut
    std::string peremption_;
    int         quantity_    = 1;
    int         pack_count_  = 1; // nombre de paquets fermes identiques (case « Paquet fermé »)
    bool        sealed_      = false;
    bool        print_items_ = true;
    bool        creating_    = false;
    Json        last_batch_;
    // Types
    std::string editing_type_;
    int         seen_types_version_ = -1;
    std::string form_code_;
    std::string form_name_;
    std::string form_description_;
    int         form_min_        = 0;
    int         form_pack_size_  = 1;
    bool        form_perishable_ = true;
    bool        form_tear_off_   = false;
    int         form_label_style_ = 0; // index dans kLabelStyles
    bool        form_non_consumable_ = false;
    bool        form_lost_object_    = false; // message « objet perdu » sur la fiche (ItemType.lost_object)
    bool        form_to_check_       = false; // item a cocher (ItemType.physical faux)
    // message « objet perdu », le meme pour tous les types coches (charge a la premiere case cochee affichee)
    std::string lost_message_;
    std::string lost_message_saved_;
    bool        lost_message_loaded_  = false;
    bool        lost_message_loading_ = false;
    std::string form_tasks_;
    int         form_revision_months_ = 0;
    bool        form_revision_usable_ = true;
    std::string form_revision_tasks_;
    int         form_disinfection_months_ = 0;
    bool        form_disinfection_usable_ = true;
    std::string form_disinfection_tasks_;
    bool        editing_type_archived_ = false;
    // attentes en quantite (ItemType.unit, group, fills, default_subtype) : type a unites (« Sérum phy » en mL, sans
    // etiquette) ou sous-type qui en remplit `form_fills_` unites
    bool        form_unit_on_ = false;
    std::string form_unit_;
    std::string form_group_;
    int         form_fills_ = 1;
    std::string form_default_subtype_;
    bool        show_archived_   = false;
    std::string type_search_;
    // Items
    std::string search_text_;
    std::string search_type_;
    int         search_status_     = 0;
    bool        search_stock_only_ = false;
    Json        items_ = Json::array();
    float       items_bottom_h_ = 0.0f; // hauteur du bas de l'onglet Items, image precedente
    Json        selected_item_;
    std::string delete_reason_;
    std::string disable_reason_;
    bool        disable_reception_ = false;
    std::string split_iid_;           // item dont les decoupages possibles sont charges
    Json        split_targets_;       // [{type, name, count}]
    int         split_choice_ = 0;
    std::string item_reception_iid_; // reception de l'item selectionne (actions avancees)
    Json        item_reception_;
    Json        receptions_ = Json::array();
    std::string receptions_type_;
    std::string selected_reception_;
    std::string store_place_; // rangement ou ranger une reception passee
    bool        storing_ = false;
    bool        first_tab_       = false; // onglet Reception a selectionner (vue par defaut)
    // Paquets
    Json packs_       = Json::array();
    bool show_opened_        = false;
    int  seen_packs_version_ = 0;

    // ---------------------------------------------------------------------------------------------------------------
    // Etat du stock, projection, commandes, transferts (avant : onglets de la Gestion de la TECH)
    std::string stock_select_; // onglet demande par App::show_stock_tab (stock, orders, transfers)
    bool        stock_projection_ = false; // Etat du stock : projection a M+k au lieu d'aujourd'hui

    // barre commune aux onglets du stock : filtre, minimum, lots comptes
    void draw_stock_toolbar(App &app) {
      if (button("Rafraîchir")) {
        app.refresh_stock();
        app.refresh_forecast();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("##stock_filter", "Filtrer", &filter_);
      ImGui::SameLine();
      ImGui::Checkbox("Sous le minimum uniquement", &only_low_);
      ImGui::SameLine();
      ImGui::Checkbox("Compter les lots", &include_lots_);
      help_marker("La barre compare les items non périmés au minimum du type. Par défaut seul le stock est compté : "
                  "items hors lots et items rangés dans un rangement du stock (armoire, tiroir : lots dont le type "
                  "est un rangement).");
      if (app.catalog.loading_stock || app.catalog.loading_forecast) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement...");
      }
    }

    // filtre par nom, code ou description du type (ex: nom commercial d'un medicament generique)
    bool filter_matches(const App &app, const Json &row) const {
      const std::string type = row["type"].str();
      return search_matches(filter_, row["name"].str() + " " + type + " " + app.catalog.item_type_description(type));
    }

    // ---- Actuel : etat du stock aujourd'hui ----
    void draw_current(App &app) {
      std::vector< const Json * > rows;
      for (const Json &row : app.catalog.stock.items())
        rows.push_back(&row);
      // les plus critiques en premier
      std::stable_sort(rows.begin(), rows.end(), [this](const Json *a, const Json *b) {
        return ratio(*a) < ratio(*b);
      });

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      if (!ImGui::BeginTable("stock", 7, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 220.0f);
      ImGui::TableSetupColumn("Quantité / minimum", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Dans les lots", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Périmés", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Bientôt périmés", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Disparus", ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Sortis / hors lot", ImGuiTableColumnFlags_WidthFixed, 140.0f);
      ImGui::TableHeadersRow();
      // types sans minimum attendu (x/0, 0/0) replies sous une ligne en bas, sauf pendant une recherche
      std::vector< const Json * > shown, zero;
      for (const Json *row_ptr : rows) {
        const Json &row      = *row_ptr;
        const int   quantity = count(row);
        const int   minimum  = row["min_quantity"].integer();
        if (!filter_matches(app, row))
          continue;
        if (only_low_ && quantity >= minimum && (quantity > 0 || minimum == 0))
          continue;
        (minimum <= 0 && filter_.empty() ? zero : shown).push_back(row_ptr);
      }
      for (const Json *row_ptr : shown)
        draw_current_row(*row_ptr);
      if (zero_fold_row(zero.size(), 7))
        for (const Json *row_ptr : zero)
          draw_current_row(*row_ptr);
      ImGui::EndTable();
    }

    // Ligne repliable des types sans minimum attendu, fermee par defaut ; vrai si depliee
    bool zero_fold_row(std::size_t hidden, int columns) {
      if (hidden == 0)
        return false;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const int         n     = static_cast< int >(hidden);
      const std::string label = std::string(zero_open_ ? "[−] " : "[+] ") + (n > 1 ? "Types" : "Type")
                              + " sans minimum attendu (" + std::to_string(n) + ")##zero_fold";
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
        zero_open_ = !zero_open_;
      ImGui::PopStyleColor();
      for (int column = 1; column < columns; ++column)
        ImGui::TableNextColumn();
      return zero_open_;
    }

    void draw_current_row(const Json &row) {
      const int quantity = count(row);
      const int minimum  = row["min_quantity"].integer();
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(stock_name(row).c_str());
      ImGui::TextDisabled("%s", row["type"].str().c_str());
      ImGui::TableNextColumn();
      stock_bar(quantity, minimum, ImVec2(-1, ImGui::GetFrameHeight() * 1.3f));
      ImGui::TableNextColumn();
      ImGui::Text("%d", row["lots_fresh"].integer());
      ImGui::TableNextColumn();
      colored_count(row["stock_expired"].integer() + row["lots_expired"].integer(), colors::red);
      ImGui::TableNextColumn();
      colored_count(row["expiring_soon"].integer(), colors::orange);
      ImGui::TableNextColumn();
      ImGui::Text("%d", row["missing"].integer());
      ImGui::TableNextColumn();
      ImGui::Text("%d", row["out"].integer());
      ImGui::SetItemTooltip("Sortis du stock sans lot : comptés utilisés s'ils ne reviennent pas à une vérif.");
      if (row["loose"].integer() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(colors::orange, "+%d hors lot", row["loose"].integer());
        ImGui::SetItemTooltip("Non consommables retirés de leur lot, pas encore remis en stock.");
      }
    }

    int count(const Json &row) const {
      return row["stock_fresh"].integer() + (include_lots_ ? row["lots_fresh"].integer() : 0);
    }

    // sous le minimum, puis bientot sous le minimum ou vide, puis 0/0, puis complets, puis x/0 tout en bas
    float ratio(const Json &row) const {
      const int soon = row["stock_expiring_soon"].integer() + (include_lots_ ? row["lots_expiring_soon"].integer() : 0);
      return stock_order(count(row), row["min_quantity"].integer(), soon);
    }

    // ---- Projection : stock a M+k, graphes du type selectionne ----
    const Json &point(const Json &row) const {
      const Json &points = row["points"];
      const int   index  = std::min(horizon_months[horizon_], static_cast< int >(points.size()) - 1);
      return points[std::max(0, index)];
    }

    int projected(const Json &row) const {
      const Json &at = point(row);
      if (with_consumption_)
        return at["stock"].integer() + (include_lots_ ? at["lots"].integer() : 0);
      return at["stock_static"].integer() + (include_lots_ ? at["lots_static"].integer() : 0);
    }

    void draw_projection(App &app) {
      const Json &forecast = app.catalog.forecast;
      if (!forecast["types"].is_array()) {
        ImGui::TextDisabled(app.catalog.loading_forecast ? "Chargement des prévisions..." : "Prévisions non chargées.");
        return;
      }
      ImGui::TextUnformatted("Horizon :");
      for (std::size_t index = 0; index < horizon_labels.size(); ++index) {
        ImGui::SameLine();
        if (ImGui::RadioButton(horizon_labels[index], horizon_ == static_cast< int >(index)))
          horizon_ = static_cast< int >(index);
      }
      const Json *any = forecast["types"].size() ? &forecast["types"][0] : nullptr;
      if (any) {
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", date_text(point(*any)["date"]).c_str());
      }
      ImGui::SameLine();
      ImGui::Checkbox("Avec consommation", &with_consumption_);
      help_marker("Avec consommation : chaque lieu utilise ses items les plus anciens en premier, au rythme mesuré "
                  "sur l'historique (items absents aux vérifs). Sans : seuls les items encore valides à la date.\n"
                  "Les items manqués à la dernière vérif ne comptent pas ; ils reviennent s'ils sont retrouvés.");
      ImGui::SameLine();
      ImGui::TextDisabled("(historique : %d j)", forecast["history_days"].integer());

      std::vector< const Json * > rows;
      for (const Json &row : forecast["types"].items()) {
        if (!filter_matches(app, row))
          continue;
        const int minimum = row["min_quantity"].integer();
        if (only_low_ && projected(row) >= minimum && projected(row) > 0 && row["below_min"].is_null())
          continue;
        rows.push_back(&row);
      }
      std::stable_sort(rows.begin(), rows.end(), [this](const Json *a, const Json *b) {
        const bool a_below = !(*a)["below_min"].is_null(), b_below = !(*b)["below_min"].is_null();
        if (a_below != b_below)
          return a_below;
        if (a_below)
          return (*a)["below_min"].str() < (*b)["below_min"].str();
        return projected_ratio(*a) < projected_ratio(*b);
      });
      // rien de selectionne : graphe de tout le stock (types affiches) ; re-clic sur la ligne ou Echap pour y revenir
      if (!selected_.empty() && escape_pressed() && !ImGui::GetIO().WantTextInput)
        selected_.clear();

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY
                                  | ImGuiTableFlags_Resizable;
      const float left = ImGui::GetContentRegionAvail().x * 0.55f;
      if (ImGui::BeginChild("projection_table", ImVec2(left, 0), ImGuiChildFlags_ResizeX)) {
        const std::string valid = "Valide au " + (any ? short_date(point(*any)["date"]) : std::string()) + " / minimum";
        if (ImGui::BeginTable("projection", 6, flags)) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 180.0f);
          ImGui::TableSetupColumn(valid.c_str(), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Périment", ImGuiTableColumnFlags_WidthFixed, 70.0f);
          ImGui::TableSetupColumn("Perdus", ImGuiTableColumnFlags_WidthFixed, 60.0f);
          ImGui::TableSetupColumn("Conso / mois", ImGuiTableColumnFlags_WidthFixed, 90.0f);
          ImGui::TableSetupColumn("Sous le min.", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableHeadersRow();
          // types sans minimum attendu replies en bas, sauf commande a passer, sous le minimum ou recherche
          std::vector< const Json * > shown, zero;
          for (const Json *row_ptr : rows) {
            const Json &row  = *row_ptr;
            const bool  fold = row["min_quantity"].integer() <= 0 && row["below_min"].is_null()
                           && !row["order"].is_object() && filter_.empty() && selected_ != row["type"].str();
            (fold ? zero : shown).push_back(row_ptr);
          }
          auto draw_row = [&](const Json &row) {
            const Json       &at   = point(row);
            const std::string type = row["type"].str();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(type.c_str());
            if (ImGui::Selectable(stock_name(row).c_str(), selected_ == type, ImGuiSelectableFlags_SpanAllColumns))
              selected_ = selected_ == type ? std::string() : type; // re-clic : retour a tout le stock
            ImGui::PopID();
            ImGui::TextDisabled("%s", type.c_str());
            ImGui::TableNextColumn();
            stock_bar(projected(row), row["min_quantity"].integer(), ImVec2(-1, ImGui::GetFrameHeight() * 1.3f));
            ImGui::TableNextColumn();
            colored_count(at["expiring"].integer(), colors::orange);
            ImGui::TableNextColumn();
            colored_count(at["lost"].integer(), colors::red);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rate_text(row["per_month"].num()).c_str());
            ImGui::TableNextColumn();
            if (row["below_min"].is_null())
              ImGui::TextColored(colors::green, "non");
            else
              ImGui::TextColored(colors::orange, "%s", date_text(row["below_min"]).c_str());
          };
          for (const Json *row_ptr : shown)
            draw_row(*row_ptr);
          if (zero_fold_row(zero.size(), 6))
            for (const Json *row_ptr : zero)
              draw_row(*row_ptr);
          ImGui::EndTable();
        }
      }
      ImGui::EndChild();
      ImGui::SameLine();
      if (ImGui::BeginChild("projection_detail", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        const Json *shown = nullptr;
        for (const Json &row : forecast["types"].items())
          if (row["type"].str() == selected_)
            shown = &row;
        if (shown) {
          if (small_button("← Tout le stock"))
            selected_.clear();
          ImGui::SameLine();
          ImGui::TextDisabled("(ou re-cliquer sur la ligne, Échap)");
          draw_type_detail(app, *shown);
        }
        else
          draw_all_stock(rows);
      }
      ImGui::EndChild();
    }

    // Aucun type selectionne : peremptions de tous les types affiches (filtre et case « sous le minimum » compris)
    void draw_all_stock(const std::vector< const Json * > &rows) {
      ImGui::TextUnformatted("Tout le stock");
      ImGui::SameLine();
      ImGui::TextDisabled("· %s", nb(static_cast< int >(rows.size()), "type").c_str());
      ImGui::TextDisabled("Cliquez sur une ligne pour le détail d'un type.");
      int expired = 0, below = 0, orders = 0;
      for (const Json *row : rows) {
        expired += (*row)["expired_now"].integer();
        below += (*row)["below_min"].is_null() ? 0 : 1;
        orders += (*row)["order"].is_object() ? 1 : 0;
      }
      if (expired > 0)
        ImGui::TextColored(colors::red, "%s aujourd'hui (stock et lots)", nb(expired, "périmé").c_str());
      if (below > 0)
        ImGui::TextColored(colors::orange, "%s sous le minimum d'ici 6 mois", nb(below, "type").c_str());
      if (orders > 0)
        ImGui::TextColored(colors::orange, "%s à commander", nb(orders, "type").c_str());
      draw_expiry_plot(rows, 320.0f);
    }

    float projected_ratio(const Json &row) const {
      return stock_order(projected(row), row["min_quantity"].integer());
    }

    // Nom d'une ligne de stock : « Sérum phy (mL) » pour un type a unites (ses sous-types comptes en unites)
    static std::string stock_name(const Json &row) {
      return row["name"].str() + (row["unit"].str().empty() ? std::string() : " (" + row["unit"].str() + ")");
    }

    // Commande d'un type a unites : en items de son sous-type par defaut (« 8 × Sachet… »)
    static std::string order_text(const Json &row) {
      const Json &order = row["order"];
      return order["subtype_name"].str().empty()
               ? std::to_string(order["quantity"].integer())
               : std::to_string(order["quantity"].integer()) + " × " + order["subtype_name"].str();
    }

    void draw_type_detail(App &app, const Json &row) {
      ImGui::TextUnformatted(stock_name(row).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("· conso %s / mois", rate_text(row["per_month"].num()).c_str());
      if (row["unconfirmed"].integer() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %d non %s", row["unconfirmed"].integer(), pl(row["unconfirmed"].integer(), "confirmé").c_str());
        help_marker("Items manqués à la dernière vérif : ils ne comptent pas dans les prévisions. S'ils sont "
                    "retrouvés (vérif, ajout à un lot), ils reviennent et ne comptent plus comme utilisés.");
      }
      const Json &order = row["order"];
      if (order.is_object()) {
        const ImVec4 color = order["urgent"].boolean() ? colors::red : colors::orange;
        ImGui::TextColored(color, "Commander %s avant le %s (sous le minimum le %s)", order_text(row).c_str(),
                           date_text(order["before"]).c_str(), date_text(order["below_min"]).c_str());
      }
      if (row["expired_now"].integer() > 0)
        ImGui::TextColored(colors::red, "%s aujourd'hui (stock et lots)", nb(row["expired_now"].integer(), "périmé").c_str());

      draw_projection_plot(row);
      draw_expiry_plot({ &row });

      if (ImGui::CollapsingHeader("Dates de péremption et emplacements")) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("batches", 5, flags)) {
          ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 95.0f);
          ImGui::TableSetupColumn("Où", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 50.0f);
          ImGui::TableSetupColumn("Utilisés", ImGuiTableColumnFlags_WidthFixed, 65.0f);
          ImGui::TableSetupColumn("Perdus", ImGuiTableColumnFlags_WidthFixed, 55.0f);
          ImGui::TableHeadersRow();
          for (const Json &batch : row["batches"].items()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(batch["peremption"].is_null() ? "aucune" : date_text(batch["peremption"]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(batch["path"].str().c_str());
            if (batch["sealed"].boolean()) {
              ImGui::SameLine();
              ImGui::TextDisabled("(scellé)");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", batch["count"].integer());
            ImGui::TableNextColumn();
            ImGui::Text("%d", batch["used"].integer());
            ImGui::TableNextColumn();
            colored_count(batch["lost"].integer(), colors::red);
          }
          ImGui::EndTable();
        }
      }
      if (ImGui::CollapsingHeader("Consommation par lot")) {
        ImGui::BulletText("Stock (sorties directes) : %s / mois", rate_text(row["stock_per_month"].num()).c_str());
        for (const Json &lot : row["lots_consumption"].items()) {
          ImGui::BulletText("%s : %s / mois", lot["path"].str().c_str(), rate_text(lot["per_month"].num()).c_str());
          if (!lot["id"].is_null()) {
            ImGui::SameLine();
            ImGui::PushID(lot["id"].str().c_str());
            if (small_button("Afficher"))
              app.show_lot(lot["id"].str());
            ImGui::PopID();
          }
        }
      }
    }

    void draw_projection_plot(const Json &row) {
      const Json         &points = row["points"];
      const int           n      = static_cast< int >(points.size());
      std::vector< double > xs, stock, still_valid;
      std::vector< std::string > labels;
      for (int index = 0; index < n; ++index) {
        const Json &at = points[index];
        xs.push_back(index);
        stock.push_back(at["stock"].integer() + (include_lots_ ? at["lots"].integer() : 0));
        still_valid.push_back(at["stock_static"].integer() + (include_lots_ ? at["lots_static"].integer() : 0));
        labels.push_back(index == 0 ? "auj." : short_date(at["date"]));
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      if (!ImPlot::BeginPlot("##projection", ImVec2(-1, 220), ImPlotFlags_NoMouseText))
        return;
      const double minimum = row["min_quantity"].integer();
      double       top     = minimum;
      for (int index = 0; index < n; ++index)
        top = std::max({ top, stock[index], still_valid[index] });
      ImPlot::SetupAxes(nullptr, include_lots_ ? "stock + lots" : "stock");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.2, std::max(1, n - 1) + 0.2, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
      if (n > 0)
        ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      if (n > 0) {
        ImPlot::PlotShaded("##stock_fill", xs.data(), stock.data(), n, 0.0,
                           ImPlotSpec(ImPlotProp_FillColor, stock_color, ImPlotProp_FillAlpha, 0.15f,
                                      ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
        ImPlot::PlotLine("Stock projeté", xs.data(), stock.data(), n,
                         ImPlotSpec(ImPlotProp_LineColor, stock_color, ImPlotProp_LineWeight, 2.0f, ImPlotProp_Marker,
                                    ImPlotMarker_Circle));
        ImPlot::PlotLine("Sans consommation", xs.data(), still_valid.data(), n,
                         ImPlotSpec(ImPlotProp_LineColor, lots_color, ImPlotProp_LineWeight, 1.0f));
        if (minimum > 0)
          ImPlot::PlotInfLines("Minimum", &minimum, 1,
                               ImPlotSpec(ImPlotProp_LineColor, colors::red, ImPlotProp_Flags,
                                          ImPlotInfLinesFlags_Horizontal));
        const double horizon = std::min(horizon_months[horizon_], n - 1);
        if (horizon > 0)
          ImPlot::PlotInfLines("##horizon", &horizon, 1,
                               ImPlotSpec(ImPlotProp_LineColor, colors::grey, ImPlotProp_Flags, ImPlotItemFlags_NoLegend));
      }
      ImPlot::EndPlot();
    }

    // Peremptions par mois, sommees sur les types donnes (un seul : detail du type ; tous : tout le stock)
    void draw_expiry_plot(const std::vector< const Json * > &rows, float height = 200.0f) {
      const int n = rows.empty() ? 0 : static_cast< int >((*rows.front())["calendar"].size());
      std::vector< double >      xs(n), stock(n), lots(n), sealed(n), lost(n);
      std::vector< std::string > labels;
      for (int index = 0; index < n; ++index) {
        xs[index] = index;
        labels.push_back(month_label((*rows.front())["calendar"][index]["start"]));
      }
      for (const Json *row : rows) {
        const Json &months = (*row)["calendar"];
        for (int index = 0; index < n && index < static_cast< int >(months.size()); ++index) {
          const Json &expiring = months[index]["expiring"];
          const Json &gone     = months[index]["lost"];
          stock[index] += expiring["stock"].integer();
          lots[index] += expiring["lots"].integer();
          sealed[index] += expiring["sealed"].integer();
          lost[index] += gone["stock"].integer() + gone["lots"].integer() + gone["sealed"].integer();
        }
      }
      for (int index = 0; index < n; ++index) { // cumuls pour les barres empilees
        lots[index] += stock[index];
        sealed[index] += lots[index];
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      if (!ImPlot::BeginPlot("Péremptions par mois", ImVec2(-1, 200), ImPlotFlags_NoMouseText))
        return;
      double top = 1;
      for (int index = 0; index < n; ++index)
        top = std::max(top, sealed[index]);
      ImPlot::SetupAxes(nullptr, "items");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, std::max(1, n) - 0.4, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
      if (n > 0)
        ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      if (n > 0) {
        // barres empilees : chaque serie est dessinee avec le cumul, de la plus haute a la plus basse
        ImPlot::PlotBars("Scellés", xs.data(), sealed.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, sealed_color));
        ImPlot::PlotBars("Lots", xs.data(), lots.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, lots_color));
        ImPlot::PlotBars("Stock", xs.data(), stock.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, stock_color));
        ImPlot::PlotBars("Perdus (pas utilisés à temps)", xs.data(), lost.data(), n, 0.25,
                         ImPlotSpec(ImPlotProp_FillColor, colors::red, ImPlotProp_FillAlpha, 0.85f));
      }
      ImPlot::EndPlot();
    }

    // ---- Commandes ----
    void draw_orders(App &app) {
      const Json &forecast = app.catalog.forecast;
      std::vector< const Json * > rows;
      for (const Json &row : forecast["types"].items())
        if (row["order"].is_object() && filter_matches(app, row))
          rows.push_back(&row);
      std::stable_sort(rows.begin(), rows.end(), [](const Json *a, const Json *b) {
        return (*a)["order"]["before"].str() < (*b)["order"]["before"].str();
      });
      ImGui::TextDisabled("Types qui passeront sous leur minimum dans les 6 mois. Délai de livraison : %d j "
                          "(Réglages).",
                          forecast["lead_days"].integer());
      if (!rows.empty()) {
        ImGui::SameLine();
        if (button("Copier la liste")) {
          std::ostringstream text;
          for (const Json *row : rows)
            text << (*row)["name"].str() << " : " << order_text(*row) << " (avant le "
                 << date_text((*row)["order"]["before"]) << ")\n";
          ImGui::SetClipboardText(text.str().c_str());
          app.notify("Liste des commandes copiée.");
        }
      }
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
      if (!ImGui::BeginTable("orders", 6, flags))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("À commander", ImGuiTableColumnFlags_WidthFixed, 170.0f);
      ImGui::TableSetupColumn("Avant le", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableSetupColumn("Sous le minimum le", ImGuiTableColumnFlags_WidthFixed, 140.0f);
      ImGui::TableSetupColumn("Stock / minimum", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Conso / mois", ImGuiTableColumnFlags_WidthFixed, 100.0f);
      ImGui::TableHeadersRow();
      for (const Json *row_ptr : rows) {
        const Json &row   = *row_ptr;
        const Json &order = row["order"];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(stock_name(row).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(order_text(row).c_str());
        if (row["pack_size"].integer() > 1 && !row["is_group"].boolean()) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%d paquets)", order["quantity"].integer() / row["pack_size"].integer());
        }
        ImGui::TableNextColumn();
        if (order["urgent"].boolean())
          ImGui::TextColored(colors::red, "%s", date_text(order["before"]).c_str());
        else
          ImGui::TextColored(colors::orange, "%s", date_text(order["before"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(date_text(order["below_min"]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", row["stock_now"].integer(), row["min_quantity"].integer());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(rate_text(row["per_month"].num()).c_str());
      }
      ImGui::EndTable();
      if (rows.empty())
        ImGui::TextDisabled("Rien à commander.");
    }

    // ---- Transferts : echanges entre lots pour utiliser les items avant leur peremption ----
    void draw_transfers(App &app) {
      const Json &transfers = app.catalog.forecast["transfers"];
      ImGui::TextWrapped("Items qui périmeraient sans servir là où ils sont. On les met là où ils seront utilisés "
                         "avant leur date, et on reprend au même endroit un plus récent pour garder le lot complet. "
                         "Les déplacements se font comme d'habitude (scan puis « Ajouter au lot ») ; un échange fait "
                         "disparaît de la liste.");
      if (transfers.size() == 0) {
        ImGui::TextDisabled("Aucun échange à faire.");
        return;
      }
      if (!ImGui::BeginChild("transfers"))
        return ImGui::EndChild();
      int group_index = 0;
      for (const Json &group : transfers.items()) {
        ImGui::PushID(group_index++);
        const Json &source = group["source"];
        ImGui::Separator();
        ImGui::TextUnformatted(source["path"].str().c_str());
        ImGui::SameLine();
        if (source["sealed"].boolean())
          ImGui::TextColored(colors::red, "· scellé, ouvrir avant le %s", date_text(group["before"]).c_str());
        else
          ImGui::TextColored(colors::orange, "· à faire avant le %s", date_text(group["before"]).c_str());
        if (!source["id"].is_null()) {
          ImGui::SameLine();
          if (small_button("Afficher le lot"))
            app.show_lot(source["id"].str());
        }
        int move_index = 0;
        for (const Json &move : group["moves"].items()) {
          ImGui::PushID(move_index++);
          int step_index = 1;
          for (const Json &step : move["steps"].items())
            ImGui::TextWrapped("%d. %s", step_index++, step.str().c_str());
          ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TextWrapped("%s", move["reason"].str().c_str());
          ImGui::PopStyleColor();
          if (ImGui::TreeNode("Items concernés")) {
            ImGui::TextDisabled("À sortir :");
            for (const Json &iid : move["take"].items())
              ImGui::BulletText("%s", iid.str().c_str());
            ImGui::TextDisabled("À reprendre :");
            for (const Json &iid : move["back"].items())
              ImGui::BulletText("%s", iid.str().c_str());
            ImGui::TreePop();
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    std::string filter_;
    std::string selected_;
    bool        only_low_         = false;
    bool        zero_open_        = false; // types sans minimum attendu deplies (actuel et projection)
    bool        include_lots_     = false;
    bool        with_consumption_ = true;
    int         horizon_          = 1;

};

} // namespace

std::unique_ptr< AppWindow > make_inventory_window() { // id historique : disposition enregistree
  return std::make_unique< InventoryWindow >(InventoryWindow::View::Stock, "inventory", "Stock");
}

std::unique_ptr< AppWindow > make_items_window() {
  return std::make_unique< InventoryWindow >(InventoryWindow::View::Items, "items", "Items");
}

std::unique_ptr< AppWindow > make_reception_window() {
  return std::make_unique< InventoryWindow >(InventoryWindow::View::Reception, "reception", "Réception");
}

} // namespace qrprotec

