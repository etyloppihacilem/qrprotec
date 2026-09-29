/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          paths.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <regex>

namespace qrprotec {

namespace {

std::mutex            mutex;
std::filesystem::path current;
bool                  initialized = false;

std::regex glob_regex(const std::string &pattern) {
  std::string expression = "^";
  for (const char character : pattern) {
    if (character == '*')
      expression += ".*";
    else if (character == '?')
      expression += '.';
    else if (std::string(".+()[]{}^$|\\").find(character) != std::string::npos)
      expression += std::string("\\") + character;
    else
      expression += character;
  }
  return std::regex(expression + "$", std::regex::icase);
}

} // namespace

std::string default_templates_dir() {
  if (const char *env = std::getenv("QRPROTEC_TEMPLATES_DIR"); env && *env)
    return env;
#ifdef QRPROTEC_TEMPLATES_DIR
  std::error_code error;
  if (std::filesystem::is_directory(QRPROTEC_TEMPLATES_DIR, error))
    return QRPROTEC_TEMPLATES_DIR;
#endif
  return ".";
}

void set_templates_dir(const std::string &directory) {
  std::lock_guard< std::mutex > lock(mutex);
  current     = directory.empty() ? std::filesystem::path(default_templates_dir()) : std::filesystem::path(directory);
  initialized = true;
  std::error_code error;
  std::filesystem::create_directories(current, error);
}

const std::filesystem::path &templates_dir() {
  std::lock_guard< std::mutex > lock(mutex);
  if (!initialized) {
    current     = default_templates_dir();
    initialized = true;
  }
  return current;
}

std::filesystem::path resolve_template_path(const std::string &path) {
  const std::filesystem::path value(path);
  if (path.empty() || value.is_absolute())
    return value;
  return templates_dir() / value;
}

std::vector< std::filesystem::path > glob_templates(const std::string &pattern) {
  std::vector< std::filesystem::path > matches;
  const std::regex                     expression = glob_regex(pattern);
  std::error_code                      error;
  for (const auto &entry : std::filesystem::directory_iterator(templates_dir(), error))
    if (entry.is_regular_file(error) && std::regex_match(entry.path().filename().string(), expression))
      matches.push_back(entry.path());
  std::sort(matches.begin(), matches.end());
  return matches;
}

} // namespace qrprotec
