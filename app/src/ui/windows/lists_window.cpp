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

// Bouton vert (ou orange) avec confirmation en deux temps : confirm_button est reserve aux actions dangereuses
bool confirm_action(const char *label, const char *question, const char *popup_id, bool warning) {
  if (warning ? warning_button(label) : primary_button(label))
    ImGui::OpenPopup(popup_id);
  bool confirmed = false;
  if (ImGui::BeginPopup(popup_id)) {
    ImGui::TextUnformatted(question);
    if (primary_button("Confirmer")) {
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

// Listes de materiel a prendre (ex : un chef demande du materiel a ses equipiers), meme page que « Listes de
// matériel » sur le front web : lots et types d'items avec une quantite, partagees par lien et QR code. Le mode scan
// dirige les scans (douchette, telephone) vers la liste ouverte : verif tant qu'elle est a preparer, retour une fois
// sortie. Validee, son materiel sort du stock ; « Clôturer le retour » compte les consommables pas rentres utilises.
class ListsWindow final : public AppWindow {
  public:
    ListsWindow() : AppWindow("lists", "Listes de matériel", false, true) {}

    void on_open(App &app) override {
      if (app.logged_in())
        load(app);
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
        message_     = result.data["message"].str("Scanné");
        message_bad_ = false;
      });
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
          ImGui::TextColored(colors::green, "Tout est prêt : validez la liste.");
        else
          ImGui::TextWrapped("%d/%s. Scannez l'étiquette des lots et le matériel pris dans le stock, cochez les tâches.",
                             list["done_count"].integer(),
                             nb(list["line_count"].integer(), "ligne prête", "lignes prêtes").c_str());
        draw_scan_toggle(app, "Scanner vers cette liste");
        ImGui::SameLine();
        if (list["complete"].boolean()) {
          if (confirm_action("Valider la liste", "Valider la liste ? Le matériel scanné sort du stock et la liste ne "
                                                 "pourra plus être vérifiée.", "confirm_validate", false))
            validate(app, false);
        } else if (confirm_action("Valider incomplète", "Liste incomplète : la valider quand même ? Ce qui manque ne "
                                                        "sera pas pris. Le matériel scanné sort du stock.",
                                  "confirm_partial", true)) {
          validate(app, true);
        }
      } else if (status == "validated") {
        ImGui::TextColored(colors::orange, "Sortie %s%s.", display_on(list["validated"]).c_str(),
                           list["validated_by"].str().empty() ? "" : (" par " + list["validated_by"].str()).c_str());
        ImGui::TextWrapped("Au retour, scannez ce qui rentre (remis en stock), puis clôturez le retour.");
        draw_scan_toggle(app, "Retour : scanner ce qui rentre");
        ImGui::SameLine();
        if (confirm_action("Clôturer le retour", "Clôturer le retour ? Les consommables pas rentrés seront comptés "
                                                 "utilisés, le matériel non consommable pas rentré reste sorti.",
                           "confirm_return", true))
          close_return(app);
      } else {
        ImGui::TextColored(colors::green, "%s %s%s. La liste ne sert plus : dupliquez-la pour la refaire.",
                           list["went_out"].boolean() ? "Rentrée" : "Terminée", display_on(list["returned"]).c_str(),
                           list["returned_by"].str().empty() ? "" : (" par " + list["returned_by"].str()).c_str());
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
      draw_lines(app, list);
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
        if (draft)
          return line["kind"].str() == "task" ? took == 0 : took < line["quantity"].integer();
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
        ImGui::TextUnformatted((lot ? line["lot_name"] : line["type_name"]).str().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lot ? ("lot · " + line["lot_type_name"].str()).c_str() : "matériel");
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
          if (line["kind"].str() == "lot") {
            row["lot"]   = line["lot"].str();
            row["label"] = line["lot_name"].str() + " (" + line["lot_type_name"].str() + ")";
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
          ImGui::TextDisabled("%s", line.contains("task") ? "tâche à cocher" : line.contains("lot") ? "lot" : "matériel");
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
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ajouter un lot");
        ImGui::SameLine(185.0f);
        if (search_select("add_lot", options_["lots"], "id", "name", add_lot_, "Rechercher un lot…", nullptr, 360.0f)
            && !add_lot_.empty()) {
          add_line(true, std::exchange(add_lot_, {}), 1);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ajouter du matériel");
        ImGui::SameLine(185.0f);
        search_select("add_type", options_["types"], "type", "name", add_type_, "Rechercher un type d'item…", nullptr,
                      360.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("##add_quantity", &add_quantity_);
        add_quantity_ = std::clamp(add_quantity_, 1, 999);
        ImGui::SameLine();
        ImGui::BeginDisabled(add_type_.empty());
        if (button("Ajouter"))
          add_line(false, std::exchange(add_type_, {}), std::exchange(add_quantity_, 1));
        ImGui::EndDisabled();
      }
      // tache libre : ni lot ni item, cochee a la verif (ex : verifier le niveau d'eau dans le sous-sol)
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Ajouter une tâche");
      ImGui::SameLine(185.0f);
      ImGui::SetNextItemWidth(360.0f);
      const bool enter = input_limited("##add_task", add_task_, 200, "Ex : vérifier le niveau d'eau dans le sous-sol",
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

    void close_return(App &app) {
      call(app, "close-return", Json::object(), [this, &app](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Liste : " + result.error, true);
          return;
        }
        stop_scan(app);
        message_.clear();
        const std::size_t still = result.data["still_out"].size();
        app.notify("Retour clos : " + nb(result.data["used"].size(), "consommable utilisé", "consommables utilisés")
                   + (still ? ", " + nb(still, "matériel encore sorti", "matériels encore sortis") : std::string()) + ".");
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
    std::string add_lot_;
    std::string add_type_;
    std::string add_task_;
    int         add_quantity_ = 1;
};

} // namespace

std::unique_ptr< AppWindow > make_lists_window() {
  return std::make_unique< ListsWindow >();
}

} // namespace qrprotec
