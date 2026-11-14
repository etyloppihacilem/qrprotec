#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace qrprotec {

// Texte lisible d'une erreur systeme (« Permission non accordée (13) »...), pour le journal et l'ecran.
std::string system_error_text(const std::error_code &code);
std::string errno_text(); // errno courant

// Ecrit `content` dans `path` par un fichier temporaire renomme (jamais de fichier a moitie ecrit), dossier cree au
// besoin. En cas d'echec : `error` rempli et ecrit dans le journal systeme sous `source`.
bool write_file_atomic(const std::filesystem::path &path, const std::string &content, const std::string &source,
                       std::string &error);
// Lit tout `path`. Fichier absent : false sans rien journaliser ; autre echec : journalise sous `source`.
bool read_file(const std::filesystem::path &path, std::string &content, const std::string &source);

// Vrai dans le bac a sable Flatpak.
bool in_flatpak();

// Probleme d'acces a un dossier (ou au dossier d'un fichier), vide si tout va bien :
// - sous Flatpak, dossier non partage avec le systeme : l'appli n'y voit pas les fichiers du disque et ce qu'elle
//   y ecrit reste dans le bac a sable (disparait a la fermeture, sans aucune erreur d'ecriture) ;
// - dossier absent, pas un dossier, illisible ou en lecture seule (`need_write`).
std::string directory_access_problem(const std::filesystem::path &directory, bool need_write);

// Sous Flatpak, chemin equivalent partage avec le systeme quand celui donne ne l'est pas (/home/x -> /var/home/x
// sur Silverblue / Kinoite, ou /home est un lien vers /var/home). Sinon le chemin tel quel.
std::filesystem::path sandbox_visible_path(const std::filesystem::path &path);

} // namespace qrprotec
