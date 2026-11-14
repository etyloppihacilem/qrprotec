/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          paths.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "paths.hpp"

#include "../printer/logging.hpp"
#include "system.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <regex>

namespace qrprotec {

namespace {

std::mutex            mutex;
std::filesystem::path current;
bool                  initialized = false;
std::string           problem;

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
  const std::filesystem::path wanted =
      directory.empty() ? std::filesystem::path(default_templates_dir()) : std::filesystem::path(directory);
  current     = sandbox_visible_path(wanted);
  initialized = true;
  if (current != wanted)
    info_log("Dossier des modèles : " + wanted.string() + " remplacé par " + current.string()
             + " (même dossier, seul chemin partagé avec le Flatpak)");
  // dossier absent : cree (sous Flatpak, un dossier non partage est cree dans le bac a sable : signale ci-dessous)
  std::error_code error;
  if (!std::filesystem::exists(current, error)) {
    error.clear();
    if (std::filesystem::create_directories(current, error))
      info_log("Dossier des modèles créé : " + current.string());
    else if (error)
      error_log("Impossible de créer le dossier des modèles " + current.string() + " : " + system_error_text(error),
                "modèles");
  }
  problem = directory_access_problem(current, true);
  if (!problem.empty())
    error_log(problem, "modèles");
  else
    info_log("Dossier des modèles : " + current.string());
}

std::string templates_dir_problem() {
  std::lock_guard< std::mutex > lock(mutex);
  return problem;
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

std::vector< std::filesystem::path > glob_templates(const std::string &pattern, const std::string &subdirectory) {
  std::vector< std::filesystem::path > matches;
  const std::regex                     expression = glob_regex(pattern);
  std::error_code                      error;
  const std::filesystem::path          directory = subdirectory.empty() ? templates_dir() : templates_dir() / subdirectory;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    if (error != std::errc::no_such_file_or_directory) // images/ absent : normal ; dossier absent : deja signale
      error_log("Lecture du dossier " + directory.string() + " impossible : " + system_error_text(error), "modèles");
    return matches;
  }
  for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
    std::error_code entry_error;
    if (iterator->is_regular_file(entry_error) && std::regex_match(iterator->path().filename().string(), expression))
      matches.push_back(iterator->path());
  }
  if (error)
    error_log("Lecture du dossier " + directory.string() + " interrompue : " + system_error_text(error), "modèles");
  std::sort(matches.begin(), matches.end());
  return matches;
}

std::vector< std::string > label_images() {
  std::vector< std::string > images;
  for (const std::string subdirectory : { std::string(LABEL_IMAGES_DIR), std::string() }) {
    std::vector< std::filesystem::path > found;
    for (const char *pattern : { "*.png", "*.jpg", "*.jpeg" })
      for (const auto &path : glob_templates(pattern, subdirectory))
        found.push_back(path);
    std::sort(found.begin(), found.end());
    for (const auto &path : found)
      images.push_back(subdirectory.empty() ? path.filename().string()
                                            : (std::filesystem::path(subdirectory) / path.filename()).generic_string());
  }
  return images;
}

} // namespace qrprotec
