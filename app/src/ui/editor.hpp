#pragma once

#include "../core/template.hpp"
#include "../render/raster.hpp"
#include "../printer/printer.hpp"

#include <unordered_map>
#include <vector>
#include <filesystem>
#include <string>
#include <future>

namespace qrprotec {

class Editor {
public:
    Editor();
    ~Editor();

    void draw();

private:
    struct PrintResult {
        bool success = false;
        std::string error;
    };

    void rebuild_preview();
    void draw_document_panel();
    void draw_element_panel();
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

    std::vector<std::filesystem::path> qr_files_;
    std::vector<TemplateDocument> document_list_;
};

} // namespace qrprotec
