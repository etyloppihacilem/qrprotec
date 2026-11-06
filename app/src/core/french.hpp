/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          french.hpp
        -\-    _|__
         |\___/  . \        Created on 6 Nov. 2026 at 19:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <cctype>
#include <string>
#include <vector>

// « de » devant un nom de lot : « de Sac de soin », « d'Armoire 1 », « du Sac O2 » (pour « Le Sac O2 »), « des … »,
// « de la … », « de l'… ». Meme regle que deName (app.js) et de_name (views.py).
inline std::string de_name(const std::string &name) {
  auto starts = [&name](const char *prefix) {
    const std::string start(prefix);
    if (name.size() < start.size())
      return false;
    for (size_t i = 0; i < start.size(); ++i)
      if (std::tolower(static_cast< unsigned char >(name[i])) != static_cast< unsigned char >(start[i]))
        return false;
    return true;
  };
  if (starts("les "))
    return "des " + name.substr(4);
  if (starts("le "))
    return "du " + name.substr(3);
  if (starts("la "))
    return "de la " + name.substr(3);
  if (starts("l'"))
    return "de l'" + name.substr(2);
  static const char *const vowels[] = {"a",  "e",  "i",  "o",  "u",  "y",  "à",  "â",  "é",  "è",  "ê",
                                       "ë",  "î",  "ï",  "ô",  "ö",  "ù",  "û",  "ü",  "À",  "Â",  "É",
                                       "È",  "Ê",  "Ë",  "Î",  "Ï",  "Ô",  "Ö",  "Ù",  "Û",  "Ü"};
  for (const char *vowel : vowels)
    if (starts(vowel))
      return "d'" + name;
  return "de " + name;
}

// « de A », « de A ou de B », « de A, de B ou de C »
inline std::string de_names(const std::vector< std::string > &names) {
  std::string result;
  for (size_t i = 0; i < names.size(); ++i)
    result += (i == 0 ? "" : i + 1 == names.size() ? " ou " : ", ") + de_name(names[i]);
  return result;
}
