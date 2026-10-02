/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          settings.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../inateck/inateck_worker.hpp"
#include "../net/http.hpp"
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
    // Connexion au back : API locale (meme machine, port non expose) par defaut, ou back distant en
    // HTTPS avec une cle de front (`qrprotec-manage frontkey add NOM` sur le serveur). Les variables
    // d'environnement QRPROTEC_API_URL, QRPROTEC_API_TOKEN, QRPROTEC_API_KEY et QRPROTEC_API_CA_FILE
    // (kiosk : /etc/qrprotec/front-api.conf) priment sur ces valeurs.
    std::string api_url = "http://127.0.0.1:8001";
    std::string api_token;   // jeton optionnel de l'API locale (QRPROTEC_LOCAL_API_TOKEN du back)
    std::string api_key;     // cle de front distant (en-tete X-QRProtec-Key)
    std::string api_ca_file; // HTTPS : certificat de l'autorite du serveur (ex. autorite interne de Caddy)

    // Interface
    float font_size             = 18.0f;
    int   inactivity_minutes    = 15;   // 0 = jamais
    int   expiring_soon_days    = 30;
    int   order_lead_days       = 15; // previsions : delai de livraison d'une commande
    int   forecast_history_months = 6; // previsions : mois d'historique pour mesurer la consommation
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
    std::string                          templates_dir; // vide = app/templates du depot (voir core/paths.hpp)

    // Signal de mauvais scan
    bool               sound_enabled = true;
    int                remote_scanner_timeout_minutes = 5; // telephone-douchette : fermeture apres deconnexion
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
    ApiEndpoint                  api_endpoint() const; // reglages + variables d'environnement
    // Valeur imposee par l'environnement (QRPROTEC_API_URL...) ou nullptr
    static const char           *api_env(const char *name);
};

} // namespace qrprotec
