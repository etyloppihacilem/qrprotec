#include "system.hpp"

#include "../printer/logging.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace qrprotec {

namespace fs = std::filesystem;

std::string system_error_text(const std::error_code &code) {
  if (!code)
    return {};
  return code.message() + " (" + std::to_string(code.value()) + ")";
}

std::string errno_text() {
  const int value = errno;
  if (value == 0)
    return "erreur inconnue";
  return std::string(std::strerror(value)) + " (" + std::to_string(value) + ")";
}

bool write_file_atomic(const fs::path &path, const std::string &content, const std::string &source, std::string &error) {
  std::error_code code;
  if (path.has_parent_path() && !fs::create_directories(path.parent_path(), code) && code) {
    error = "Impossible de créer le dossier " + path.parent_path().string() + " : " + system_error_text(code);
    error_log(error, source);
    return false;
  }
  const fs::path temp = path.string() + ".tmp";
  {
    errno = 0;
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "Impossible d'écrire " + temp.string() + " : " + errno_text();
      error_log(error, source);
      return false;
    }
    output << content;
    output.close();
    if (!output) {
      error = "Erreur d'écriture de " + temp.string() + " : " + errno_text();
      error_log(error, source);
      fs::remove(temp, code);
      return false;
    }
  }
  fs::rename(temp, path, code);
  if (code) {
    error = "Impossible de remplacer " + path.string() + " : " + system_error_text(code);
    error_log(error, source);
    std::error_code ignored;
    fs::remove(temp, ignored);
    return false;
  }
  return true;
}

bool read_file(const fs::path &path, std::string &content, const std::string &source) {
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (errno != ENOENT)
      error_log("Impossible de lire " + path.string() + " : " + errno_text(), source);
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (input.bad()) {
    error_log("Erreur de lecture de " + path.string() + " : " + errno_text(), source);
    return false;
  }
  content = buffer.str();
  return true;
}

bool in_flatpak() {
  static const bool flatpak = [] {
    std::error_code code;
    return std::getenv("FLATPAK_ID") != nullptr || fs::exists("/.flatpak-info", code);
  }();
  return flatpak;
}

namespace {

// Champ de /proc/self/mountinfo : espaces, tabulations... echappes en octal (\040)
std::string unescape_mount(const std::string &value) {
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\\' && index + 3 < value.size()) {
      const std::string octal = value.substr(index + 1, 3);
      if (octal.size() == 3 && octal.find_first_not_of("01234567") == std::string::npos) {
        result += static_cast< char >(std::stoi(octal, nullptr, 8));
        index += 3;
        continue;
      }
    }
    result += value[index];
  }
  return result;
}

bool has_prefix(const std::string &path, const std::string &mount) {
  if (mount == "/")
    return true;
  return path.compare(0, mount.size(), mount) == 0 && (path.size() == mount.size() || path[mount.size()] == '/');
}

// Point de montage qui contient `path` (le plus long prefixe de /proc/self/mountinfo), vide si illisible
std::string mount_point_of(const fs::path &path) {
  std::ifstream input("/proc/self/mountinfo");
  std::string   line;
  std::string   best;
  const std::string target = path.generic_string();
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string        id, parent, device, root, mount;
    if (!(fields >> id >> parent >> device >> root >> mount))
      continue;
    mount = unescape_mount(mount);
    if (has_prefix(target, mount) && mount.size() >= best.size())
      best = mount;
  }
  return best;
}

// Chemin absolu, liens resolus pour la partie qui existe
fs::path real_path(const fs::path &path) {
  std::error_code code;
  fs::path        absolute = fs::absolute(path, code);
  if (code)
    absolute = path;
  fs::path canonical = fs::weakly_canonical(absolute, code);
  return code ? absolute.lexically_normal() : canonical;
}

// Sous Flatpak, la racine du bac a sable est un tmpfs : un chemin dont le point de montage est « / » n'est pas un
// dossier partage avec le systeme (/home/<user> y existe, vide, quand le dossier personnel n'est pas partage).
bool shared_with_host(const fs::path &path) {
  if (!in_flatpak())
    return true;
  const fs::path    real = real_path(path);
  const std::string text = real.generic_string();
  // dossiers temporaires : ephemeres par nature (et /tmp de flatpak-builder, ou passent les tests)
  std::error_code temp_error;
  const fs::path  temp = fs::temp_directory_path(temp_error);
  for (const std::string &root : { std::string("/tmp"), std::string("/var/tmp"),
                                   temp_error ? std::string() : real_path(temp).generic_string() })
    if (!root.empty() && root != "/" && has_prefix(text, root))
      return true;
  const std::string mount = mount_point_of(real);
  return !mount.empty() && mount != "/";
}

} // namespace

fs::path sandbox_visible_path(const fs::path &path) {
  if (!in_flatpak() || path.empty() || shared_with_host(path))
    return path;
  const std::string text = real_path(path).generic_string();
  if (text.rfind("/home/", 0) == 0) {
    const fs::path alternative = fs::path("/var" + text);
    if (shared_with_host(alternative))
      return alternative;
  }
  return path;
}

std::string directory_access_problem(const fs::path &directory, bool need_write) {
  const fs::path  shown = directory.empty() ? fs::path(".") : directory;
  const fs::path  real  = real_path(shown);
  std::error_code code;
  if (!shared_with_host(shown)) {
    const char *id = std::getenv("FLATPAK_ID");
    return "Le dossier " + real.string()
         + " n'est pas partagé avec le Flatpak : l'application n'y voit pas les fichiers et ce qu'elle y écrit disparaît à la "
           "fermeture. Donnez-lui accès avec « flatpak override --user --filesystem="
         + real.string() + " " + (id && *id ? id : "io.github.etyloppihacilem.QRProtec")
         + " » (chemin réel, sans lien symbolique : /var/home/… sur Silverblue), puis relancez l'application.";
  }
  const fs::file_status status = fs::status(shown, code);
  if (code || !fs::exists(status))
    return "Le dossier " + real.string() + " n'existe pas" + (code && code != std::errc::no_such_file_or_directory ? " : " + system_error_text(code) : std::string()) + ".";
  if (!fs::is_directory(status))
    return real.string() + " n'est pas un dossier.";
#ifndef _WIN32
  if (::access(shown.c_str(), R_OK | X_OK) != 0)
    return "Le dossier " + real.string() + " n'est pas lisible : " + errno_text() + ".";
  if (need_write && ::access(shown.c_str(), W_OK) != 0)
    return "Impossible d'écrire dans le dossier " + real.string() + " : " + errno_text() + ".";
#endif
  return {};
}

} // namespace qrprotec
