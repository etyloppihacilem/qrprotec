/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          verif_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../widgets.hpp"
#include "windows.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace qrprotec {

namespace {

std::string iid_label(const App &app, const std::string &iid) {
  const ParsedScan scan = parse_scan(iid);
  std::string      text = app.catalog.item_type_name(scan.item_type);
  if (scan.peremption)
    text += " - exp. " + scan.peremption->display();
  return text;
}

class VerifWindow final : public AppWindow {
  public:
    VerifWindow() : AppWindow("verif", "Vérif", false, true) {}

    bool can_close(const App &app) const override { return !app.verif.active; }

    void draw(App &app) override {
      if (app.verif.active) {
        draw_leave_prompt(app);
        draw_active(app);
      }
      else if (!app.last_report.is_null())
        draw_report(app);
      else
        ImGui::TextWrapped("Aucune vérif en cours. Scannez l'étiquette d'un lot ou choisissez un lot dans la "
                           "fenêtre Lots pour commencer.");
    }

  private:
    // vérif en cascade : on quitte un sous-lot incomplet (scan d'un autre lot). Le scan suivant vaut « Passer ».
    void draw_leave_prompt(App &app) {
      const LeavePrompt &prompt = app.verif.leave_prompt;
      const char        *title  = "Passer à un autre lot ?###leave-level";
      if (prompt.open && !ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);
      if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
      if (!prompt.open || !app.verif_lot(prompt.level) || !app.verif_lot(prompt.target)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
      }
      const std::string level  = (*app.verif_lot(prompt.level))["name"].str();
      const std::string target = (*app.verif_lot(prompt.target))["name"].str();
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      ImGui::Text("Passer à %s ?", target.c_str());
      ImGui::PopFont();
      ImGui::Text("%s est incomplet : %d item(s) attendu(s) pas encore scanné(s).", level.c_str(), prompt.missing);
      ImGui::Spacing();
      const ImVec2 size(ImGui::GetFontSize() * 30.0f, 0.0f);
      int          choice = -1;
      if (primary_button(("Valider " + level + " incomplet (les non scannés passent manquants)").c_str(), size))
        choice = 0;
      if (button(("Passer sans valider (" + level + " reste à faire, scans gardés)").c_str(), size))
        choice = 1;
      if (button(("Rester dans " + level).c_str(), size) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        choice = 2;
      ImGui::TextDisabled("Scanner autre chose vaut « Passer sans valider ».");
      if (choice >= 0) {
        ImGui::CloseCurrentPopup();
        app.answer_leave(choice);
      }
      ImGui::EndPopup();
    }

    void draw_active(App &app) {
      VerifSession &verif = app.verif;
      const bool    cascade = app.verif_cascade();
      const std::string level = cascade ? app.verif_level() : verif.lot_id;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      if (cascade) {
        // fil du lot global au lot en cours
        std::string title = verif.lot["name"].str(verif.lot_id);
        for (const std::string &id : verif.path)
          if (const Json *lot = app.verif_lot(id))
            title += "  ›  " + (*lot)["name"].str();
        ImGui::Text("Vérif : %s", title.c_str());
      } else {
        ImGui::Text("Vérif : %s", app.verif_title().c_str());
      }
      ImGui::PopFont();
      if (verif.loading) {
        ImGui::TextDisabled("Chargement du contenu du lot...");
        return;
      }
      const std::vector< VerifPlanLot > plan  = app.plan_verif();
      const bool                        multi = !cascade && plan.size() > 1;
      if (!verif.lot["global"].is_null()) {
        const Json &global = verif.lot["global"];
        ImGui::TextDisabled("Lot global %s : %s – vérif la plus ancienne : %s", global["name"].str().c_str(),
                            global["label"].str().c_str(), display_datetime(global["last_verif"]).c_str());
      }
      std::string                   sealed;
      const std::set< std::string > verified = cascade ? app.verif_scope(level) : std::set< std::string >();
      for (const VerifPlanLot &row : plan)
        if ((*row.lot)["is_sealed"].boolean() && (!cascade || verified.count((*row.lot)["id"].str())))
          sealed += (sealed.empty() ? "" : ", ") + (*row.lot)["name"].str();
      if (!sealed.empty())
        status_banner("Scellé (" + sealed + ") : valider cette vérif brisera le scellé", colors::orange, 1.0f);
      // etiquette du scelle scannee : lot scelle compte comme verifie sans etre ouvert (scelle garde)
      const std::set< std::string > intact = app.verif_intact();
      std::string                   kept;
      for (const VerifSealed &lot : verif.sealed)
        if (intact.count(lot.id) || !app.verif_covers(lot.id))
          kept += (kept.empty() ? "" : ", ") + lot.name;
      if (!kept.empty())
        status_banner("Scellé intact (" + kept + ") : compté comme vérifié, sans l'ouvrir", colors::green, 1.0f);
      if (!verif.key.empty() || ((!verif.extras.empty() || !verif.keys.empty()) && app.verif_key_ok()))
        ImGui::TextColored(colors::green, "Étiquette clé scannée : la vérif peut être validée.");
      else if (app.verif_key_ok())
        ImGui::TextColored(colors::green, "Mode gestion : validation autorisée sans étiquette clé.");
      else {
        ImGui::PushStyleColor(ImGuiCol_Text, colors::orange);
        ImGui::TextWrapped("Scannez l'étiquette clé du lot pour pouvoir valider. %s", key_hint(verif.lot).c_str());
        ImGui::PopStyleColor();
      }
      if (cascade) {
        if (draw_cascade(app, level, plan))
          return;
      } else {
        ImGui::TextWrapped("Scannez chaque item du lot : il passe dans la pile et disparaît de la liste ci-dessous. "
                           "Scannez aussi les items périmés que vous retirez, puis leurs remplaçants.%s",
                           multi ? "" : " L'étiquette clé d'un autre lot du même lot global l'ajoute à la vérif.");
        ImGui::TextDisabled("L'étiquette du scellé d'un lot scellé du même lot global le compte comme vérifié sans l'ouvrir.");
      }

      std::set< std::string >      scanned;
      std::map< std::string, int > last_scan; // rang du dernier scan de chaque type d'item
      int                          rank = 0;
      for (const std::string &iid : app.stack.iids()) {
        scanned.insert(iid);
        last_scan[parse_scan(iid).item_type] = rank++;
      }
      // vérif en cascade : lots verifies a la validation du lot en cours, et scans faits dedans
      const std::set< std::string > scope = cascade ? app.verif_scope(level) : std::set< std::string >();
      std::set< std::string >       in_scope; // iids scannes dans ces lots
      for (const ScanEntry &entry : app.stack.entries())
        if (cascade && scope.count(entry.place.empty() ? verif.lot_id : entry.place)) {
          if (entry.scan.kind == ScanKind::Item)
            in_scope.insert(entry.scan.id);
          else
            in_scope.insert(entry.pack_items.begin(), entry.pack_items.end());
        }
      std::set< std::string > expected; // items connus des lots de la verif
      for (const VerifPlanLot &row : plan)
        if (!cascade || scope.count((*row.lot)["id"].str()))
          for (const Json &item : (*row.lot)["items"].items())
            expected.insert(item["iid"].str());
      int remaining = 0;

      ProblemTarget report; // (!) touche dans la liste : signalement apres la table
      const auto item_row = [&](const Json &item) {
        ImGui::TableNextRow();
        if (item["expired"].boolean())
          row_color(colors::red, 0.45f);
        ImGui::TableNextColumn();
        ImGui::Indent();
        ImGui::PushID(item["iid"].str().c_str());
        if (report_mark("##report"))
          report = { item["location"].str(), item["location_name"].str(), item["iid"].str(), "",
                     item["type_name"].str() + " (" + item["iid"].str() + ")" };
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextUnformatted(item["type_name"].str().c_str());
        if (item["disabled"].boolean() && !item["outdated"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::red, "(désactivé : à remplacer)");
        } else if (item["unusable"].boolean() && !item["outdated"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::red, "(signalé inutilisable : à remplacer)");
        } else if (item["revision_late"].boolean() && !item["outdated"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(item["expired"].boolean() ? colors::red : colors::orange,
                             item["expired"].boolean() ? "(révision en retard : inutilisable)" : "(révision en retard)");
        } else if (item["expired"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::red, "(périmé : à remplacer)");
        } else if (item["reported"].boolean()) {
          ImGui::SameLine();
          ImGui::TextColored(colors::orange, "(problème signalé)");
        } else if (item["missed_verifs"].integer() > 0) {
          ImGui::SameLine();
          ImGui::TextDisabled("(non vu à la dernière vérif)");
        }
        ImGui::Unindent();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(display_date(item["peremption"]).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", item["iid"].str().c_str());
      };

      int added = 0, known_scanned = 0;
      for (const std::string &iid : cascade ? in_scope : scanned)
        if (!expected.count(iid))
          ++added;
        else
          ++known_scanned;
      // seulement des items qui ne sont pas dans le lot : probablement un reassort, pas une verif
      const bool                  restock = added > 0 && known_scanned == 0;
      std::vector< const Json * > partial;
      if (!cascade) {
        partial = app.partial_verif_lots();
      } else if (!in_scope.empty()) {
        // vérif partielle du lot en cours : des lots sont complets avec les scans, pas tous
        bool all = true;
        for (const VerifPlanLot &row : plan)
          if (scope.count((*row.lot)["id"].str()))
            all = all && row.complete;
        if (!all)
          for (const VerifPlanLot &row : plan)
            if (scope.count((*row.lot)["id"].str()) && row.complete && row.touched > 0)
              partial.push_back(row.lot);
      }
      const bool                        orange  = restock || !partial.empty();

      // vérif en cascade : un lot qui ne fait que regrouper des sous-lots n'a rien a scanner lui-meme
      const Json &level_lot = cascade ? *app.verif_lot(level) : verif.lot;
      const bool  grouping  = cascade && level_lot["requirements"].size() == 0 && level_lot["items"].size() == 0
                           && !level_lot["storage"].boolean();
      if (!grouping)
        ImGui::SeparatorText("À scanner");
      const float footer = ImGui::GetFrameHeightWithSpacing() * (orange ? 4.4f : 3.2f);
      if (grouping)
        ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - footer)));
      else if (ImGui::BeginTable("expected", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                            ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Péremption", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Scannés / attendus", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();
        for (const VerifPlanLot &plan_row : plan) {
          const Json &lot = *plan_row.lot;
          if (cascade && lot["id"].str() != level)
            continue; // vérif en cascade : le contenu du lot en cours, ses sous-lots sont au-dessus
          // items du lot pas encore scannes, par type
          std::map< std::string, std::vector< const Json * > > known_by_type;
          for (const Json &item : lot["items"].items())
            if (!scanned.count(item["iid"].str()))
              known_by_type[item["type"].str()].push_back(&item);
          // verif groupee : titre de chaque lot (un lot qui ne contient rien et n'attend rien n'est qu'un regroupement)
          if (multi) {
            if (lot["requirements"].size() == 0 && lot["items"].size() == 0)
              continue;
            ImGui::TableNextRow();
            row_color(plan_row.complete ? colors::green : colors::grey, 0.55f);
            ImGui::TableNextColumn();
            ImGui::Indent(plan_row.depth * 16.0f + 1.0f);
            ImGui::Text("%s", lot["name"].str().c_str());
            ImGui::Unindent(plan_row.depth * 16.0f + 1.0f);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(plan_row.complete ? "complet" : "");
            ImGui::TableNextColumn();
          }
          // ordre des types : en cours (orange, dernier type scanne en haut), puis rien de scanne (rouge),
          // puis les autres items du lot, et les types complets (vert) en bas
          struct TypeRow {
              std::string type, type_name, location;
              int         required, done, missing, rank, last_scan;
              bool        storage;
          };
          std::vector< TypeRow >  type_rows;
          std::set< std::string > required_types;
          const auto add_type = [&](const std::string &type, const std::string &type_name,
                                    const std::string &location, int required, int done, bool storage) {
            const int  missing = std::max(0, required - done);
            const auto last    = last_scan.find(type);
            required_types.insert(type);
            remaining += missing;
            type_rows.push_back({ type, type_name, location, required, done, missing,
                                  missing == 0 ? 2 : done == 0 ? 1 : 0,
                                  last == last_scan.end() ? -1 : last->second, storage });
          };
          if (lot["storage"].boolean()) {
            // rangement du stock : pas de contenu attendu par type, chaque item range est attendu (on verifie
            // qu'il est toujours la), compte par type sur les items connus du rangement
            std::map< std::string, std::pair< std::string, std::pair< int, int > > > by_type; // nom, connus, scannes
            for (const Json &item : lot["items"].items()) {
              auto &entry = by_type[item["type"].str()];
              entry.first = item["type_name"].str();
              ++entry.second.first;
              if (scanned.count(item["iid"].str()))
                ++entry.second.second;
            }
            for (const auto &[type, entry] : by_type)
              add_type(type, entry.first, "", entry.second.first, entry.second.second, true);
          } else {
            for (const Json &row : lot["requirements"].items()) {
              const std::string type  = row["type"].str();
              const auto        found = plan_row.fresh.find(type);
              add_type(type, row["type_name"].str(), row["location"].str(), row["required"].integer(),
                       found == plan_row.fresh.end() ? 0 : found->second, false);
            }
          }
          std::stable_sort(type_rows.begin(), type_rows.end(), [](const TypeRow &a, const TypeRow &b) {
            return a.rank != b.rank ? a.rank < b.rank : a.rank == 0 && a.last_scan > b.last_scan;
          });
          const auto type_row = [&](const TypeRow &entry) {
            const std::string &type = entry.type;
            // ligne du type : progression coloree (rouge = rien, orange = partiel, vert = complet)
            ImGui::TableNextRow();
            row_color(entry.missing == 0 ? colors::green : entry.done == 0 ? colors::red : colors::orange, 0.25f);
            ImGui::TableNextColumn();
            ImGui::PushID((lot["id"].str() + ":" + type).c_str());
            if (report_mark("##report"))
              report = { lot["id"].str(), lot["name"].str(), "", type, entry.type_name + " (" + lot["name"].str() + ")" };
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::Text("%s", entry.type_name.c_str());
            if (!entry.location.empty()) {
              ImGui::SameLine();
              ImGui::TextColored(colors::grey, "– %s", entry.location.c_str());
            }
            ImGui::TableNextColumn();
            if (entry.missing == 0)
              ImGui::TextColored(colors::green, "complet");
            else
              ImGui::TextColored(entry.done == 0 ? colors::red : colors::orange, "encore %d", entry.missing);
            ImGui::TableNextColumn();
            stock_bar(entry.done, entry.required, ImVec2(-1, 0));
            // items connus du lot pour ce type, puis ce qu'il faut ajouter depuis le stock
            int known_fresh = 0;
            for (const Json *item : known_by_type[type]) {
              item_row(*item);
              known_fresh += (*item)["expired"].boolean() ? 0 : 1;
            }
            if (!entry.storage && entry.missing > known_fresh) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::Indent();
              ImGui::TextColored(colors::orange, "+ %d à prendre dans le stock", entry.missing - known_fresh);
              ImGui::Unindent();
            }
          };
          for (const TypeRow &entry : type_rows)
            if (entry.rank < 2)
              type_row(entry);
          // items du lot dont le type n'est pas (ou plus) dans la definition
          bool header = false;
          for (const auto &[type, items] : known_by_type) {
            if (required_types.count(type))
              continue;
            if (!header) {
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextDisabled("Autres items du lot (hors définition du type de lot)");
              header = true;
            }
            for (const Json *item : items) {
              item_row(*item);
              ++remaining;
            }
          }
          for (const TypeRow &entry : type_rows)
            if (entry.rank == 2)
              type_row(entry);
        }
        ImGui::EndTable();
      }
      if (!report.label.empty())
        app.report_problem(report);
      if (grouping)
        status_banner(level == verif.lot_id ? "Les sous-lots validés sont enregistrés : terminez quand vous avez fini"
                                            : "Validez les sous-lots, puis revenez", colors::grey, 1.0f);
      else if (cascade && remaining == 0)
        status_banner("✔ Tout est scanné dans " + (*app.verif_lot(level))["name"].str(), colors::green, 1.0f);
      else if (remaining == 0)
        status_banner(multi ? "✔ Tout est scanné : les lots seront complets" : "✔ Tout est scanné : le lot sera complet",
                      colors::green, 1.0f);
      else
        status_banner(std::to_string(remaining) + " item(s) encore attendu(s) – " + std::to_string(added)
                        + " nouvel(s) item(s) scanné(s)",
                      colors::red, 1.0f);

      ImGui::BeginDisabled(!app.verif_key_ok() || verif.submitting);
      if (!partial.empty()) {
        std::string names;
        for (const Json *lot : partial)
          names += (names.empty() ? "" : ", ") + (*lot)["name"].str();
        const std::string label = "Vérif partielle : " + names + " – réassort pour le reste";
        if (warning_button(label.c_str(), ImVec2(-1, 0)))
          app.submit_verif(true);
        ImGui::SetItemTooltip("Seuls les lots complets avec les scans sont vérifiés (%s).\n"
                              "Les autres items scannés sont ajoutés à leur lot, qui sera signalé « vérif "
                              "recommandée » ;\nle reste de leur contenu n'est pas touché.",
                              names.c_str());
      } else if (restock) {
        const std::string label = "Ajouter " + std::to_string(added) + " item(s) au lot – réassort, sans vérif";
        if (warning_button(label.c_str(), ImVec2(-1, 0)))
          app.restock_verif();
        ImGui::SetItemTooltip("Aucun item déjà présent n'a été scanné : les items sont simplement ajoutés au lot.\n"
                              "Le lot sera signalé « vérif recommandée » (orange) pour que la personne suivante\n"
                              "fasse une vérif complète.");
      }
      const float width = ImGui::GetContentRegionAvail().x;
      std::string validate = verif.submitting ? "Envoi..." : orange ? "Valider une vérif complète" : "Valider la vérif";
      if (cascade && !verif.submitting) {
        const Json &lot     = *app.verif_lot(level);
        const bool  at_root = level == verif.lot_id;
        const bool  content = lot["requirements"].size() > 0 || lot["items"].size() > 0 || lot["storage"].boolean()
                             || !in_scope.empty() || scope.size() > 1 || !verif.sealed.empty();
        validate = !content ? (at_root ? "Terminer la vérif" : "Revenir (rien à valider)")
                   : (orange ? "Vérif complète de " : "Valider ") + lot["name"].str() + (at_root ? " et terminer" : "");
      }
      if (primary_button(validate.c_str(), ImVec2(width * 0.45f, 0)))
        app.submit_verif();
      if (!app.verif_key_ok())
        ImGui::SetItemTooltip("Scannez d'abord l'étiquette clé du lot : c'est le QR code rangé à l'intérieur du lot.");
      else
        ImGui::SetItemTooltip("Enregistre la vérif : les items scannés sont présents, ceux de la liste « À scanner »\n"
                              "sont signalés manquants. %s",
                              remaining == 0 ? "Tout est scanné : le lot sera complet."
                                             : (std::to_string(remaining) + " item(s) encore attendu(s) : le lot sera "
                                                                            "incomplet.").c_str());
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(app.stack.empty() || verif.submitting);
      if (button("Annuler le dernier scan", ImVec2(width * 0.3f, 0)))
        app.stack.undo_last();
      ImGui::SetItemTooltip("Retire de la pile le dernier item scanné (erreur de scan).");
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (danger_button(cascade && !verif.done.empty() ? "Quitter la vérif" : "Annuler la vérif", ImVec2(-1, 0)))
        app.cancel_verif();
      if (cascade && !verif.done.empty())
        ImGui::SetItemTooltip("Les lots déjà validés restent enregistrés ; les scans pas encore validés sont oubliés.");
    }

    // Vérif en cascade : retour au lot qui contient le lot en cours, et ses sous-lots (faits en vert, commencés en
    // orange, à faire) ; cliquer un sous-lot (ou scanner son étiquette) y fait entrer. Renvoie true si l'on vient de
    // changer de lot (la suite de la fenêtre est dessinée a la prochaine image).
    bool draw_cascade(App &app, const std::string &level, const std::vector< VerifPlanLot > &plan) {
      VerifSession &verif = app.verif;
      if (level != verif.lot_id) {
        const Json *parent = app.verif_lot(app.verif_parent(level));
        const std::string back = "← Revenir à " + (parent ? (*parent)["name"].str() : std::string("lot global"));
        if (button(back.c_str())) {
          app.leave_level();
          return true;
        }
        ImGui::SetItemTooltip("Sans valider ce lot : ses scans sont gardés.");
        ImGui::SameLine();
        ImGui::TextDisabled("Validez ce lot pour revenir au lot qui le contient.");
      }
      const std::vector< const Json * > children = app.verif_children(level);
      if (children.empty())
        return false;
      ImGui::SeparatorText("Sous-lots : scannez l'étiquette d'un sous-lot (ou cliquez-le) pour y entrer");
      const std::set< std::string > intact = app.verif_intact();
      bool                          moved  = false;
      for (const Json *child : children) {
        const std::string id   = (*child)["id"].str();
        const auto        done = verif.done.find(id);
        std::string       text = std::string((*child)["is_sealed"].boolean() ? "[scellé] " : "") + (*child)["name"].str();
        ImVec4            color;
        if (done != verif.done.end()) {
          color = done->second.complete || done->second.sealed ? colors::green : colors::red;
          text += done->second.sealed ? "  –  scellé intact constaté à " + done->second.at
                                      : "  –  validé à " + done->second.at + (done->second.complete ? "" : ", incomplet");
        } else if (intact.count(id)) {
          color = colors::green;
          text += "  –  scellé intact : enregistré à la validation";
        } else if (app.verif_touched(id)) {
          color = colors::orange;
          text += "  –  commencé, pas encore validé";
        } else {
          color = colors::grey;
          text += std::string("  –  à faire · état actuel : ") + lot_status_label(lot_status(*child));
        }
        const std::size_t subs = app.verif_children(id).size();
        if (subs)
          text += " · " + std::to_string(subs) + " sous-lot(s)";
        ImGui::PushID(id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(color.x, color.y, color.z, 0.65f));
        if (ImGui::Selectable(text.c_str(), true)) {
          app.enter_level(id);
          moved = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
      }
      (void)plan;
      return moved;
    }

    void draw_list(const App &app, const char *title, const Json &iids, const ImVec4 &color) {
      if (iids.size() == 0)
        return;
      const std::string header = std::string(title) + " (" + std::to_string(iids.size()) + ")";
      ImGui::TextColored(color, "%s", header.c_str());
      ImGui::Indent();
      for (const Json &iid : iids.items())
        ImGui::BulletText("%s  [%s]", iid_label(app, iid.str()).c_str(), iid.str().c_str());
      ImGui::Unindent();
    }

    // Raccourci gestion / admin : sceller tout de suite un lot que la verif vient de trouver complet (ex : un lot a
    // scelle obligatoire, rouge tant qu'il n'est pas scelle). Memes etiquettes que depuis la Gestion des lots.
    void draw_seal_shortcut(App &app, const Json &report) {
      if (!app.privileged())
        return;
      const std::string verif = report["verif_id"].str();
      if (verif != seal_report_) {
        seal_report_ = verif;
        sealed_.clear();
        seal_number_.clear();
      }
      // lots deja scelles (scelle intact constate, ou scelle depuis) : rien a resceller
      std::set< std::string > already;
      for (const Json &lot : app.catalog.lots.items())
        if (lot["is_sealed"].boolean())
          already.insert(lot["id"].str());
      std::vector< const Json * > lots;
      for (const Json &row : report["lots"].items())
        if (row["verified"].boolean() && row["complete"].boolean() && !row["sealed"].boolean()
            && (!already.count(row["id"].str()) || sealed_.count(row["id"].str())))
          lots.push_back(&row);
      if (lots.empty())
        return;
      ImGui::SetNextItemWidth(160.0f);
      input_limited("Numéro du scellé", seal_number_, 32, "facultatif");
      for (const Json *row : lots) {
        const std::string id = (*row)["id"].str();
        ImGui::SameLine();
        if (sealed_.count(id)) {
          ImGui::TextColored(colors::green, "✔ %s scellé", (*row)["name"].str().c_str());
          continue;
        }
        ImGui::PushID(id.c_str());
        if (button(("Sceller " + (*row)["name"].str()).c_str()))
          seal(app, id);
        ImGui::PopID();
      }
    }

    void seal(App &app, const std::string &id) {
      Json body;
      body["user"]        = app.user_ref();
      body["seal_number"] = seal_number_;
      app.api.post("/api/lots/" + url_encode(id) + "/seal/", body, [this, &app, id](const ApiResult &result) {
        if (!result.ok) {
          app.notify(result.error, true);
          return;
        }
        const Json &lot = result.data;
        sealed_.insert(id);
        seal_number_.clear();
        app.preview_seal_labels(lot);
        app.notify("Lot " + lot["name"].str()
                   + " scellé : rangez l'étiquette d'ouverture dans le lot et collez celle du scellé sur le scellé.");
        app.refresh_lots();
      });
    }

    std::string             seal_report_; // verif du compte rendu affiche (remise a zero du raccourci)
    std::string             seal_number_;
    std::set< std::string > sealed_;

    void draw_report(App &app) {
      const Json &report = app.last_report;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      ImGui::Text("Compte rendu : %s", app.last_report_lot.c_str());
      ImGui::PopFont();
      const bool several = report["lots"].size() > 1;
      if (report["complete"].boolean())
        status_banner(several ? "✔ VÉRIF ENREGISTRÉE : LOTS COMPLETS" : "✔ VÉRIF ENREGISTRÉE : LOT COMPLET", colors::green,
                      1.4f);
      else if (report["partial"].boolean())
        status_banner("↷ VÉRIF PARTIELLE ENREGISTRÉE – voir ci-dessous", colors::orange, 1.4f);
      else
        status_banner("✘ VÉRIF ENREGISTRÉE : INCOMPLET – voir ci-dessous", colors::red, 1.4f);
      if (report["unsealed"].boolean()) {
        std::string names;
        for (const Json &name : report["unsealed_lots"].items())
          names += (names.empty() ? "" : ", ") + name.str();
        ImGui::TextColored(colors::orange, "Scellé brisé (%s) : resceller depuis la Gestion des lots.",
                           names.empty() ? "lot" : names.c_str());
      }
      ImGui::Text("%zu item(s) présent(s).", report["present"].size());
      if (report["sealed_lots"].size() > 0) {
        std::string names;
        for (const Json &name : report["sealed_lots"].items())
          names += (names.empty() ? "" : ", ") + name.str();
        ImGui::TextColored(colors::green, "✔ Scellé intact, compté comme vérifié : %s.", names.c_str());
      }
      draw_seal_shortcut(app, report);
      // verif groupee : resultat de chaque lot
      if (several)
        for (const Json &row : report["lots"].items()) {
          ImGui::Indent(row["depth"].integer() * 16.0f + 1.0f);
          if (row["sealed"].boolean())
            ImGui::TextColored(colors::green, "✔ %s : scellé intact", row["name"].str().c_str());
          else if (!row["verified"].boolean())
            ImGui::TextColored(colors::orange, "↷ %s : non vérifié%s", row["name"].str().c_str(),
                               row["restocked"].integer() > 0
                                 ? (", " + row["restocked"].str() + " item(s) ajouté(s) en réassort").c_str()
                                 : "");
          else if (row["complete"].boolean())
            ImGui::TextColored(colors::green, "✔ %s : complet", row["name"].str().c_str());
          else
            ImGui::TextColored(colors::red, "✘ %s : incomplet", row["name"].str().c_str());
          ImGui::Unindent(row["depth"].integer() * 16.0f + 1.0f);
        }
      for (const Json &row : report["requirements"].items()) {
        const std::string label = row["lot_name"].str().empty() ? row["type_name"].str()
                                                                : row["lot_name"].str() + " · " + row["type_name"].str();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(320.0f);
        stock_bar(row["present"].integer(), row["required"].integer(), ImVec2(200.0f, 0));
      }
      ImGui::Separator();
      ImGui::BeginChild("report", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
      draw_list(app, "Périmés ou signalés inutilisables, toujours dans le lot : à remplacer", report["expired"],
                colors::red);
      draw_list(app, "Périmés considérés comme remplacés", report["replaced"], colors::green);
      // etiquette a dechirer absente : l'ensemble a ete entame, il compte comme utilise
      Json missing = Json::array();
      for (const Json &iid : report["missing"].items()) {
        bool torn = false;
        for (const Json &other : report["torn"].items())
          torn = torn || other.str() == iid.str();
        if (!torn)
          missing.push_back(iid);
      }
      draw_list(app, "Étiquette déchirée : utilisés", report["torn"], colors::orange);
      draw_list(app, "Attendus mais non scannés", missing, colors::orange);
      // non consommable absent du lot : retire du lot, hors lot (pas perdu) jusqu'a son retour en stock
      draw_list(app, "Non consommables absents : hors lot (à remettre en stock)", report["loose"], colors::orange);
      draw_list(app, "Retrouvés (étaient signalés disparus)", report["reactivated"], colors::green);
      draw_list(app, "Ajoutés à leur lot en réassort (lot non vérifié)", report["restocked"], colors::orange);
      draw_list(app, "Codes inconnus ignorés", report["unknown"], colors::red);
      ImGui::EndChild();
      if (button("Fermer le compte rendu", ImVec2(-1, 0))) {
        app.last_report = Json();
        open            = false;
      }
    }
};

} // namespace

std::unique_ptr< AppWindow > make_verif_window() {
  return std::make_unique< VerifWindow >();
}

} // namespace qrprotec
