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

namespace qrprotec {

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

    void rebuild_preview();
    void draw_document_panel();
    void draw_element_panel();
    void draw_properties(TemplateElement &element);
    void draw_placeholders_panel();
    void insert_placeholder(const std::string &name);
    void refresh_placeholders();
    void draw_preview_panel();
    void draw_print_test_popup();
    void open_print_test();
    void poll_print_task();

    void reload_templates();
    void new_template();

    TemplateDocument document_;
    RasterImage preview_;
    int selected_element_ = -1;
    int selected_kind_ = 0;
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
    std::vector<std::filesystem::path> image_files_;
    std::vector<TemplateDocument> document_list_;
};

} // namespace qrprotec
