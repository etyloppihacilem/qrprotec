/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          settings.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../inateck/inateck_worker.hpp"
#include "../printer/printer.hpp"
#include "../render/raster.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace qrprotec {

// Position d'une fenetre en fraction de la zone de travail (independant de la resolution).
struct WindowLayout {
    bool  open = false;
    float x    = 0.0f;
    float y    = 0.0f;
    float w    = 0.3f;
    float h    = 0.5f;
};

struct AppSettings {
    // Connexion a l'API locale
    std::string api_url = "http://127.0.0.1:8001";
    std::string api_token;

    // Interface
    float font_size             = 18.0f;
    int   inactivity_minutes    = 15;   // 0 = jamais
    int   expiring_soon_days    = 30;
    bool  require_private_label = true; // hors mode privilegie, valider une verif exige l'etiquette privee

    // Impression
    PrintSettings                        print;
    // Etiquettes chargees dans l'imprimante : largeur dans le sens de la tete, hauteur dans le sens du
    // defilement. Un modele dans l'autre sens est tourne d'un quart de tour a l'impression.
    float                                label_width_mm      = 40.0f;
    float                                label_height_mm     = 30.0f;
    bool                                 rotate_counterclockwise = false;
    bool                                 flip_labels             = false;
    std::string                          label_title = "PROTECTION CIVILE\nPARIS CENTRE"; // {{titre}}
    std::map< std::string, std::string > label_templates; // id de categorie -> fichier .qr

    // Signal de mauvais scan
    bool               sound_enabled = true;
    std::string        beep_command; // vide = bip integre (aplay/paplay). %f = fichier wav genere
    bool               flash_enabled     = true;
    bool               flash_in_sdk_mode = true;
    ScannerErrorSignal scanner_signal;

    // Disposition par defaut (restauree a la reinitialisation)
    std::map< std::string, WindowLayout > layout;
    static constexpr int                  kLayoutVersion = 2; // les dispositions plus anciennes sont ignorees

    static AppSettings           defaults();
    static std::filesystem::path file_path();
    void                         load();
    bool                         save(std::string &error) const;
    PhysicalLabel                physical_label() const;
};

} // namespace qrprotec
