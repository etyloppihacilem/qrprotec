/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          lot_types_window.cpp
        -\-    _|__
         |\___/  . \        Created on 18 Nov. 2026 at 02:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Types de lots (F8) : comment chaque lot est defini (contenu, emplacements, scelle, referentiels). Fenetre a part
// pour l'avoir a cote des Lots (F2) ou des Items (F7, types d'items).

#include "../widgets.hpp"
#include "windows.hpp"

#include <utility>

namespace qrprotec {

namespace {

class LotTypesWindow final : public AppWindow {
  public:
    LotTypesWindow() : AppWindow("lot_types", "Types de lots", true, true) {}

    void on_open(App &app) override {
      app.refresh_lot_types();
      app.refresh_item_types();
      app.refresh_lots();
    }

    void reset_view(App &) override { panel_->reset(); }

    void draw(App &app) override {
      // type choisi depuis « Aller a... » : son formulaire
      if (app.type_to_show_.rfind("lot:", 0) == 0)
        panel_->select_lot_type(app, std::exchange(app.type_to_show_, std::string()).substr(4));
      panel_->draw_lot_types(app);
    }

  private:
    std::unique_ptr< LotPanel > panel_ = make_lot_panel();
};

} // namespace

std::unique_ptr< AppWindow > make_lot_types_window() {
  return std::make_unique< LotTypesWindow >();
}

} // namespace qrprotec
