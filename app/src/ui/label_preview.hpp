/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          label_preview.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 18:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

namespace qrprotec {

class App;

// Fenetre "Apercu d'etiquette" : image de l'etiquette (telle que dessinee dans le modele), navigation
// entre plusieurs etiquettes et boutons d'impression. Ouverte par App::preview_labels().
void draw_label_preview(App &app);

} // namespace qrprotec
