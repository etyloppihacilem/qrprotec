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
#include "feedback.hpp"
#include "print_queue.hpp"
#include "scan_stack.hpp"
#include "settings.hpp"

#include "imgui.h"

#include <functional>
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
    bool        privileged = false;

    std::string display() const { return prenom + " " + nom; }
};

struct VerifSession {
    bool        active = false;
    std::string lot_id;
    std::string key; // cle de l'etiquette privee si scannee
    Json        lot; // detail du lot (items attendus, exigences)
    bool        loading    = false;
    bool        submitting = false;
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
    bool loading_lots  = false;
    bool loading_stock = false;
    // incrementes a chaque rechargement : les fenetres rechargent alors leur volet de detail
    int item_types_version = 0;
    int lot_types_version  = 0;
    int lots_version       = 0;
    int users_version      = 0;

    std::string item_type_name(const std::string &type) const;
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
    void        logout(const std::string &reason = {});
    void        reset_session(); // retour a l'etat initial (inactivite)
    void        require_login(const std::string &what, std::function< void() > action);
    Json        user_ref() const; // valeur du champ "user" des requetes
    std::string user_name() const;

    // Verifs et mouvements
    void start_verif(const std::string &lot_id, const std::string &key);
    void show_lot(const std::string &lot_id);  // ouvre la fiche du lot (fenetre Lots)
    std::string take_lot_to_show();            // lu par la fenetre Lots
    void cancel_verif();
    void submit_verif();
    bool verif_key_ok() const;
    void add_stack_to_lot();
    void verif_target_lot();
    void stack_to_stock();
    void stock_verif();

    // Donnees
    void refresh_item_types();
    void refresh_lot_types();
    void refresh_lots();
    void refresh_users();
    void refresh_stock();

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
    VerifSession                 verif;
    Json                         last_report; // dernier compte rendu de verif
    std::string                  last_report_lot;
    LabelPreviewState            preview;

    std::vector< std::unique_ptr< AppWindow > > windows;

  private:
    void resolve_item(int entry_id);
    void resolve_pack(int entry_id);
    void login_with_badge(const ParsedScan &scan, ScanSource source);
    void scan_lot(const ParsedScan &scan, ScanSource source);
    void entry_error(int entry_id, const std::string &message, ScanSource source);
    void draw_menu_bar();
    void draw_windows();
    void draw_login_modal();
    void check_setup();
    void draw_setup_modal();
    void create_first_admin();
    void draw_toasts();
    void check_inactivity();
    void apply_default_open_state();

    void   work_area(ImVec2 &origin, ImVec2 &size) const;

    float                   menu_bar_height_ = 0.0f;
    std::string             lot_to_show_;
    // Premiere configuration : aucun responsable avec un badge valide
    bool                    setup_known_     = false;
    bool                    needs_admin_     = false;
    bool                    setup_busy_      = false;
    double                  next_setup_check_ = 0.0;
    std::string             setup_matricule_;
    std::string             setup_nom_;
    std::string             setup_prenom_;
    Json                    setup_created_; // compte cree, pour imprimer le badge
    double                  last_activity_ = 0.0;
    bool                    reset_done_    = false;
    bool                    layout_pending_ = true;
    std::vector< Toast >    toasts_;
    bool                    login_prompt_ = false;
    bool                    login_prompt_opened_ = false;
    std::string             login_reason_;
    std::string             login_manual_;
    std::function< void() > pending_action_;
};

// Petites aides communes aux fenetres
std::string display_date(const Json &value);
std::string display_datetime(const Json &value);

} // namespace qrprotec
