/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          updater.hpp
        -\-    _|__
         |\___/  . \        Created on 10 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/update_check.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace qrprotec {

// Mise a jour automatique de QRProtec.app (macOS) depuis les releases GitHub :
//   - au demarrage (apres une minute) puis toutes les 6 h, GET api.github.com/.../releases/latest ;
//   - version plus recente : le .dmg est telecharge en tache de fond (curl du systeme) dans
//     ~/Library/Caches/QRProtec/updates et son SHA-256 compare a l'empreinte publiee par GitHub ;
//   - pret : installe a la fermeture de l'application, ou tout de suite (« Installer et redemarrer ») ;
//     Contents/Resources/qrprotec-update.sh attend la fin de l'application, monte le .dmg, verifie le
//     nouveau .app (identifiant, signature) et le met a la place de l'ancien.
// Ailleurs (RPM, Flatpak, constructions de developpement) : rien, les mises a jour passent par dnf / flatpak.
class Updater {
  public:
    enum class State { Unsupported, Disabled, Idle, Checking, UpToDate, Downloading, Ready, Error };

    struct Status {
        State       state = State::Unsupported;
        std::string available;   // version trouvee (Downloading, Ready)
        std::string message;     // erreur, ou raison de l'absence de mise a jour automatique
        std::string checked_at;  // derniere verification reussie, "HH:MM"
        bool        needs_admin = false; // dossier de l'application non modifiable : mot de passe demande
        bool        launched    = false; // installation lancee (l'application va se fermer)
    };

    Updater();
    ~Updater();
    Updater(const Updater &)            = delete;
    Updater &operator=(const Updater &) = delete;

    // Version de cette construction (-DQRPROTEC_VERSION, tag sans le v), vide hors release
    static std::string current_version();

    void   start(bool enabled); // fil de verification ; sans effet si non pris en charge
    void   set_enabled(bool enabled);
    void   check_now();
    Status status() const;

    // Lance l'installation de la version prete. restart : relance l'application ensuite. L'appelant ferme
    // l'application juste apres (le script attend sa fin).
    bool install(bool restart, std::string &error);
    // Fermeture de l'application : installe la version prete si c'est possible sans mot de passe
    void on_exit();

  private:
    void run();
    void check();
    void set_state(State state, const std::string &message = {});
    bool run_curl(const std::string &url, const std::string &output, int max_seconds, std::string &error);

    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    std::thread             thread_;
    Status                  status_;
    ReleaseAsset            ready_;      // Ready : version telechargee
    std::string             ready_file_; // Ready : chemin du .dmg verifie
    std::string             bundle_;     // chemin de QRProtec.app
    std::string             cache_dir_;
    bool                    enabled_   = false;
    bool                    requested_ = false;
    bool                    stop_      = false;
    std::atomic< int >      child_{ 0 }; // curl en cours (tue a la fermeture)
};

} // namespace qrprotec
