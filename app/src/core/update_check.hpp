/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          update_check.hpp
        -\-    _|__
         |\___/  . \        Created on 10 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>

namespace qrprotec {

// Mise a jour automatique de l'application macOS : derniere release GitHub (API releases/latest).

struct ReleaseAsset {
    std::string version; // "1.2.5" (tag v1.2.5 sans le v)
    std::string url;     // browser_download_url du fichier
    std::string sha256;  // empreinte publiee par GitHub (champ digest), en hexadecimal minuscule
    long long   size = 0;
};

// Compare deux versions "1.2.10" / "1.2.9" champ par champ (nombres) : < 0, 0 ou > 0. Un "v" en tete est
// ignore ; un champ non numerique compte pour 0.
int compare_versions(const std::string &left, const std::string &right);

// Version publiable : chiffres separes par des points ("1.2.5"), autre chose que 0.0.0 (constructions de
// developpement ou hors tag, qui ne se mettent jamais a jour).
bool is_release_version(const std::string &version);

// Lit la reponse de GET /repos/<depot>/releases/latest et y cherche le fichier asset_name. Refuse un
// brouillon, une pre-release, un tag qui n'est pas une version et un fichier sans empreinte SHA-256.
bool parse_latest_release(
  const std::string &json, const std::string &asset_name, ReleaseAsset &asset, std::string &error
);

// SHA-256 d'un contenu, en hexadecimal minuscule
std::string sha256_hex(const std::string &data);

} // namespace qrprotec
