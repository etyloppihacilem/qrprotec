/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          hotspot_state.hpp
        -\-    _|__
         |\___/  . \        Created on 23 Nov. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Wifi de secours de la borne (packaging/files/qrprotec-hotspot) : lecture de son fichier d'etat et QR codes affiches
// par le poste. Sans dependance a ImGui, pour les tests.

#pragma once

#include <string>

namespace qrprotec {

struct HotspotState {
    bool        active = false;
    std::string ssid, password, address, domain, iface;
    long long   since = 0; // debut (secondes depuis 1970), 0 si inconnu
};

// Fichier /run/qrprotec-hotspot/state (lignes CLE=valeur ecrites par qrprotec-hotspot start). Actif seulement avec un
// nom et un mot de passe.
HotspotState parse_hotspot_state(const std::string &text);

// Contenu d'un QR code « rejoindre ce wifi » (WPA), compris par l'appareil photo d'Android et d'iOS :
// WIFI:T:WPA;S:<nom>;P:<mot de passe>;; avec \ ; , : " echappes.
std::string wifi_qr_payload(const std::string &ssid, const std::string &password);

} // namespace qrprotec
