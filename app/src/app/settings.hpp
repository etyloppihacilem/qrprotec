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
    std::map< std::string, std::string > label_templates; // id de categorie -> fichier .qr

    // Signal de mauvais scan
    bool               sound_enabled = true;
    std::string        beep_command; // vide = bip integre (aplay/paplay). %f = fichier wav genere
    bool               flash_enabled     = true;
    bool               flash_in_sdk_mode = true;
    ScannerErrorSignal scanner_signal;

    // Disposition par defaut (restauree a la reinitialisation)
    std::map< std::string, WindowLayout > layout;

    static AppSettings           defaults();
    static std::filesystem::path file_path();
    void                         load();
    bool                         save(std::string &error) const;
};

} // namespace qrprotec
