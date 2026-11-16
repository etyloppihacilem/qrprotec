/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          app_internal.hpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:40
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Aides partagees par les fichiers de App (app.cpp, app_*.cpp), pas par le reste du poste.

#pragma once

#include <string>

namespace qrprotec {

// Heure locale « HH:MM » (envois en attente, etapes de la vérif en cascade)
std::string now_hhmm();

} // namespace qrprotec
