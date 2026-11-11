/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lists_window.cpp
        -\-    _|__
         |\___/  . \        Created on 16 Oct. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "../../core/plural.hpp"
#include "windows.hpp"

#include "imgui_stdlib.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace qrprotec {

namespace {

constexpr double kPollSeconds = 3.0; // la liste ouverte est relue (verif a plusieurs, depuis le web)

ImVec4 status_color(const Json &list) {
  if (list["template"].boolean())
    return colors::grey;
  const std::string status = list["status"].str();
  return status == "returned" ? colors::green : status == "validated" ? colors::orange : ImVec4(0.14f, 0.39f, 0.70f, 1.0f);
}

// Bouton vert (ou orange) avec confirmation en deux temps : confirm_button est reserve aux actions dangereuses. La
// question est coupee a la largeur de la fenetre de confirmation ; confirm_label nomme le bouton de confirmation.
bool confirm_action(const char *label, const std::string &question, const char *popup_id, bool warning,
                    const char *confirm_label = "Confirmer") {
  if (warning ? warning_button(label) : primary_button(label))
    ImGui::OpenPopup(popup_id);
  bool confirmed = false;
  if (ImGui::BeginPopup(popup_id)) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
    ImGui::TextUnformatted(question.c_str());
    ImGui::PopTextWrapPos();
    if (warning ? warning_button(confirm_label) : primary_button(confirm_label)) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Annuler"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return confirmed;
}

// « Compresses ×2, Garrot » : noms regroupes, dans l'ordre de la liste
std::string grouped_names(const Json &names) {
  std::vector< std::pair< std::string, int > > counts;
  for (std::size_t index = 0; index < names.size(); ++index) {
    const std::string name = names[index].str();
    auto found = std::find_if(counts.begin(), counts.end(), [&name](const auto &row) { return row.first == name; });
    if (found == counts.end())
      counts.emplace_back(name, 1);
    else
      ++found->second;
  }
  std::string out;
  for (const auto &[name, count] : counts)
    out += (out.empty() ? "" : ", ") + name + (count > 1 ? " ×" + std::to_string(count) : std::string());
  return out;
}

// Liste sortie (retour a scanner) : question de « Clôturer le retour », avec ce qui n'est pas rentre et ce
// qui en sera fait (return_preview du serveur), comme sur le web
std::string return_question(const Json &preview) {
  std::string     steps;
  const Json     &lots  = preview["lots"];
  const long long nlots = static_cast< long long >(lots.size());
  if (nlots > 0) {
    std::string names;
    for (std::size_t index = 0; index < lots.size(); ++index)
      names += (index ? ", " : "") + lots[index].str();
    steps += "- " + names + " : " + pl(nlots, "passe", "passent") + " en vérif recommandée, " + (nlots > 1 ? "leur" : "son")
             + " contenu a pu servir.\n";
  }
  const long long used = static_cast< long long >(preview["used"].size());
  if (used > 0)
    steps += "- " + grouped_names(preview["used"]) + " : consommable pas rentré, compté comme utilisé.\n";
  const long long out = static_cast< long long >(preview["still_out"].size());
  if (out > 0)
    steps += "- " + grouped_names(preview["still_out"]) + " : matériel pas rentré, il reste sorti et rentrera en stock au "
             "prochain scan.\n";
  if (steps.empty()) {
    const long long back = preview["back"].integer();
    return "Clôturer le retour ? Tout est rentré" + (back > 0 ? " (" + nb(back, "scan") + ")" : std::string())
           + ".\nLa liste est ensuite terminée.";
  }
  return "Clôturer le retour sans tout ? Pas encore rentré :\n" + steps
         + "Du matériel est revenu ? Annulez et scannez-le avant de clôturer. La liste est ensuite terminée.";
}

// Listes de materiel a prendre (ex : un chef demande du materiel a ses equipiers), meme page que « Listes de
// matériel » sur le front web : lots et types d'items avec une quantite, partagees par lien et QR code. Le mode scan
// dirige les scans (douchette, telephone) vers la liste ouverte : verif tant qu'elle est a preparer, retour une fois
// sortie. Validee (« Prêt au départ »), son materiel sort du stock et la liste est terminee ; seules les anciennes
// listes sorties ont encore un retour a scanner, que « Clôturer le retour » clot.
class ListsWindow final : public AppWindow {
  public:
    ListsWindow() : AppWindow("lists", "Listes de matériel", false, true) {}

    void on_open(App &app) override {
      if (app.logged_in())
        load(app);
    }

    void reset_view(App &app) override {
      select(app, ""); // plus de liste ouverte ni de scan dirige vers elle
      show_ = "mine";
      member_query_.clear();
      member_results_ = Json::array();
    }

    void draw(App &app) override {
      if (!app.checklist_to_open.empty() && app.logged_in()) {
        // QR code d'une liste scanne : elle s'ouvre (verif en direct)
        select(app, std::exchange(app.checklist_to_open, {}));
        load(app);
      }
      if (!app.logged_in()) {
        stop_scan(app);
        ImGui::TextWrapped("Connectez-vous (badge) pour voir, préparer et vérifier les listes de matériel.");
        return;
      }
      // chefs, gestion et admins : creent des listes, voient toutes les listes et les modeles ; un secouriste ne voit
      // que les listes qui lui sont attribuees ou qu'il a ouvertes par leur lien ou leur QR code
      if (can_create_ || can_see_all_) {
        if (ImGui::RadioButton("Mes listes", show_ == "mine")) {
          show_ = "mine";
          load(app);
        }
        if (can_create_) {
          ImGui::SameLine();
          if (ImGui::RadioButton("Modèles", show_ == "templates")) {
            show_ = "templates";
            load(app);
          }
        }
        if (can_see_all_) {
          ImGui::SameLine();
          if (ImGui::RadioButton("Toutes", show_ == "all")) {
            show_ = "all";
            load(app);
          }
        }
        ImGui::SameLine();
      }
      if (button(loading_ ? "Chargement..." : "Actualiser"))
        load(app);
      if (can_create_) {
        ImGui::SameLine();
        if (primary_button(show_ == "templates" ? "Nouveau modèle" : "Nouvelle liste"))
          start_edit(app, nullptr);
      }
      // « Partir avec » (tout le monde) : liste de depart generee depuis un type de lot
      if (show_ != "templates") {
        ImGui::SameLine();
        if (button("Partir avec un lot")) {
          departure_pick_.clear();
          Json body;
          body["user"] = app.user_ref();
          app.api.post("/api/checklists/departure/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Partir avec : " + result.error, true);
              return;
            }
            departure_ = result.data["lot_types"];
            for (Json &row : departure_.items())
              row["label"] = row["name"].str() + " — " + (row["available"].integer() > 0
                               ? std::to_string(row["available"].integer()) + " disponibles, dont "
                                     + std::to_string(row["available_ok"].integer()) + " valides"
                               : std::to_string(row["lines"].integer()) + " lignes à prendre");
          });
        }
      }
      if (departure_.size() > 0) {
        ImGui::TextUnformatted("Partir avec :");
        ImGui::SameLine();
        if (search_select("departure", departure_, "type", "label", departure_pick_, "Tapez le type de lot (lot C, B+…)",
                          nullptr, 420.0f)
            && !departure_pick_.empty()) {
          Json body;
          body["user"]     = app.user_ref();
          body["lot_type"] = std::exchange(departure_pick_, {});
          departure_       = Json::array();
          app.api.post("/api/checklists/departure/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Partir avec : " + result.error, true);
              return;
            }
            app.notify("Liste « " + result.data["name"].str() + " » créée : scannez l'étiquette de chaque lot pris.");
            load(app);
            select(app, result.data["id"].str());
          });
        }
        ImGui::SameLine();
        if (small_button("Annuler##departure"))
          departure_ = Json::array();
      }
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (can_create_)
        ImGui::TextWrapped("Une liste sert à demander à des secouristes de prendre du matériel précis pour un poste en "
                           "particulier : les lots et le matériel à emporter, avec les quantités. Elle se partage par "
                           "lien ou QR code et se vérifie en direct, à plusieurs. Validée, son matériel sort du stock. "
                           "Un modèle se duplique pour les postes qui reviennent souvent. Ajoutez des secouristes à "
                           "une liste pour qu'elle apparaisse dans leurs listes.");
      else
        ImGui::TextWrapped("Les listes de matériel qui vous sont attribuées par un chef, ou que vous avez ouvertes par "
                           "leur lien ou leur QR code. Scannez le matériel demandé pour la vérifier, à plusieurs.");
      ImGui::PopStyleColor();

      if (editing_) {
        draw_editor(app);
        return;
      }
      if (!list_id_.empty() && !busy_ && ImGui::GetTime() >= next_poll_)
        load_detail(app);
      if (scanning_ && !scannable())
        stop_scan(app);

      const float table_height = list_id_.empty() ? 0.0f : std::max(ImGui::GetFrameHeightWithSpacing() * 5.0f,
                                                                    ImGui::GetContentRegionAvail().y * 0.28f);
      if (ImGui::BeginTable("lists", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, table_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Liste", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Par", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Lignes", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (const Json &row : lists_.items()) {
          const std::string id = row["id"].str();
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::PushID(id.c_str());
          if (ImGui::Selectable(row["name"].str().c_str(), id == list_id_, ImGuiSelectableFlags_SpanAllColumns))
            select(app, id);
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::TextColored(status_color(row), "%s", row["status_label"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row["owner_name"].str().c_str());
          ImGui::TableNextColumn();
          if (row["template"].boolean())
            ImGui::Text("%s", nb(row["line_count"].integer(), "ligne").c_str());
          else
            ImGui::Text("%d/%d %s", row["done_count"].integer(), row["line_count"].integer(),
                        pl(row["line_count"].integer(), "prête").c_str());
        }
        ImGui::EndTable();
      }
      if (lists_.size() == 0 && !loading_)
        ImGui::TextDisabled(show_ == "templates" ? "Aucun modèle." : "Aucune liste.");
      draw_detail(app);
    }

  private:
    bool scannable() const {
      const std::string status = list_["status"].str();
      return !list_.is_null() && !list_["template"].boolean() && (status == "draft" || status == "validated");
    }

    // la ligne de la liste ouverte dans le tableau suit sa relecture (avancement, etat)
    void set_list(const Json &list) {
      list_ = list;
      for (Json &row : lists_.items())
        if (row["id"].str() == list["id"].str())
          for (const char *key : { "name", "status", "status_label", "template", "done_count", "line_count", "updated" })
            row[key] = list[key];
    }

    void select(App &app, const std::string &id) {
      if (id != list_id_) {
        stop_scan(app);
        list_    = Json();
        message_.clear();
      }
      list_id_   = id;
      editing_   = false;
      next_poll_ = 0.0;
    }

    void start_scan(App &app) {
      scanning_          = true;
      app.checklist_scan = { list_id_, [this, &app](const ParsedScan &scan, ScanSource source) {
                              scan_code(app, scan, source);
                            } };
    }

    void stop_scan(App &app) {
      scanning_ = false;
      if (app.checklist_scan.id == list_id_ || list_id_.empty())
        app.checklist_scan = {};
    }

    void scan_code(App &app, const ParsedScan &scan, ScanSource source) {
      const bool back = list_["status"].str() == "validated";
      Json       body;
      body[scan.kind == ScanKind::Lot ? "lot" : "item"] = scan.id;
      call(app, back ? "return-scan" : "scan", body, [this, &app, source](const ApiResult &result) {
        if (!result.ok) {
          app.feedback.error(source, app.settings);
          message_     = result.error;
          message_bad_ = true;
          return;
        }
        if (result.data.contains("ask_add") && !result.data["ask_add"].is_null()) {
          // lot hors de la liste : « Ajouter ce lot à la liste ? » (ou a la place du lot du meme type pas coche)
          app.feedback.notice(source, app.settings);
          ask_add_      = result.data["ask_add"];
          open_ask_add_ = true;
          message_.clear();
          return;
        }
        message_     = result.data["message"].str("Scanné");
        message_bad_ = false;
      });
    }

    // Reponse a ask_add d'un scan (dessinee dans le detail de la liste, ou le popup est ouvert)
    void draw_ask_add(App &app) {
      if (open_ask_add_) {
        ImGui::OpenPopup("Lot hors de la liste###ask_add");
        open_ask_add_ = false;
      }
      // au centre de l'ecran (le scan vient de la douchette, pas d'un clic) : on ne la rate pas
      ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      if (!ImGui::BeginPopupModal("Lot hors de la liste###ask_add", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
      const Json       &ask      = ask_add_;
      const std::string name     = ask["unit_name"].str();
      const bool        replaces = !ask["replaces"].is_null();
      const std::string scanned  = ask["lot_name"].str() != name ? " (étiquette de " + ask["lot_name"].str() + ")" : "";
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
      if (replaces) {
        const std::string old = ask["replaces"]["lot_name"].str();
        ImGui::TextUnformatted(("Prendre " + name + " à la place de " + old + " ?").c_str());
        ImGui::TextUnformatted((name + scanned + " n'est pas dans la liste. " + old + ", du même type, n'est pas encore "
                                "coché : " + name + " le remplace et il est coché.").c_str());
      } else {
        ImGui::TextUnformatted(("Ajouter " + name + " à la liste ?").c_str());
        ImGui::TextUnformatted((name + scanned + " n'est pas dans la liste. Il y est ajouté et coché.").c_str());
      }
      ImGui::PopTextWrapPos();
      const std::string label = replaces ? "Remplacer " + ask["replaces"]["lot_name"].str() : std::string("Ajouter à la liste");
      if (primary_button(label.c_str())) {
        Json body;
        body["lot"] = ask["lot"].str();
        call(app, "add-lot", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            message_     = result.error;
            message_bad_ = true;
            app.notify("Liste : " + result.error, true);
            return;
          }
          message_     = result.data["message"].str("Ajouté");
          message_bad_ = false;
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (button("Annuler"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    void draw_detail(App &app) {
      if (list_id_.empty())
        return;
      if (list_.is_null()) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", error_.empty() ? "Chargement..." : error_.c_str());
        return;
      }
      const Json        list   = list_; // copie : la liste peut etre relue pendant le dessin
      const std::string status = list["status"].str();
      const bool        draft  = status == "draft" && !list["template"].boolean();
      ImGui::SeparatorText(list["name"].str().c_str());
      ImGui::TextColored(status_color(list), "%s", list["status_label"].str().c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("par %s · modifiée %s", list["owner_name"].str().c_str(),
                          display_datetime(list["updated"]).c_str());
      if (!list["note"].str().empty())
        ImGui::TextWrapped("%s", list["note"].str().c_str());

      const bool  share   = draft || list["template"].boolean();
      const float qr_side = share ? 170.0f : 0.0f;
      ImGui::BeginGroup();
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - qr_side - 16.0f);
      if (list["template"].boolean()) {
        ImGui::TextWrapped("Modèle : dupliquez-le pour faire une liste à vérifier.");
        if (list["can_duplicate"].boolean() && primary_button("Utiliser ce modèle"))
          duplicate(app, false);
      } else if (draft) {
        if (list["complete"].boolean())
          ImGui::TextColored(colors::green, "Tout est pris : cliquez « Prêt au départ ».");
        else
          ImGui::TextWrapped("%d/%s. Scannez l'étiquette des lots et le matériel pris dans le stock, cochez les tâches.",
                             list["done_count"].integer(),
                             nb(list["line_count"].integer(), "ligne prête", "lignes prêtes").c_str());
        draw_scan_toggle(app, "Scanner vers cette liste");
        ImGui::SameLine();
        draw_validate(app, list);
        draw_ask_add(app);
      } else if (status == "validated") {
        // ecran du retour : bandeau marine, progression de ce qui est rentre (comme le front web)
        const std::string out = "Sortie " + display_on(list["validated"])
                              + (list["validated_by"].str().empty() ? "" : " par " + list["validated_by"].str());
        status_banner("RETOUR DE POSTE : " + (scanning_ ? std::string("scannez ce qui rentre, il est remis en stock.")
                                                        : out + ". Scannez ce qui rentre."),
                      return_color, 1.25f);
        const ReturnSplit split = return_split(list);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colors::green);
        const std::string progress = (split.pending.empty() ? "✓ Tout est rentré " : "Rentré ")
                                   + std::to_string(split.back) + "/" + std::to_string(split.out);
        ImGui::ProgressBar(split.out ? float(split.back) / float(split.out) : 1.0f, ImVec2(-1.0f, 0.0f), progress.c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled(split.pending.empty() ? "Cliquez « Clôturer le retour » pour terminer."
                                                  : "Ce qui rentre est remis en stock. À la clôture, les consommables pas "
                                                    "rentrés sont comptés utilisés.");
        draw_scan_toggle(app, "Retour : scanner ce qui rentre");
        ImGui::SameLine();
        const Json &preview  = list["return_preview"];
        const bool  all_back = preview["lots"].size() == 0 && preview["used"].size() == 0 && preview["still_out"].size() == 0;
        if (confirm_action("Clôturer le retour", return_question(preview), "confirm_return", !all_back,
                           all_back ? "Clôturer le retour" : "Clôturer sans tout"))
          close_return(app);
        draw_reopen(app, list);
      } else {
        // status_label : Terminée (rien de sorti), Validée (sortie sans retour a scanner), Rentrée (ancien retour)
        const bool reopenable = list["can_reopen"].boolean();
        ImGui::TextColored(colors::green, "%s %s%s. %s", list["status_label"].str().c_str(),
                           display_on(list["returned"]).c_str(),
                           list["returned_by"].str().empty() ? "" : (" par " + list["returned_by"].str()).c_str(),
                           reopenable ? "" : "La liste ne sert plus : dupliquez-la pour la refaire.");
        draw_reopen(app, list);
      }
      if (scanning_ && !message_.empty())
        ImGui::TextColored(message_bad_ ? colors::red : colors::green, "%s", message_.c_str());
      // gestion
      bool same_line = false;
      if (list["can_edit"].boolean() && draft) {
        if (button("Modifier"))
          start_edit(app, &list);
        same_line = true;
      }
      if (list["can_duplicate"].boolean()) {
        if (!list["template"].boolean()) {
          if (same_line)
            ImGui::SameLine();
          if (button("Dupliquer"))
            duplicate(app, false);
        }
        if (same_line || !list["template"].boolean())
          ImGui::SameLine();
        if (button(list["template"].boolean() ? "Dupliquer le modèle" : "En faire un modèle"))
          duplicate(app, true);
        same_line = true;
      }
      if (list["can_edit"].boolean() && status != "validated") {
        if (same_line)
          ImGui::SameLine();
        if (confirm_button("Supprimer", "Supprimer cette liste ?", "confirm_delete"))
          call(app, "delete", Json::object(), [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Liste : " + result.error, true);
              return;
            }
            app.notify("Liste supprimée.");
            stop_scan(app);
            list_id_.clear();
            list_ = Json();
            load(app);
          });
      }
      if (!list["template"].boolean())
        draw_members(app, list);
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      if (share) {
        // lien et QR code : la liste s'ouvre sur le telephone (front web) pour la verifier en direct
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - qr_side + ImGui::GetCursorPosX() - 8.0f);
        ImGui::BeginGroup();
        draw_qr(list["url"].str(), qr_side);
        if (small_button("Copier le lien"))
          ImGui::SetClipboardText(list["url"].str().c_str());
        ImGui::SetItemTooltip("%s", list["url"].str().c_str());
        ImGui::EndGroup();
      }
      if (status == "validated" && !list["template"].boolean())
        draw_return_lines(list);
      else
        draw_lines(app, list);
    }

    // Liste sortie : ce qui est parti, compte un par un (un lot, un item), en « Pas encore rentré » ou « Rentré » ;
    // plus les cases et compteurs du depart (tout vert, on croyait tout fait). Taches et lignes pas prises : rien a
    // rentrer.
    struct ReturnEntry {
      std::string                name;
      std::string                detail;
      int                        count = 0; // items (0 : un lot)
      std::vector< std::string > scans;     // une ligne par item (date de peremption)
    };
    struct ReturnSplit {
      int                        out  = 0;
      int                        back = 0;
      std::vector< ReturnEntry > pending;
      std::vector< ReturnEntry > returned;
    };
    static constexpr ImVec4 return_color = ImVec4(0.0f, 0.298f, 0.596f, 1.0f); // bleu de la marque #004c98

    static ReturnSplit return_split(const Json &list) {
      std::vector< std::string > back_iids, back_lots;
      for (const Json &scan : list["back"].items()) {
        if (!scan["iid"].str().empty())
          back_iids.push_back(scan["iid"].str());
        if (!scan["lot"].str().empty())
          back_lots.push_back(scan["lot"].str());
      }
      const auto has = [](const std::vector< std::string > &ids, const std::string &id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
      };
      ReturnSplit split;
      const auto  lot = [&](const std::string &name, const std::string &detail, bool home) {
        split.out += 1;
        split.back += home ? 1 : 0;
        (home ? split.returned : split.pending).push_back({ name, detail, 0, {} });
      };
      for (const Json &line : list["lines"].items()) {
        const std::string kind = line["kind"].str();
        if (kind == "task" || line["scanned"].integer() == 0)
          continue;
        if (kind == "lot") {
          lot(line["lot_name"].str(), "lot · " + line["lot_type_name"].str(), line["back"].boolean());
          continue;
        }
        if (kind == "lot_type") {
          for (const Json &scan : line["scans"].items())
            lot(scan["lot_name"].str(), "lot · " + line["type_name"].str(), has(back_lots, scan["lot"].str()));
          continue;
        }
        ReturnEntry gone{ line["type_name"].str(), "", 0, {} }, home{ line["type_name"].str(), "", 0, {} };
        int         total = 0;
        for (const Json &scan : line["scans"].items()) {
          ReturnEntry &entry = has(back_iids, scan["iid"].str()) ? home : gone;
          entry.count += 1;
          total += 1;
          entry.scans.push_back(scan["iid"].str()
                                + (scan["peremption"].is_null() ? " · non périssable"
                                                                : " · périme le " + display_date(scan["peremption"])));
        }
        split.out += total;
        split.back += home.count;
        if (gone.count) {
          gone.detail = gone.count == total ? nb(total, "sorti") : std::to_string(gone.count) + " sur " + nb(total, "sorti");
          split.pending.push_back(gone);
        }
        if (home.count) {
          home.detail = (home.count == total ? std::string() : std::to_string(home.count) + " sur "
                                                                   + std::to_string(total) + " · ")
                      + "remis à sa place";
          split.returned.push_back(home);
        }
      }
      return split;
    }

    void draw_return_lines(const Json &list) {
      const ReturnSplit split = return_split(list);
      if (!split.out) {
        ImGui::TextDisabled("Rien n'est sorti avec cette liste.");
        return;
      }
      if (!ImGui::BeginChild("return", ImVec2(0, 0)))
        return ImGui::EndChild();
      const auto count = [](const std::vector< ReturnEntry > &entries) {
        int total = 0;
        for (const ReturnEntry &entry : entries)
          total += entry.count ? entry.count : 1;
        return total;
      };
      const auto table = [&](const char *id, const std::vector< ReturnEntry > &entries, bool home) {
        if (entries.empty())
          return;
        if (home)
          ImGui::TextColored(colors::green, "✓ Rentré : %d", count(entries));
        else
          ImGui::SeparatorText(("Pas encore rentré : " + std::to_string(count(entries))).c_str());
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
          return;
        ImGui::TableSetupColumn("nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("nombre", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        for (const ReturnEntry &entry : entries) {
          ImGui::TableNextRow();
          if (home)
            row_color(colors::green, 0.20f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(entry.name.c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", entry.detail.c_str());
          ImGui::TableNextColumn();
          if (entry.count)
            ImGui::Text("×%d", entry.count);
          else if (home)
            ImGui::TextColored(colors::green, "✓");
          for (const std::string &scan : entry.scans) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent();
            ImGui::TextDisabled("%s%s", scan.c_str(), home ? " · ✓ rentré" : "");
            ImGui::Unindent();
            ImGui::TableNextColumn();
          }
        }
        ImGui::EndTable();
      };
      table("pending", split.pending, false);
      if (!split.pending.empty() && !split.returned.empty())
        ImGui::Spacing();
      if (!split.returned.empty() && split.pending.empty())
        ImGui::SeparatorText("Tout est rentré");
      table("returned", split.returned, true);
      ImGui::EndChild();
    }

    // Lots de la liste en vérif recommandee (rentres d'un poste, marques utilises, reassortis) : lots fixes (pris ou
    // non, sauf taken_only) et lots au choix deja pris. Tant que la liste est a preparer, « Lancer la vérif » incite a
    // la faire avant de partir, dans la liste et dans « Prêt au départ ? », comme sur le web.
    static std::vector< std::pair< std::string, std::string > > verif_lots(const Json &list, bool taken_only) {
      std::vector< std::pair< std::string, std::string > > lots;
      for (const Json &line : list["lines"].items()) {
        if (line["kind"].str() == "lot" && line["verif_recommended"].boolean() && (!taken_only || line["scanned"].integer() > 0))
          lots.emplace_back(line["lot"].str(), line["lot_name"].str());
        if (line["kind"].str() == "lot_type")
          for (const Json &scan : line["scans"].items())
            if (scan["verif_recommended"].boolean())
              lots.emplace_back(scan["lot"].str(), scan["lot_name"].str());
      }
      return lots;
    }

    // la vérif s'ouvre sur le lot ; les scans ne vont plus vers la liste, qui reste ouverte pour la suite du depart
    void verif_from_list(App &app, const std::string &lot_id) {
      stop_scan(app);
      app.start_verif(lot_id, "", app.lot_label_scanned(lot_id));
    }

    // « Prêt au départ » (ou « Partir sans tout ») : lots pris en vérif recommandee, la vérif d'abord (boutons en
    // premier), partir reste possible
    void draw_validate(App &app, const Json &list) {
      const bool  complete = list["complete"].boolean();
      const auto  verifs   = verif_lots(list, true);
      const char *label    = complete ? "Prêt au départ" : "Partir sans tout";
      if (complete ? primary_button(label) : warning_button(label))
        ImGui::OpenPopup("confirm_validate");
      if (!ImGui::BeginPopup("confirm_validate"))
        return;
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
      ImGui::TextUnformatted(complete ? "Prêt au départ ? Le matériel scanné sort du stock. Au retour, scannez ce qui "
                                        "rentre et clôturez le retour : les lots pris passent alors en vérif recommandée."
                                      : "Liste incomplète : partir quand même ? Ce qui manque ne sera pas pris. Le "
                                        "matériel scanné sort du stock ; les lots pris passent en vérif recommandée au "
                                        "retour.");
      if (!verifs.empty()) {
        std::string names;
        for (const auto &[id, name] : verifs)
          names += (names.empty() ? "" : ", ") + name;
        ImGui::TextColored(colors::orange, "⚠ Vérif recommandée avant de partir : %s. Faites-la maintenant, la liste "
                                           "reste ouverte pour la suite.", names.c_str());
      }
      ImGui::PopTextWrapPos();
      for (const auto &[id, name] : verifs) {
        ImGui::PushID(id.c_str());
        if (primary_button(verifs.size() > 1 ? ("Lancer la vérif de " + name).c_str() : "Lancer la vérif")) {
          ImGui::CloseCurrentPopup();
          verif_from_list(app, id);
        }
        ImGui::PopID();
      }
      const char *go      = !verifs.empty() ? (complete ? "Partir sans la vérif" : "Partir sans tout ni la vérif")
                                            : "Confirmer";
      const bool  pressed = complete && verifs.empty() ? primary_button(go) : warning_button(go);
      if (pressed) {
        ImGui::CloseCurrentPopup();
        validate(app, !complete);
      }
      ImGui::SameLine();
      if (button("Annuler"))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // Secouristes attribues a la liste : elle apparait dans leurs listes. Qui modifie la liste les ajoute (recherche
    // par prenom) ou les retire ; un secouriste qui ouvre le lien ou le QR code de la liste s'y ajoute.
    void draw_members(App &app, const Json &list) {
      const bool can_edit = list["can_edit"].boolean();
      if (!can_edit && list["members"].size() == 0)
        return;
      ImGui::SeparatorText("Secouristes");
      if (list["members"].size() == 0)
        ImGui::TextDisabled("Aucun secouriste : ajoutez-en pour que la liste apparaisse dans leurs listes.");
      bool first = true;
      for (const Json &member : list["members"].items()) {
        if (!first)
          ImGui::SameLine();
        first = false;
        ImGui::PushID(member["matricule"].str().c_str());
        ImGui::TextUnformatted(member["name"].str().c_str());
        if (can_edit) {
          ImGui::SameLine(0.0f, 4.0f);
          if (small_button("x"))
            change_members(app, "remove", member["matricule"].str(), member["name"].str());
          ImGui::SetItemTooltip("Retirer de la liste");
        }
        ImGui::PopID();
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
      }
      if (!can_edit)
        return;
      ImGui::SetNextItemWidth(260.0f);
      if (ImGui::InputTextWithHint("##member", "Ajouter un secouriste : prénom", &member_query_))
        member_search_at_ = ImGui::GetTime() + 0.25;
      if (member_search_at_ > 0.0 && ImGui::GetTime() >= member_search_at_) {
        member_search_at_ = 0.0;
        search_members(app);
      }
      if (member_query_.empty())
        return;
      int shown = 0;
      for (const Json &user : member_results_.items()) {
        const std::string matricule = user["matricule"].str();
        bool              taken     = false;
        for (const Json &member : list["members"].items())
          taken = taken || member["matricule"].str() == matricule;
        if (taken)
          continue;
        ++shown;
        ImGui::PushID(matricule.c_str());
        if (ImGui::Selectable(user["name"].str().c_str(), false, 0, ImVec2(260.0f, 0.0f))) {
          change_members(app, "add", matricule, user["name"].str());
          member_query_.clear();
          member_results_ = Json::array();
        }
        ImGui::PopID();
      }
      if (shown == 0 && !member_searching_)
        ImGui::TextDisabled("Aucun secouriste");
    }

    void search_members(App &app) {
      Json body;
      body["user"]      = app.user_ref();
      body["q"]         = member_query_;
      member_searching_ = true;
      const int request = ++member_request_;
      app.api.post("/api/checklists/users/", body, [this, &app, request](const ApiResult &result) {
        if (request != member_request_)
          return;
        member_searching_ = false;
        if (!result.ok) {
          app.notify("Secouristes : " + result.error, true);
          return;
        }
        member_results_ = result.data["users"];
      });
    }

    void change_members(App &app, const char *action, const std::string &matricule, const std::string &name) {
      Json body;
      Json users = Json::array();
      users.push_back(matricule);
      body[action]         = users;
      const bool adding    = std::string(action) == "add";
      call(app, "members", body, [&app, adding, name](const ApiResult &result) {
        if (!result.ok)
          app.notify("Liste : " + result.error, true);
        else
          app.notify(adding ? "Liste attribuée à " + name + "." : name + " retiré de la liste.");
      });
    }

    void draw_scan_toggle(App &app, const char *label) {
      if (scanning_) {
        if (warning_button("Arrêter le scan"))
          stop_scan(app);
        ImGui::SameLine();
        ImGui::TextColored(colors::green, "● Les scans vont dans cette liste");
      } else if (primary_button(label)) {
        start_scan(app);
      }
    }

    void draw_lines(App &app, const Json &list) {
      const bool draft = list["status"].str() == "draft" && !list["template"].boolean();
      const bool out   = list["status"].str() != "draft";
      std::vector< std::string > back;
      for (const Json &scan : list["back"].items())
        if (!scan["iid"].str().empty())
          back.push_back(scan["iid"].str());
      if (!ImGui::BeginTable("lines", out ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                                                       | ImGuiTableFlags_ScrollY))
        return;
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("À prendre", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Demandé", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Pris", ImGuiTableColumnFlags_WidthFixed, 190.0f);
      if (out)
        ImGui::TableSetupColumn("Rentré", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableHeadersRow();
      // lignes a traiter en haut (ordre de la liste a egalite) : a preparer, ce qui n'est pas encore pris ou coche ;
      // sortie, ce qui est pris et pas encore rentre. Un modele ou une liste rentree garde son ordre.
      const bool validated = list["status"].str() == "validated" && !list["template"].boolean();
      const auto todo      = [&](const Json &line) {
        const int took = line["scanned"].integer();
        if (draft) {
          // vérif recommandee sur un lot de la ligne : a faire avant de partir, en haut aussi
          bool verif = line["verif_recommended"].boolean();
          for (const Json &scan : line["scans"].items())
            verif = verif || scan["verif_recommended"].boolean();
          return line["kind"].str() == "task" ? took == 0 : took < line["quantity"].integer() || verif;
        }
        if (!validated || line["kind"].str() == "task" || took == 0)
          return false;
        return line["kind"].str() == "lot" ? !line["back"].boolean() : line["back"].integer() < took;
      };
      std::vector< const Json * > lines;
      for (const Json &line : list["lines"].items())
        lines.push_back(&line);
      std::stable_sort(lines.begin(), lines.end(), [&](const Json *a, const Json *b) { return todo(*a) && !todo(*b); });
      for (const Json *line_row : lines) {
        const Json &line = *line_row;
        ImGui::PushID(line["id"].integer());
        if (line["kind"].str() == "task") {
          // tache libre (ni lot ni item) : cochee a la verif de la liste
          bool checked = line["scanned"].integer() > 0;
          ImGui::TableNextRow();
          if (checked)
            row_color(colors::green, 0.20f);
          ImGui::TableNextColumn();
          ImGui::BeginDisabled(!draft || busy_);
          if (ImGui::Checkbox(line["task"].str().c_str(), &checked)) {
            Json body;
            body["line"] = line["id"].integer();
            body["done"] = checked;
            call(app, "check", body, [&app](const ApiResult &result) {
              if (!result.ok)
                app.notify("Liste : " + result.error, true);
            });
          }
          ImGui::EndDisabled();
          ImGui::TableNextColumn();
          ImGui::TextDisabled("tâche");
          ImGui::TableNextColumn();
          if (checked)
            ImGui::TextDisabled("fait%s", line["done_by"].str().empty() ? "" : (" · " + line["done_by"].str()).c_str());
          if (out)
            ImGui::TableNextColumn();
          ImGui::PopID();
          continue;
        }
        const bool lot  = line["kind"].str() == "lot";
        const int  took = line["scanned"].integer();
        const bool done = took >= line["quantity"].integer();
        ImGui::TableNextRow();
        if (done)
          row_color(colors::green, 0.20f);
        ImGui::TableNextColumn();
        const bool lot_type = line["kind"].str() == "lot_type"; // n'importe quel lot de ce type
        ImGui::TextUnformatted((lot ? line["lot_name"] : line["type_name"]).str().c_str());
        ImGui::SameLine();
        // sac d'un B+ de referentiel (sans etiquette) : chaque sac se coche par son etiquette
        const std::string part_of = line["part_of_name"].str();
        ImGui::TextDisabled("%s", lot ? (part_of.empty() ? "lot · " + line["lot_type_name"].str()
                                                         : line["lot_type_name"].str() + " · dans " + part_of).c_str()
                                      : lot_type ? "lot au choix" : "matériel");
        if (lot && draft && line["verif_recommended"].boolean()) {
          ImGui::TextColored(colors::orange, "⚠ Vérif recommandée avant de partir");
          ImGui::SameLine();
          if (primary_button("Lancer la vérif"))
            verif_from_list(app, line["lot"].str());
        }
        if (lot_type && !done && line["available"].size() > 0) {
          std::string available;
          for (const Json &row : line["available"].items())
            available += (available.empty() ? "" : ", ") + row["name"].str()
                       + (row["state"]["kind"].str() == "ok" ? "" : " (" + row["state"]["label"].str() + ")");
          ImGui::TextDisabled("  disponibles : %s", available.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", line["quantity"].integer());
        ImGui::TableNextColumn();
        if (lot)
          ImGui::TextUnformatted(took ? "✓ coché" : "–");
        else
          ImGui::Text("%d/%d", took, line["quantity"].integer());
        if (lot && took && draft) {
          ImGui::SameLine();
          if (small_button("Décocher")) {
            Json body;
            body["lot"] = line["lot"].str();
            unscan(app, body);
          }
        } else if (lot && draft && !part_of.empty() && list["can_edit"].boolean()) {
          // sac d'un B+ choisi d'un bloc : le B+ est separe, ce sac retire de la liste
          ImGui::SameLine();
          if (small_button("Retirer")) {
            Json body;
            body["lot"] = line["lot"].str();
            call(app, "remove-part", body, [&app](const ApiResult &result) {
              app.notify(result.ok ? result.data["message"].str() : "Liste : " + result.error, !result.ok);
            });
          }
        }
        if (out) {
          ImGui::TableNextColumn();
          if (lot)
            ImGui::TextUnformatted(!took ? "" : line["back"].boolean() ? "✓ rentré" : "pas rentré");
          else if (took)
            ImGui::Text("%d/%d", line["back"].integer(), took);
        }
        for (const Json &scan : line["scans"].items()) {
          if (lot)
            continue;
          if (lot_type) {
            const std::string lot_id = scan["lot"].str();
            bool              returned = false;
            for (const Json &row : list["back"].items())
              returned = returned || row["lot"].str() == lot_id;
            if (scan.contains("returned")) // B+ de referentiel : rentre quand tous ses sacs le sont
              returned = scan["returned"].boolean();
            std::string missing; // B+ de referentiel commence : les sacs qui restent a scanner
            for (const Json &name : scan["missing"].items())
              missing += (missing.empty() ? " · reste à scanner : " : ", ") + name.str();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Indent();
            ImGui::TextDisabled("%s · %s%s", scan["lot_name"].str().c_str(), scan["by"].str("?").c_str(), missing.c_str());
            if (draft && scan["verif_recommended"].boolean()) {
              ImGui::SameLine();
              ImGui::TextColored(colors::orange, "⚠ vérif recommandée");
              ImGui::SameLine();
              ImGui::PushID(lot_id.c_str());
              if (primary_button("Lancer la vérif"))
                verif_from_list(app, lot_id);
              ImGui::PopID();
            }
            ImGui::Unindent();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (draft) {
              ImGui::PushID(lot_id.c_str());
              if (small_button("Retirer")) {
                Json body;
                body["lot"] = lot_id;
                unscan(app, body);
              }
              ImGui::PopID();
            }
            if (out) {
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(returned ? "✓ rentré" : "pas rentré");
            }
            continue;
          }
          const std::string iid      = scan["iid"].str();
          const bool        returned = std::find(back.begin(), back.end(), iid) != back.end();
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Indent();
          ImGui::TextDisabled("%s%s · %s", iid.c_str(),
                              scan["peremption"].is_null() ? "" : (" · périme le " + display_date(scan["peremption"])).c_str(),
                              scan["by"].str("?").c_str());
          ImGui::Unindent();
          ImGui::TableNextColumn();
          ImGui::TableNextColumn();
          if (draft) {
            ImGui::PushID(iid.c_str());
            if (small_button("Retirer")) {
              Json body;
              body["item"] = iid;
              unscan(app, body);
            }
            ImGui::PopID();
          }
          if (out) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(returned ? "✓ rentré" : scan["status"].str() == "missing" ? "utilisé" : "");
          }
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    // Editeur : nouvelle liste (list nul) ou liste a preparer
    void start_edit(App &app, const Json *list) {
      stop_scan(app);
      editing_       = true;
      edit_id_       = list ? (*list)["id"].str() : "";
      edit_name_     = list ? (*list)["name"].str() : "";
      edit_note_     = list ? (*list)["note"].str() : "";
      edit_template_ = list ? (*list)["template"].boolean() : show_ == "templates";
      edit_started_  = false;
      edit_lines_    = Json::array();
      if (list)
        for (const Json &line : (*list)["lines"].items()) {
          Json row;
          if (line["kind"].str() == "task") {
            row["task"]  = line["task"].str();
            row["label"] = line["task"].str();
            edit_lines_.push_back(row);
            continue;
          }
          edit_started_ = edit_started_ || line["scanned"].integer() > 0;
          if (line["kind"].str() == "lot" && !line["part_of"].str().empty()) {
            // sacs d'un B+ de referentiel : coches a part, mais choisis d'un bloc (le B+), une seule ligne
            bool seen = false;
            for (const Json &other : edit_lines_.items())
              seen = seen || other["lot"].str() == line["part_of"].str();
            if (seen)
              continue;
            row["lot"]   = line["part_of"].str();
            row["label"] = line["part_of_name"].str() + " (" + line["part_of_type_name"].str() + ")";
          } else if (line["kind"].str() == "lot") {
            row["lot"]   = line["lot"].str();
            row["label"] = line["lot_name"].str() + " (" + line["lot_type_name"].str() + ")";
          } else if (line["kind"].str() == "lot_type") {
            row["lot_types"] = line["lot_types"];
            row["label"]     = line["type_name"].str() + " (lot au choix)";
            row["quantity"]  = line["quantity"].integer();
          } else {
            row["type"]     = line["type"].str();
            row["label"]    = line["type_name"].str();
            row["quantity"] = line["quantity"].integer();
          }
          edit_lines_.push_back(row);
        }
      if (options_.is_null()) {
        Json body;
        body["user"] = app.user_ref();
        app.api.post("/api/checklists/options/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify("Lots et types : " + result.error, true);
            return;
          }
          options_ = result.data;
          build_choices();
        });
      }
    }

    void draw_editor(App &app) {
      ImGui::SeparatorText(edit_id_.empty() ? (edit_template_ ? "Nouveau modèle" : "Nouvelle liste") : "Modifier la liste");
      ImGui::SetNextItemWidth(420.0f);
      input_limited("Nom", edit_name_, 80, "Ex : poste de secours du stade");
      ImGui::InputTextMultiline("Remarque", &edit_note_, ImVec2(420.0f, ImGui::GetTextLineHeight() * 3.0f));
      ImGui::BeginDisabled(edit_started_);
      ImGui::Checkbox("Modèle (se duplique, ne se vérifie pas)", &edit_template_);
      ImGui::EndDisabled();
      if (edit_started_)
        ImGui::SetItemTooltip("Liste déjà commencée : dupliquez-la pour en faire un modèle.");

      ImGui::SeparatorText("À prendre");
      int remove = -1;
      if (ImGui::BeginTable("edit_lines", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH,
                            ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Lot ou matériel", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        int index = 0;
        for (Json &line : edit_lines_.items()) {
          ImGui::PushID(index);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(line["label"].str().c_str());
          ImGui::SameLine();
          ImGui::TextDisabled("%s", line.contains("task") ? "tâche à cocher" : line.contains("lot") ? "lot"
                              : line.contains("lot_types") ? "lot au choix" : "matériel");
          ImGui::TableNextColumn();
          if (!line.contains("lot") && !line.contains("task")) {
            int quantity = line["quantity"].integer(1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##quantity", &quantity))
              line["quantity"] = std::clamp(quantity, 1, 999);
          }
          ImGui::TableNextColumn();
          if (small_button("Retirer"))
            remove = index;
          ImGui::PopID();
          ++index;
        }
        ImGui::EndTable();
      }
      if (remove >= 0)
        edit_lines_.items().erase(edit_lines_.items().begin() + remove);
      if (edit_lines_.size() == 0)
        ImGui::TextDisabled("Ajoutez des lots (sacs, armoires), du matériel avec sa quantité et des tâches à cocher.");

      if (options_.is_null()) {
        ImGui::TextDisabled("Chargement des lots et du matériel...");
      } else {
        // une seule recherche pour tout ce qui se scanne : lots precis, lots au choix (« 1 lot B+ », n'importe
        // lequel de ce type, choisi en scannant son etiquette) et materiel (type d'item), voir build_choices
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ajouter");
        ImGui::SameLine(185.0f);
        if (search_select("add_any", choices_, "key", "name", add_pick_, "Rechercher un lot, un type de lot ou du matériel…",
                          nullptr, 480.0f, "code")
            && !add_pick_.empty()) {
          const std::string pick = std::exchange(add_pick_, {});
          if (pick.rfind("lot:", 0) == 0)
            add_line(true, pick.substr(4), 1);
          else if (pick.rfind("type:", 0) == 0)
            add_line(false, pick.substr(5), 1);
          else if (pick.rfind("lots:", 0) == 0)
            add_lot_type(pick.substr(5));
        }
        ImGui::SameLine();
        help_marker("Lot : ce lot-là précisément. Lot au choix : n'importe quel lot de ce type, choisi en scannant "
                    "son étiquette. Matériel : un type d'item, la quantité se règle dans le tableau.");
      }
      // tache libre : ni lot ni item, cochee a la verif (ex : prendre des bonbons)
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Ajouter une tâche");
      ImGui::SameLine(185.0f);
      ImGui::SetNextItemWidth(360.0f);
      const bool enter = input_limited("##add_task", add_task_, 200, "Ex : prendre des bonbons",
                                       ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      ImGui::BeginDisabled(add_task_.empty());
      if ((button("Ajouter la tâche") || enter) && !add_task_.empty()) {
        const std::string task = std::exchange(add_task_, {});
        bool              found = false;
        for (const Json &line : edit_lines_.items())
          found = found || line["task"].str() == task;
        if (!found) {
          Json row;
          row["task"]  = task;
          row["label"] = task;
          edit_lines_.push_back(row);
        }
      }
      ImGui::EndDisabled();
      ImGui::Spacing();
      if (button("Annuler"))
        editing_ = false;
      ImGui::SameLine();
      ImGui::BeginDisabled(busy_ || edit_name_.empty());
      if (primary_button("Enregistrer"))
        save(app);
      ImGui::EndDisabled();
    }

    // Choix de la recherche unique d'ajout, construits une fois a la reception des options (pas a chaque image : il
    // peut y avoir des centaines de types). Le genre de chaque resultat est ecrit entre parentheses, la cle porte
    // un prefixe (lot:, lots:, type:) pour les distinguer ; la description (marques, autres noms ; celle du type
    // pour un lot) est cherchee aussi, apres les noms.
    void build_choices() {
      choices_ = Json::array();
      const auto choice = [&](const std::string &key, const Json &option, const std::string &code) {
        Json row;
        row["key"]         = key;
        row["name"]        = option["name"].str();
        row["code"]        = code;
        row["description"] = option["description"].str();
        choices_.push_back(row);
      };
      for (const Json &option : options_["lot_types"].items())
        choice("lots:" + option["type"].str(), option, "lot au choix, " + option["type"].str());
      for (const Json &option : options_["lots"].items())
        choice("lot:" + option["id"].str(), option,
               option["lot_type_name"].str().empty() ? "lot" : "lot, " + option["lot_type_name"].str());
      for (const Json &option : options_["types"].items())
        choice("type:" + option["type"].str(), option, "matériel, " + option["type"].str());
    }

    // ligne « lot au choix » d'un type de lot : une de plus si la ligne existe deja
    void add_lot_type(const std::string &code) {
      for (Json &line : edit_lines_.items())
        if (line.contains("lot_types") && line["lot_types"].size() == 1 && line["lot_types"][std::size_t{ 0 }].str() == code) {
          line["quantity"] = std::min(99, static_cast< int >(line["quantity"].integer()) + 1);
          return;
        }
      for (const Json &option : options_["lot_types"].items())
        if (option["type"].str() == code) {
          Json row;
          row["lot_types"] = Json::array();
          row["lot_types"].push_back(code);
          row["label"]    = option["name"].str() + " (lot au choix)";
          row["quantity"] = 1;
          edit_lines_.push_back(row);
          return;
        }
    }

    void add_line(bool lot, const std::string &key, int quantity) {
      for (Json &line : edit_lines_.items())
        if (line[lot ? "lot" : "type"].str() == key) {
          if (!lot)
            line["quantity"] = std::min(999, line["quantity"].integer() + quantity);
          return;
        }
      for (const Json &option : options_[lot ? "lots" : "types"].items()) {
        if (option[lot ? "id" : "type"].str() != key)
          continue;
        if (lot && option.contains("unit")) {
          // sac d'un B+ de referentiel (sans etiquette) : le B+ entier (un sac se retire ensuite dans la liste)
          const Json &unit = option["unit"];
          for (const Json &line : edit_lines_.items())
            if (line["lot"].str() == unit["id"].str())
              return;
          Json row;
          row["lot"]   = unit["id"].str();
          row["label"] = unit["name"].str() + " (" + unit["lot_type_name"].str() + ")";
          edit_lines_.push_back(row);
          return;
        }
        Json row;
        row[lot ? "lot" : "type"] = key;
        row["label"] = lot ? option["name"].str() + " (" + option["lot_type_name"].str() + ")" : option["name"].str();
        if (!lot)
          row["quantity"] = quantity;
        edit_lines_.push_back(row);
        return;
      }
    }

    void save(App &app) {
      Json body;
      body["user"]     = app.user_ref();
      body["name"]     = edit_name_;
      body["note"]     = edit_note_;
      body["template"] = edit_template_;
      Json lines       = Json::array();
      for (const Json &line : edit_lines_.items()) {
        Json row;
        if (line.contains("task")) {
          row["task"] = line["task"].str();
        } else if (line.contains("lot")) {
          row["lot"] = line["lot"].str();
        } else if (line.contains("lot_types")) {
          row["lot_types"] = line["lot_types"];
          row["quantity"]  = line["quantity"].integer(1);
        } else {
          row["type"]     = line["type"].str();
          row["quantity"] = line["quantity"].integer(1);
        }
        lines.push_back(row);
      }
      body["lines"] = lines;
      busy_         = true;
      const std::string path = edit_id_.empty() ? "/api/checklists/" : "/api/checklists/" + url_encode(edit_id_) + "/update/";
      app.api.post(path, body, [this, &app](const ApiResult &result) {
        busy_ = false;
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        app.notify("Liste enregistrée.");
        editing_ = false;
        select(app, result.data["id"].str());
        list_ = result.data;
        load(app);
      });
    }

    void call(App &app, const std::string &action, Json body, std::function< void(const ApiResult &) > done) {
      body["user"]          = app.user_ref();
      busy_                 = true;
      const std::string id  = list_id_;
      app.api.post("/api/checklists/" + url_encode(id) + "/" + action + "/", body,
                   [this, id, done](const ApiResult &result) {
                     busy_ = false;
                     if (result.ok && id == list_id_ && result.data["id"].str() == id)
                       set_list(result.data);
                     next_poll_ = ImGui::GetTime() + kPollSeconds;
                     done(result);
                   });
    }

    void unscan(App &app, const Json &body) {
      call(app, "unscan", body, [&app](const ApiResult &result) {
        if (!result.ok)
          app.notify("Liste : " + result.error, true);
      });
    }

    void validate(App &app, bool partial) {
      Json body;
      body["partial"] = partial;
      call(app, "validate", body, [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        stop_scan(app);
        message_.clear();
        app.notify("Liste validée : le matériel est sorti du stock.");
        load(app);
      });
    }

    // « Rouvrir le départ » (validee par erreur, 30 min) : petit lien grise sous les boutons, pas un vrai bouton
    void draw_reopen(App &app, const Json &list) {
      if (!list["can_reopen"].boolean())
        return;
      ImGui::TextDisabled("Validée par erreur ? Rouvrir le départ");
      if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsItemClicked())
        ImGui::OpenPopup("confirm_reopen");
      if (ImGui::BeginPopup("confirm_reopen")) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
        ImGui::TextUnformatted("Rouvrir le départ ? Le matériel sorti revient où il était. La liste redevient à "
                               "préparer, avec ce qui est déjà coché.");
        ImGui::PopTextWrapPos();
        if (primary_button("Rouvrir le départ")) {
          reopen(app);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button("Annuler"))
          ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }

    void reopen(App &app) {
      call(app, "reopen", Json::object(), [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        app.notify("Départ rouvert : la liste est de nouveau à préparer.");
        load(app);
      });
    }

    void close_return(App &app) {
      call(app, "close-return", Json::object(), [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        stop_scan(app);
        message_.clear();
        std::string done;
        const auto  add = [&done](std::size_t count, const char *word, const char *plural) {
          if (count > 0)
            done += (done.empty() ? "" : ", ") + nb(static_cast< long long >(count), word, plural);
        };
        add(result.data["lots"].size(), "lot en vérif recommandée", "lots en vérif recommandée");
        add(result.data["used"].size(), "consommable utilisé", "consommables utilisés");
        add(result.data["still_out"].size(), "matériel encore sorti", "matériels encore sortis");
        app.notify("Retour clôturé : " + (done.empty() ? std::string("tout est rentré") : done) + ".");
        load(app);
      });
    }

    void duplicate(App &app, bool as_template) {
      Json body;
      body["template"] = as_template;
      call(app, "duplicate", body, [this, &app, as_template](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        app.notify(as_template ? "Modèle créé." : "Liste créée : à vérifier.");
        // les modeles ne sont que dans l'onglet Modeles : on va dans l'onglet de la copie
        if (as_template) {
          show_ = "templates";
        } else if (show_ == "templates") {
          show_ = "mine";
        }
        select(app, result.data["id"].str());
        list_ = result.data;
        load(app);
      });
    }

    void load(App &app) {
      Json body;
      body["user"] = app.user_ref();
      body["show"] = show_;
      loading_     = true;
      const int request = ++request_;
      app.api.post("/api/checklists/list/", body, [this, &app, request](const ApiResult &result) {
        if (request != request_)
          return;
        loading_ = false;
        if (!result.ok) {
          app.notify("Listes : " + result.error, true);
          return;
        }
        lists_        = result.data["lists"];
        can_create_   = result.data["can_create"].boolean();
        can_see_all_  = result.data["can_see_all"].boolean();
        if (!can_create_ && show_ != "mine") {
          show_ = "mine"; // secouriste : ni modeles ni « Toutes »
          load(app);
        }
      });
      next_poll_ = 0.0;
    }

    void load_detail(App &app) {
      next_poll_ = ImGui::GetTime() + kPollSeconds;
      Json body;
      body["user"]          = app.user_ref();
      const std::string id  = list_id_;
      // premiere lecture de la liste ouverte (QR code scanne, tableau) : l'ajoute aux listes d'un secouriste
      body["open"]          = opened_ != id;
      const int request     = ++detail_request_;
      app.api.post("/api/checklists/" + url_encode(id) + "/", body, [this, id, request](const ApiResult &result) {
        if (request != detail_request_ || id != list_id_)
          return;
        if (result.ok) {
          opened_ = id;
          set_list(result.data);
          error_.clear();
        } else if (result.status == 404) {
          list_  = Json();
          error_ = "Liste introuvable (supprimée ?)";
        } else if (list_.is_null()) {
          error_ = result.error;
        }
      });
    }

    Json        lists_ = Json::array();
    std::string show_  = "mine";
    bool        can_create_  = false; // chef, gestion, admin (reponse de /api/checklists/list/)
    bool        can_see_all_ = false;
    std::string opened_;              // derniere liste ouverte (lue une premiere fois avec "open")
    // secouristes attribues : recherche par prenom
    std::string member_query_;
    Json        member_results_   = Json::array();
    double      member_search_at_ = 0.0;
    bool        member_searching_ = false;
    int         member_request_   = 0;
    std::string list_id_;
    Json        list_;
    std::string error_;
    bool        scanning_ = false;
    std::string message_;
    bool        message_bad_ = false;
    Json        ask_add_;              // lot hors liste scanne : question « Ajouter ce lot à la liste ? »
    bool        open_ask_add_ = false;
    bool        loading_     = false;
    bool        busy_        = false;
    int         request_     = 0;
    int         detail_request_ = 0;
    double      next_poll_   = 0.0;
    // editeur
    bool        editing_ = false;
    std::string edit_id_;
    std::string edit_name_;
    std::string edit_note_;
    bool        edit_template_ = false;
    bool        edit_started_  = false;
    Json        edit_lines_    = Json::array();
    Json        options_;
    Json        choices_ = Json::array(); // recherche unique d'ajout (build_choices)
    std::string add_pick_; // recherche unique d'ajout : lot:<id>, lots:<type de lot> ou type:<type d'item>
    Json        departure_ = Json::array(); // types proposes par « Partir avec un lot »
    std::string departure_pick_;
    std::string add_task_;
};

} // namespace

std::unique_ptr< AppWindow > make_lists_window() {
  return std::make_unique< ListsWindow >();
}

} // namespace qrprotec
