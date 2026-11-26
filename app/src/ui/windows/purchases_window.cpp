/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          purchases_window.cpp
        -\-    _|__
         |\___/  . \        Created on 26 Nov. 2026 at 18:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../../core/plural.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "implot.h"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

namespace qrprotec {

namespace {

// Commandes fournisseurs (poste seulement, roles gestion et admin ; calcul dans database/inventory/purchases.py) :
// - Commandes : toutes les commandes, ce qui merite l'attention d'abord (en retard, reçues en partie, en attente) ;
// - Nouvelle commande : fournisseur (noms deja enregistres suggeres), numero, date, frais de livraison, lignes ;
// - Suggestions : commandes recommandees groupees par fournisseur pour economiser la livraison ;
// - Statistiques : depenses, fournisseurs et delais, prix par type, valeur du stock, estimations.
// La reception d'une commande est dans la fenetre Reception (OrderReception, plus bas).

const ImVec4 items_color(0.11f, 0.31f, 0.57f, 1.0f);
const ImVec4 shipping_color(0.93f, 0.55f, 0.10f, 1.0f);

std::string with_comma(std::string text) {
  for (char &c : text)
    if (c == '.')
      c = ',';
  return text;
}

std::string thousands(const std::string &number) {
  // « 1234,50 » -> « 1 234,50 »
  const std::size_t comma  = number.find(',');
  std::string       digits = number.substr(0, comma);
  const std::string rest   = comma == std::string::npos ? std::string() : number.substr(comma);
  const bool        minus  = !digits.empty() && digits[0] == '-';
  if (minus)
    digits.erase(0, 1);
  for (int i = static_cast< int >(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast< std::size_t >(i), " ");
  return (minus ? "-" : "") + digits + rest;
}

std::string euros(double value) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.2f", value);
  return thousands(with_comma(text)) + " €";
}

std::string euros(const Json &value) {
  return value.is_null() ? std::string("–") : euros(value.num());
}

// prix unitaire : 2 a 4 decimales (0,12 € ; 0,1234 €)
std::string unit_price_text(double value, bool unit = true) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.4f", value);
  std::string result = text;
  while (result.size() > 1 && result.back() == '0' && result.find('.') + 3 < result.size())
    result.pop_back();
  return thousands(with_comma(result)) + (unit ? " €" : "");
}

std::string unit_price_text(const Json &value) {
  return value.is_null() ? std::string("–") : unit_price_text(value.num());
}

// montant tape (« 12,50 », « 12.5 », « 1 200 ») : vide -> nullopt, pas un nombre -> nullopt et *bad
std::optional< double > parse_amount(const std::string &text, bool *bad = nullptr) {
  std::string clean;
  for (char c : text)
    if (c == ',')
      clean += '.';
    else if (c != ' ' && c != '\t')
      clean += c;
  if (bad)
    *bad = false;
  if (clean.empty())
    return std::nullopt;
  char        *end   = nullptr;
  const double value = std::strtod(clean.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(value) || value < 0) {
    if (bad)
      *bad = true;
    return std::nullopt;
  }
  return value;
}

std::string amount_input(double value, int decimals = 2) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.*f", decimals, value);
  std::string result = text;
  if (decimals > 2)
    while (result.back() == '0' && result.find('.') + 3 < result.size())
      result.pop_back();
  return with_comma(result);
}

std::string date_text(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : std::string("–");
}

std::string month_label(const std::string &key) {
  static const char *const kShort[12] = { "janv.", "févr.", "mars", "avr.", "mai", "juin",
                                          "juil.", "août", "sept.", "oct.", "nov.", "déc." };
  if (key.size() < 7)
    return key;
  const int month = std::atoi(key.substr(5, 2).c_str());
  return month >= 1 && month <= 12 ? std::string(kShort[month - 1]) + " " + key.substr(2, 2) : key;
}

ImVec4 state_color(const std::string &state) {
  if (state == "late")
    return colors::red;
  if (state == "partial")
    return colors::orange;
  if (state == "received")
    return colors::green;
  if (state == "waiting")
    return colors::blue;
  return colors::grey;
}

StateKind state_badge_kind(const std::string &state) {
  return state == "late" ? StateKind::Bad : state == "partial" ? StateKind::Warn
                                          : state == "received" ? StateKind::Ok
                                                                : StateKind::None;
}

void grey_wrapped(const std::string &text) {
  if (text.empty())
    return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

// types que l'on peut commander et receptionner : actifs, avec une etiquette, pas remplis par des sous-types
Json orderable_types(const App &app) {
  Json       result = Json::array();
  const Json types  = without_archived(app.catalog.item_types);
  for (const Json &item_type : types.items())
    if (!item_type["is_group"].boolean() && item_type["physical"].boolean(true))
      result.push_back(item_type);
  return result;
}

std::string type_name(const App &app, const std::string &code) {
  for (const Json &item_type : app.catalog.item_types.items())
    if (item_type["type"].str() == code)
      return item_type["name"].str();
  return code;
}

// ---- Formulaire d'une commande ----------------------------------------------------------------------------------

struct LineForm {
    int         id = 0; // 0 : nouvelle ligne
    int         received = 0;
    std::string type;
    int         quantity = 1;
    std::string unit_price; // texte tape (virgule acceptee), vide : prix inconnu
    std::string total;
};

struct OrderForm {
    int                     id = 0; // 0 : nouvelle commande
    std::string             supplier;
    std::string             number;
    std::string             ordered; // JJ/MM/AAAA tape librement
    std::string             shipping;
    std::string             note;
    std::vector< LineForm > lines;
};

class PurchasesWindow final : public AppWindow {
  public:
    PurchasesWindow() : AppWindow("purchases", "Commandes fournisseurs", true, true) {}

    void on_open(App &app) override {
      seen_version_ = app.catalog.purchases_version;
      load_orders(app);
      load_suppliers(app);
      load_suggestions(app);
      load_stats(app);
    }

    void reset_view(App &) override {
      selected_ = 0;
      filter_.clear();
      form_ = OrderForm{};
      form_open_ = false;
      price_type_.clear();
    }

    void draw(App &app) override {
      if (seen_version_ != app.catalog.purchases_version) {
        seen_version_ = app.catalog.purchases_version; // commande recue (fenetre Reception)
        load_orders(app);
        load_stats(app);
        load_suggestions(app);
      }
      std::string tab = std::exchange(app.stock_tab_to_show_, std::string());
      if (tab.rfind("purchase", 0) != 0) {
        app.stock_tab_to_show_ = tab; // pour une autre fenetre
        tab.clear();
      }
      if (tab == "purchase_new")
        start_new(app);
      if (std::exchange(select_form_, false))
        tab = "purchase_new";
      if (std::exchange(select_orders_, false))
        tab = "purchases";
      const auto selected = [&tab](const char *key) { return tab == key ? ImGuiTabItemFlags_SetSelected : 0; };
      if (!ImGui::BeginTabBar("purchases_tabs"))
        return;
      int open = 0;
      for (const Json &order : orders_.items())
        open += order["open"].boolean() ? 1 : 0;
      const std::string orders_tab = "Commandes" + (open ? " (" + std::to_string(open) + " en cours)" : std::string())
                                   + "###orders";
      if (tab_item(orders_tab.c_str(), selected("purchases"))) {
        draw_orders(app);
        ImGui::EndTabItem();
      }
      const std::string form_tab = form_.id ? "Modifier la commande###form" : "Nouvelle commande###form";
      if (tab_item(form_tab.c_str(), selected("purchase_new"))) {
        if (!form_open_)
          start_new(app);
        draw_form(app);
        ImGui::EndTabItem();
      }
      const std::string suggestions_tab = "Suggestions (" + std::to_string(suggestions_["groups"].size()) + ")###suggestions";
      if (tab_item(suggestions_tab.c_str(), selected("purchase_suggestions"))) {
        draw_suggestions(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Statistiques", selected("purchase_stats"))) {
        draw_stats(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    // ---------------------------------------------------------------------------------------------------------------
    // Chargements
    void load_orders(App &app) {
      loading_orders_ = true;
      app.api.get("/api/purchases/orders/", [this](const ApiResult &result) {
        loading_orders_ = false;
        if (result.ok)
          orders_ = result.data;
        orders_error_ = result.ok ? std::string() : result.error;
      });
    }

    void load_suppliers(App &app) {
      app.api.get("/api/purchases/suppliers/", [this](const ApiResult &result) {
        if (result.ok)
          suppliers_ = result.data;
      });
    }

    void load_suggestions(App &app) {
      loading_suggestions_ = true;
      app.api.get("/api/purchases/suggestions/", [this](const ApiResult &result) {
        loading_suggestions_ = false;
        if (!result.ok) {
          suggestions_error_ = result.error;
          return;
        }
        suggestions_error_.clear();
        suggestions_ = result.data;
        unchecked_.clear(); // tout coche par defaut, commandes a grouper comprises
      });
    }

    void load_stats(App &app) {
      loading_stats_ = true;
      app.api.get("/api/purchases/stats/", [this](const ApiResult &result) {
        loading_stats_ = false;
        if (result.ok)
          stats_ = result.data;
        stats_error_ = result.ok ? std::string() : result.error;
      });
    }

    void reload_all(App &app) {
      on_open(app);
      app.refresh_tasks(); // suggestions de commande du TECH : un type commande n'y revient pas
    }

    const Json *find_order(int id) const {
      for (const Json &order : orders_.items())
        if (order["id"].integer() == id)
          return &order;
      return nullptr;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Commandes
    void draw_orders(App &app) {
      if (primary_button("Nouvelle commande")) {
        start_new(app);
        select_form_ = true;
      }
      ImGui::SameLine();
      if (button("Rafraîchir"))
        reload_all(app);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x * 0.45f));
      input_limited("##orders_filter", filter_, 64, "Rechercher (fournisseur, numéro, type d'item)…");
      if (loading_orders_) {
        ImGui::SameLine();
        ImGui::TextDisabled("chargement…");
      }
      if (!orders_error_.empty())
        ImGui::TextColored(colors::red, "%s", orders_error_.c_str());
      if (orders_.size() == 0) {
        grey_wrapped("Aucune commande enregistrée. Saisissez chaque commande au moment où elle est passée : articles, "
                     "quantités et prix, frais de livraison et fournisseur. À la livraison, elle est proposée sur la "
                     "page Réception pour imprimer toutes les étiquettes d'un coup.");
        return;
      }
      std::vector< const Json * > shown;
      for (const Json &order : orders_.items())
        if (matches(order))
          shown.push_back(&order);
      const float detail_height = selected_ && find_order(selected_) ? ImGui::GetContentRegionAvail().y * 0.5f : 0.0f;
      if (ImGui::BeginTable("orders", 5,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, detail_height > 0 ? ImGui::GetContentRegionAvail().y - detail_height : 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.5f);
        ImGui::TableSetupColumn("Commande", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Reçu", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
        ImGui::TableSetupColumn("Livraison", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        for (const Json *row : shown) {
          const Json &order = *row;
          const int   id    = order["id"].integer();
          const std::string state = order["state"].str();
          ImGui::PushID(id);
          ImGui::TableNextRow();
          if (state == "cancelled")
            row_color(colors::grey, 0.15f);
          ImGui::TableNextColumn();
          state_badge(state_badge_kind(state));
          ImGui::SameLine();
          ImGui::TextColored(state_color(state), "%s", order["state_text"].str().c_str());
          ImGui::TableNextColumn();
          if (ImGui::Selectable(order["label"].str().c_str(), selected_ == id,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            selected_ = selected_ == id ? 0 : id;
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", lines_summary(order).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%d / %d", order["received"].integer(), order["quantity"].integer());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(order["priced"].boolean() ? euros(order["total"]).c_str() : "?");
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", order["short"].str(order["detail"].str()).c_str());
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", order["detail"].str().c_str());
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (shown.empty())
        ImGui::TextDisabled("Aucune commande trouvée.");
      if (const Json *order = find_order(selected_)) {
        ImGui::BeginChild("order_detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
        draw_order_detail(app, *order);
        ImGui::EndChild();
      }
    }

    bool matches(const Json &order) const {
      if (filter_.empty())
        return true;
      if (search_matches(filter_, order["supplier"].str()) || search_matches(filter_, order["number"].str())
          || search_matches(filter_, order["label"].str()))
        return true;
      for (const Json &line : order["lines"].items())
        if (search_matches(filter_, line["name"].str()) || search_matches(filter_, line["type"].str()))
          return true;
      return false;
    }

    static std::string lines_summary(const Json &order) {
      std::string text;
      for (const Json &line : order["lines"].items())
        text += (text.empty() ? "" : "\n") + std::to_string(line["quantity"].integer()) + " × " + line["name"].str();
      return text;
    }

    void draw_order_detail(App &app, const Json &order) {
      const std::string state = order["state"].str();
      const int         id    = order["id"].integer();
      ImGui::TextUnformatted(order["label"].str().c_str());
      ImGui::SameLine();
      ImGui::TextColored(state_color(state), "· %s", order["state_text"].str().c_str());
      grey_wrapped(order["detail"].str());
      if (order["open"].boolean()) {
        ImGui::TextDisabled("Délai estimé : %s.", order["delay_text"].str().c_str());
        help_marker("Jours ouvrés : sans les week-ends ni les jours fériés français. Le délai d'un fournisseur est "
                    "le délai médian de ses dernières commandes, de la date de commande à la première réception.");
      }
      if (!order["note"].str().empty())
        ImGui::TextWrapped("Note : %s", order["note"].str().c_str());
      ImGui::TextDisabled("Saisie par %s.", order["created_by"].str().empty() ? "?" : order["created_by"].str().c_str());
      if (ImGui::BeginTable("lines", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Reçu", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
        ImGui::TableSetupColumn("Prix unitaire", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
        ImGui::TableHeadersRow();
        for (const Json &line : order["lines"].items()) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(line["name"].str().c_str());
          if (line["cancelled"].boolean() && line["received"].integer() < line["quantity"].integer()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(reste pas livré)");
          }
          ImGui::TableNextColumn();
          ImGui::Text("%d", line["quantity"].integer());
          ImGui::TableNextColumn();
          const int  received = line["received"].integer();
          const bool full     = received >= line["quantity"].integer();
          ImGui::TextColored(full ? colors::green : received ? colors::orange : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                             "%d", received);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(unit_price_text(line["unit_price"]).c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(euros(line["total"]).c_str());
        }
        ImGui::EndTable();
      }
      ImGui::Text("Articles : %s · Livraison : %s · Total : %s", euros(order["items_total"]).c_str(),
                  euros(order["shipping"]).c_str(), euros(order["total"]).c_str());
      if (!order["priced"].boolean()) {
        ImGui::SameLine();
        ImGui::TextColored(colors::orange, "(prix manquants)");
      }
      ImGui::Spacing();
      if (order["open"].boolean()) {
        if (primary_button("Réceptionner")) {
          app.receive_purchase(id);
        }
        ImGui::SameLine();
      }
      if (state != "cancelled" && button("Modifier")) {
        edit(app, order);
        select_form_ = true;
      }
      if (order["open"].boolean()) {
        ImGui::SameLine();
        if (order["received"].integer() > 0) {
          if (confirm_button("Clore la commande", "Le reste ne viendra pas : la commande n'attend plus de livraison.",
                             "close_order"))
            send(app, "/api/purchases/orders/" + std::to_string(id) + "/close/", Json::object(), "Commande close.");
        } else if (confirm_button("Annuler la commande", "Annuler cette commande (jamais passée, ou saisie par erreur) ?",
                                  "cancel_order")) {
          send(app, "/api/purchases/orders/" + std::to_string(id) + "/cancel/", Json::object(), "Commande annulée.");
        }
      } else if (state == "closed") {
        ImGui::SameLine();
        Json body;
        body["reopen"] = true;
        if (button("Rouvrir (attendre le reste)"))
          send(app, "/api/purchases/orders/" + std::to_string(id) + "/close/", body, "Commande rouverte.");
      }
    }

    void send(App &app, const std::string &path, Json body, const std::string &done) {
      body["user"] = app.user_ref();
      app.api.post(path, body, [this, &app, done](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Commande : " + result.error, true);
          return;
        }
        app.notify(done);
        reload_all(app);
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Saisie d'une commande
    void start_new(App &app) {
      form_          = OrderForm{};
      form_.ordered  = app.today().display();
      form_.lines.push_back(LineForm{});
      form_open_     = true;
      form_error_.clear();
    }

    void edit(App &, const Json &order) {
      form_          = OrderForm{};
      form_.id       = order["id"].integer();
      form_.supplier = order["supplier"].str();
      form_.number   = order["number"].str();
      form_.ordered  = date_text(order["ordered"]);
      form_.shipping = order["shipping"].num() > 0 ? amount_input(order["shipping"].num()) : std::string();
      form_.note     = order["note"].str();
      for (const Json &line : order["lines"].items()) {
        LineForm row;
        row.id       = line["id"].integer();
        row.received = line["received"].integer();
        row.type     = line["type"].str();
        row.quantity = line["quantity"].integer();
        if (!line["unit_price"].is_null()) {
          row.unit_price = amount_input(line["unit_price"].num(), 4);
          row.total      = amount_input(line["unit_price"].num() * row.quantity);
        }
        form_.lines.push_back(row);
      }
      form_open_ = true;
      form_error_.clear();
    }

    // dernier et meilleur prix connus d'un type (statistiques)
    const Json *known_prices(const std::string &type) const {
      for (const Json &row : stats_["types"].items())
        if (row["type"].str() == type)
          return &row;
      return nullptr;
    }

    void draw_form(App &app) {
      const float label_width = ImGui::GetFontSize() * 11.0f;
      const auto  label       = [label_width](const char *text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_width);
      };
      if (form_.id)
        ImGui::TextColored(colors::blue, "Modification de la commande n° %s", form_.number.empty() ? "?" : form_.number.c_str());
      grey_wrapped("Saisissez la commande au moment où elle est passée. Elle est identifiée par sa date, son "
                   "fournisseur et son numéro, et sera proposée sur la page Réception à la livraison.");

      label("Fournisseur");
      std::vector< std::string > names;
      for (const Json &supplier : suppliers_.items())
        names.push_back(supplier["name"].str());
      input_suggest("supplier", form_.supplier, names, 80, "Nom du fournisseur",
                    ImGui::GetFontSize() * 22.0f);
      if (!form_.supplier.empty()) {
        // SameLine seulement si quelque chose suit (sinon le champ suivant viendrait sur cette ligne)
        const std::string existing = name_taken(suppliers_, "id", form_.supplier, "");
        if (existing.empty()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::blue, "nouveau fournisseur");
        } else if (existing != form_.supplier) {
          ImGui::SameLine();
          ImGui::TextDisabled("= %s", existing.c_str());
        }
      }
      label("Numéro de commande");
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
      input_limited("##number", form_.number, 64, "ex : CMD-2026-1187");
      label("Date de commande");
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
      ImGui::InputTextWithHint("##ordered", "JJ/MM/AAAA", &form_.ordered);
      const auto ordered = parse_user_date(form_.ordered);
      if (ImGui::IsItemDeactivatedAfterEdit() && ordered)
        form_.ordered = ordered->display();
      ImGui::SameLine();
      if (ordered)
        ImGui::TextDisabled("%s %s", ordered->weekday().c_str(), ordered->display().c_str());
      else
        ImGui::TextColored(colors::red, "date non reconnue");
      label("Frais de livraison (€)");
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
      input_limited("##shipping", form_.shipping, 12, "0,00");
      bool       bad_shipping = false;
      const auto shipping     = parse_amount(form_.shipping, &bad_shipping);
      if (bad_shipping) {
        ImGui::SameLine();
        ImGui::TextColored(colors::red, "montant non reconnu");
      }
      label("Note");
      ImGui::SetNextItemWidth(-FLT_MIN);
      input_limited("##note", form_.note, 200, "facultatif (ex : promo -10 %, livraison en 2 colis)");

      ImGui::SeparatorText("Articles");
      const Json types      = orderable_types(app);
      double     items_total = 0.0;
      bool       missing_price = false, bad_price = false;
      int        remove = -1;
      if (ImGui::BeginTable("form_lines", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité (items)", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
        ImGui::TableSetupColumn("Prix unitaire (€)", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
        ImGui::TableSetupColumn("Total ligne (€)", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < form_.lines.size(); ++index) {
          LineForm &line = form_.lines[index];
          ImGui::PushID(static_cast< int >(index));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::BeginDisabled(line.received > 0); // deja recu : le type ne change plus
          if (search_select("type", types, "type", "name", line.type, "Tapez le nom ou le code du type…")
              && line.quantity <= 1) {
            for (const Json &item_type : types.items())
              if (item_type["type"].str() == line.type)
                line.quantity = std::max(1, item_type["default_pack_size"].integer(1));
          }
          ImGui::EndDisabled();
          if (const Json *known = line.type.empty() ? nullptr : known_prices(line.type)) {
            std::string hint = "dernier prix " + unit_price_text((*known)["last_price"]) + " (" + (*known)["last_supplier"].str() + ")";
            if ((*known)["best_supplier"].str() != (*known)["last_supplier"].str())
              hint += " · meilleur " + unit_price_text((*known)["best_price"]) + " (" + (*known)["best_supplier"].str() + ")";
            grey_wrapped(hint);
          }
          if (line.received > 0)
            ImGui::TextDisabled("%s déjà", nb(line.received, "reçu", "reçus").c_str());
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-FLT_MIN);
          const int before = line.quantity;
          ImGui::InputInt("##quantity", &line.quantity);
          line.quantity = std::clamp(line.quantity, std::max(1, line.received), 100000);
          bool       bad  = false;
          const auto unit = parse_amount(line.unit_price, &bad);
          if (line.quantity != before && unit)
            line.total = amount_input(*unit * line.quantity);
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (input_limited("##unit", line.unit_price, 12, "0,00")) {
            if (const auto typed = parse_amount(line.unit_price))
              line.total = amount_input(*typed * line.quantity);
            else
              line.total.clear();
          }
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-FLT_MIN);
          if (input_limited("##total", line.total, 12, "0,00")) {
            // prix de la ligne tape : prix unitaire deduit (facture qui donne le total)
            if (const auto typed = parse_amount(line.total))
              line.unit_price = amount_input(*typed / std::max(1, line.quantity), 4);
            else
              line.unit_price.clear();
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Le prix unitaire est calculé à partir du total de la ligne.");
          ImGui::TableNextColumn();
          ImGui::BeginDisabled(line.received > 0 || form_.lines.size() == 1);
          if (small_button("✕"))
            remove = static_cast< int >(index);
          ImGui::EndDisabled();
          const auto parsed = parse_amount(line.unit_price, &bad);
          bad_price |= bad;
          if (parsed)
            items_total += *parsed * line.quantity;
          else if (!line.type.empty())
            missing_price = true;
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (remove >= 0)
        form_.lines.erase(form_.lines.begin() + remove);
      if (button("+ Ajouter une ligne"))
        form_.lines.push_back(LineForm{});
      const double total = items_total + shipping.value_or(0.0);
      ImGui::Text("Articles : %s · Livraison : %s · Total : %s", euros(items_total).c_str(),
                  euros(shipping.value_or(0.0)).c_str(), euros(total).c_str());

      // controles avant envoi (le serveur verifie aussi)
      std::string problem;
      std::set< std::string > seen;
      bool duplicate = false;
      for (const LineForm &line : form_.lines)
        duplicate |= !line.type.empty() && !seen.insert(line.type).second;
      if (form_.supplier.empty())
        problem = "Nom du fournisseur obligatoire.";
      else if (!ordered)
        problem = "Date de commande non reconnue.";
      else if (std::any_of(form_.lines.begin(), form_.lines.end(), [](const LineForm &line) { return line.type.empty(); }))
        problem = "Choisissez le type d'item de chaque ligne (ou retirez la ligne vide).";
      else if (bad_price || bad_shipping)
        problem = "Un montant n'est pas un nombre.";
      if (!problem.empty())
        ImGui::TextColored(colors::orange, "%s", problem.c_str());
      else if (missing_price)
        ImGui::TextColored(colors::orange, "Des lignes n'ont pas de prix : elles ne compteront pas dans les statistiques.");
      if (duplicate)
        ImGui::TextColored(colors::orange, "Le même type apparaît sur plusieurs lignes (possible, mais vérifiez).");
      if (!form_error_.empty())
        ImGui::TextColored(colors::red, "%s", form_error_.c_str());
      ImGui::BeginDisabled(!problem.empty() || saving_);
      const char *save = form_.id ? "Enregistrer les modifications" : "Enregistrer la commande";
      if (primary_button(save, ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, ImGui::GetFrameHeight() * 1.5f)))
        save_form(app, *ordered);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (button(form_.id ? "Annuler la modification" : "Vider", ImVec2(-1, ImGui::GetFrameHeight() * 1.5f)))
        start_new(app);
    }

    void save_form(App &app, const Date &ordered) {
      Json body;
      body["user"]     = app.user_ref();
      body["supplier"] = form_.supplier;
      body["number"]   = form_.number;
      body["ordered"]  = ordered.iso();
      body["shipping"] = form_.shipping;
      body["note"]     = form_.note;
      body["lines"]    = Json::array();
      for (const LineForm &line : form_.lines) {
        Json row;
        if (line.id)
          row["id"] = line.id;
        row["type"]       = line.type;
        row["quantity"]   = line.quantity;
        row["unit_price"] = line.unit_price;
        body["lines"].push_back(row);
      }
      const std::string path = form_.id ? "/api/purchases/orders/" + std::to_string(form_.id) + "/" : "/api/purchases/orders/";
      saving_ = true;
      form_error_.clear();
      app.api.post(path, body, [this, &app](const ApiResult &result) {
        saving_ = false;
        if (!result.ok) {
          form_error_ = result.error;
          return;
        }
        app.notify(form_.id ? "Commande modifiée." : "Commande enregistrée : " + result.data["label"].str());
        selected_ = result.data["id"].integer();
        start_new(app);
        select_orders_ = true;
        reload_all(app);
      });
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Suggestions groupees par fournisseur
    void draw_suggestions(App &app) {
      grey_wrapped("Commandes recommandées par les prévisions à passer dans les "
                   + std::to_string(suggestions_["window_days"].integer(30))
                   + " jours, chacune chez le fournisseur le moins cher connu, groupées par fournisseur. Les "
                     "commandes du même fournisseur à passer d'ici "
                   + std::to_string(suggestions_["horizon_days"].integer(90))
                   + " jours sont ajoutées pour ne payer qu'une livraison : décochez ce qui peut attendre.");
      if (button("Rafraîchir"))
        load_suggestions(app);
      if (loading_suggestions_) {
        ImGui::SameLine();
        ImGui::TextDisabled("calcul des prévisions…");
      }
      if (!suggestions_error_.empty())
        ImGui::TextColored(colors::red, "%s", suggestions_error_.c_str());
      if (suggestions_["saved"].num() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(colors::green, "En groupant : ≈ %s de livraison économisés.", euros(suggestions_["saved"]).c_str());
      }
      if (suggestions_["groups"].size() == 0 && !loading_suggestions_) {
        ImGui::TextDisabled("Rien à commander dans les %d jours (les commandes en attente de livraison sont comptées).",
                            suggestions_["window_days"].integer(30));
        return;
      }
      ImGui::BeginChild("suggestion_groups", ImVec2(0, 0));
      int group_index = 0;
      for (const Json &group : suggestions_["groups"].items()) {
        ImGui::PushID(group_index++);
        const bool  known    = !group["supplier_id"].is_null();
        std::string supplier = known ? group["supplier"].str() : std::string("Fournisseur à choisir");
        ImGui::Separator();
        state_badge(group["urgent"].boolean() ? StateKind::Bad : StateKind::Warn);
        ImGui::SameLine();
        ImGui::TextUnformatted(supplier.c_str());
        ImGui::SameLine();
        ImGui::TextColored(group["urgent"].boolean() ? colors::red : colors::orange, "· %s",
                           group["urgent"].boolean() ? "dès que possible"
                                                     : ("avant le " + date_text(group["before"])).c_str());
        double items_total = 0.0;
        bool   unpriced    = false;
        int    checked     = 0;
        int    line_index  = 0;
        for (const Json &line : group["lines"].items()) {
          ImGui::PushID(line_index++);
          const std::string key = check_key(group, line);
          bool              on  = !unchecked_.count(key);
          if (ImGui::Checkbox("##on", &on)) {
            if (on)
              unchecked_.erase(key);
            else
              unchecked_.insert(key);
          }
          ImGui::SameLine();
          ImGui::Text("%d × %s", line["quantity"].integer(), line["name"].str().c_str());
          ImGui::SameLine();
          if (line["due"].boolean())
            ImGui::TextColored(line["urgent"].boolean() ? colors::red : colors::orange, "%s", line["when"].str().c_str());
          else
            ImGui::TextColored(colors::blue, "à grouper (%s)", line["when"].str().c_str());
          if (!line["total"].is_null()) {
            ImGui::SameLine();
            ImGui::TextDisabled("· %s × %d = %s", unit_price_text(line["unit_price"]).c_str(), line["quantity"].integer(),
                                euros(line["total"]).c_str());
          }
          ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
          grey_wrapped(line["why"].str());
          ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
          if (on) {
            ++checked;
            if (line["total"].is_null())
              unpriced = true;
            else
              items_total += line["total"].num();
          }
          ImGui::PopID();
        }
        const double shipping = group["shipping"].num();
        std::string  totals   = "Estimation : " + euros(items_total) + " d'articles";
        if (!group["shipping"].is_null() && shipping > 0)
          totals += " + " + euros(shipping) + " de livraison (habituelle) = " + euros(items_total + shipping);
        else if (!group["shipping"].is_null())
          totals += " (livraison habituellement gratuite)";
        if (unpriced)
          totals += " (+ lignes sans prix connu)";
        ImGui::TextUnformatted(totals.c_str());
        if (!group["advice"].str().empty())
          ImGui::TextColored(group["saved"].num() > 0 ? colors::green : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                             "%s", group["advice"].str().c_str());
        ImGui::BeginDisabled(checked == 0);
        if (primary_button(("Préparer cette commande (" + nb(checked, "ligne") + ")").c_str())) {
          prepare(app, group);
          select_form_ = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    static std::string check_key(const Json &group, const Json &line) {
      return (group["supplier_id"].is_null() ? std::string("?") : std::to_string(group["supplier_id"].integer())) + ":"
           + line["type"].str();
    }

    void prepare(App &app, const Json &group) {
      start_new(app);
      form_.lines.clear();
      if (!group["supplier_id"].is_null())
        form_.supplier = group["supplier"].str();
      if (!group["shipping"].is_null() && group["shipping"].num() > 0)
        form_.shipping = amount_input(group["shipping"].num());
      for (const Json &line : group["lines"].items()) {
        if (unchecked_.count(check_key(group, line)))
          continue;
        LineForm row;
        row.type     = line["type"].str();
        row.quantity = line["quantity"].integer(1);
        if (!line["unit_price"].is_null()) {
          row.unit_price = amount_input(line["unit_price"].num(), 4);
          row.total      = amount_input(line["unit_price"].num() * row.quantity);
        }
        form_.lines.push_back(row);
      }
      if (form_.lines.empty())
        form_.lines.push_back(LineForm{});
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Statistiques
    void draw_stats(App &app) {
      if (button("Rafraîchir"))
        load_stats(app);
      if (loading_stats_) {
        ImGui::SameLine();
        ImGui::TextDisabled("calcul…");
      }
      if (!stats_error_.empty())
        ImGui::TextColored(colors::red, "%s", stats_error_.c_str());
      if (!stats_.is_object() || !stats_.contains("year"))
        return;
      ImGui::BeginChild("stats", ImVec2(0, 0));
      const Json &year = stats_["year"];
      ImGui::SeparatorText("Sur les 12 derniers mois");
      if (ImGui::BeginTable("kpis", 5, ImGuiTableFlags_SizingStretchSame)) {
        const auto kpi = [](const char *title, const std::string &value, const std::string &sub = std::string()) {
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", title);
          ImGui::SetWindowFontScale(1.4f);
          ImGui::TextUnformatted(value.c_str());
          ImGui::SetWindowFontScale(1.0f);
          if (!sub.empty())
            ImGui::TextDisabled("%s", sub.c_str());
        };
        kpi("Dépensé", euros(year["spent"]), "≈ " + euros(year["monthly_average"]) + " / mois");
        char share[32] = "";
        if (!year["shipping_share"].is_null())
          std::snprintf(share, sizeof(share), "%.1f %% des dépenses", year["shipping_share"].num());
        kpi("dont livraison", euros(year["shipping"]), with_comma(share));
        kpi("Commandes", std::to_string(year["orders"].integer()), nb(year["items_bought"].integer(), "item acheté", "items achetés"));
        kpi("Commande moyenne", euros(year["average_order"]));
        const Json &delay = stats_["delay"];
        kpi("Délai de livraison", delay["median"].is_null() ? std::string("–") : nb(delay["median"].integer(), "jour ouvré", "jours ouvrés"),
            delay["median"].is_null() ? "pas encore de livraison (" + std::to_string(delay["default"].integer()) + " par défaut)"
                                      : "médian, sur " + nb(delay["samples"].integer(), "commande"));
        ImGui::EndTable();
      }
      draw_months_plot();

      const Json &estimates = stats_["estimates"];
      ImGui::SeparatorText("Estimations");
      bool any = false;
      const auto bullet = [](const std::string &text) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", text.c_str());
      };
      for (const char *key : { "next_text", "yearly_text", "savings_text" })
        if (!estimates[key].str().empty()) {
          bullet(estimates[key].str());
          any = true;
        }
      if (estimates["grouping_saved"].num() > 0) {
        bullet("En groupant les commandes suggérées par fournisseur : ≈ " + euros(estimates["grouping_saved"])
               + " de livraison économisés.");
        any = true;
      }
      if (!any)
        ImGui::TextDisabled("Pas encore assez de commandes avec des prix pour estimer.");

      draw_stock_stats();
      draw_suppliers_stats();
      draw_types_stats();

      if (stats_["holidays"].size() > 0 && ImGui::TreeNode("Prochains jours fériés (comptés dans les délais)")) {
        for (const Json &day : stats_["holidays"].items()) {
          const auto date = Date::parse(day["date"].str());
          ImGui::BulletText("%s %s : %s", date ? date->weekday().c_str() : "", date_text(day["date"]).c_str(),
                            day["name"].str().c_str());
        }
        ImGui::TreePop();
      }
      ImGui::EndChild();
    }

    void draw_months_plot() {
      const Json &months = stats_["months"];
      const int   n      = static_cast< int >(months.size());
      if (n == 0)
        return;
      std::vector< double >      xs, items, totals;
      std::vector< std::string > labels;
      double                     top = 0.0;
      for (int index = 0; index < n; ++index) {
        const Json &month = months[index];
        xs.push_back(index);
        items.push_back(month["items"].num());
        totals.push_back(month["items"].num() + month["shipping"].num());
        labels.push_back(month_label(month["month"].str()));
        top = std::max(top, totals.back());
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      if (!ImPlot::BeginPlot("Dépenses par mois (date de commande)", ImVec2(-1, 220), ImPlotFlags_NoMouseText))
        return;
      ImPlot::SetupAxes(nullptr, "€");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.6, n - 0.4, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.15 + 1, ImPlotCond_Always);
      ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::SetupLegend(ImPlotLocation_North, ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);
      // livraison empilee au-dessus des articles : barre du total, puis articles par-dessus
      ImPlot::PlotBars("Livraison", xs.data(), totals.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, shipping_color));
      ImPlot::PlotBars("Articles", xs.data(), items.data(), n, 0.6, ImPlotSpec(ImPlotProp_FillColor, items_color));
      if (ImPlot::IsPlotHovered()) {
        const int index = static_cast< int >(std::lround(ImPlot::GetPlotMousePos().x));
        if (index >= 0 && index < n) {
          const Json &month = months[index];
          ImGui::SetTooltip("%s : %s (articles %s, livraison %s), %s", labels[index].c_str(),
                            euros(totals[index]).c_str(), euros(month["items"]).c_str(), euros(month["shipping"]).c_str(),
                            nb(month["orders"].integer(), "commande").c_str());
        }
      }
      ImPlot::EndPlot();
    }

    void draw_stock_stats() {
      const Json &stock = stats_["stock"];
      ImGui::SeparatorText("Valeur du stock");
      ImGui::Text("Matériel présent : %s (stock %s, lots %s)", euros(stock["value"]).c_str(),
                  euros(stock["value_stock"]).c_str(), euros(stock["value_lots"]).c_str());
      grey_wrapped(stock["text"].str());
      ImGui::Text("Utilisé sur l'année : %s (%s)", euros(stock["used_year"]).c_str(),
                  nb(stock["used_count"].integer(), "item").c_str());
      if (!stock["waste_text"].str().empty())
        ImGui::TextColored(colors::orange, "%s", stock["waste_text"].str().c_str());
      if (stock["top"].size() > 0 && ImGui::TreeNode("Types qui valent le plus en stock")) {
        for (const Json &row : stock["top"].items())
          ImGui::BulletText("%s : %s", row["name"].str().c_str(), euros(row["value"]).c_str());
        ImGui::TreePop();
      }
    }

    void draw_suppliers_stats() {
      const Json &suppliers = stats_["suppliers"];
      ImGui::SeparatorText("Fournisseurs");
      if (suppliers.size() == 0) {
        ImGui::TextDisabled("Aucune commande.");
        return;
      }
      if (!ImGui::BeginTable("suppliers", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        return;
      ImGui::TableSetupColumn("Fournisseur", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Commandes", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5f);
      ImGui::TableSetupColumn("12 mois", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
      ImGui::TableSetupColumn("Livraison moy.", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.5f);
      ImGui::TableSetupColumn("Délai médian", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.5f);
      ImGui::TableSetupColumn("Types", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
      ImGui::TableSetupColumn("Dernière", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6.0f);
      ImGui::TableHeadersRow();
      for (const Json &row : suppliers.items()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row["name"].str().c_str());
        if (row["open"].integer() > 0 && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s en cours", nb(row["open"].integer(), "commande").c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%d", row["orders"].integer());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(euros(row["spent_year"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(euros(row["shipping_avg"]).c_str());
        if (ImGui::IsItemHovered() && row["free_shipping"].integer() > 0)
          ImGui::SetTooltip("%s sans frais de livraison", nb(row["free_shipping"].integer(), "commande").c_str());
        ImGui::TableNextColumn();
        if (row["delay_median"].is_null())
          ImGui::TextDisabled("pas encore livré");
        else {
          ImGui::Text("%s", nb(row["delay_median"].integer(), "j. ouvré", "j. ouvrés").c_str());
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("De %d à %d jours ouvrés sur %s.\n%s plus lente%s que d'habitude.", row["delay_min"].integer(),
                              row["delay_max"].integer(), nb(row["delays"].integer(), "livraison").c_str(),
                              nb(row["slower"].integer(), "livraison").c_str(), row["slower"].integer() > 1 ? "s" : "");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", row["types"].integer());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(date_text(row["last_order"]).c_str());
      }
      ImGui::EndTable();
    }

    void draw_types_stats() {
      const Json &types = stats_["types"];
      ImGui::SeparatorText("Prix par type d'item");
      if (types.size() == 0) {
        ImGui::TextDisabled("Aucun achat avec un prix.");
        return;
      }
      grey_wrapped("Cliquez sur un type pour voir l'évolution de son prix. Économie : au meilleur prix de l'année "
                   "plutôt qu'au dernier, au rythme de consommation actuel.");
      if (ImGui::BeginTable("types", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupColumn("Type d'item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Dernier prix", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("Meilleur prix", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("Évol.", ImGuiTableColumnFlags_WidthFixed, em * 4.0f);
        ImGui::TableSetupColumn("12 mois", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
        ImGui::TableSetupColumn("Coût / an", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
        ImGui::TableSetupColumn("Économie", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : types.items()) {
          const std::string code = row["type"].str();
          ImGui::PushID(code.c_str());
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          // deux lignes par type : prix, puis fournisseur en gris
          if (ImGui::Selectable(row["name"].str().c_str(), price_type_ == code,
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.0f - ImGui::GetStyle().ItemSpacing.y)))
            price_type_ = price_type_ == code ? std::string() : code;
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(unit_price_text(row["last_price"]).c_str());
          ImGui::TextDisabled("%s", row["last_supplier"].str().c_str());
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Prix moyen payé : %s", unit_price_text(row["average_price"]).c_str());
          ImGui::TableNextColumn();
          if (row["best_price"].num() < row["last_price"].num()) {
            ImGui::TextColored(colors::green, "%s", unit_price_text(row["best_price"]).c_str());
            ImGui::TextDisabled("%s", row["best_supplier"].str().c_str());
          } else {
            ImGui::TextDisabled("= dernier");
          }
          ImGui::TableNextColumn();
          if (row["change"].is_null() || std::fabs(row["change"].num()) < 0.05) {
            ImGui::TextDisabled("–");
          } else {
            char text[24];
            std::snprintf(text, sizeof(text), "%+.1f %%", row["change"].num());
            ImGui::TextColored(row["change"].num() > 0 ? colors::red : colors::green, "%s", with_comma(text).c_str());
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Du premier prix connu au dernier (de %s à %s selon les achats).",
                                unit_price_text(row["min_price"]).c_str(), unit_price_text(row["max_price"]).c_str());
          }
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(euros(row["spent_year"]).c_str());
          ImGui::TextDisabled("%s", nb(row["bought_year"].integer(), "item").c_str());
          ImGui::TableNextColumn();
          if (row["per_year"].num() > 0) {
            ImGui::TextUnformatted(euros(row["yearly_cost"]).c_str());
            ImGui::TextDisabled("%ld / an", std::lround(row["per_year"].num()));
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Items consommés par an (rythme actuel), au dernier prix");
          } else {
            ImGui::TextDisabled("–");
          }
          ImGui::TableNextColumn();
          if (row["saving"].num() > 0) {
            ImGui::TextColored(colors::green, "%s", euros(row["saving"]).c_str());
            ImGui::TextDisabled("par an");
          } else
            ImGui::TextDisabled("–");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      for (const Json &row : types.items())
        if (row["type"].str() == price_type_)
          draw_price_history(row);
    }

    static void draw_price_history(const Json &row) {
      const Json &history = row["history"];
      const int   n       = static_cast< int >(history.size());
      std::vector< double >      xs, prices;
      std::vector< std::string > labels;
      double                     top = 0.0;
      for (int index = 0; index < n; ++index) {
        xs.push_back(index);
        prices.push_back(history[index]["price"].num());
        const auto date = Date::parse(history[index]["date"].str());
        char       text[16] = "";
        if (date)
          std::snprintf(text, sizeof(text), "%02d/%02d/%02d", date->day, date->month, date->year % 100);
        labels.push_back(text);
        top = std::max(top, prices.back());
      }
      std::vector< const char * > label_ptrs;
      for (const std::string &label : labels)
        label_ptrs.push_back(label.c_str());
      const std::string title = "Prix unitaire : " + row["name"].str();
      if (n == 0 || !ImPlot::BeginPlot(title.c_str(), ImVec2(-1, 200), ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;
      ImPlot::SetupAxes(nullptr, "€");
      ImPlot::SetupAxisLimits(ImAxis_X1, -0.5, std::max(1, n - 1) + 0.5, ImPlotCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0, top * 1.25 + 0.01, ImPlotCond_Always);
      ImPlot::SetupAxisTicks(ImAxis_X1, xs.data(), n, label_ptrs.data());
      ImPlot::PlotLine("Prix", xs.data(), prices.data(), n, ImPlotSpec(ImPlotProp_LineColor, items_color,
                                                                        ImPlotProp_Marker, ImPlotMarker_Circle));
      if (ImPlot::IsPlotHovered()) {
        const int index = static_cast< int >(std::lround(ImPlot::GetPlotMousePos().x));
        if (index >= 0 && index < n) {
          const Json &point = history[index];
          ImGui::SetTooltip("%s : %s chez %s (%s)", labels[index].c_str(), unit_price_text(point["price"]).c_str(),
                            point["supplier"].str().c_str(), nb(point["quantity"].integer(), "item").c_str());
        }
      }
      ImPlot::EndPlot();
    }

    Json        orders_      = Json::array();
    Json        suppliers_   = Json::array();
    Json        suggestions_ = Json::object();
    Json        stats_       = Json::object();
    std::string orders_error_, suggestions_error_, stats_error_, form_error_;
    bool        loading_orders_ = false, loading_suggestions_ = false, loading_stats_ = false, saving_ = false;
    int         selected_ = 0;
    std::string filter_;
    OrderForm   form_;
    bool        form_open_   = false;
    bool        select_form_ = false; // onglet du formulaire au prochain affichage
    bool        select_orders_ = false; // onglet Commandes apres l'enregistrement
    std::set< std::string > unchecked_; // lignes suggerees decochees (« fournisseur:type »)
    std::string price_type_;            // type dont l'historique des prix est affiche
    int         seen_version_ = 0;
};

// ---- Reception d'une commande (fenetre Reception) ---------------------------------------------------------------

// Repartition de `count` items en `packs` paquets fermes : d'apres le nombre d'items par paquet enregistre s'il
// convient (36 par 15 en 3 → 15, 15, 6), sinon egale si elle tombe juste (36 en 4 → 9), sinon en dizaines si possible
// (50 en 3 → 20, 20, 10), en cinq, ou au plus juste (7 en 2 → 4, 3). Les premiers paquets sont pleins, le dernier prend le reste.
std::vector< int > split_packs(int count, int packs, int per_pack) {
  packs = std::clamp(packs, 1, std::max(1, count));
  if (count <= 0)
    return std::vector< int >(static_cast< std::size_t >(packs), 0);
  if (per_pack > 1 && per_pack * (packs - 1) < count && count <= per_pack * packs) {
    std::vector< int > sizes(static_cast< std::size_t >(packs - 1), per_pack);
    sizes.push_back(count - per_pack * (packs - 1));
    return sizes;
  }
  if (count % packs == 0)
    return std::vector< int >(static_cast< std::size_t >(packs), count / packs);
  const int share = (count + packs - 1) / packs;
  for (const int step : { 10, 5 }) {
    const int size = (share + step - 1) / step * step;
    if (share >= step && size * (packs - 1) < count) {
      std::vector< int > sizes(static_cast< std::size_t >(packs - 1), size);
      sizes.push_back(count - size * (packs - 1));
      return sizes;
    }
  }
  std::vector< int > sizes;
  for (int index = 0; index < packs; ++index)
    sizes.push_back(count / packs + (index < count % packs ? 1 : 0));
  return sizes;
}

// Nombre de paquets propose : d'apres le nombre d'items par paquet enregistre pour le type, sinon un seul
int default_packs(int count, int per_pack) {
  if (per_pack <= 1 || count <= 0)
    return 1;
  return std::clamp((count + per_pack - 1) / per_pack, 1, 500);
}

struct ReceptionRow {
    int                count = 0;
    std::string        peremption;          // tape librement (parse_user_date)
    int                packs        = 1;     // paquets fermes
    bool               packs_edited = false; // nombre de paquets tape : plus recalcule avec le nombre d'items
    std::vector< int > sizes;               // items par paquet (repartition modifiable)
    int                sizes_count = -1;     // nombre d'items et de paquets de la repartition proposee
    int                sizes_packs = -1;
};

struct ReceptionLine {
    int                        line = 0;
    std::string                type;
    std::string                name;
    bool                       perishable = false;
    int                        remaining  = 0;
    std::string                place;     // rangement du stock (vide : en vrac)
    std::string                suggested; // dernier rangement de ce type
    bool                       sealed   = false;
    int                        per_pack = 1;
    std::vector< ReceptionRow > rows;
};

class OrderReceptionPanel final : public OrderReception {
  public:
    void reload(App &app) override {
      loaded_at_ = ImGui::GetTime();
      app.api.get("/api/purchases/orders/?open=1", [this, &app](const ApiResult &result) {
        if (!result.ok)
          return;
        open_ = result.data;
        if (pending_) {
          for (const Json &order : open_.items())
            if (order["id"].integer() == pending_)
              select(app, order);
          pending_ = 0;
        }
      });
    }

    bool draw(App &app, const Json &places) override {
      if (const int wanted = std::exchange(app.purchase_to_receive_, 0)) {
        pending_ = wanted;
        reload(app);
      } else if (!order_.is_object() && ImGui::GetTime() - loaded_at_ > 30.0) {
        reload(app); // commande saisie depuis un autre ordinateur, ou livree entre-temps
      }
      if (!order_.is_object())
        return draw_open_orders(app);
      draw_reception(app, places);
      return true;
    }

  private:
    bool draw_open_orders(App &app) {
      if (!last_.is_null())
        draw_last(app);
      if (open_.size() == 0)
        return false;
      ImGui::SeparatorText(("Commandes à réceptionner (" + std::to_string(open_.size()) + ")").c_str());
      const std::size_t shown = show_all_ ? open_.size() : std::min< std::size_t >(open_.size(), 4);
      for (std::size_t index = 0; index < shown; ++index) {
        const Json &order = open_[index];
        ImGui::PushID(order["id"].integer());
        if (primary_button("Réceptionner"))
          select(app, order);
        ImGui::SameLine();
        state_badge(state_badge_kind(order["state"].str()));
        ImGui::SameLine();
        ImGui::TextUnformatted(order["label"].str().c_str());
        ImGui::SameLine();
        ImGui::TextColored(state_color(order["state"].str()), "· %s", order["short"].str(order["detail"].str()).c_str());
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", order["detail"].str().c_str());
        std::string lines;
        for (const Json &line : order["lines"].items())
          if (line["remaining"].integer() > 0)
            lines += (lines.empty() ? "" : ", ") + std::to_string(line["remaining"].integer()) + " × " + line["name"].str();
        ImGui::Indent(ImGui::GetFontSize() * 2.0f);
        grey_wrapped("À recevoir : " + lines);
        ImGui::Unindent(ImGui::GetFontSize() * 2.0f);
        ImGui::PopID();
      }
      if (open_.size() > 4 && small_button(show_all_ ? "[−] Moins" : ("[+] " + nb(open_.size() - 4, "autre commande", "autres commandes")).c_str()))
        show_all_ = !show_all_;
      ImGui::SeparatorText("Réception sans commande");
      return false;
    }

    void select(App &app, const Json &order) {
      order_ = order;
      lines_.clear();
      error_.clear();
      for (const Json &line : order["lines"].items()) {
        if (line["remaining"].integer() <= 0)
          continue;
        ReceptionLine row;
        row.line       = line["id"].integer();
        row.type       = line["type"].str();
        row.name       = line["name"].str();
        row.perishable = line["perishable"].boolean();
        row.remaining  = line["remaining"].integer();
        row.per_pack   = std::max(1, line["pack_size"].integer(1));
        row.rows.push_back(ReceptionRow{ row.remaining, std::string() });
        lines_.push_back(row);
        // rangement propose : le dernier ou ce type a ete range
        const int         id   = row.line;
        const std::string type = row.type;
        app.api.get("/api/receptions/storage/?type=" + url_encode(type), [this, id](const ApiResult &result) {
          if (!result.ok || result.data["location"].is_null())
            return;
          for (ReceptionLine &target : lines_)
            if (target.line == id && target.place.empty()) {
              target.suggested = result.data["location"].str();
              target.place     = target.suggested;
            }
        });
      }
    }

    void draw_reception(App &app, const Json &places) {
      ImGui::SeparatorText("Réception d'une commande");
      ImGui::TextUnformatted(order_["label"].str().c_str());
      ImGui::SameLine();
      if (small_button("← Autre réception")) {
        order_ = Json();
        return;
      }
      grey_wrapped("Indiquez ce qui est arrivé (quantité, et date de péremption s'il y a lieu ; « + Autre date » si le "
                   "colis mélange plusieurs dates). Tous les items sont créés d'un coup et toutes les étiquettes "
                   "sont prêtes à imprimer. Mettez 0 pour ce qui n'est pas encore arrivé.");
      int  total   = 0;
      bool invalid = false, past = false;
      const Date today = app.today();
      for (std::size_t index = 0; index < lines_.size(); ++index) {
        ReceptionLine &line = lines_[index];
        ImGui::PushID(line.line);
        ImGui::Separator();
        ImGui::TextUnformatted(line.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", nb(line.remaining, "item attendu", "items attendus").c_str());
        if (!line.perishable) {
          ImGui::SameLine();
          ImGui::TextDisabled("(non périssable)");
        }
        int remove   = -1;
        int line_sum = 0;
        for (std::size_t r = 0; r < line.rows.size(); ++r) {
          ReceptionRow &row = line.rows[r];
          ImGui::PushID(static_cast< int >(r));
          ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
          ImGui::InputInt("items", &row.count);
          row.count = std::clamp(row.count, 0, 10000);
          line_sum += row.count;
          if (line.perishable) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            ImGui::InputTextWithHint("##peremption", "Péremption", &row.peremption);
            const auto date = parse_user_date(row.peremption);
            if (ImGui::IsItemDeactivatedAfterEdit() && date)
              row.peremption = date->display();
            if (row.count > 0 && !date) {
              ImGui::SameLine();
              ImGui::TextColored(row.peremption.empty() ? colors::orange : colors::red,
                                 row.peremption.empty() ? "date de péremption ?" : "date non reconnue");
              invalid = true;
            } else if (date && *date < today) {
              ImGui::SameLine();
              ImGui::TextColored(colors::orange, "déjà passée");
              past = true;
            } else if (date) {
              ImGui::SameLine();
              ImGui::TextColored(colors::green, "→ %s", date->display().c_str());
            }
          }
          if (line.rows.size() > 1) {
            ImGui::SameLine();
            if (small_button("✕"))
              remove = static_cast< int >(r);
          }
          if (line.sealed && row.count > 0 && !draw_packs(line, row))
            invalid = true;
          ImGui::PopID();
        }
        if (remove >= 0)
          line.rows.erase(line.rows.begin() + remove);
        if (line.perishable && small_button("+ Autre date")) {
          const int left = std::max(0, line.remaining - line_sum);
          line.rows.push_back(ReceptionRow{ left, std::string() });
        }
        if (line.perishable)
          ImGui::SameLine();
        if (ImGui::Checkbox("Paquets fermés", &line.sealed))
          for (ReceptionRow &row : line.rows)
            row.packs_edited = false; // nombre de paquets repropose d'apres le nombre d'items
        help_marker("Paquets que l'on n'ouvre pas tout de suite : une étiquette par paquet, les étiquettes des items "
                    "sont imprimées à l'ouverture. Indiquez le nombre de paquets ; s'ils ne sont pas tous pareils, "
                    "corrigez la répartition proposée.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        search_select("place", places, "id", "label", line.place, "Rangement du stock…", "Stock (sans rangement)",
                      ImGui::GetFontSize() * 18.0f);
        if (!line.place.empty() && line.place == line.suggested) {
          ImGui::SameLine();
          ImGui::TextDisabled("(dernier rangement de ce type)");
        }
        if (line_sum > line.remaining)
          ImGui::TextColored(colors::blue, "%s de plus que prévu.", nb(line_sum - line.remaining, "item").c_str());
        else if (line_sum > 0 && line_sum < line.remaining)
          ImGui::TextDisabled("Le reste (%d) restera attendu.", line.remaining - line_sum);
        total += line_sum;
        ImGui::PopID();
      }
      ImGui::Separator();
      if (past)
        ImGui::TextColored(colors::orange, "Attention : une date de péremption est déjà passée.");
      if (!error_.empty())
        ImGui::TextColored(colors::red, "%s", error_.c_str());
      ImGui::BeginDisabled(total == 0 || invalid || sending_);
      const std::string label = "Créer " + nb(total, "item") + " et voir toutes les étiquettes";
      if (primary_button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetFrameHeight() * 1.5f)))
        send(app);
      ImGui::EndDisabled();
    }

    // Paquets fermes d'une date : leur nombre, puis la repartition des items si elle n'est pas egale. Faux si la
    // repartition ne fait pas le nombre d'items.
    static bool draw_packs(ReceptionLine &line, ReceptionRow &row) {
      if (!row.packs_edited)
        row.packs = default_packs(row.count, line.per_pack);
      row.packs = std::clamp(row.packs, 1, std::clamp(row.count, 1, 500));
      if (row.sizes_count != row.count || row.sizes_packs != row.packs) {
        row.sizes       = split_packs(row.count, row.packs, line.per_pack);
        row.sizes_count = row.count;
        row.sizes_packs = row.packs;
      }
      ImGui::Indent(ImGui::GetFontSize() * 1.5f);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
      if (ImGui::InputInt("paquets", &row.packs)) {
        row.packs_edited = true;
        row.packs        = std::clamp(row.packs, 1, std::clamp(row.count, 1, 500));
        row.sizes        = split_packs(row.count, row.packs, line.per_pack);
        row.sizes_packs  = row.packs;
      }
      const bool equal = std::all_of(row.sizes.begin(), row.sizes.end(), [&](int size) { return size == row.sizes[0]; });
      ImGui::SameLine();
      if (equal)
        ImGui::TextDisabled("de %s", nb(row.sizes[0], "item").c_str());
      else
        ImGui::TextDisabled("répartition (modifiable) :");
      int sum = 0;
      for (const int size : row.sizes)
        sum += size;
      if (!equal) {
        if (ImGui::BeginTable("sizes", static_cast< int >(std::min< std::size_t >(row.sizes.size(), 8)),
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
          for (std::size_t index = 0; index < row.sizes.size(); ++index) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast< int >(index));
            ImGui::TextDisabled("Paquet %d", static_cast< int >(index) + 1);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
            ImGui::InputInt("##size", &row.sizes[index], 0);
            row.sizes[index] = std::clamp(row.sizes[index], 1, 10000);
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
      }
      ImGui::Unindent(ImGui::GetFontSize() * 1.5f);
      if (sum != row.count) {
        ImGui::Indent(ImGui::GetFontSize() * 1.5f);
        ImGui::TextColored(colors::red, "Les paquets font %s, pour %s reçus.", nb(sum, "item").c_str(),
                           nb(row.count, "item").c_str());
        ImGui::Unindent(ImGui::GetFontSize() * 1.5f);
        return false;
      }
      return true;
    }

    void send(App &app) {
      Json body;
      body["user"]       = app.user_ref();
      body["receptions"] = Json::array();
      for (const ReceptionLine &line : lines_)
        for (const ReceptionRow &row : line.rows) {
          if (row.count <= 0)
            continue;
          Json item;
          item["line"]     = line.line;
          const auto date  = parse_user_date(row.peremption);
          item["peremption"] = line.perishable && date ? Json(date->iso()) : Json();
          item["location"] = line.place.empty() ? Json() : Json(line.place);
          if (!line.sealed) {
            item["count"] = row.count;
            body["receptions"].push_back(item);
            continue;
          }
          // paquets fermes : une reception par suite de paquets de meme taille (20, 20 puis 10)
          for (std::size_t index = 0; index < row.sizes.size();) {
            std::size_t end = index;
            while (end < row.sizes.size() && row.sizes[end] == row.sizes[index])
              ++end;
            Json packs           = item;
            packs["sealed_pack"] = true;
            packs["count"]       = row.sizes[index];
            packs["packs"]       = static_cast< int >(end - index);
            body["receptions"].push_back(packs);
            index = end;
          }
        }
      sending_ = true;
      error_.clear();
      const int id = order_["id"].integer();
      app.api.post("/api/purchases/orders/" + std::to_string(id) + "/receive/", body, [this, &app](const ApiResult &result) {
        sending_ = false;
        if (!result.ok) {
          error_ = result.error;
          return;
        }
        last_ = result.data;
        app.notify(nb(result.data["items"].size(), "item créé", "items créés") + " : " + result.data["order"]["label"].str());
        preview(app);
        ++app.catalog.packs_version;
        ++app.catalog.purchases_version;
        app.refresh_tasks();
        // commande pas encore toute arrivee : on reste dessus avec ce qui manque
        const Json order = result.data["order"];
        order_ = Json();
        if (order["open"].boolean() && order["state"].str() != "waiting")
          last_["still_open"] = true;
        reload(app);
      });
    }

    // toutes les etiquettes de la reception ensemble : paquets fermes, puis items
    void preview(App &app) {
      std::vector< PrintJob > jobs;
      for (const Json &reception : last_["receptions"].items()) {
        if (reception["sealed_packs"].is_array() && reception["sealed_packs"].size() > 0) {
          std::vector< Parameters > packs;
          for (const Json &pack : reception["sealed_packs"].items())
            packs.push_back(sealed_pack_parameters(pack));
          app.build_label_jobs(TemplateCategory::ItemPack, packs, "Paquet", jobs);
        } else {
          std::vector< Parameters > labels;
          const Json               &items = reception["items"];
          const int                 count = static_cast< int >(items.size());
          for (int index = 0; index < count; ++index)
            labels.push_back(item_parameters(items[index], index + 1, count));
          app.build_label_jobs(TemplateCategory::Item, labels, "Réception", jobs);
        }
      }
      app.preview_jobs(std::move(jobs), "Réception de la commande");
    }

    void draw_last(App &app) {
      ImGui::SeparatorText("Dernière réception de commande");
      ImGui::TextColored(colors::green, "%s créés : %s", nb(last_["items"].size(), "item", "items").c_str(),
                         last_["order"]["label"].str().c_str());
      ImGui::SameLine();
      if (small_button("Revoir les étiquettes"))
        preview(app);
      ImGui::SameLine();
      if (small_button("✕##last"))
        last_ = Json();
      if (!last_.is_null() && last_["still_open"].boolean())
        ImGui::TextDisabled("La commande attend encore le reste : elle reste dans la liste ci-dessous.");
    }

    Json                         open_  = Json::array();
    Json                         order_; // commande en cours de reception (null : aucune)
    Json                         last_;  // reponse de la derniere reception
    std::vector< ReceptionLine > lines_;
    std::string                  error_;
    bool                         sending_  = false;
    bool                         show_all_ = false;
    int                          pending_  = 0; // commande a selectionner des que la liste est rechargee
    double                       loaded_at_ = 0.0;
};

} // namespace

std::unique_ptr< AppWindow > make_purchases_window() {
  return std::make_unique< PurchasesWindow >();
}

std::unique_ptr< OrderReception > make_order_reception() {
  return std::make_unique< OrderReceptionPanel >();
}

} // namespace qrprotec
