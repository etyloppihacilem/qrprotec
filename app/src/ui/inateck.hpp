/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inateck.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 11:29
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../inateck/inateck_worker.hpp"
#include "../inateck/hid_classifier.hpp"

namespace qrprotec {

class Inateck {

  public:
    Inateck();
    ~Inateck();

    void draw();
    void update(); // a appeler a chaque image (memorise la douchette connectee)
    void draw_menu();   // contenu du menu "Douchette" (a appeler dans une barre de menu)
    void draw_window(); // fenetre de parametrage si ouverte
    void open_window() { inateck_window_open_ = true; }
    // Recherche et connexion ouvertes a tous ; volume, mode inventaire et deconnexion aux utilisateurs
    // connectes ; les autres parametres (HID, modele, parametres du SDK, entretien) aux admins.
    void set_access(bool logged_in, bool admin) {
      logged_in_ = logged_in;
      admin_     = admin;
    }
    // Volume voulu (celui de l'utilisateur connecte, sinon le volume par defaut du serveur), applique des que la
    // douchette est authentifiee et a chaque reconnexion. -1 : ne touche pas au volume de la douchette.
    void set_user_volume(int volume) { desired_volume_ = volume; }
    // Appele quand l'utilisateur connecte change le volume (pour l'enregistrer sur le serveur)
    void set_volume_changed_callback(std::function<void(int)> callback) { volume_changed_ = std::move(callback); }

    std::vector<ScanEvent> take_scans() { return inateck_worker_.take_scans(); }
    void signal_error(const ScannerErrorSignal& signal) { inateck_worker_.signal_error(signal); }
    bool hid_enabled() const { return hid_enabled_; }
    bool sdk_connected() const { return inateck_worker_.snapshot().authenticated; }
    std::vector<unsigned int> handle_hid_character(unsigned int character);
    std::vector<unsigned int> flush_hid_characters();
    // A appeler juste avant glfwPollEvents : mesure le temps ecoule depuis la lecture precedente
    void begin_poll();
    struct HidKeyResult {
      bool consume = false;
      std::vector<unsigned int> replay;
    };
    HidKeyResult handle_hid_key(int key, int action);
    void handle_window_focus(bool focused);

  private:
    void draw_inateck_window();
    void draw_scanner_settings(const InateckSnapshot& state);
    void draw_scanner_setting(const ScannerSettingValue& setting);
    void draw_scanner_actions(const InateckSnapshot& state);
    // Volume et mode inventaire : dans la fenetre et dans le menu Douchette
    void draw_quick_settings(const InateckSnapshot& state);
    void draw_quick_menu(const InateckSnapshot& state);
    void draw_inventory(const InateckSnapshot& state); // mode inventaire : memoire de la douchette
    bool confirm_clear_cache_ = false; // ouvre la confirmation (aussi depuis le menu)

    InateckWorker inateck_worker_;
    bool inateck_window_open_ = false;
    bool logged_in_ = false;
    bool admin_ = false;
    int desired_volume_ = -1;
    int applied_volume_ = -1; // volume envoye a la douchette connectee (-1 apres une deconnexion)
    std::function<void(int)> volume_changed_;
    void change_volume(int volume); // choix de l'utilisateur : envoye a la douchette et enregistre
    void apply_volume(const InateckSnapshot& state);
    int scanner_model_ = 0; // 0 : deduit du nom de la douchette
    std::map<int, int> pending_values_; // curseurs en cours de deplacement (flag -> valeur)
    bool inateck_sdk_output_ = true;
    bool hid_enabled_ = false;
    int hid_timeout_ms_ = 50;
    int hid_minimum_length_ = 3;
    char inateck_prefix_[64] = {};
    char inateck_suffix_[64] = {};
    char inateck_name_[128] = {};
    std::string saved_device_id_;
    // Reconnexion automatique a la derniere douchette tant qu'elle n'est pas connectee
    std::chrono::steady_clock::time_point next_reconnect_{};
    bool manual_disconnect_ = false; // pas de reconnexion automatique apres un « Déconnecter »
    void start_pairing();
    void reconnect();
    bool can_reconnect(const InateckSnapshot& state) const;
    HidScanClassifier hid_classifier_;
    std::vector<unsigned int> pending_hid_characters_;
    HidScanClassifier::TimePoint last_hid_character_{};
    bool has_last_hid_character_ = false;
    HidScanClassifier::TimePoint last_poll_{};
    HidScanClassifier::Clock::duration poll_slack_{};
    bool has_last_poll_ = false;

    void load_hid_settings();
    void save_hid_settings() const;
    void apply_hid_settings();
};

} // namespace qrprotec
