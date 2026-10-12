#pragma once

#include <string>

namespace qrprotec {

void set_verbose_logging(bool enabled);
bool verbose_logging_enabled();
void debug_log(const std::string& message);
// Erreur affichee a l'ecran, aussi ecrite dans le journal systeme (syslog, donc journalctl ; sur la
// sortie d'erreur si c'est un terminal). Avec une `source`, un message identique au precedent de cette source n'est
// pas repete (erreurs reaffichees a chaque tentative de connexion...).
void error_log(const std::string& message, const std::string& source = {});

} // namespace qrprotec
