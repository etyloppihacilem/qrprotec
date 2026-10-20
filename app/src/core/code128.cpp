/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          code128.cpp
        -\-    _|__
         |\___/  . \        Created on 20 Oct. 2026 at 22:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "code128.hpp"

#include <cctype>
#include <cstddef>

namespace qrprotec {

const char *const kCode128Patterns[107] = {
  "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212", "221213", "221312",
  "231212", "112232", "122132", "122231", "113222", "123122", "123221", "223211", "221132", "221231", "213212",
  "223112", "312131", "311222", "321122", "321221", "312212", "322112", "322211", "212123", "212321", "232121",
  "111323", "131123", "131321", "112313", "132113", "132311", "211313", "231113", "231311", "112133", "112331",
  "132131", "113123", "113321", "133121", "313121", "211331", "231131", "213113", "213311", "213131", "311123",
  "311321", "331121", "312113", "312311", "332111", "314111", "221411", "431111", "111224", "111422", "121124",
  "121421", "141122", "141221", "112214", "112412", "122114", "122411", "142112", "142211", "241211", "221114",
  "413111", "241112", "134111", "111242", "121142", "121241", "114212", "124112", "124211", "411212", "421112",
  "421211", "212141", "214121", "412121", "111143", "111341", "131141", "114113", "114311", "411113", "411311",
  "113141", "114131", "311141", "411131", "211412", "211214", "211232", "2331112",
};

namespace {

constexpr int kCodeC  = 99;
constexpr int kCodeB  = 100;
constexpr int kStartB = 104;
constexpr int kStartC = 105;
constexpr int kStop   = 106;

std::size_t digit_run(const std::string &text, std::size_t from) {
  std::size_t end = from;
  while (end < text.size() && std::isdigit(static_cast< unsigned char >(text[end])))
    ++end;
  return end - from;
}

} // namespace

std::vector< int > code128_symbols(const std::string &text) {
  if (text.empty())
    return {};
  for (const char character : text)
    if (static_cast< unsigned char >(character) < 32 || static_cast< unsigned char >(character) > 126)
      return {};
  // jeu C pour une suite de chiffres assez longue pour gagner de la place malgre le changement de jeu : 4 en
  // debut ou en fin de texte (un seul changement), 6 au milieu (deux changements) ; une suite impaire garde son
  // premier chiffre en jeu B
  std::vector< int > symbols;
  bool               set_c = false;
  for (std::size_t index = 0; index < text.size();) {
    const std::size_t run = digit_run(text, index);
    const bool        at_edge = index == 0 || index + run == text.size();
    const bool        use_c   = run >= (at_edge ? 4u : 6u) || (run == text.size() && run % 2 == 0);
    if (use_c && run % 2 == 1 && !set_c) {
      // chiffre impair en jeu B, la suite paire en jeu C
      if (symbols.empty())
        symbols.push_back(kStartB);
      symbols.push_back(text[index] - 32);
      ++index;
      continue;
    }
    if (use_c) {
      if (symbols.empty())
        symbols.push_back(kStartC);
      else if (!set_c)
        symbols.push_back(kCodeC);
      set_c = true;
      for (std::size_t end = index + run - run % 2; index < end; index += 2)
        symbols.push_back((text[index] - '0') * 10 + (text[index + 1] - '0'));
      continue;
    }
    if (symbols.empty())
      symbols.push_back(kStartB);
    else if (set_c)
      symbols.push_back(kCodeB);
    set_c = false;
    // caracteres en jeu B jusqu'a la prochaine suite de chiffres qui merite le jeu C
    const std::size_t stop = run > 0 ? index + run : index + 1;
    for (; index < stop; ++index)
      symbols.push_back(text[index] - 32);
  }
  int checksum = symbols.front();
  for (std::size_t position = 1; position < symbols.size(); ++position)
    checksum += static_cast< int >(position) * symbols[position];
  symbols.push_back(checksum % 103);
  symbols.push_back(kStop);
  return symbols;
}

std::vector< bool > code128_modules(const std::string &text) {
  std::vector< bool > modules;
  for (const int symbol : code128_symbols(text)) {
    bool bar = true;
    for (const char *width = kCode128Patterns[symbol]; *width; ++width, bar = !bar)
      modules.insert(modules.end(), static_cast< std::size_t >(*width - '0'), bar);
  }
  return modules;
}

} // namespace qrprotec
