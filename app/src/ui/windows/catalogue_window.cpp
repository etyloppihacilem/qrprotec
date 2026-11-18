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
      if (!ImGui::BeginTabBar("catalogue_tabs"))
        return;
      if (tab_item("Types de lots")) {
        lots_->draw_lot_types(app);
        ImGui::EndTabItem();
      }
      if (tab_item("Types d'items")) {
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
