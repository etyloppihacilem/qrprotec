/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hotspot.hpp
        -\-    _|__
         |\___/  . \        Created on 23 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Wifi de secours de la borne : quand le wifi du local tombe, la borne (serveur sur la meme machine) diffuse son
// propre wifi et les telephones continuent d'utiliser QRProtec par ce wifi. Le poste demarre et arrete
// qrprotec-hotspot.service avec systemctl (regle polkit du paquet kiosk) et lit son etat dans /run/qrprotec-hotspot.

#pragma once

#include "../core/hotspot_state.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace qrprotec {

class Hotspot {
  public:
    // Borne (QRPROTEC_KIOSK) avec le serveur sur cette machine et le paquet kiosk qui fournit le service
    static bool supported();

    // Relit l'etat (toutes les 2 s au plus, ou tout de suite avec force)
    void poll(double now, bool force = false);
    void start();
    void stop();

    bool         busy() const { return busy_; }
    HotspotState state() const;
    std::string  error() const; // dernier echec (demarrage, arret), vide sinon

  private:
    void run(const std::string &verb);
    void read_files();

    mutable std::mutex mutex_;
    HotspotState       state_;
    std::string        error_;
    std::atomic< bool > busy_{ false };
    double             next_poll_ = 0.0;
};

} // namespace qrprotec
