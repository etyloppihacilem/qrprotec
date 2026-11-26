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

// Mode gestion : une fenetre par domaine
std::unique_ptr< AppWindow > make_stock_window();     // Gestion de la TECH : a faire, a venir
std::unique_ptr< AppWindow > make_inventory_window(); // Stock : etat, projection, commandes, transferts
std::unique_ptr< AppWindow > make_items_window();     // Items : types d'items, items, entretien, paquets fermes
std::unique_ptr< AppWindow > make_lot_types_window(); // Types de lots
std::unique_ptr< AppWindow > make_reception_window(); // Reception : nouvelle, passees
std::unique_ptr< AppWindow > make_purchases_window(); // Commandes fournisseurs : commandes, saisie, suggestions, stats
std::unique_ptr< AppWindow > make_pack_window(); // fiche d'un paquet ferme, ouverture
std::unique_ptr< AppWindow > make_users_window();
std::unique_ptr< AppWindow > make_journal_window(); // journal des operations (qui fait quoi et quand)
std::unique_ptr< AppWindow > make_reports_window(); // signalements de problemes (admins)
std::unique_ptr< AppWindow > make_editor_window();
std::unique_ptr< AppWindow > make_print_counts_window(); // compteur d'impressions par mois et categorie d'etiquette
std::unique_ptr< AppWindow > make_settings_window();

// Gestion des lots sans fenetre a elle : la fiche complete d'un lot (onglets de gestion, creation, ordre, archives)
// vit dans Lots (F2), les types de lots dans leur fenetre (F8). Une instance par fenetre.
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

// Reception d'une commande fournisseur, en haut de l'onglet Nouvelle reception : commandes pas encore recues
// proposees, puis toutes les lignes d'une commande (quantites, peremptions, rangements) et toutes les etiquettes
// d'un coup. Voir purchases_window.cpp.
class OrderReception {
  public:
    virtual ~OrderReception() = default;
    virtual void reload(App &app) = 0; // commandes en attente de livraison
    // true : une commande est en cours de reception (le formulaire de reception libre est cache). `places` : rangements
    // du stock (id, label).
    virtual bool draw(App &app, const Json &places) = 0;
};
std::unique_ptr< OrderReception > make_order_reception();

// Connexion au back (URL, cle de front, autorite HTTPS, jeton) avec bouton de test : fenetre Reglages, et
// fenetre « Connexion au serveur » accessible sans badge tant que l'API est injoignable. save_on_success :
// les reglages sont enregistres des que le test reussit (premier lancement d'un front sans back local).
void draw_server_settings(App &app, bool save_on_success = false);

} // namespace qrprotec
