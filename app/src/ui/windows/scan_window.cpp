/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          scan_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../app/labels.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

namespace qrprotec {

namespace {

class ScanWindow final : public AppWindow {
  public:
    ScanWindow() : AppWindow("scan", "Pile de scans", false, false) {}

    void draw(App &app) override {
      draw_user(app);
      ImGui::Separator();
      draw_context(app);
      draw_manual_input(app);
      draw_entries(app);
      ImGui::Separator();
      draw_actions(app);
    }

  private:
    void draw_user(App &app) {
      if (app.logged_in()) {
        ImGui::TextColored(app.privileged() ? colors::orange : colors::green, "Connecté : %s%s",
                           app.user_name().c_str(), app.user ? app.user->role_suffix().c_str() : "");
        if (danger_button("Se déconnecter", ImVec2(-1, ImGui::GetFrameHeight() * 1.3f)))
          app.logout("Déconnecté.");
      } else {
        ImGui::TextColored(colors::orange, "Non connecté : scannez votre badge.");
      }
    }

    void draw_context(App &app) {
      if (app.verif.active) {
        ImGui::TextColored(colors::orange, "Vérif en cours : %s", app.verif_title().c_str());
        ImGui::SameLine();
        if (small_button("Voir"))
          app.open_window("verif");
      }
      if (app.stack.target.valid()) {
        ImGui::TextColored(colors::green, "Lot cible (étiquette privée) : %s", app.stack.target.name.c_str());
        ImGui::SameLine();
        if (small_button("Oublier"))
          app.stack.target = {};
      }
    }

    void draw_manual_input(App &app) {
      ImGui::SetNextItemWidth(-80.0f);
      const bool submit = ImGui::InputTextWithHint("##manual", "Saisie manuelle d'un code", &manual_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((button("Ajouter") || submit) && !manual_.empty()) {
        app.handle_scan(manual_, ScanSource::Manual);
        manual_.clear();
      }
    }

    void draw_entries(App &app) {
      const float footer = ImGui::GetFrameHeightWithSpacing() * (app.privileged() ? 5.5f : 3.5f);
      const auto &entries = app.stack.entries();
      ImGui::Text("%zu scan(s), %zu item(s)", entries.size(), app.stack.iids().size());
      if (ImGui::GetTime() - app.last_duplicate_time_ < 3.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("· déjà scanné : %s", app.last_duplicate_.c_str());
      }
      if (app.stack.expired_count() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(colors::red, "- %zu périmé(s) !", app.stack.expired_count());
      }
      int remove_id = 0;
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
      if (ImGui::BeginTable("stack", 3, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        // le dernier scan en haut
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
          ImGui::TableNextRow();
          const bool highlighted = ImGui::GetTime() < entry->highlight_until;
          if (highlighted) {
            row_color(ImVec4(0.25f, 0.55f, 0.95f, 1.0f), 0.45f); // rescanne : bref surlignage
            ImGui::SetScrollHereY(0.5f);
          } else if (entry->expired || entry->state == EntryState::Error)
            row_color(colors::red, 0.55f);
          else if (entry->duplicate || entry->warning)
            row_color(colors::yellow, 0.45f);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(entry->title.c_str());
          ImGui::TextDisabled("%s", entry->scan.raw.c_str());
          if (!entry->detail.empty())
            ImGui::TextWrapped("%s", entry->detail.c_str());
          ImGui::TableNextColumn();
          if (entry->expired)
            ImGui::TextColored(colors::red, "PÉRIMÉ");
          else if (entry->state == EntryState::Error)
            ImGui::TextColored(colors::red, "Erreur");
          else if (entry->duplicate)
            ImGui::TextUnformatted("Doublon");
          else if (entry->state == EntryState::Pending)
            ImGui::TextDisabled("...");
          else
            ImGui::TextColored(colors::green, "OK");
          ImGui::TableNextColumn();
          ImGui::PushID(entry->id);
          if (small_button("X"))
            remove_id = entry->id;
          ImGui::SetItemTooltip("Retirer ce scan de la pile");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (remove_id)
        app.stack.remove(remove_id);
    }

    void draw_actions(App &app) {
      ScanStack &stack = app.stack;
      const bool empty = stack.iids().empty();
      const float width = ImGui::GetContentRegionAvail().x;
      const float third = (width - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
      ImGui::BeginDisabled(stack.empty());
      if (button("Annuler", ImVec2(third, 0)))
        stack.undo_last();
      ImGui::SetItemTooltip("Retire le dernier scan de la pile.");
      ImGui::SameLine();
      if (button("Nettoyer", ImVec2(third, 0))) {
        stack.remove_duplicates();
        stack.remove_errors();
      }
      ImGui::SetItemTooltip("Retire les codes en erreur (inconnus).");
      ImGui::SameLine();
      if (danger_button("Vider la pile", ImVec2(third, 0)))
        stack.clear();
      ImGui::EndDisabled();

      // etiquette privee d'un lot de la verif en cours : on termine la verif (pas de reassort a part)
      const bool in_verif = app.verif.active && stack.target.valid() && app.verif_covers(stack.target.id);
      if (stack.target.valid() && !in_verif) {
        ImGui::BeginDisabled(empty);
        const std::string add = "Ajouter au lot " + stack.target.name + " (réassort)";
        if (warning_button(add.c_str(), ImVec2(-1, 0)))
          app.add_stack_to_lot();
        ImGui::SetItemTooltip("Range les items scannés dans ce lot, sans toucher au reste de son contenu.\n"
                              "Le lot sera signalé « vérif recommandée » pour la personne suivante.");
        ImGui::EndDisabled();
        if (primary_button("Valider comme vérif", ImVec2(-1, 0)))
          app.verif_target_lot();
        ImGui::SetItemTooltip("Le contenu du lot devient exactement la pile : les items absents sont signalés.");
      } else if (app.verif.active) {
        ImGui::BeginDisabled(!app.verif_key_ok() || app.verif.submitting);
        if (primary_button("Terminer la vérif", ImVec2(-1, 0)))
          app.submit_verif();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Les options (vérif partielle, réassort) sont dans la fenêtre Vérif.");
      } else if (!empty) {
        // pas de lot scanne : la pile est du materiel sorti du stock (pris pour une intervention, prete...)
        const std::string out = "Sortir " + std::to_string(stack.iids().size()) + " item(s) du stock";
        if (warning_button(out.c_str(), ImVec2(-1, 0)))
          app.stack_out();
        ImGui::SetItemTooltip("Pour une vérif ou un réassort, scannez d'abord l'étiquette privée d'un lot\n"
                              "(QR code rangé à l'intérieur du lot).\n"
                              "Les items sortis redeviennent normaux s'ils sont scannés à une vérif ;\n"
                              "sinon ils sont comptés comme utilisés au bout d'un mois (ou à leur péremption).");
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Scannez l'étiquette privée d'un lot (QR code rangé à l'intérieur du lot) pour y ajouter "
                           "la pile.");
        ImGui::PopStyleColor();
      }

      if (app.privileged()) {
        ImGui::BeginDisabled(empty);
        if (button("Vérif du stock", ImVec2(third, 0)))
          app.stock_verif();
        ImGui::SetItemTooltip("Stock non rangé : les items scannés sont déclarés en stock (ceux d'un rangement y restent) ;\n"
                              "les items en stock hors rangement non scannés sont signalés.\n"
                              "Pour un tiroir ou une armoire : scannez son étiquette, comme pour un lot.");
        ImGui::SameLine();
        if (button("En stock", ImVec2(third, 0)))
          app.stack_to_stock();
        ImGui::SetItemTooltip("Remet les items scannés en stock (sortis de leur lot).");
        ImGui::SameLine();
        if (button("Réimprimer", ImVec2(third, 0)))
          reprint(app);
        ImGui::SetItemTooltip("Réimprime l'étiquette de chaque item scanné (étiquette abîmée).");
        ImGui::EndDisabled();
      }
    }

    void reprint(App &app) {
      std::vector< Parameters > labels;
      for (const ScanEntry &entry : app.stack.entries())
        if (entry.scan.kind == ScanKind::Item && entry.state == EntryState::Ok && !entry.duplicate)
          labels.push_back(item_parameters(entry.data));
      if (labels.empty())
        app.notify("Aucun item reconnu à réimprimer.", true);
      else
        app.print_labels(TemplateCategory::Item, labels, "Réimpression");
    }

    std::string manual_;
};

} // namespace

std::unique_ptr< AppWindow > make_scan_window() {
  return std::make_unique< ScanWindow >();
}

} // namespace qrprotec
