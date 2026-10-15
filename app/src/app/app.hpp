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
#include <set>
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
    bool              resize_pending = false; // reprendre last_pos et last_size (ecran redimensionne)
    bool              fullscreen    = false; // dessinee en plein ecran (mode plein ecran de l'application)
    ImGuiKeyChord     shortcut      = 0;     // raccourci d'ouverture (F1 a F12)
    std::string       shortcut_label;        // affiche dans les menus
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

// Connexion sans badge (nom, prenom et code PIN) dans la fenetre « Connexion requise »
struct NameLogin {
    bool        open  = false;
    bool        busy  = false;
    bool        focus = false;
    std::string prenom;
    std::string nom;
    std::string pin;
    std::string error;
};

// Autre lot du meme lot global ajoute a la verif en cours par son etiquette privee
struct VerifExtra {
    std::string id;
    std::string key;
    Json        lot; // detail du lot (items attendus, exigences, sous-lots)
};

// Lot scelle du meme lot global dont l'etiquette du scelle a ete scannee pendant la verif : scelle intact, compte
// comme verifie avec ses sous-lots sans etre ouvert (services.perform_verif)
struct VerifSealed {
    std::string id;
    std::string code; // code du scelle (preuve du scan)
    std::string name;
};

// Vérif en cascade : lot valide pendant la verif (enregistre a sa validation)
struct CascadeDone {
    std::string at; // heure (HH:MM)
    bool        complete = false;
    bool        sealed   = false; // scelle intact constate
};

struct VerifSession {
    bool                      active = false;
    std::string               lot_id;
    std::string               key; // cle de l'etiquette privee si scannee
    Json                      lot; // detail du lot (items attendus, exigences, sous-lots)
    std::vector< VerifExtra > extras;
    std::vector< VerifSealed > sealed;
    std::string               current; // lot de la derniere etiquette scannee : les items du stock y vont
    // Vérif en cascade (lot global avec des sous-lots, ex : VPS > armoires > sacs) : la verif est celle du lot global
    // (lot_id). On entre dans un sous-lot en scannant son etiquette (ou un de ses items), on le valide a part, et l'on
    // revient au lot qui le contient (meme fonctionnement que le front web).
    std::vector< std::string >            path;    // sous-lots ou l'on est entre, du lot global au lot en cours
    std::map< std::string, CascadeDone >  done;    // lots valides pendant la verif
    std::map< std::string, std::string >  keys;    // etiquettes cles de sous-lots scannees
    std::set< std::string >               entered; // lots ou l'on est entre : leurs sous-lots non faits restent tels quels
    // scan d'un item, d'une etiquette ou d'un scelle d'un lot hors du lot en cours, alors que celui-ci est complet : il
    // est valide automatiquement, puis on continue vers ce lot (on y entre si after_enter)
    std::string                           after_step;
    bool                                  after_enter = false;
    std::vector< std::string >            auto_done; // lots valides automatiquement, pour le message
    bool                      loading    = false;
    bool                      submitting = false;
};

// Repartition des items scannes entre les lots d'une verif (meme regle que le serveur,
// services.assign_items) : un item deja dans un des lots y reste, un nouvel item va dans le lot de la derniere
// etiquette scannee avant lui (ou un de ses sous-lots) qui en attend encore, sinon le premier lot qui en attend.
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

// Fenetre des taches d'un item scanne (etiquette de taches) : toutes cochees et validees, l'item compte comme vu
struct TaskPopup {
    int                        entry_id = 0; // 0 : fermee
    std::vector< std::string > tasks;
    std::vector< char >        checked;
    bool                       open_pending = false;
};

// Signalement d'un probleme : un item, un type d'item d'un lot, ou un lot (fenetre « Signaler un problème »)
struct ProblemTarget {
    std::string lot;      // lot concerne (vide pour un item : le serveur prend son lot)
    std::string lot_name;
    std::string iid;      // item signale
    std::string type;     // type d'item d'un lot (ligne d'un type pendant la verif)
    std::string label;    // ce qui est signale, affiche dans la fenetre
    std::string pack;     // paquet ferme signale (vaut pour tous ses items)
};

// Item ou paquet signale scanne : popup des signalements ouverts et de leurs commentaires
struct ReportsAlert {
    bool        open_pending = false;
    std::string title;
    Json        reports;
    std::string last_key; // meme objet sur une courte duree : une seule popup (rescan, son propre signalement)
    double      last_at = -1000.0;
};

// Etiquette d'ouverture d'un scelle scannee : ecran en grand (scelle valide jusqu'au..., responsables prevenus,
// contenu), sans verif
struct SealOpening {
    bool open_pending = false;
    Json result; // reponse de /api/lots/<id>/seal-open/
};

struct ProblemPrompt {
    bool          open_pending = false;
    bool          busy         = false;
    ProblemTarget target;
    int           usable = -1; // reponse a « toujours utilisable ? » : -1 pas encore, 1 oui, 0 non (a remplacer)
    std::string   comment;
    std::string   error;
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
    void        manual_logout(); // « Se deconnecter » : oublie aussi la session d'une machine non partagee
    // reglage « Machine partagee » enregistre : garde ou efface la session de cette machine
    void        apply_shared_machine();
    void        reset_session(); // retour a l'etat initial (inactivite)
    // allow_declared : le nom declare sans badge (reglage du serveur) suffit pour cette action
    void        require_login(const std::string &what, std::function< void() > action, bool allow_declared = true);
    Json        user_ref() const; // valeur du champ "user" des requetes
    // identite d'une operation sur un lot (verif, ajout) : "user" connecte, sinon "name" declare sans badge
    void        add_identity(Json &body) const;
    // refus du serveur faute d'identite (login_required) : session expiree, ou reglage du nom declare change
    bool        identity_refused(const ApiResult &result);
    void        refresh_server_rules(); // relit declared_identity et le volume par defaut sur le serveur
    // volume de la douchette sans utilisateur connecte : celui par defaut du serveur
    void        apply_default_scanner_volume();
    std::string user_name() const;

    // Verifs et mouvements
    void start_verif(const std::string &lot_id, const std::string &key);
    void show_lot(const std::string &lot_id, const std::string &seal_code = {}); // fiche du lot (fenetre Lots)
    std::string take_lot_to_show();            // lu par la fenetre Lots
    void show_lots_to_treat();                 // fenetre Lots filtree sur les lots a traiter (voyant de la barre)
    bool take_lots_to_treat();                 // lu par la fenetre Lots
    // lots qui demandent de l'attention (incomplets, perimes, jamais verifies, verif recommandee...), hors
    // rangements du stock et lots qui ne font que regrouper des sous-lots (comptes par leurs sous-lots)
    struct LotsAttention {
      int total = 0, red = 0, recommended = 0, expired = 0;
    };
    LotsAttention lots_attention() const;
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
    std::string verif_place() const;
    std::vector< const Json * >   verif_lots() const; // lots couverts : lot scanne, sous-lots, lots ajoutes
    bool                          verif_covers(const std::string &lot_id) const;
    std::string                   verif_title() const; // ex : "Sac de soin + Sac O2"
    std::vector< VerifPlanLot >   plan_verif() const;
    std::set< std::string >       verif_intact() const; // lots scelles intacts de la verif et leurs sous-lots
    std::vector< const Json * >   partial_verif_lots() const; // lots complets d'une verif groupee incomplete
    // Vérif en cascade
    bool                        verif_cascade() const; // lot global avec des sous-lots
    std::string                 verif_level() const;   // lot en cours (le lot global au depart)
    const Json                 *verif_lot(const std::string &id) const;
    std::string                 verif_parent(const std::string &id) const;
    std::vector< const Json * > verif_children(const std::string &id) const;
    bool                        verif_under(const std::string &id, const std::string &top) const;
    std::set< std::string >     verif_subtree(const std::string &id) const;
    bool                        verif_touched(const std::string &id) const; // entre dedans ou scans dedans
    // sous-lots laisses de cote a la validation de top : deja valides ou pas commences
    std::vector< std::string >  verif_excluded(const std::string &top) const;
    std::set< std::string >     verif_scope(const std::string &top) const; // lots verifies a la validation de top
    void                        enter_level(const std::string &id);
    void                        leave_level(); // retour au lot qui contient le lot en cours, sans le valider
    // valide le lot en cours (partial : verif partielle ou reassort de ce lot) et revient au lot qui le contient
    void                        submit_step(bool partial = false);
    bool                        verif_level_complete() const; // lot en cours complet, validable sans question
    bool                        leave_complete_level(const std::string &id, bool enter); // validation auto, true si lancee
    void                        finish_cascade();
    void add_stack_to_lot();
    void verif_target_lot();
    void stack_to_stock();
    // items scannes sans lot : sortis du stock (comptes utilises s'ils ne reviennent pas a une verif)
    void stack_out();
    void stock_verif();

    // Signalements de problemes : connexion demandee avant la fenetre (pas de nom declare), envoi independant de la
    // verif. Pendant la verif, un item signale compte comme vu (ajoute a la pile).
    void report_problem(const ProblemTarget &target);
    // popup et bip si l'objet scanne a des signalements ouverts (key : "item:IID" ou "pack:ID")
    void alert_reports(const std::string &key, const std::string &title, const Json &reports, ScanSource source);
    void refresh_report_counts(); // nouveaux signalements (admins) : rappel dans la barre de menu
    // connexion d'un admin : Signalements ouverte a cote de la fenetre Lots s'il y a des signalements non traites
    void open_reports_beside_lots();
    Json report_counts = Json::object();

    // Telephone-douchette
    void start_remote_session();
    void close_remote_session(const std::string &reason = "fermée depuis le poste");

    // Donnees
    void refresh_item_types();
    void refresh_lot_types();
    void refresh_lots(bool quiet = false); // quiet : rafraichissement periodique, pas de message d'erreur
    void refresh_users();
    void refresh_stock();
    void refresh_forecast();

    // Impression : un jeu de parametres par etiquette
    bool print_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what);
    // Fenetre d'apercu (image de l'etiquette + bouton Imprimer) ; plusieurs etiquettes se parcourent
    bool preview_labels(TemplateCategory category, const std::vector< Parameters > &labels, const std::string &what);
    void preview_jobs(std::vector< PrintJob > jobs, const std::string &title);
    // etiquettes d'un scellage (Gestion des lots, compte rendu de vérif) : scelle et ouverture seulement
    void preview_seal_labels(const Json &lot);
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
    void        request_layout_reset() {
      layout_pending_ = true;
      fullscreen_     = false;
    }
    // Plein ecran : chaque fenetre ouverte occupe toute la zone sous la barre de menu, on passe de l'une a
    // l'autre avec Ctrl+Tab, les raccourcis ou le menu Fenetres. Ctrl+F11 bascule.
    bool        fullscreen() const { return fullscreen_; }
    void        toggle_fullscreen() { fullscreen_ = !fullscreen_; }
    void        focus_next_window(int direction); // fenetre ouverte suivante (1) ou precedente (-1)
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
    // reglage du serveur (/api/health/) : volume de la douchette des utilisateurs qui n'en ont pas choisi (-1 : inconnu)
    int                          scanner_volume_default = -1;
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
    // Machine non partagee : session gardee d'un demarrage a l'autre (voir StoredLogin)
    void remember_login(StoredLogin login);
    void forget_login();
    void restore_login();   // reconnexion avec la session gardee (PIN redemande par le serveur s'il en a un)
    void session_expired(); // jeton refuse par le serveur : deconnexion, puis reconnexion si machine non partagee
    void show_pin_blocked(const Json &data, ScanSource source);
    void forgot_pin();
    void draw_pin_modal();
    void scan_lot(const ParsedScan &scan, ScanSource source);
    void join_verif(const std::string &lot_id, const std::string &key, ScanSource source);
    void scan_lot_seal(const ParsedScan &scan, ScanSource source);
    void scan_lot_seal_open(const ParsedScan &scan, ScanSource source);
    void entry_error(int entry_id, const std::string &message, ScanSource source);
    void draw_menu_bar();
    void window_menu_item(AppWindow &window);
    void handle_shortcuts();
    void assign_shortcuts();
    void draw_windows();
    void draw_login_modal();
    void draw_name_login();
    void send_name_login();
    void draw_douchette_modal(); // easter egg : la douchette a ete scannee
    void open_tasks(int entry_id);
    void finish_tasks(bool done); // done : toutes les taches validees, l'item compte comme vu
    void draw_tasks_modal();
    void draw_problem_modal();
    void draw_reports_alert();
    void draw_seal_opening_modal();
    void send_problem();
    void reload_verif_lots(); // contenu a jour des lots de la verif (item signale inutilisable : a remplacer)
    void flush_cascade_seals(); // vérif en cascade : scelles intacts constates enregistres tout de suite
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
    bool                    lots_to_treat_ = false;
    bool                    reports_on_login_ = false; // en attente du compte des signalements
    bool                    reports_beside_lots_ = false; // Lots retrecie : reprend sa place a la deconnexion
    double                  next_lots_poll_ = 0.0;
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
    StoredLogin             last_login_;            // derniere connexion reussie (en memoire)
    bool                    restore_pending_ = true; // reconnexion au premier contact avec le serveur
    bool                    layout_pending_ = true;
    ImVec2                  last_work_origin_{ 0, 0 }; // zone de travail de l'image precedente
    ImVec2                  last_work_size_{ 0, 0 };
    bool                    fullscreen_     = false;
    std::string             focused_window_; // derniere fenetre de l'application ayant eu le focus
    std::vector< Toast >    toasts_;
    bool                    login_prompt_ = false;
    PinPrompt               pin_;
    NameLogin               name_login_;
    bool                    login_prompt_opened_ = false;
    bool                    douchette_pending_   = false;
    std::size_t             douchette_index_     = 0;
    TaskPopup               tasks_;
    ProblemPrompt           problem_;
    ReportsAlert            reports_alert_;
    SealOpening             seal_opening_;
    double                  next_reports_poll_ = 0.0;
    bool                    login_declared_    = true; // nom declare accepte pour l'action en attente
    std::string             login_reason_;
    std::string             login_manual_;
    std::string             declared_name_;   // nom saisi dans la fenetre de connexion (sans badge)
    std::string             declared_action_; // nom declare pendant l'action en attente, vide sinon
    std::function< void() > pending_action_;
};

// Petites aides communes aux fenetres
std::string display_date(const Json &value);
// Action passee : « aujourd'hui / hier à HH:MM », sinon la date ; long_form ajoute le jour de la semaine (fiches)
std::string display_datetime(const Json &value, bool long_form = false);
// Idem apres un verbe : « le 12/10/2026 à 09:00 » ou « aujourd'hui à 09:00 » (« scellé %s »)
std::string display_on(const Json &value, bool long_form = false);
// Explication de l'etiquette privee d'un lot (QR code dans le lot) et son emplacement s'il est renseigne
std::string key_hint(const Json &lot);

} // namespace qrprotec
