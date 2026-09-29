/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          search.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "search.hpp"

#include <cctype>
#include <algorithm>
#include <vector>

namespace qrprotec {

namespace {

std::vector< std::string > words(const std::string &text) {
  std::vector< std::string > result;
  std::string                current;
  for (const char character : text) {
    if (std::isalnum(static_cast< unsigned char >(character))) {
      current += character;
    } else if (!current.empty()) {
      result.push_back(current);
      current.clear();
    }
  }
  if (!current.empty())
    result.push_back(current);
  return result;
}

} // namespace

std::string normalize_search(const std::string &text) {
  // caracteres accentues courants en francais (UTF-8, 2 octets)
  static const struct {
    const char *from;
    const char *to;
  } folds[] = { { "à", "a" }, { "â", "a" }, { "ä", "a" }, { "á", "a" }, { "é", "e" }, { "è", "e" }, { "ê", "e" },
                { "ë", "e" }, { "î", "i" }, { "ï", "i" }, { "í", "i" }, { "ô", "o" }, { "ö", "o" }, { "ó", "o" },
                { "ù", "u" }, { "û", "u" }, { "ü", "u" }, { "ú", "u" }, { "ç", "c" }, { "ÿ", "y" }, { "œ", "oe" },
                { "æ", "ae" }, { "À", "a" }, { "Â", "a" }, { "Ä", "a" }, { "É", "e" }, { "È", "e" }, { "Ê", "e" },
                { "Ë", "e" }, { "Î", "i" }, { "Ï", "i" }, { "Ô", "o" }, { "Ö", "o" }, { "Ù", "u" }, { "Û", "u" },
                { "Ü", "u" }, { "Ç", "c" }, { "Œ", "oe" }, { "Æ", "ae" } };
  std::string output;
  for (std::size_t index = 0; index < text.size();) {
    bool folded = false;
    if (static_cast< unsigned char >(text[index]) >= 0x80) {
      for (const auto &fold : folds) {
        const std::string from(fold.from);
        if (text.compare(index, from.size(), from) == 0) {
          output += fold.to;
          index += from.size();
          folded = true;
          break;
        }
      }
    }
    if (!folded) {
      output += static_cast< char >(std::tolower(static_cast< unsigned char >(text[index])));
      ++index;
    }
  }
  return output;
}

int search_score(const std::string &query, const std::string &text) {
  const std::string          needle   = normalize_search(query);
  const std::string          haystack = normalize_search(text);
  const std::vector< std::string > query_words = words(needle);
  if (query_words.empty())
    return 0; // recherche vide : tout correspond
  const std::vector< std::string > text_words = words(haystack);
  int                              score      = 0;
  for (const std::string &word : query_words) {
    int best = -1;
    for (std::size_t position = 0; position < text_words.size(); ++position)
      if (text_words[position].compare(0, word.size(), word) == 0)
        best = std::max(best, position == 0 ? 4 : 3); // debut du premier mot : encore mieux
    if (best < 0 && haystack.find(word) != std::string::npos)
      best = 1;
    if (best < 0)
      return -1;
    score += best;
  }
  if (haystack.compare(0, needle.size(), needle) == 0)
    score += 5;
  if (haystack == needle)
    score += 10;
  return score;
}

} // namespace qrprotec
