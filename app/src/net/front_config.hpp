/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          front_config.hpp
        -\-    _|__
         |\___/  . \        Created on 7 Nov. 2026 at 13:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <filesystem>
#include <string>

namespace qrprotec {

// Fichier de configuration d'un front distant, cree sur le serveur par
// `qrprotec-manage frontkey add NOM --config FICHIER` (voir database/inventory/front_config.py) :
// URL, cle du front et certificat de l'autorite, chiffres en AES-256-GCM avec une cle derivee du mot
// de passe (PBKDF2-HMAC-SHA256). Lu sans connexion au serveur, le mot de passe est demande a l'import.
struct FrontConfig {
    std::string name;       // nom du front sur le serveur (frontkey list)
    std::string api_url;    // https://nom-du-serveur
    std::string api_key;    // qrpf_...
    std::string api_ca_pem; // vide = autorites du systeme
    std::string created;
};

// Dechiffre le texte du fichier. false + message en francais (mot de passe incorrect, fichier invalide...).
bool decrypt_front_config(const std::string &text, const std::string &password, FrontConfig &config,
                          std::string &error);
bool read_front_config(const std::filesystem::path &path, const std::string &password, FrontConfig &config,
                       std::string &error);

// Ecrit le certificat de l'autorite dans `path` (lisible par tous : ce n'est pas un secret).
bool write_server_ca(const std::filesystem::path &path, const std::string &pem, std::string &error);

} // namespace qrprotec
