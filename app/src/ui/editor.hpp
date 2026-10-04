#pragma once

#include "../core/template.hpp"
#include "../render/raster.hpp"
#include "../printer/printer.hpp"

#include <unordered_map>
#include <vector>
#include <filesystem>
#include <string>
#include <future>
#include <functional>
#include <optional>

#include "imgui.h"

namespace qrprotec {

// Editeur d'etiquettes, organise comme un logiciel de dessin : barre d'outils (nouveau, ouvrir,
// enregistrer, annuler...), calques a gauche, etiquette au centre (selection, deplacement et
// redimensionnement a la souris, zoom), proprietes a droite.
class Editor {
public:
    Editor();
    ~Editor();

    void draw();          // fenetre autonome
    void draw_contents(); // contenu seul (la fenetre est geree par l'application)

    // Reglages de l'application : titre ({{titre}}), taille d'etiquette par defaut, impression via la
    // file de l'application (adaptation au sens de l'etiquette physique)
    void set_label_title(const std::string &title);
    void set_default_media(double width_mm, double height_mm);
    void set_print_callback(std::function< void(const TemplateDocument &) > callback) { print_callback_ = std::move(callback); }

private:
    struct PrintResult {
        bool success = false;
        std::string error;
    };

    // Zone d'un element en mm (QR : carre)
    struct Box {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };

    enum class DragMode { None, Move, Resize, Pan };
    enum class PendingAction { None, New, Open };

    void rebuild_preview();
    void draw_toolbar();
    void draw_layers_panel();
    void draw_canvas_panel();
    void draw_canvas();
    void draw_canvas_menu();
    void draw_side_panel();
    void draw_document_properties();
    void draw_properties(TemplateElement &element);
    void draw_placeholders_panel();
    void insert_placeholder(const std::string &name);
    void refresh_placeholders();
    void draw_print_test_popup();
    void open_print_test();
    void poll_print_task();

    // Modeles : ouverture, enregistrement, modifications non enregistrees
    void draw_open_popup();
    void draw_save_as_popup();
    void draw_unsaved_popup();
    void draw_export_popup();
    void request(PendingAction action, int index = -1);
    void run_pending();
    bool save();
    bool save_to(const std::string &file);
    void load_document(const TemplateDocument &document);

    // Historique (annuler / retablir) et etat "modifie"
    void reset_history();
    void track_history();
    void undo();
    void redo();
    bool dirty() const { return dirty_; }

    // Elements
    bool has_selection() const;
    TemplateElement *selected();
    void select(int index);
    void add_element(TemplateElement element);
    void delete_selected();
    void duplicate_selected();
    void copy_selected();
    void paste();
    void move_selected(int delta); // +1 : vers l'avant (au-dessus), -1 : vers l'arriere
    void move_element(int from, int to);
    void align_selected(int horizontal, int vertical); // -1 / 0 / 1, 2 = inchange
    void nudge_selected(float dx, float dy);
    void handle_shortcuts();

    static Box element_box(const TemplateElement &element);
    static void set_element_box(TemplateElement &element, const Box &box);
    std::string unique_id(const std::string &base) const;

    // Vue de l'etiquette : zoom (pixels ecran par pixel d'etiquette) et decalage dans le canevas
    void fit_view();
    void zoom_at(float factor, ImVec2 anchor);

    void reload_templates();
    void new_template();

    TemplateDocument document_;
    RasterImage preview_;
    int selected_element_ = -1;
    bool preview_dirty_ = true;
    std::string message_;
    std::string export_path_ = "label.png";
    std::string template_path_ = "";
    unsigned int texture_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    bool print_test_open_ = false;
    bool print_test_popup_pending_ = false;
    std::vector<std::string> placeholder_names_;
    std::unordered_map<std::string, std::string> print_values_;
    PrintSettings print_settings_;
    NiimbotB1Printer printer_;
    std::future<PrintResult> print_task_;

    std::string label_title_ = "PROTECTION CIVILE\nPARIS CENTRE";
    double default_width_mm_ = 40.0;
    double default_height_mm_ = 30.0;
    std::function< void(const TemplateDocument &) > print_callback_;

    std::string loaded_templates_dir_;
    std::vector<std::filesystem::path> qr_files_;
    std::vector<std::string> image_files_; // relatifs au dossier des modeles (images/...)
    std::vector<TemplateDocument> document_list_;

    // Historique
    std::vector<TemplateDocument> undo_;
    std::vector<TemplateDocument> redo_;
    TemplateDocument committed_;
    std::string committed_serial_;
    std::string saved_serial_;
    bool dirty_ = false;

    // Dialogues
    bool open_requested_ = false;
    bool save_as_requested_ = false;
    bool unsaved_requested_ = false;
    bool export_requested_ = false;
    PendingAction pending_ = PendingAction::None;
    int pending_index_ = -1;
    std::string save_as_name_;
    std::string open_filter_;

    // Canevas
    float zoom_ = 1.0f;
    ImVec2 offset_ = ImVec2(0.0f, 0.0f);
    bool fit_pending_ = true;
    ImVec2 canvas_size_ = ImVec2(0.0f, 0.0f);
    bool show_grid_ = true;
    bool snap_ = true;
    bool show_frames_ = true;
    float grid_mm_ = 0.5f;
    DragMode drag_ = DragMode::None;
    int drag_handle_ = -1;
    ImVec2 drag_mouse_ = ImVec2(0.0f, 0.0f);
    ImVec2 drag_offset_ = ImVec2(0.0f, 0.0f);
    Box drag_box_;
    bool guide_x_ = false; // repere de centrage affiche pendant un deplacement
    bool guide_y_ = false;
    bool canvas_focused_ = false;
    bool cursor_valid_ = false;
    ImVec2 cursor_mm_ = ImVec2(0.0f, 0.0f);
    bool focus_element_tab_ = false;
    std::optional<TemplateElement> clipboard_;
};

} // namespace qrprotec
