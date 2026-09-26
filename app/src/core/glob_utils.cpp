/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          glob_utils.cpp
        -\-    _|__
         |\___/  . \        Created on 26 Sep. 2026 at 11:13
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */


#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Convertit un motif glob (ex: "*.txt") en std::regex
std::regex glob_to_regex(const std::string &pattern) {
  std::string regex_str = "^";
  for (char c : pattern) {
    switch (c) {
      case '*':
        regex_str += ".*";
        break;
      case '?':
        regex_str += ".";
        break;
      case '.':
        regex_str += "\\.";
        break;
      default:
        regex_str += c;
        break;
    }
  }
  regex_str += "$";
  return std::regex(regex_str);
}

// Effectue le glob dans le répertoire courant
std::vector< fs::path > glob_current_dir(const std::string &pattern) {
  std::vector< fs::path > matches;
  std::regex              reg = glob_to_regex(pattern);

  for (const auto &entry : fs::directory_iterator(".")) {
    std::string filename = entry.path().filename().string();
    if (std::regex_match(filename, reg))
      matches.push_back(entry.path());
  }
  return matches;
}
