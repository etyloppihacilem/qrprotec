/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          fonts.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "fonts.hpp"
#include "../printer/logging.hpp"
#include "system.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <vector>


namespace qrprotec {

namespace {

bool usable(const std::filesystem::path &path) {
  std::error_code error;
  return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::string fontconfig_match() {
#ifdef _WIN32
  return {}; // pas de fontconfig : polices fournies dans fonts/ a cote de l'executable
#else
  FILE *pipe = popen("fc-match -f '%{file}' 'DejaVu Sans:lang=fr' 2>/dev/null", "r");
  if (!pipe)
    return {};
  std::array< char, 1024 > buffer{};
  std::string              result;
  while (fgets(buffer.data(), static_cast< int >(buffer.size()), pipe))
    result += buffer.data();
  pclose(pipe);
  return result;
#endif
}

// file : nom du fichier DejaVu ; env : variable qui l'impose ; system_dirs : dossiers ou le chercher sur le systeme
std::vector< std::filesystem::path > font_candidates(const char *file, const char *env,
                                                     std::initializer_list< const char * > system_dirs) {
  std::vector< std::filesystem::path > candidates;
  if (const char *value = std::getenv(env))
    candidates.emplace_back(value);
  if (const std::filesystem::path executable = executable_dir(); !executable.empty())
    candidates.push_back(executable / "fonts" / file);
#ifdef QRPROTEC_FONT_DIR
  candidates.push_back(std::filesystem::path(QRPROTEC_FONT_DIR) / file);
#endif
  for (const char *dir : system_dirs)
    candidates.emplace_back(std::string(dir) + file);
  return candidates;
}

std::string first_usable(const std::vector< std::filesystem::path > &candidates) {
  for (const auto &candidate : candidates)
    if (usable(candidate))
      return candidate.string();
  return {};
}

std::string find_font(bool bold) {
  const char *file = bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf";
  std::vector< std::filesystem::path > candidates =
    font_candidates(file, bold ? "QRPROTEC_FONT_BOLD" : "QRPROTEC_FONT",
                    { "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/", "/usr/share/fonts/dejavu/",
                      "/usr/share/fonts/TTF/" });
  if (bold)
    return first_usable(candidates);
  for (const char *path : { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/TTF/DejaVuSans.ttf",
                            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" })
    candidates.emplace_back(path);
  if (std::string found = first_usable(candidates); !found.empty())
    return found;
  const std::string matched = fontconfig_match();
  return usable(matched) ? matched : std::string();
}

} // namespace

const std::string &ui_font_path() {
  static const std::string path = [] {
    std::string found = find_font(false);
    if (found.empty())
      error_log("Aucune police trouvée (DejaVu Sans, Liberation, Noto ; ni fc-match) : textes des étiquettes non dessinés",
                "polices");
    return found;
  }();
  return path;
}

const std::string &bold_font_path() {
  static const std::string path = [] {
    std::string found = find_font(true);
    if (found.empty())
      info_log("Police DejaVu Sans Bold introuvable : gras simulé");
    return found;
  }();
  return path;
}

const std::string &mono_font_path() {
  static const std::string path = [] {
    std::string found = first_usable(font_candidates(
      "DejaVuSansMono.ttf", "QRPROTEC_FONT_MONO",
      { "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-mono-fonts/", "/usr/share/fonts/dejavu/",
        "/usr/share/fonts/TTF/" }));
    if (found.empty())
      info_log("Police DejaVu Sans Mono introuvable : blocs de code de la documentation en police normale");
    return found;
  }();
  return path;
}

} // namespace qrprotec
