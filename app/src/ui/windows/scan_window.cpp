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
        ImGui::TextColored(app.privileged() ? colors::orange : colors::green, "Connecte : %s%s",
                           app.user_name().c_str(), app.privileged() ? " (responsable)" : "");
        if (danger_button("Se deconnecter", ImVec2(-1, ImGui::GetFrameHeight() * 1.3f)))
          app.logout("Deconnecte.");
      } else {
        ImGui::TextColored(colors::orange, "Non connecte : scannez votre badge.");
      }
    }

    void draw_context(App &app) {
      if (app.verif.active) {
        const std::string name = app.verif.lot["name"].str(app.verif.lot_id);
        ImGui::TextColored(colors::orange, "Verif en cours : %s", name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Voir"))
          app.open_window("verif");
      }
      if (app.stack.target.valid()) {
        ImGui::TextColored(colors::green, "Lot cible (etiquette privee) : %s", app.stack.target.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Oublier"))
          app.stack.target = {};
      }
    }

    void draw_manual_input(App &app) {
      ImGui::SetNextItemWidth(-80.0f);
      const bool submit = ImGui::InputTextWithHint("##manual", "Saisie manuelle d'un code", &manual_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((ImGui::Button("Ajouter") || submit) && !manual_.empty()) {
        app.handle_scan(manual_, ScanSource::Manual);
        manual_.clear();
      }
    }

    void draw_entries(App &app) {
      const float footer = ImGui::GetFrameHeightWithSpacing() * (app.privileged() ? 5.5f : 3.5f);
      const auto &entries = app.stack.entries();
      ImGui::Text("%zu scan(s), %zu item(s)", entries.size(), app.stack.iids().size());
      if (app.stack.expired_count() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(colors::red, "- %zu perime(s) !", app.stack.expired_count());
      }
      int remove_id = 0;
      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
      if (ImGui::BeginTable("stack", 3, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Etat", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        // le dernier scan en haut
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
          ImGui::TableNextRow();
          if (entry->expired || entry->state == EntryState::Error)
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
            ImGui::TextColored(colors::red, "PERIME");
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
          if (ImGui::SmallButton("X"))
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
      if (ImGui::Button("Annuler", ImVec2(third, 0)))
        stack.undo_last();
      ImGui::SetItemTooltip("Retire le dernier scan de la pile.");
      ImGui::SameLine();
      if (ImGui::Button("Nettoyer", ImVec2(third, 0))) {
        stack.remove_duplicates();
        stack.remove_errors();
      }
      ImGui::SetItemTooltip("Retire les doublons et les codes en erreur.");
      ImGui::SameLine();
      if (danger_button("Vider la pile", ImVec2(third, 0)))
        stack.clear();
      ImGui::EndDisabled();

      if (stack.target.valid()) {
        ImGui::BeginDisabled(empty);
        const std::string add = "Ajouter au lot " + stack.target.name;
        if (primary_button(add.c_str(), ImVec2(-1, 0)))
          app.add_stack_to_lot();
        ImGui::SetItemTooltip("Range les items scannes dans ce lot, sans toucher au reste de son contenu.");
        ImGui::EndDisabled();
        if (primary_button("Valider comme verif", ImVec2(-1, 0)))
          app.verif_target_lot();
        ImGui::SetItemTooltip("Le contenu du lot devient exactement la pile : les items absents sont signales.");
      } else if (app.verif.active) {
        ImGui::BeginDisabled(!app.verif_key_ok() || app.verif.submitting);
        if (primary_button("Terminer la verif", ImVec2(-1, 0)))
          app.submit_verif();
        ImGui::EndDisabled();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Scannez l'etiquette privee d'un lot pour y ajouter la pile.");
        ImGui::PopStyleColor();
      }

      if (app.privileged()) {
        ImGui::BeginDisabled(empty);
        if (ImGui::Button("Verif du stock", ImVec2(third, 0)))
          app.stock_verif();
        ImGui::SetItemTooltip("Les items scannes sont declares en stock ; les items du stock non scannes sont signales.");
        ImGui::SameLine();
        if (ImGui::Button("En stock", ImVec2(third, 0)))
          app.stack_to_stock();
        ImGui::SetItemTooltip("Remet les items scannes en stock (sortis de leur lot).");
        ImGui::SameLine();
        if (ImGui::Button("Reimprimer", ImVec2(third, 0)))
          reprint(app);
        ImGui::SetItemTooltip("Reimprime l'etiquette de chaque item scanne (etiquette abimee).");
        ImGui::EndDisabled();
      }
    }

    void reprint(App &app) {
      std::vector< Parameters > labels;
      for (const ScanEntry &entry : app.stack.entries())
        if (entry.scan.kind == ScanKind::Item && entry.state == EntryState::Ok && !entry.duplicate)
          labels.push_back(item_parameters(entry.data));
      if (labels.empty())
        app.notify("Aucun item reconnu a reimprimer.", true);
      else
        app.print_labels(TemplateCategory::Item, labels, "Reimpression");
    }

    std::string manual_;
};

} // namespace

std::unique_ptr< AppWindow > make_scan_window() {
  return std::make_unique< ScanWindow >();
}

} // namespace qrprotec
