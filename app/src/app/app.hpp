/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/json.hpp"
#include "../core/placeholders.hpp"
#include "../net/api_client.hpp"
#include "../net/remote_scanner_link.hpp"
#include "feedback.hpp"
#include "print_queue.hpp"
#include "scan_stack.hpp"
#include "settings.hpp"

#include "imgui.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qrprotec {

class App;
class Inateck;

// Fenetre de l'application. Les fenetres privilegiees ne sont dessinees qu'en mode privilegie.
class AppWindow {
  public:
    AppWindow(std::string id, std::string title, bool privileged, bool closable)
        : id(std::move(id)), title(std::move(title)), privileged(privileged), closable(closable) {}
    virtual ~AppWindow() = default;

    virtual void             draw(App &app) = 0;     // contenu, entre Begin et End
    virtual void             on_open(App &) {}       // (re)chargement des donnees
    virtual ImGuiWindowFlags flags() const { return 0; }
    virtual bool             can_close(const App &) const { return closable; }

    const std::string id;
    const std::string title;
    const bool        privileged;
    const bool        closable;
    bool              admin_only = false; // reserve au role admin (Reglages, Utilisateurs)
    bool              open = false;
    ImVec2            last_pos{ 0, 0 };
    ImVec2            last_size{ 0, 0 };
    bool              was_open = false;
    bool              place_pending = true; // appliquer la disposition par defaut au prochain affichage
    bool              focus_pending = false;
};

struct SessionUser {
    std::string matricule;
    std::string nom;
    std::string prenom;
    std::string key_expires;
    bool        privileged = false; // role gestion ou admin : mode privilegie
    std::string role       = "normal"; // normal, gestion, admin

    std::string display() const { return prenom + " " + nom; }
    bool        admin() const { return role == "admin"; }
    // " (gestion)", " (admin)" ou vide
    std::string role_suffix() const { return role == "admin" ? " (admin)" : privileged ? " (gestion)" : ""; }
};

// Saisie du PIN apres le scan d'un badge (admins, ou utilisateurs ayant un PIN)
struct PinPrompt {
    bool        active = false;
    bool        setup  = false; // admin sans PIN : il le choisit (saisi deux fois)
    bool        busy   = false;
    bool        focus  = false;
    std::string matricule;
    std::string key;
    std::string name;
    std::string pin;
    std::string confirm;
    std::string error;
    int         source = 0; // ScanSource du badge
};

// Autre lot du meme lot global ajoute a la verif en cours par son etiquette privee
struct VerifExtra {
    std::string id;
    std::string key;
    Json        lot; // detail du lot (items attendus, exigences, sous-lots)
};

struct VerifSession {
    bool                      active = false;
    std::string               lot_id;
    std::string               key; // cle de l'etiquette privee si scannee
    Json                      lot; // detail du lot (items attendus, exigences, sous-lots)
    std::vector< VerifExtra > extras;
    bool                      loading    = false;
    bool                      submitting = false;
};

// Repartition des items scannes entre les lots d'une verif (meme regle que le serveur,
// services.assign_items) : un item deja dans un des lots y reste, un nouvel item va dans le premier lot
// qui en attend encore.
struct VerifPlanLot {
    const Json                   *lot = nullptr;
    int                           depth = 0;
    std::map< std::string, int >  fresh;      // items frais scannes par type
    int                           touched = 0; // items scannes ranges dans ce lot
    bool                          complete = false; // complet avec ces scans (verif partielle possible)
};

// Session de telephone-douchette (voir database/inventory/remote_scanner.py)
struct RemoteSession {
    bool        active   = false;
    bool        creating = false;
    std::string id;
    std::string url;          // contenu du QR code a scanner avec le telephone
    int         timeout = 0;  // secondes de deconnexion avant fermeture
    bool        phone_connected = false;
    bool        phone_seen      = false; // le telephone s'est connecte au moins une fois
    std::string phone_agent;
    double      phone_deadline = -1.0; // ImGui::GetTime() de fermeture si le telephone reste deconnecte
    int         scans          = 0;
    std::string ended_reason;          // derniere session fermee : pourquoi
    bool        feedback_pending = false;
    std::string feedback_message;
};

struct LabelPreviewState {
    std::vector< PrintJob > jobs;
    std::string             title;
    int                     index = 0;
    bool                    open  = false;
    bool                    dirty = true;  // image a regenerer
    bool                    focus = false; // mettre la fenetre au premier plan
};

struct Toast {
    std::string message;
    bool        error = false;
    double      time  = 0.0;
};

// Donnees de reference chargees depuis l'API et partagees entre fenetres.
struct Catalog {
    Json item_types = Json::array();
    Json lot_types  = Json::array();
    Json lots       = Json::array();
    Json users      = Json::array();
    Json stock      = Json::array();
    Json forecast   = Json::object(); // previsions de stock (/api/stock/forecast/)
    bool loading_lots     = false;
    bool loading_stock    = false;
    bool loading_forecast = false;
    // incrementes a chaque rechargement : les fenetres rechargent alors leur volet de detail
    int item_types_version = 0;
    int lot_types_version  = 0;
    int lots_version       = 0;
    int users_version      = 0;
    int packs_version      = 0; // paquet ouvert : listes a recharger

    std::string item_type_name(const std::string &type) const;
    std::string item_type_description(const std::string &type) const;
};

class App {
  public:
    explicit App(Inateck &inateck);
    ~App();

    // Boucle principale
    void   begin_frame();
    void   draw();
    ImVec4 background_color() const;
    void   note_activity();
    void   handle_scan(const std::string &code, ScanSource source);

    // Session
    bool        logged_in() const { return user.has_value(); }
    bool        privileged() const { return user && user->privileged; }
    bool        admin() const { return user && user->admin(); } // reglages du front, utilisateurs
    bool        can_open(const AppWindow &window) const {
      return (!window.privileged || privileged()) && (!window.admin_only || admin());
    }
    void        logout(const std::string &reason = {});
    void        reset_session(); // retour a l'etat initial (inactivite)
    void        require_login(const std::string &what, std::function< void() > action);
    Json        user_ref() const; // valeur du champ "user" des requetes
    // identite d'une operation sur un lot (verif, ajout) : "user" connecte, sinon "name" declare sans badge
    void        add_identity(Json &body) const;
    // refus du serveur faute d'identite (login_required) : session expiree, ou reglage du nom declare change
    bool        identity_refused(const ApiResult &result);
    void        refresh_server_rules(); // relit declared_identity sur le serveur
    std::string user_name() const;

    // Verifs et mouvements
    void start_verif(const std::string &lot_id, const std::string &key);
    void show_lot(const std::string &lot_id, const std::string &seal_code = {}); // fiche du lot (fenetre Lots)
    std::string take_lot_to_show();            // lu par la fenetre Lots
    std::string take_seal_to_show();           // code du QR de scelle scanne (vide sinon)
    void show_pack(const std::string &pack_id); // ouvre la fiche du paquet ferme (mode privilegie)
    std::string take_pack_to_show();            // lu par la fenetre Paquet
    void show_journal(const std::string &lot_id); // journal des operations filtre sur un lot (mode privilegie)
    std::string take_journal_lot();               // lu par la fenetre Journal
    void pack_opened(const std::string &pack_id); // paquet ouvert ou referme : met a jour la pile et les listes
    void cancel_verif();
    // partial : verif partielle, seuls les lots que les scans rendent complets sont verifies, les autres items
    // scannes sont ajoutes a leur lot (reassort)
    void submit_verif(bool partial = false);
    void restock_verif(); // reassort : ajoute les items scannes au lot sans verif complete
    bool verif_key_ok() const;
    std::vector< const Json * >   verif_lots() const; // lots couverts : lot scanne, sous-lots, lots ajoutes
    bool                          verif_covers(const std::string &lot_id) const;
    std::string                   verif_title() const; // ex : "Sac de soin + Sac O2"
    std::vector< VerifPlanLot >   plan_verif() const;
    std::vector< const Json * >   partial_verif_lots() const; // lots complets d'une verif groupee incomplete
    void add_stack_to_lot();
    void verif_target_lot();
    void stack_to_stock();
    // items scannes sans lot : sortis du stock (comptes utilises s'ils ne reviennent pas a une verif)
    void stack_out();
    void stock_verif();

    // Telephone-douchette
    void start_remote_session();
    void close_remote_session(const std::string &reason = "fermée depuis le poste");

    // Donnees
    void refresh_item_types();
    void refresh_lot_types();
    void refresh_lots();
    void refresh_users();
    void refresh_stock();
    void refresh_forecast();

    // Impression : un jeu de parametres par etiquette
    bool print_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what);
    // Fenetre d'apercu (image de l'etiquette + bouton Imprimer) ; plusieurs etiquettes se parcourent
    bool preview_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what);
    void preview_jobs(std::vector< PrintJob > jobs, const std::string &title);
    bool build_label_jobs(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what,
                          std::vector< PrintJob > &jobs);
    void print_documents(std::vector< PrintJob > jobs); // documents deja remplis (test de l'editeur)
    TemplateDocument qr_only_template(TemplateCategory category) const;

    // Interface
    void        notify(const std::string &message, bool error = false);
    void        open_window(const std::string &id);
    AppWindow  *window(const std::string &id);
    void        apply_settings();
    void        select_default_templates(); // modeles fournis choisis pour les usages sans modele
    bool        save_settings();
    void        save_current_layout();
    void        request_layout_reset() { layout_pending_ = true; }
    Date        today() const { return Date::today(); }

    AppSettings                  settings;
    ApiClient                    api;
    ScanStack                    stack;
    PrintQueue                   printer;
    Feedback                     feedback;
    Inateck                     &inateck;
    Catalog                      catalog;
    std::optional< SessionUser > user;
    // reglage du serveur (/api/health/) : verif et ajout possibles sans badge, sous un nom declare non verifie
    bool                         declared_identity = false;
    VerifSession                 verif;
    Json                         last_report; // dernier compte rendu de verif
    std::string                  last_report_lot;
    LabelPreviewState            preview;
    RemoteSession                remote;
    RemoteScannerLink            remote_link;
    std::string                  last_duplicate_;      // dernier doublon ignore (mention discrete)
    double                       last_duplicate_time_ = -100.0;

    std::vector< std::unique_ptr< AppWindow > > windows;

  private:
    void poll_remote();
    void resolve_item(int entry_id);
    void resolve_pack(int entry_id);
    void login_with_badge(const ParsedScan &scan, ScanSource source);
    void send_auth(const std::string &matricule, const std::string &key, const std::string &pin,
                   const std::string &new_pin, ScanSource source);
    void complete_login(const Json &data);
    void show_pin_blocked(const Json &data, ScanSource source);
    void forgot_pin();
    void draw_pin_modal();
    void scan_lot(const ParsedScan &scan, ScanSource source);
    void join_verif(const std::string &lot_id, const std::string &key, ScanSource source);
    void scan_lot_seal(const ParsedScan &scan, ScanSource source);
    void scan_lot_seal_open(const ParsedScan &scan, ScanSource source);
    void entry_error(int entry_id, const std::string &message, ScanSource source);
    void draw_menu_bar();
    void draw_windows();
    void draw_login_modal();
    void draw_douchette_modal(); // easter egg : la douchette a ete scannee
    void check_setup();
    void draw_setup_modal();
    void draw_server_modal(); // connexion au back, sans badge tant que l'API est injoignable
    void create_first_admin();
    void draw_toasts();
    void check_inactivity();
    void apply_default_open_state();

    void   work_area(ImVec2 &origin, ImVec2 &size) const;

    float                   menu_bar_height_ = 0.0f;
    std::string             lot_to_show_;
    std::string             seal_to_show_;
    std::string             pack_to_show_;
    std::string             journal_lot_;
    // Premiere configuration : aucun responsable avec un badge valide
    bool                    setup_known_     = false;
    bool                    needs_admin_     = false;
    bool                    server_modal_requested_ = false;
    bool                    setup_busy_      = false;
    double                  next_setup_check_ = 0.0;
    std::string             setup_matricule_;
    std::string             setup_nom_;
    std::string             setup_prenom_;
    std::string             setup_pin_;
    std::string             setup_pin_confirm_;
    Json                    setup_created_; // compte cree, pour imprimer le badge
    double                  last_activity_ = 0.0;
    bool                    reset_done_    = false;
    bool                    layout_pending_ = true;
    std::vector< Toast >    toasts_;
    bool                    login_prompt_ = false;
    PinPrompt               pin_;
    bool                    login_prompt_opened_ = false;
    bool                    douchette_pending_   = false;
    std::size_t             douchette_index_     = 0;
    std::string             login_reason_;
    std::string             login_manual_;
    std::string             declared_name_;   // nom saisi dans la fenetre de connexion (sans badge)
    std::string             declared_action_; // nom declare pendant l'action en attente, vide sinon
    std::function< void() > pending_action_;
};

// Petites aides communes aux fenetres
std::string display_date(const Json &value);
std::string display_datetime(const Json &value);

} // namespace qrprotec
