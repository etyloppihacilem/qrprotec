/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hub.cpp
        -\-    _|__
         |\___/  . \        Created on 18 Nov. 2026 at 03:10
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// App (suite de app.cpp) : hub « Outils ». Ctrl+K ou le bouton « ☰ Outils » de la barre l'ouvrent par-dessus l'ecran.
// - Champ vide : toutes les fenetres en tuiles, rangees par question (Terrain, Gestion, Administration), chacune
//   avec sa touche F et un compteur de la couleur de l'urgence (rouge a traiter, orange a surveiller, bleu en
//   attente, vert rien a faire).
// - Texte tape : « Aller a... », un lot, un item (iid), un type, une fenetre ou une action. Fleches pour choisir,
//   Entree pour ouvrir, Echap pour fermer. Tri : ce qui commence par le texte tape d'abord, les lots a traiter
//   avant les autres (attention d'abord).
// Il se ferme des qu'on choisit : aucune place prise en permanence sur l'ecran.

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

// Compteur d'une tuile : texte et couleur de l'urgence
struct TileBadge {
    std::string text;
    ImVec4      color = ImVec4(0, 0, 0, 0);
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
  hub_.opened_frame = ImGui::GetFrameCount();
  hub_.cursor = 0;
  hub_.query.clear();
  // compteurs des tuiles a jour
  if (privileged()) {
    refresh_tasks();
    refresh_report_counts();
  }
}

void App::draw_hub() {
  if (!hub_.open)
    return;
  if (hub_.focus) {
    ImGui::OpenPopup("##hub");
  }
  ImVec2 origin, work;
  work_area(origin, work);
  const ImVec2 size(std::min(work.x * 0.86f, ImGui::GetFontSize() * 62.0f), work.y * 0.84f);
  ImGui::SetNextWindowPos(ImVec2(origin.x + (work.x - size.x) * 0.5f, origin.y + work.y * 0.06f));
  ImGui::SetNextWindowSize(size);
  if (!ImGui::BeginPopupModal("##hub", nullptr,
                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    hub_.open = false;
    return;
  }
  bool close = false;
  // ---- champ « Aller a... » ----
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
  ImGui::TextUnformatted("Outils");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Ctrl+K · Échap pour fermer");
  if (hub_.focus) {
    ImGui::SetKeyboardFocusHere();
    hub_.focus = false;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  const std::string hint = privileged() ? "Aller à… un lot, un item (iid), un type, une fenêtre ou une action"
                                        : "Aller à… un lot, une fenêtre ou une action";
  if (ImGui::InputTextWithHint("##hub_query", hint.c_str(), &hub_.query))
    hub_.cursor = 0;
  const bool typing = !hub_.query.empty();
  ImGui::Spacing();

  // ---- compteurs des tuiles ----
  const auto badge = [this](const std::string &id) -> TileBadge {
    if (id == "lots") {
      const LotsAttention lots = lots_attention();
      if (lots.total == 0)
        return { "Tous valides", colors::green };
      return { nb(lots.total, "lot") + " à traiter", lots.red > 0 ? colors::red : colors::orange };
    }
    if (id == "stock") {
      const int count = catalog.tasks["count"].integer();
      if (!catalog.tasks.is_object() || catalog.tasks["categories"].size() == 0)
        return {};
      return count == 0 ? TileBadge{ "Rien à faire", colors::green }
                        : TileBadge{ nb(count, "tâche"), colors::orange };
    }
    if (id == "reports") {
      const int fresh = report_counts["new"].integer();
      const int open  = report_counts["progress"].integer();
      if (fresh > 0)
        return { nb(fresh, "nouveau", "nouveaux"), colors::red };
      if (open > 0)
        return { std::to_string(open) + " en cours", colors::orange };
      return { "Aucun en cours", colors::green };
    }
    if (id == "print_counts") {
      const int requests = static_cast< int >(label_prints_.data["pending"].size());
      if (requests > 0)
        return { nb(requests, "impression demandée", "impressions demandées"), colors::blue };
    }
    if (id == "lists") {
      // listes sorties ou a preparer : rien de charge ici, pas de compteur
    }
    return {};
  };

  std::string to_open;
  if (!typing) {
    // ---- tuiles par question ----
    struct Group {
        const char                 *title;
        const char                 *question;
        std::vector< const char * > ids;
    };
    static const Group kGroups[] = {
      { "Terrain", "Vérifier, préparer, partir", { "scan", "lots", "lists", "phone" } },
      { "Gestion", "Ce qu'il faut faire, ce que j'ai, comment c'est défini, qui a fait quoi",
        { "stock", "inventory", "catalogue", "journal", "reports", "print_counts" } },
      { "Administration", "Qui a accès, étiquettes, réglages", { "users", "editor", "settings" } },
    };
    static const std::pair< const char *, const char * > kHints[] = {
      { "scan", "Scans en cours, connexion" },
      { "lots", "État des lots, fiche, vérif" },
      { "lists", "Matériel à prendre pour un poste" },
      { "phone", "Téléphone comme douchette" },
      { "stock", "À faire, à venir" },
      { "inventory", "Réception, état du stock, items, commandes" },
      { "catalogue", "Types de lots, types d'items" },
      { "journal", "Qui a fait quoi, et quand" },
      { "reports", "Problèmes signalés" },
      { "print_counts", "Compteur, rouleaux, références" },
      { "users", "Secouristes, ordinateurs connectés" },
      { "editor", "Modèles d'étiquettes" },
      { "settings", "Serveur et poste" },
    };
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int   columns = std::max(2, static_cast< int >((ImGui::GetContentRegionAvail().x + spacing)
                                                       / (ImGui::GetFontSize() * 14.0f + spacing)));
    const float tile_w  = (ImGui::GetContentRegionAvail().x - spacing * static_cast< float >(columns - 1))
                       / static_cast< float >(columns);
    const float tile_h  = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::BeginChild("hub_tiles");
    for (const Group &group : kGroups) {
      std::vector< AppWindow * > shown;
      for (const char *id : group.ids)
        if (AppWindow *target = window(id); target && can_open(*target))
          shown.push_back(target);
      if (shown.empty())
        continue;
      ImGui::SeparatorText((std::string(group.title) + "  ·  " + group.question).c_str());
      int column = 0;
      for (AppWindow *target : shown) {
        if (column > 0)
          ImGui::SameLine();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(target->id.c_str());
        if (ImGui::Button("##tile", ImVec2(tile_w, tile_h)))
          to_open = target->id;
        ImGui::PopID();
        ImDrawList  *draw = ImGui::GetWindowDrawList();
        const ImVec2 pad  = ImGui::GetStyle().FramePadding;
        draw->PushClipRect(pos, ImVec2(pos.x + tile_w - pad.x, pos.y + tile_h), true); // texte coupe au bord
        const float  line = ImGui::GetTextLineHeightWithSpacing();
        draw->AddText(ImVec2(pos.x + pad.x * 2, pos.y + pad.y), ImGui::GetColorU32(ImGuiCol_Text), target->title.c_str());
        if (target->shortcut) {
          const float key_w = ImGui::CalcTextSize(target->shortcut_label.c_str()).x;
          draw->AddText(ImVec2(pos.x + tile_w - key_w - pad.x * 2, pos.y + pad.y),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), target->shortcut_label.c_str());
        }
        for (const auto &[id, text] : kHints)
          if (target->id == id)
            draw->AddText(ImVec2(pos.x + pad.x * 2, pos.y + pad.y + line), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        if (const TileBadge count = badge(target->id); !count.text.empty())
          draw->AddText(ImVec2(pos.x + pad.x * 2, pos.y + pad.y + line * 2), ImGui::GetColorU32(count.color),
                        ("● " + count.text).c_str());
        draw->PopClipRect();
        column = (column + 1) % columns;
      }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Tapez pour chercher un lot%s ou une action. Les touches %s ouvrent aussi les fenêtres.",
                        privileged() ? ", un item, un type" : "", privileged() ? "F1 à F12" : "F1 à F4");
    ImGui::EndChild();
    if (escape_pressed())
      close = true;
  } else {
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
      { "inventory", "inventaire reception items paquets commandes transferts stock" },
      { "catalogue", "types de lots types d'items referentiel contenu" },
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
      { "Réceptions passées", "historique reception", 1, [this]() { show_stock_tab("past_receptions"); } },
      { "État du stock", "stock minimum", 1, [this]() { show_stock_tab("stock"); } },
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
    // lots (attention d'abord), sans les rangements du stock pour les secouristes
    for (const Json &lot : catalog.lots.items()) {
      if (lot["storage"].boolean() && !privileged())
        continue;
      const std::string id     = lot["id"].str();
      const LotStatus   status = lot_status(lot);
      const ImVec4      color  = lot_is_group(lot) ? colors::grey : lot_status_color(status);
      add(lot["name"].str(), lot["name_short"].str() + " " + lot["lot_type_name"].str() + " " + id,
          std::string(lot["storage"].boolean() ? "Rangement" : "Lot") + " · " + lot_status_label(status), color,
          lot["attention_order"].integer(), [this, id]() { show_lot(id); });
    }
    if (privileged()) {
      for (const Json &type : catalog.lot_types.items()) {
        const std::string key = "lot:" + type["type"].str();
        add(type["name"].str(), type["type"].str(), "Type de lot · Catalogue", kNoColor, 70,
            [this, key]() { show_catalogue_type(key); });
      }
      for (const Json &type : catalog.item_types.items()) {
        const std::string key = "item:" + type["type"].str();
        add(type["name"].str(), type["type"].str() + " " + type["description"].str(), "Type d'item · Catalogue",
            kNoColor, 70, [this, key]() { show_catalogue_type(key); });
      }
      // iid tape (ou scanne) : fiche de l'item
      if (query.size() >= 6 && query.find(' ') == std::string::npos)
        entries.push_back({ "Fiche de l'item " + query, "Item · Stock", kNoColor, 2, 80,
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
    if (escape_pressed()) {
      hub_.query.clear();
      hub_.focus = true;
    }
  }
  if (!to_open.empty()) {
    open_window(to_open);
    close = true;
  }
  // Ctrl+K de nouveau : ferme
  if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_K, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive)
      && ImGui::GetFrameCount() != hub_.opened_frame)
    close = true;
  if (close) {
    hub_.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

} // namespace qrprotec
