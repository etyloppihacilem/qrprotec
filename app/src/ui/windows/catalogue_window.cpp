/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          catalogue_window.cpp
        -\-    _|__
         |\___/  . \        Created on 18 Nov. 2026 at 02:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Catalogue (F7) : comment le materiel est defini. Types de lots (contenu, emplacements, scelle, referentiels) et
// types d'items (consommable, taches, revision, desinfection, etiquette), reunis dans une seule fenetre. Les lots
// eux-memes (fiche, creation, ordre) sont dans la fenetre Lots.

#include "../widgets.hpp"
#include "windows.hpp"

#include <utility>

namespace qrprotec {

namespace {

class CatalogueWindow final : public AppWindow {
  public:
    CatalogueWindow() : AppWindow("catalogue", "Catalogue", true, true) {}

    void on_open(App &app) override {
      app.refresh_lot_types();
      app.refresh_item_types();
      app.refresh_lots();
    }

    void reset_view(App &) override {
      lots_->reset();
      items_->reset();
    }

    void draw(App &app) override {
      // type choisi depuis le hub (« Aller a... ») : son onglet et son formulaire
      std::string select;
      if (std::string key = std::exchange(app.catalogue_type_to_show_, std::string()); !key.empty()) {
        const std::size_t colon = key.find(':');
        select                  = key.substr(0, colon);
        const std::string code  = colon == std::string::npos ? std::string() : key.substr(colon + 1);
        if (select == "lot")
          lots_->select_lot_type(app, code);
        else
          items_->select_item_type(app, code);
      }
      if (!ImGui::BeginTabBar("catalogue_tabs"))
        return;
      if (tab_item("Types de lots", select == "lot" ? ImGuiTabItemFlags_SetSelected : 0)) {
        lots_->draw_lot_types(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Types d'items", select == "item" ? ImGuiTabItemFlags_SetSelected : 0)) {
        items_->draw_item_types(app);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

  private:
    std::unique_ptr< LotPanel >       lots_  = make_lot_panel();
    std::unique_ptr< InventoryPanel > items_ = make_inventory_panel();
};

} // namespace

std::unique_ptr< AppWindow > make_catalogue_window() {
  return std::make_unique< CatalogueWindow >();
}

} // namespace qrprotec
