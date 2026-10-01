/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          windows.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../../app/app.hpp"

#include <memory>

namespace qrprotec {

// Mode normal
std::unique_ptr< AppWindow > make_scan_window();  // pile de scans (toujours ouverte)
std::unique_ptr< AppWindow > make_lots_window();  // etat des lots, lancement de verif
std::unique_ptr< AppWindow > make_verif_window(); // verif en cours : items attendus
std::unique_ptr< AppWindow > make_phone_window(); // telephone utilise comme douchette (QR de connexion)

// Mode privilegie
std::unique_ptr< AppWindow > make_stock_window();
std::unique_ptr< AppWindow > make_inventory_window();
std::unique_ptr< AppWindow > make_pack_window(); // fiche d'un paquet ferme, ouverture
std::unique_ptr< AppWindow > make_lot_admin_window();
std::unique_ptr< AppWindow > make_users_window();
std::unique_ptr< AppWindow > make_editor_window();
std::unique_ptr< AppWindow > make_settings_window();

// Connexion au back (URL, cle de front, autorite HTTPS, jeton) avec bouton de test : fenetre Reglages, et
// fenetre « Connexion au serveur » accessible sans badge tant que l'API est injoignable.
void draw_server_settings(App &app);

} // namespace qrprotec
