/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          inateck.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 11:29
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <chrono>
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
    // Parametres (HID, volume, prefixe...) et deconnexion reserves aux utilisateurs connectes
    void set_settings_unlocked(bool unlocked) { settings_unlocked_ = unlocked; }

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

    InateckWorker inateck_worker_;
    bool inateck_window_open_ = false;
    bool settings_unlocked_ = false;
    int inateck_volume_ = 2;
    bool inateck_vibration_ = false;
    bool inateck_sdk_output_ = true;
    bool hid_enabled_ = false;
    int hid_timeout_ms_ = 50;
    int hid_minimum_length_ = 3;
    char inateck_prefix_[64] = {};
    char inateck_suffix_[64] = {};
    char inateck_name_[128] = {};
    std::string saved_device_id_;
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
