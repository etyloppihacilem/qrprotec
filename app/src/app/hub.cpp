/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hub.cpp
        -\-    _|__
         |\___/  . \        Created on 18 Nov. 2026 at 03:10
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : « Aller a... » (Ctrl+K, ou le menu Fenetres), par-dessus l'ecran. Un lot, un item (iid),
// un type, une fenetre ou une action : fleches pour choisir, Entree pour ouvrir. Champ vide : les fenetres et les
// actions. Tri : ce qui commence par le texte tape d'abord, les lots a traiter avant les autres (attention d'abord).
// Il se ferme des qu'on choisit, qu'on clique a cote ou qu'on tape Echap : aucune place prise en permanence.

#include "app.hpp"
#include "../core/plural.hpp"
#include "../ui/widgets.hpp"

#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <vector>

namespace qrprotec {

namespace {

struct HubEntry {
    std::string           label;
    std::string           detail;   // en gris a droite (genre de resultat, touche)
    ImVec4                color;    // pastille d'etat (alpha 0 : pas de pastille)
    int                   rank = 0; // 0 : commence par le texte tape, 1 : le contient ; puis ordre d'attention
    int                   attention = 0;
    std::function< void() > run;
};

std::string lower_ascii(std::string text) {
  for (char &c : text)
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  return text;
}

// commence par le texte tape (0), le contient (1), sinon -1 (recherche sans accents de search_matches)
int match_rank(const std::string &query, const std::string &text) {
  if (!search_matches(query, text))
    return -1;
  return lower_ascii(text).rfind(lower_ascii(query), 0) == 0 ? 0 : 1;
}

const ImVec4 kNoColor(0, 0, 0, 0);

} // namespace

void App::open_hub() {
  hub_.open   = true;
  hub_.focus  = true;
  hub_.close  = false;
  hub_.cursor = 0;
  hub_.query.clear();
}

void App::draw_hub() {
  if (!hub_.open)
    return;
  if (hub_.focus)
    ImGui::OpenPopup("##hub");
  ImVec2 origin, work;
  work_area(origin, work);
  const ImVec2 size(std::min(work.x * 0.9f, ImGui::GetFontSize() * 44.0f), work.y * 0.6f);
  ImGui::SetNextWindowPos(ImVec2(origin.x + (work.x - size.x) * 0.5f, origin.y + work.y * 0.08f));
  ImGui::SetNextWindowSize(size);
  // popup simple (pas modale) : un clic a cote la ferme
  if (!ImGui::BeginPopup("##hub", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    hub_.open = false;
    return;
  }
  bool close = std::exchange(hub_.close, false);
  if (hub_.focus) {
    ImGui::SetKeyboardFocusHere();
    hub_.focus = false;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  const std::string hint = privileged() ? "Aller à… un lot, un item (iid), un type, une fenêtre ou une action"
                                        : "Aller à… un lot, une fenêtre ou une action";
  if (ImGui::InputTextWithHint("##hub_query", hint.c_str(), &hub_.query))
    hub_.cursor = 0;
  ImGui::TextDisabled("Flèches pour choisir, Entrée pour ouvrir, Échap ou clic à côté pour fermer.");
  {
    // ---- resultats « Aller a... » ----
    std::vector< HubEntry > entries;
    const std::string       query = hub_.query;
    const auto add = [&](const std::string &label, const std::string &keywords, const std::string &detail,
                         const ImVec4 &color, int attention, std::function< void() > run) {
      int rank = match_rank(query, label);
      if (rank < 0) {
        if (keywords.empty() || !search_matches(query, keywords))
          return;
        rank = 1;
      }
      entries.push_back({ label, detail, color, rank, attention, std::move(run) });
    };
    // fenetres
    static const std::pair< const char *, const char * > kWords[] = {
      { "stock", "TECH taches a faire a venir evenements" },
      { "inventory", "inventaire etat du stock projection commandes transferts" },
      { "items", "types d'items paquets entretien revisions desinfections materiel" },
      { "lot_types", "types de lots referentiel contenu emplacements" },
      { "reception", "reception commande livraison" },
      { "purchases", "commandes fournisseurs achats prix statistiques depenses delais" },
      { "journal", "operations historique" },
      { "reports", "signalements problemes" },
      { "print_counts", "impressions consommables rouleaux compteur" },
      { "phone", "douchette telephone" },
      { "lists", "listes materiel poste depart" },
      { "users", "utilisateurs secouristes badges pin ordinateurs" },
      { "editor", "editeur etiquettes modeles" },
      { "settings", "reglages serveur" },
    };
    for (auto &target : windows) {
      if (!can_open(*target) || target->id == "pack" || target->id == "verif")
        continue;
      std::string words;
      for (const auto &[id, text] : kWords)
        if (target->id == id)
          words = text;
      const std::string id = target->id;
      add(target->title, words, "Fenêtre" + (target->shortcut ? " · " + target->shortcut_label : std::string()),
          kNoColor, 50, [this, id]() { open_window(id); });
    }
    // actions
    struct Action {
        const char           *label;
        const char           *keywords;
        int                   access; // 0 tous, 1 gestion, 2 admin
        std::function< void() > run;
    };
    const std::vector< Action > actions = {
      { "Nouvelle réception", "reception commande livraison", 1, [this]() { show_stock_tab("reception"); } },
      { "Entretien : révisions et désinfections", "materiel reviser desinfecter periodicite", 1,
        [this]() { show_stock_tab("equipment"); } },
      { "Paquets fermés", "paquet ouverture", 1, [this]() { show_stock_tab("packs"); } },
      { "À faire (TECH)", "taches", 1, [this]() { show_tech_tab("tasks"); } },
      { "À venir (TECH)", "evenements peremptions revisions", 1, [this]() { show_tech_tab("events"); } },
      { "Mode normal", "secouriste chef ctrl g", 1, [this]() { set_gestion_mode(false); } },
      { "Réceptions passées", "historique reception", 1, [this]() { show_stock_tab("past_receptions"); } },
      { "Nouvelle commande fournisseur", "commande fournisseur achat saisir", 1,
        [this]() { show_stock_tab("purchase_new"); } },
      { "Statistiques des achats", "prix depenses economies delais fournisseurs", 1,
        [this]() { show_stock_tab("purchase_stats"); } },
      { "Suggestions de commande par fournisseur", "grouper livraison commander", 1,
        [this]() { show_stock_tab("purchase_suggestions"); } },
      { "État du stock", "stock minimum", 1, [this]() { show_stock_tab("stock"); } },
      { "Projection du stock", "previsions mois", 1, [this]() { show_stock_tab("projection"); } },
      { "Commandes à passer", "commander", 1, [this]() { show_stock_tab("orders"); } },
      { "Transferts entre lots", "deplacements peremption", 1, [this]() { show_stock_tab("transfers"); } },
      { "Nouveau lot", "creer lot serie", 1, [this]() { show_lots_mode("new"); } },
      { "Ordre des lots", "trier ordre", 1, [this]() { show_lots_mode("order"); } },
      { "Lots archivés", "archive desarchiver", 1, [this]() { show_lots_mode("archived"); } },
      { "Lots à traiter", "incomplets perimes rouges", 0, [this]() { show_lots_to_treat(); } },
      { "Impressions demandées", "files impression etiquettes web", 1,
        [this]() {
          label_prints_.next_poll = 0.0;
          open_label_prints();
        } },
      { "Changer de rouleau", "imprimante rouleau etiquettes", 1, [this]() { open_roll_wizard(); } },
      { "Réglages d'impression", "imprimante modeles", 2, [this]() { open_settings_section("Étiquettes et impression"); } },
      { "Signal de mauvais scan", "bip douchette son", 2, [this]() { open_settings_section("Signal de mauvais scan"); } },
      { "Disposition par défaut", "fenetres demarrage", 2, [this]() { open_settings_section("Disposition par défaut"); } },
      { "Remettre les fenêtres en place", "ctrl espace disposition", 0, [this]() { request_layout_reset(); } },
      { "Plein écran", "ctrl f11", 0, [this]() { toggle_fullscreen(); } },
    };
    for (const Action &action : actions)
      if (action.access == 0 || (action.access == 1 && privileged()) || (action.access == 2 && admin()))
        add(action.label, action.keywords, "Action", kNoColor, 60, action.run);
    // lots (attention d'abord), sans les rangements du stock pour les secouristes ; champ vide : pas de lots
    static const Json kNoLots = Json::array();
    for (const Json &lot : (query.empty() ? kNoLots : catalog.lots).items()) {
      if (lot["storage"].boolean() && !privileged())
        continue;
      const std::string id     = lot["id"].str();
      const LotStatus   status = lot_status(lot);
      const ImVec4      color  = lot_is_group(lot) ? colors::grey : lot_status_color(status);
      add(lot["name"].str(), lot["name_short"].str() + " " + lot["lot_type_name"].str() + " " + id,
          std::string(lot["storage"].boolean() ? "Rangement" : "Lot") + " · " + lot_status_label(status), color,
          lot["attention_order"].integer(), [this, id]() { show_lot(id); });
    }
    if (privileged() && !query.empty()) {
      for (const Json &type : catalog.lot_types.items()) {
        const std::string key = "lot:" + type["type"].str();
        add(type["name"].str(), type["type"].str(), "Type de lot · Types de lots", kNoColor, 70,
            [this, key]() { show_type(key); });
      }
      for (const Json &type : catalog.item_types.items()) {
        const std::string key = "item:" + type["type"].str();
        add(type["name"].str(), type["type"].str() + " " + type["description"].str(), "Type d'item · Items",
            kNoColor, 70, [this, key]() { show_type(key); });
      }
      // iid tape (ou scanne) : fiche de l'item
      if (query.size() >= 6 && query.find(' ') == std::string::npos)
        entries.push_back({ "Fiche de l'item " + query, "Item · Items", kNoColor, 2, 80,
                            [this, query]() { show_item(query); } });
    }
    std::stable_sort(entries.begin(), entries.end(), [](const HubEntry &a, const HubEntry &b) {
      return a.rank != b.rank ? a.rank < b.rank : a.attention < b.attention;
    });
    if (entries.size() > 40)
      entries.resize(40);
    // fleches : choix ; Entree : ouvrir ; Echap : effacer le texte
    const int count = static_cast< int >(entries.size());
    if (count > 0) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        hub_.cursor = (hub_.cursor + 1) % count;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        hub_.cursor = (hub_.cursor + count - 1) % count;
      hub_.cursor = std::clamp(hub_.cursor, 0, count - 1);
    }
    // resultats hors de la navigation clavier d'ImGui : les fleches choisissent ici, le champ garde le focus
    ImGui::BeginChild("hub_results", ImVec2(0, 0), 0, ImGuiWindowFlags_NoNav);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    int clicked = -1;
    for (int index = 0; index < count; ++index) {
      const HubEntry &entry = entries[static_cast< std::size_t >(index)];
      ImGui::PushID(index);
      const bool selected = index == hub_.cursor;
      if (ImGui::Selectable("##result", selected, ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0, ImGui::GetTextLineHeightWithSpacing())))
        clicked = index;
      if (selected)
        ImGui::SetScrollHereY(0.5f);
      ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
      if (entry.color.w > 0.0f)
        ImGui::TextColored(entry.color, "●");
      else
        ImGui::TextDisabled("›");
      ImGui::SameLine();
      ImGui::TextUnformatted(entry.label.c_str());
      ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.62f + ImGui::GetCursorStartPos().x);
      ImGui::TextDisabled("%s", entry.detail.c_str());
      ImGui::PopID();
    }
    if (count == 0)
      ImGui::TextDisabled("Rien trouvé pour « %s ».", query.c_str());
    ImGui::PopItemFlag();
    ImGui::EndChild();
    if (clicked < 0 && count > 0 && enter_pressed(false))
      clicked = hub_.cursor;
    if (clicked >= 0) {
      entries[static_cast< std::size_t >(clicked)].run();
      close = true;
    }
    // Echap : efface le texte, puis ferme
    if (escape_pressed()) {
      if (hub_.query.empty())
        close = true;
      hub_.query.clear();
      hub_.focus = true;
    }
  }
  if (close) {
    hub_.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

} // namespace qrprotec
