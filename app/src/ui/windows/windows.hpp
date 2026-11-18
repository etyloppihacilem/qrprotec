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
std::unique_ptr< AppWindow > make_lists_window(); // listes de materiel a prendre (verif, retour)

// Mode privilegie
std::unique_ptr< AppWindow > make_stock_window();
std::unique_ptr< AppWindow > make_inventory_window();
std::unique_ptr< AppWindow > make_pack_window(); // fiche d'un paquet ferme, ouverture
std::unique_ptr< AppWindow > make_catalogue_window(); // types de lots et types d'items
std::unique_ptr< AppWindow > make_users_window();
std::unique_ptr< AppWindow > make_journal_window(); // journal des operations (qui fait quoi et quand)
std::unique_ptr< AppWindow > make_reports_window(); // signalements de problemes (admins)
std::unique_ptr< AppWindow > make_editor_window();
std::unique_ptr< AppWindow > make_print_counts_window(); // compteur d'impressions par mois et categorie d'etiquette
std::unique_ptr< AppWindow > make_settings_window();

// Panneaux partages entre fenetres : une seule fiche par objet, montree la ou on l'attend.
// Gestion des lots (fiche complete d'un lot, creation, ordre, types de lots) : fiche du lot dans Lots (F2), types
// de lots dans le Catalogue (F7).
class LotPanel {
  public:
    virtual ~LotPanel() = default;
    virtual void sync(App &app)                                       = 0; // listes rechargees : lot et type a jour
    virtual void draw_state(App &app, const std::string &lot_id)      = 0; // etat et contenu (attendu, ranges)
    virtual void draw_sheet_tabs(App &app, const std::string &lot_id) = 0; // onglets de gestion (dans un tab bar)
    virtual void        start_new()                                   = 0; // avant draw_new_lot : formulaire vide
    virtual std::string draw_new_lot(App &app)                        = 0; // id du lot cree, vide sinon
    virtual void        draw_lot_order(App &app)                      = 0;
    virtual std::string draw_archived(App &app)                       = 0; // id du lot archive choisi, vide sinon
    virtual void        draw_lot_types(App &app)                      = 0;
    virtual void        select_lot_type(App &app, const std::string &code) = 0; // formulaire de ce type
    virtual void        reset()                                       = 0;
};
std::unique_ptr< LotPanel > make_lot_panel();

// Inventaire : types d'items (Catalogue) et materiel suivi (Gestion de la TECH > À venir)
class InventoryPanel {
  public:
    virtual ~InventoryPanel()              = default;
    virtual void draw_item_types(App &app) = 0;
    virtual void draw_equipment(App &app)  = 0;
    virtual void select_item_type(App &app, const std::string &code) = 0; // formulaire de ce type
    virtual void reset()                   = 0;
};
std::unique_ptr< InventoryPanel > make_inventory_panel();

// Connexion au back (URL, cle de front, autorite HTTPS, jeton) avec bouton de test : fenetre Reglages, et
// fenetre « Connexion au serveur » accessible sans badge tant que l'API est injoignable. save_on_success :
// les reglages sont enregistres des que le test reussit (premier lancement d'un front sans back local).
void draw_server_settings(App &app, bool save_on_success = false);

} // namespace qrprotec
