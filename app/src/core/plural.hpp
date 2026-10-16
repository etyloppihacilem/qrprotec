/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          plural.hpp
        -\-    _|__
         |\___/  . \        Created on 16 Oct. 2026 at 22:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>

// Accord en nombre : singulier pour 0 et 1, pluriel au-dela (par defaut le mot suivi d'un s).
inline std::string pl(long long count, const std::string &word, const std::string &plural = std::string()) {
  return count > 1 ? (plural.empty() ? word + "s" : plural) : word;
}

// « 3 items », « 1 item », « 0 item ».
inline std::string nb(long long count, const std::string &word, const std::string &plural = std::string()) {
  return std::to_string(count) + " " + pl(count, word, plural);
}
