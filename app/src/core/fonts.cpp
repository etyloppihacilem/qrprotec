/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          fonts.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 11:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "fonts.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace qrprotec {

namespace {

bool usable(const std::filesystem::path &path) {
  std::error_code error;
  return !path.empty() && std::filesystem::is_regular_file(path, error);
}

std::string fontconfig_match() {
  FILE *pipe = popen("fc-match -f '%{file}' 'DejaVu Sans:lang=fr' 2>/dev/null", "r");
  if (!pipe)
    return {};
  std::array< char, 1024 > buffer{};
  std::string              result;
  while (fgets(buffer.data(), static_cast< int >(buffer.size()), pipe))
    result += buffer.data();
  pclose(pipe);
  return result;
}

std::string find_font(bool bold) {
  const char *file = bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf";
  std::vector< std::filesystem::path > candidates;
  if (const char *env = std::getenv(bold ? "QRPROTEC_FONT_BOLD" : "QRPROTEC_FONT"))
    candidates.emplace_back(env);
  std::error_code error;
  const auto      executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error)
    candidates.push_back(executable.parent_path() / "fonts" / file);
#ifdef QRPROTEC_FONT_DIR
  candidates.push_back(std::filesystem::path(QRPROTEC_FONT_DIR) / file);
#endif
  for (const std::string dir : { "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/",
                                 "/usr/share/fonts/dejavu/", "/usr/share/fonts/TTF/" })
    candidates.emplace_back(dir + file);
  if (bold) {
    for (const auto &candidate : candidates)
      if (usable(candidate))
        return candidate.string();
    return {};
  }
  for (const char *path : { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/TTF/DejaVuSans.ttf",
                            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" })
    candidates.emplace_back(path);
  for (const auto &candidate : candidates)
    if (usable(candidate))
      return candidate.string();
  const std::string matched = fontconfig_match();
  return usable(matched) ? matched : std::string();
}

} // namespace

const std::string &ui_font_path() {
  static const std::string path = find_font(false);
  return path;
}

const std::string &bold_font_path() {
  static const std::string path = find_font(true);
  return path;
}

} // namespace qrprotec
