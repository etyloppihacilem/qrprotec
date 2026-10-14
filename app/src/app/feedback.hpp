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
    // Etiquette de taches scannee : douchette en bleu avec un autre bip, son montant et ecran bleu.
    void task(ScanSource source, const AppSettings &settings);
    // Item ou paquet signale scanne : bip de l'ordinateur (deux notes egales) et signal de la douchette, sans
    // clignotement (la popup des signalements s'affiche).
    void notice(ScanSource source, const AppSettings &settings);
    void test(const AppSettings &settings); // pour la fenetre Reglages
    void draw_overlay();                    // a appeler a chaque frame, apres les fenetres

    std::function< void() > on_phone_error; // mauvais scan venant du telephone-douchette

  private:
    void play_beep(const AppSettings &settings, const std::string &wav);

    Inateck    &inateck_;
    double      flash_start_ = -1.0;
    bool        flash_task_  = false; // clignotement bleu (taches) plutot que rouge
    std::string wav_path_;
    std::string task_wav_path_;
    std::string notice_wav_path_;
};

// Genere le fichier WAV du bip d'erreur (deux tons descendants).
bool write_error_wav(const std::string &path);
// Genere le fichier WAV du signal des taches (trois notes montantes).
bool write_task_wav(const std::string &path);
bool write_notice_wav(const std::string &path);

} // namespace qrprotec
