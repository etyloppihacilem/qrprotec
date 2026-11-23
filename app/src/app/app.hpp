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
#include "../core/verif_cache.hpp"
#include "../core/verif_plan.hpp"
#include "../net/api_client.hpp"
#include "../net/websocket_link.hpp"
#include "feedback.hpp"
#ifdef QRPROTEC_HOTSPOT
#include "hotspot.hpp"
#endif
#include "../printer/label_languages.hpp"
#include "print_queue.hpp"
#include "scan_stack.hpp"
#include "settings.hpp"
#include "updater.hpp"

#include "imgui.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qrprotec {

// Format a envoyer a une imprimante TSPL / ZPL pour un rouleau (maintenance : calibrage...) : taille, support et
// positions des Reglages
labels::RawJob printer_media_job(const PrintSettings &print, double width_mm, double height_mm, bool ribbon,
                                 double dots_per_mm);

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
    virtual void             reset_view(App &) {}    // vue par defaut : selection, filtres, cases, onglet (inactivite, deconnexion)
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
    bool              reload_pending = false; // on_open au prochain affichage (ecran rallume)
    bool              reload_on_wake = true;  // false : saisies en cours a ne pas ecraser (Reglages)
    bool              place_pending = true; // appliquer la disposition par defaut au prochain affichage
    bool              focus_pending = false;
    bool              resize_pending = false; // reprendre last_pos et last_size (ecran redimensionne)
    bool              fullscreen    = false; // dessinee en plein ecran (mode plein ecran de l'application)
    // rangee sur une moitie ou un quart de l'ecran : snap_h -1 gauche / 1 droite, snap_v -1 haut / 1 bas, 0 toute
    // la largeur ou hauteur (0, 0 : pas rangee). snap_restore_* : place et taille d'avant, reprises en la sortant.
    int               snap_h = 0;
    int               snap_v = 0;
    ImVec2            snap_restore_pos{ 0, 0 };
    ImVec2            snap_restore_size{ 0, 0 };
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
    std::string fonction;              // role dans l'equipe (l'OPE, le TECH...), texte libre sans effet sur les droits

    std::string display() const { return prenom + " " + nom; }
    bool        admin() const { return role == "admin"; }
    // " (chef)", " (gestion)", " (admin)" ou vide
    std::string role_suffix() const {
      return role == "admin" ? " (admin)" : privileged ? " (gestion)" : role == "chef" ? " (chef)" : "";
    }
};

// Saisie du PIN apres le scan d'un badge (admins, ou utilisateurs ayant un PIN)
struct PinPrompt {
    bool        active = false;
    bool        setup  = false; // code obligatoire pas encore choisi : il le choisit (saisi deux fois)
    // case « Demander a chaque connexion » du choix du code : cochee par defaut si le code est obligatoire, imposee
    // pour un admin. Decochee, le code ne sert qu'a la connexion sans badge (nom, prenom et code).
    bool        at_login        = true;
    bool        at_login_locked = false;
    bool        busy   = false;
    bool        focus  = false;
    std::string matricule;
    std::string key;
    std::string name;
    std::string pin;
    std::string confirm;
    std::string error;
    int         source = 0; // ScanSource du badge
    // carte d'invite jamais utilisee : nom et prenom de l'invite au lieu du PIN (guest_name_required)
    bool        guest       = false;
    int         guest_hours = 48;
    std::string guest_prenom;
    std::string guest_nom;
    std::string guest_antenne;
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

// Vérif en cascade : popup « Passer a … ? » quand on quitte un sous-lot incomplet (scan d'une etiquette ou d'un item
// d'un autre lot). Ignoree, le scan suivant vaut « Passer » (meme fonctionnement que le front web).
struct LeavePrompt {
    bool        open = false;
    std::string level;          // lot incomplet que l'on quitte
    std::string target;         // lot scanne
    int         entry_id = -1;  // item scanne (-1 : etiquette du lot)
    int         missing  = 0;   // items attendus pas encore scannes
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
    // lots dont l'etiquette (publique, cle ou du scelle) a ete scannee : la verif est bien celle de ces lots
    std::set< std::string >               labels;
    std::set< std::string >               entered; // lots ou l'on est entre : leurs sous-lots non faits restent tels quels
    std::map< std::string, CascadeDone >  reopened; // lots valides rouverts : validation d'avant, rendue si l'on en ressort sans scan
    // scan d'un item, d'une etiquette ou d'un scelle d'un lot hors du lot en cours, alors que celui-ci est complet : il
    // est valide automatiquement, puis on continue vers ce lot (on y entre si after_enter)
    std::string                           started_in; // sous-lot dont on a lance la verif (ex : un kit du VPS)
    std::set< std::string >               open_seals; // lots scelles dont on a choisi de verifier le contenu
    // items a cocher (gobelets, sucres : sans etiquette) coches a la main, par lot (services.perform_verif checked)
    std::map< std::string, std::set< std::string > > checked;
    std::string                           seal_finish; // lot scelle dont le scelle constate termine la verif
    std::string                           after_step;
    bool                                  after_enter = false;
    std::vector< std::string >            auto_done; // lots valides automatiquement, pour le message
    LeavePrompt                           leave_prompt;
    int                                   after_entry = -1; // item scanne qui a lance la validation (popup)
    bool                      loading    = false;
    bool                      submitting = false;
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

// Revision ou desinfection d'un item (materiel non consommable) : taches toutes cochees, prochaine echeance (par
// defaut aujourd'hui + la periodicite du type, modifiable)
struct RevisionPrompt {
    bool                       open_pending = false;
    std::string                kind = "revision"; // ou "disinfection"
    std::string                label = "Révision", noun = "révision";
    std::string                iid, type_name, due;
    std::string                lot; // desinfection d'un lot lui-meme (iid vide, type_name = nom du lot)
    int                        months = 0;
    std::vector< std::string > tasks;
    std::vector< char >        checked;
    std::string                next_due; // saisie libre (parse_user_date)
    std::string                note, error;
    bool                       busy = false;
};

// Date de revision ou de desinfection saisie a la main (gestion/admin) : derniere faite et/ou prochaine, pour
// recaler le cycle sans faire l'entretien dans le logiciel
struct MaintenanceDatePrompt {
    bool        open_pending = false;
    std::string kind, label, noun, iid, type_name, due, cycle;
    std::string lot; // desinfection d'un lot lui-meme (iid vide, type_name = nom du lot)
    int         months = 0;
    std::string last_done, next_due; // saisies libres (parse_user_date)
    bool        next_touched = false; // prochaine modifiee a la main : plus recalculee depuis la derniere
    std::string note, error;
    bool        busy = false;
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

// Fenetre « Impressions en attente » (database/inventory/label_prints.py) : demandes du web, imprimees au poste.
// Une file par personne qui demande (plusieurs portables envoient en meme temps : chacun retrouve son ruban). Sur un
// autre ordinateur, les admins y gerent les files du poste a distance (remote).
struct LabelPrints {
    Json            data = Json::object(); // {"pending", "queues", "recent"} de /api/label-prints/pending/
    std::set< int > seen;                  // demandes deja vues : seule une nouvelle rouvre la fenetre
    std::set< int > launched;              // demandes lancees a distance deja parties a l'imprimante
    bool            open    = false;
    bool            focus   = false;
    bool            loading = false;
    bool            busy    = false; // impression lancee, demandes envoyees une par une
    bool            remote  = false; // autre ordinateur (admins) : files du poste, lancees a distance
    double          next_poll = 0.0;
    std::string     queue;         // file choisie (vide : pas encore choisie, plusieurs files)
    bool            queue_by_user = false; // choisie a la main ou par badge : gardee tant qu'elle existe
    double          empty_since = -1.0;    // plus rien en attente depuis (fermeture au bout de 15 min)
    // en direct (poste principal) : WebSocket /ws/label-prints ouvert jusqu'a live_until (15 min apres la derniere
    // impression), l'interrogation toutes les 20 s s'arrete tant qu'il est connecte
    double          live_until       = 0.0;
    bool            live_unsupported = false; // serveur sans ce WebSocket (ancienne version) : interrogation seule
    bool            again            = false; // changement signale pendant une lecture : relire a la fin
    std::string     checked;                  // heure de la derniere lecture reussie (« 21:14 »)
    // badge scanne fenetre ouverte : on montre ses impressions, la connexion attend « Se connecter »
    std::string     badge_matricule;
    std::string     badge_key;
    int             badge_source = 0;
    std::string     error;
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
    Json tasks      = Json::object(); // liste de taches de la gestion (/api/tasks/), fenetre Gestion de la TECH
    Json events     = Json::object(); // evenements a venir (/api/events/), onglet « À venir » de la meme fenetre
    int  events_days = 90;            // horizon des evenements a venir (30, 90 ou 365 jours)
    bool loading_lots     = false;
    bool loading_stock    = false;
    bool loading_forecast = false;
    bool loading_tasks    = false;
    bool loading_events   = false;
    // incrementes a chaque rechargement : les fenetres rechargent alors leur volet de detail
    int item_types_version = 0;
    int lot_types_version  = 0;
    int lots_version       = 0;
    int users_version      = 0;
    int packs_version      = 0; // paquet ouvert : listes a recharger
    int maintenance_version = 0; // revision, desinfection ou date enregistree : onglet Materiel a recharger

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
    // Liste de materiel ouverte dans la fenetre Listes, scans (lot, item) diriges vers elle : verif ou retour
    struct ChecklistScanTarget {
        std::string                                          id;
        std::function< void(const ParsedScan &, ScanSource) > handle;
    } checklist_scan;
    std::string checklist_to_open; // QR code d'une liste scanne : la fenetre Listes l'ouvre

    // Session
    bool        logged_in() const { return user.has_value(); }
    // Deux modes, selon pourquoi on est devant le poste. Mode normal : ce que fait un secouriste ou un chef (scans,
    // lots, verif, listes). Mode gestion (roles gestion et admin, bascule dans la barre ou Ctrl+G) : le mode normal
    // plus la gestion de la TECH, des lots, des items, du stock, des receptions, des etiquettes et de l'impression.
    // privileged() et admin() suivent le mode : en mode normal, un gestionnaire voit le poste comme un chef.
    bool        manager() const { return user && user->privileged; } // role gestion ou admin, quel que soit le mode
    bool        privileged() const { return manager() && gestion_mode_; }
    bool        admin() const { return user && user->admin() && gestion_mode_; } // reglages du front, utilisateurs
    bool        gestion_mode_ = true; // remis a chaque connexion
    // place des fenetres communes aux deux modes (lots, pile de scans...) dans chaque mode : chacun garde sa disposition
    struct ModePlace {
        ImVec2 pos, size;
        int    snap_h = 0, snap_v = 0;
        ImVec2 restore_pos, restore_size;
    };
    std::map< std::string, ModePlace > mode_places_[2];
    void        set_gestion_mode(bool on);
    // options de debug du poste (reglage), pour un admin seulement : le serveur verifie aussi le role
    bool        debug_options() const { return settings.debug_options && admin(); }
    void        notify_refused(const Json &refused, const std::string &prefix);
    bool        can_open(const AppWindow &window) const {
      return (!window.privileged || privileged()) && (!window.admin_only || admin());
    }
    void        logout(const std::string &reason = {});
    void        manual_logout(); // « Se deconnecter » : oublie aussi la session d'une machine non partagee
    // reglage « Machine partagee » enregistre : garde ou efface la session de cette machine
    void        apply_shared_machine();
    void        reset_session(); // retour a l'etat initial (inactivite)
    void        reset_windows_view(); // chaque fenetre revient a sa vue par defaut (personne suivante)
    // allow_declared : le nom declare sans badge (reglage du serveur) suffit pour cette action
    void        require_login(const std::string &what, std::function< void() > action, bool allow_declared = true);
    // « Non connecte » de la pile de scans : fenetre de connexion, saisie nom, prenom et PIN deja ouverte
    void        open_login();
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
    // label : l'etiquette du lot a ete scannee (la fiche ouverte par son scan compte, lot_label_scanned)
    void start_verif(const std::string &lot_id, const std::string &key, bool label = false);
    bool lot_label_scanned(const std::string &lot_id) const { return !lot_id.empty() && lot_id == label_shown_; }
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
    // « de B+ (ou celle de Sac de soin ou de Sac O2) » : etiquette a scanner pour valider (vide sinon)
    std::string verif_label_missing() const;
    std::string verif_children_de(const std::string &id) const; // « de A ou de B » : sous-lots nommes
    bool verif_label_ok() const { return verif_label_missing().empty(); }
    bool verif_ready() const { return verif_key_ok() && verif_label_ok(); }
    Json verif_labels() const;
    std::string verif_place() const;
    std::vector< const Json * >   verif_lots() const; // lots couverts : lot scanne, sous-lots, lots ajoutes
    bool                          verif_covers(const std::string &lot_id) const;
    std::string                   verif_title() const; // ex : "Sac de soin + Sac O2"
    std::vector< VerifPlanLot >   plan_verif() const;
    // Items a cocher de la verif : coche ou non dans un lot, et "checked" envoye (lots de scope, tous si nullptr)
    bool verif_checked(const std::string &lot, const std::string &type) const;
    void toggle_verif_check(const std::string &lot, const std::string &type);
    Json verif_checked_body(const std::set< std::string > *scope = nullptr) const;
    std::string                   discard_advice(const Json &item) const; // perime scanne : a jeter ou a remplacer
    std::set< std::string >       verif_intact() const; // lots scelles intacts de la verif et leurs sous-lots
    std::vector< const Json * >   partial_verif_lots() const; // lots complets d'une verif groupee incomplete
    // Vérif en cascade
    bool                        verif_cascade() const; // lot global avec des sous-lots
    std::string                 verif_level() const;   // lot en cours (le lot global au depart)
    const Json                 *verif_lot(const std::string &id) const;
    std::string                 verif_parent(const std::string &id) const;
    std::vector< const Json * > verif_children(const std::string &id) const;
    std::string                 verif_visible(const std::string &id) const; // lot cache -> lot affiche
    bool                        verif_under(const std::string &id, const std::string &top) const;
    std::set< std::string >     verif_subtree(const std::string &id) const;
    bool                        verif_touched(const std::string &id) const; // entre dedans ou scans dedans
    // lot scelle de la verif dont le scelle n'est pas encore constate : sa verif est le scan de l'etiquette du scelle
    bool                        verif_seal_waiting(const std::string &id) const;
    // sous-lots laisses de cote a la validation de top : deja valides ou pas commences
    std::vector< std::string >  verif_excluded(const std::string &top) const;
    std::set< std::string >     verif_scope(const std::string &top) const; // lots verifies a la validation de top
    void                        enter_level(const std::string &target);
    void                        leave_level(); // retour au lot qui contient le lot en cours, sans le valider
    void                        leave_level_validated(); // bouton « ← Revenir à » : valide le lot en cours (même incomplet), puis remonte
    // valide le lot en cours (partial : verif partielle ou reassort de ce lot) et revient au lot qui le contient
    void                        submit_step(bool partial = false);
    bool                        verif_level_complete() const; // lot en cours complet, validable sans question
    bool                        leave_complete_level(const std::string &id, bool enter); // validation auto, true si lancee
    // sous-lot en cours incomplet, deja commence : ouvre la popup « Passer a target ? » (true si ouverte)
    bool                        ask_leave(const std::string &target, int entry_id);
    void                        answer_leave(int choice); // 0 : valider incomplet, 1 : passer sans valider, 2 : rester
    void                        finish_cascade(const std::string &message = "");
    void add_stack_to_lot();
    void verif_target_lot();
    void stack_to_stock();
    // items scannes sans lot : sortis du stock (comptes utilises s'ils ne reviennent pas a une verif)
    void stack_out();
    void stock_verif();

    // Signalements de problemes : connexion demandee avant la fenetre (pas de nom declare), envoi independant de la
    // verif. Pendant la verif, un item signale compte comme vu (ajoute a la pile).
    void report_problem(const ProblemTarget &target);
    // « Réviser » / « Désinfecter » (connexion demandee d'abord) : item (fiche du serveur, ou evenement a venir avec
    // son objet maintenance) et entretien
    void open_revision(const Json &item, const std::string &kind = "revision");
    // « Saisir la date » d'une revision ou d'une desinfection (gestion/admin)
    void open_maintenance_date(const Json &item, const std::string &kind);
    // desinfection d'un lot lui-meme (lot_disinfection du serveur : {"lot", "lot_name", "due", "months"...})
    void open_lot_disinfection(const Json &info);
    void open_lot_disinfection_date(const Json &info);
    // periodicite de desinfection d'un lot (0 : plus de desinfection), gestion/admin
    void set_lot_disinfection_months(const std::string &lot, int months);
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
    void refresh_tasks();
    void refresh_events();
    // Case « Commandé » d'une commande suggeree (taches du TECH) / decocher une commande passee
    void place_order(const std::string &type, const std::string &name, int quantity);
    void cancel_order(long long id, const std::string &name);
    // Etiquettes d'items demandees depuis le web par un admin : fenetre « Impressions en attente », ouverte d'elle-meme
    // a chaque nouvelle demande, meme sans personne de connecte (voir label_prints.cpp)
    void refresh_label_prints();
    // relit la file maintenant (bouton Rafraichir, signal du WebSocket), ou des la fin de la lecture en cours
    void request_label_prints_refresh();
    // WebSocket des impressions en direct : ouvert, lu et ferme selon live_until (voir label_prints.cpp)
    void update_label_prints_live();
    // Fenetres de gestion ouvertes sur un onglet : stock, orders, transfers (Stock) ; types, items, equipment, packs
    // (Items) ; reception, past_receptions (Reception). La fenetre concernee le prend au prochain affichage.
    std::string stock_tab_to_show_;
    void        show_stock_tab(const std::string &tab);
    // Gestion de la TECH ouverte sur un onglet : tasks (a faire) ou events (a venir)
    std::string tech_tab_to_show_;
    void        show_tech_tab(const std::string &tab) {
      tech_tab_to_show_ = tab;
      open_window("stock");
    }
    // fiche d'un item : Items > Items, recherche sur son iid et item choisi
    std::string item_to_show_;
    void        show_item(const std::string &iid) {
      item_to_show_ = iid;
      show_stock_tab("items");
    }
    // receptions passees d'un type d'item (bouton d'un item)
    std::string receptions_type_to_show_;
    void        show_receptions(const std::string &type) {
      receptions_type_to_show_ = type;
      show_stock_tab("past_receptions");
    }
    // Lots ouverts sur un mode de gestion : "new" (nouveau lot), "order" (ordre des lots), "archived"
    std::string lots_mode_to_open_;
    void        show_lots_mode(const std::string &mode) {
      lots_mode_to_open_ = mode;
      open_window("lots");
    }
    // Type ouvert dans sa fenetre : "lot:<code>" (Types de lots) ou "item:<code>" (Items > Types d'items)
    std::string type_to_show_;
    void        show_type(const std::string &key) {
      type_to_show_ = key;
      if (key.rfind("lot:", 0) == 0)
        open_window("lot_types");
      else
        show_stock_tab("types");
    }
    // « Aller a... » (Ctrl+K) : recherche d'un lot, d'un item, d'un type, d'une fenetre ou d'une action, par-dessus
    // l'ecran. Fermee des qu'on choisit, qu'on clique a cote ou qu'on tape Echap : aucune place prise en permanence.
    struct Hub {
        bool        open    = false;
        bool        focus   = false;
        bool        close   = false; // Ctrl+K de nouveau
        int         cursor  = 0;
        std::string query;
    } hub_;
    void open_hub();
    void draw_hub();
    // Reglages ouverts sur une section (depliee, en haut), depuis le menu qui la concerne
    std::string settings_section_;
    void        open_settings_section(const std::string &label) {
      settings_section_ = label;
      open_window("settings");
    }
    void open_label_prints() {
      label_prints_.open = label_prints_.focus = true;
      label_prints_.remote = !main_poste();
    }
    // badge scanne fenetre « Impressions en attente » ouverte : file de ce badge, pas de connexion tout de suite
    bool label_prints_badge(const std::string &code, ScanSource source);
    // front d'un autre ordinateur (cle de front) sans imprimante : « Imprimer » envoie les etiquettes au poste
    // Poste principal : front de l'API locale (meme machine que le back), ou front distant dont la cle est marquee
    // « poste principal » sur le serveur (kiosk sur une autre machine, back sans kiosk). Il imprime les
    // « Impressions en attente » et gere les ordinateurs connectes. Lu dans /api/health/ (champ poste).
    bool main_poste() const { return settings.api_endpoint().key.empty() || poste_key; }
    bool prints_to_poste() const { return !kLocalPrinting || (!settings.print_here && !main_poste()); }
    void send_to_poste(std::vector< PrintJob > jobs);
    // « Marquer ce lot comme utilisé (demander une vérif) » : tout utilisateur identifie, lot en vérif recommandee
    void mark_lot_used(const std::string &lot_id, const std::string &name);

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
    // Assistant « Changer de rouleau » : gestes pas a pas puis commandes a l'imprimante (calibrage...) ;
    // type_name : rouleau attendu (vide = rouleau en place)
    void open_roll_wizard(const std::string &type_name = "");
    TemplateDocument qr_only_template(TemplateCategory category) const;

    // Interface
    void        notify(const std::string &message, bool error = false);
    void        open_window(const std::string &id);
    AppWindow  *window(const std::string &id);
    void        apply_settings();
    void        select_default_templates(); // modeles fournis choisis pour les usages sans modele
    bool        save_settings();
    // Etiquettes du type `name` chargees dans l'imprimante (vide : format libre) : enregistre et reprend la file
    // si elle attendait ce rouleau
    void        set_loaded_label_type(const std::string &name);
    // Gestion de la TECH, tache « Commander » d'un consommable : la fenetre Impressions et consommables s'ouvre sur
    // l'onglet Consommables
    bool        show_consumables = false;
    // Codes d'action rapide des postes (poste_actions.cpp) : scan d'un code « qrprotec:poste/... »
    void        handle_poste_action(const ParsedScan &scan, ScanSource source);
    // Etiquette du code « rouleau en place » de `roll`, imprimee sur le rouleau `print_on`
    void        print_roll_code(const LabelType &roll, const LabelType &print_on);
    // Modeles sans type d'etiquette : type construit (etiquettes reglees dans les Reglages, orientation qu'ils
    // recevaient a l'impression) et fichier mis a jour. Sans rouleau charge, prend le type le plus courant.
    void        add_missing_label_types();
    // Fichier de configuration chiffre cree sur le serveur (frontkey add NOM --config) : URL, cle et
    // certificat de l'autorite remplacent les reglages du serveur, enregistres aussitot, puis la connexion
    // est testee. Refuse sur une borne ou si l'environnement impose la connexion (front-api.conf).
    bool        import_front_config(const std::string &path, const std::string &password);
    // Fichier deposé sur la fenetre (glisser-deposer) : propose a l'import, fenetre « Connexion au serveur »
    // ouverte si le serveur est injoignable
    void        file_dropped(const std::string &path);
    std::string front_config_path;     // champ « Fichier » de l'import (partage modale / Reglages)
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
    Updater                      updater;        // macOS : mise a jour automatique depuis les releases GitHub
#ifdef QRPROTEC_HOTSPOT
    Hotspot                      hotspot;        // borne (Linux) : wifi de secours (TECH > Wifi de secours)
    void                         open_hotspot() { hotspot_open_pending_ = true; }
#endif
    bool                         quit_requested = false; // ferme l'application (main.cpp), ex. pour installer une mise a jour
    ApiClient                    api;
    ScanStack                    stack;
    // etiquettes envoyees a l'imprimante pas encore recues par le serveur (avant `printer`, qui l'utilise)
    PrintCounter                 print_counter;
    PrintQueue                   printer;
    Feedback                     feedback;
    Inateck                     &inateck;
    Catalog                      catalog;
    std::optional< SessionUser > user;
    // reglage du serveur (/api/health/) : verif et ajout possibles sans badge, sous un nom declare non verifie
    bool                         declared_identity = false;
    bool                         poste_key         = false; // cle de ce front marquee poste principal (serveur)
    // lecture des codes-barres par le front web : activee par la premiere impression d'un code-barres
    bool                         web_barcodes = false;
    bool                         restock_verif_recommended = true; // reglage du serveur : reassort → verif recommandee
    // reglage du serveur (/api/health/) : volume de la douchette des utilisateurs qui n'en ont pas choisi (-1 : inconnu)
    int                          scanner_volume_default = -1;
    std::string                  server_version; // version de release du serveur (/api/health/), vide hors paquet
    VerifSession                 verif;
    Json                         last_report; // dernier compte rendu de verif
    // Vérif hors connexion (une partie de la reserve n'a ni reseau ni wifi) : cache de tout ce qu'une verif lit au
    // serveur, tenu a jour en arriere-plan, et envois de verif en attente renvoyes au retour du reseau
    VerifCache                   vcache;
    VerifOutbox                  outbox;
    bool                         offline() const { return offline_; }
    // cache de vérif mis a jour si plus vieux que max_age secondes (0 : maintenant)
    void                         sync_cache(double max_age = 0.0);
    std::string                  last_report_lot;
    LabelPreviewState            preview;
    RemoteSession                remote;
    WebSocketLink                remote_link;
    std::string                  last_duplicate_;      // dernier doublon ignore (mention discrete)
    double                       last_duplicate_time_ = -100.0;

    std::vector< std::unique_ptr< AppWindow > > windows;

  private:
    void poll_remote();
    void resolve_item(int entry_id);
    void adopt_prescans();
    void resolve_pack(int entry_id);
    void item_resolved(int entry_id, const Json &item, ScanSource source);
    void item_confirmed(int entry_id, const Json &fresh, ScanSource source);
    void pack_resolved(int entry_id, const Json &pack, ScanSource source);
    Json cached_seal_lot(const std::string &id) const;
    void cache_changed(const std::vector< std::string > &keys);
    // POST /api/verifs/ : hors connexion (ou envois deja en attente), mis en file et `done` recoit le compte rendu
    // local `offline` (data["offline"] vrai)
    void post_verif(const Json &body, const std::string &label, Json offline, ApiClient::Callback done);
    Json offline_report(const std::set< std::string > &lots, bool partial, const std::set< std::string > &sealed) const;
    void flush_outbox();
    // compteur d'impressions envoye au serveur (fenetre Gestion > Impressions d'étiquettes)
    void   flush_print_counts();
    bool   print_counts_busy_     = false;
    double next_print_counts_try_ = 0.0;
    bool   offline_         = false;
    bool   cache_syncing_   = false;
    double cache_synced_at_ = -1.0e9;
    bool   outbox_busy_     = false;
    double next_outbox_try_ = 0.0;
    void login_with_badge(const ParsedScan &scan, ScanSource source);
    void send_temp_login(const std::string &matricule, const std::string &token, ScanSource source);
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
    void draw_guest_name();
    void scan_lot(const ParsedScan &scan, ScanSource source);
    void join_verif(const std::string &lot_id, const std::string &key, ScanSource source);
    void scan_lot_seal(const ParsedScan &scan, ScanSource source);
    void scan_lot_seal_open(const ParsedScan &scan, ScanSource source);
    void entry_error(int entry_id, const std::string &message, ScanSource source);
    void draw_menu_bar();
    void draw_print_menu(bool dark_bar, const PrintQueueStatus &print); // menu Impression de la barre
    void draw_gestion_menus(bool dark_bar); // mode gestion : un menu par domaine (TECH, Lots, Items, Stock...)
    void draw_mode_switch(bool dark_bar);   // bascule Normal / Gestion (roles gestion et admin)
    void draw_update_menu(bool dark_bar); // barre de menu : mise a jour prete a installer
    void window_menu_item(AppWindow &window);
    void handle_shortcuts();
    void assign_shortcuts();
    void draw_windows();
    // Rangement des fenetres : glissee sur un bord, une fenetre prend la moitie de l'ecran, dans un coin un quart
    // (apercu pendant le glisser) ; Super+fleches au clavier.
    bool can_snap(const AppWindow &target) const;
    void snap_rect(int h, int v, ImVec2 &pos, ImVec2 &size) const;
    void snap_window(AppWindow &target, int h, int v);
    void snap_focused(int dh, int dv);
    void update_snap_drag(AppWindow *moving);
    void draw_login_modal();
    void draw_name_login();
    void send_name_login();
    void draw_douchette_modal(); // easter egg : la douchette a ete scannee
    void open_tasks(int entry_id);
    void finish_tasks(bool done); // done : toutes les taches validees, l'item compte comme vu
    void draw_tasks_modal();
    void draw_problem_modal();
    void draw_revision_modal();
    void send_revision();
    void draw_maintenance_date_modal();
#ifdef QRPROTEC_HOTSPOT
    void draw_hotspot_modal();
#endif
    void send_maintenance_date();
    void after_maintenance(const std::string &iid);
    void draw_reports_alert();
    void draw_seal_opening_modal();
    void draw_label_prints();
    // file en pause sur une etiquette d'un autre rouleau : fenetre « Changez de rouleau » (meme sans personne de
    // connecte), « Plus tard » la laisse dans la barre de menu jusqu'au prochain rouleau demande
    void        draw_roll_change_modal();
    std::string roll_change_dismissed_;
    // assistant « Changer de rouleau » (roll_change.cpp)
    struct RollWizard {
      bool                     open_pending = false;
      int                      step         = 0;
      std::vector< LabelType > types;
      std::string              chosen;
      bool                     calibrate = false;
      bool                     factory   = false;
    };
    RollWizard roll_wizard_;
    void       draw_roll_wizard();
    void launch_label_prints(std::vector< int > ids);
    void label_queue_action(const std::string &queue, const std::string &action); // admins : launch ou cancel
    void draw_label_queue(const Json &queue);
    void print_label_request(const Json &request); // etiquettes d'une demande (items crees ou images)
    void send_problem();
    void reload_verif_lots(); // contenu a jour des lots de la verif (item signale inutilisable : a remplacer)
    void flush_cascade_seals(); // vérif en cascade : scelles intacts constates enregistres tout de suite
    void check_setup();
    void draw_setup_modal();
    void draw_server_modal(); // connexion au back, sans badge tant que l'API est injoignable
    void create_first_admin();
    void draw_toasts();
    void check_inactivity();
    void reload_after_wake(); // ecran rallume : donnees des fenetres ouvertes rechargees
    void apply_default_open_state();

    void   work_area(ImVec2 &origin, ImVec2 &size) const;

    float                   menu_bar_height_ = 0.0f;
    std::string             lot_to_show_;
    bool                    lots_to_treat_ = false;
    bool                    reports_on_login_ = false; // en attente du compte des signalements
    bool                    reports_beside_lots_ = false; // Lots retrecie : reprend sa place a la deconnexion
    double                  next_lots_poll_ = 0.0;
    std::string             seal_to_show_;
    std::string             label_shown_; // lot dont la fiche a ete ouverte par le scan de son etiquette
    std::string             public_label_scanned_; // etiquette publique du dernier scan (fiche ouverte), sinon vide
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
    // kiosk : extinction de l'ecran apres QRPROTEC_SCREEN_BLANK_SECONDS sans activite (0 = jamais)
    double                  screen_blank_seconds_ = 0.0;
    bool                    reset_done_    = false;
    StoredLogin             last_login_;            // derniere connexion reussie (en memoire)
    bool                    restore_pending_ = true; // reconnexion au premier contact avec le serveur
    bool                    layout_pending_ = true;
    ImVec2                  last_work_origin_{ 0, 0 }; // zone de travail de l'image precedente
    ImVec2                  last_work_size_{ 0, 0 };
    bool                    fullscreen_     = false;
    std::string             snap_drag_id_;            // fenetre glissee au dessus d'une zone de rangement
    int                     snap_drag_h_ = 0;
    int                     snap_drag_v_ = 0;
    bool                    snap_drag_restored_ = false; // fenetre rangee glissee : taille d'avant deja reprise
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
    RevisionPrompt          revision_;
    MaintenanceDatePrompt   maintenance_date_;
#ifdef QRPROTEC_HOTSPOT
    bool                    hotspot_open_pending_ = false;
#endif
    ReportsAlert            reports_alert_;
    SealOpening             seal_opening_;
    LabelPrints             label_prints_;
    WebSocketLink           label_prints_link_; // impressions en direct (poste principal, /ws/label-prints)
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
// Reponses du TECH a un signalement : celle donnee en le passant en cours (« note »), puis celle de la resolution
// (« resolution_note »). Comme sur le front web.
void draw_report_replies(const Json &report);
// Explication de l'etiquette privee d'un lot (QR code dans le lot) et son emplacement s'il est renseigne
std::string key_hint(const Json &lot);

// Attendu sous lequel afficher un item de ce type : son type s'il est attendu, sinon son type a unites
std::string                                  display_row(const Json &requirements, const std::string &type);
// Ligne d'un type parent (pas le minimum d'un sous-type « dont ») : compte en unites (is_group), a unites (« Sérum
// phy » en mL) ou sans unite (« Bandes » remplies par Bandes L ou S, un item = 1)
bool                                         unit_row(const Json &row);
// « 50 mL de Sérum phy » pour un type a unites, « 3 × Bandes » sinon (services.amount_text)
std::string                                  amount_text(int count, const std::string &unit, const std::string &name);
// « mL » precede d'une espace, vide pour un type sans unite
std::string                                  unit_suffix(const Json &row);
// Items a scanner pour `units` unites d'une ligne : le moins d'items qui les remplissent (sinon `units` lui-meme)
int                                          row_items(const Json &row, int units);
// Nom affiche d'un attendu : « Sérum phy (mL) », « dont Sachet… »
std::string                                  requirement_name(const Json &row);
// Ce qui manque : « 3 × Compresses », « 50 mL de Sérum phy (2 × Sachet…) » (services.lacking_text)
std::string                                  lacking_text(const Json &row, int lacking);

} // namespace qrprotec
