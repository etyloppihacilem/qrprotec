/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          feedback.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../inateck/inateck_worker.hpp"
#include "settings.hpp"

#include <functional>
#include <string>

namespace qrprotec {

class Inateck;

// Signal "mauvais scan" : bip + LED rouge sur la douchette (SDK), bip de l'ordinateur et
// clignotement rouge de l'ecran (mode HID).
class Feedback {
  public:
    explicit Feedback(Inateck &inateck);

    void error(ScanSource source, const AppSettings &settings);
    void test(const AppSettings &settings); // pour la fenetre Reglages
    void draw_overlay();                    // a appeler a chaque frame, apres les fenetres

    std::function< void() > on_phone_error; // mauvais scan venant du telephone-douchette

  private:
    void play_beep(const AppSettings &settings);

    Inateck    &inateck_;
    double      flash_start_ = -1.0;
    std::string wav_path_;
};

// Genere le fichier WAV du bip d'erreur (deux tons descendants).
bool write_error_wav(const std::string &path);

} // namespace qrprotec
