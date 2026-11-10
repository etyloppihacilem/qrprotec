/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          updater.cpp
        -\-    _|__
         |\___/  . \        Created on 10 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "updater.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

#ifndef QRPROTEC_VERSION
#define QRPROTEC_VERSION ""
#endif
#ifndef QRPROTEC_UPDATE_REPO
#define QRPROTEC_UPDATE_REPO "etyloppihacilem/qrprotec"
#endif
#ifndef QRPROTEC_UPDATE_ASSET
#define QRPROTEC_UPDATE_ASSET "QRProtec-macos-arm64.dmg"
#endif

namespace qrprotec {

namespace fs = std::filesystem;

namespace {

constexpr auto kFirstCheck = std::chrono::minutes(1);
constexpr auto kInterval   = std::chrono::hours(6);
constexpr auto kRetry      = std::chrono::hours(1);

#ifdef __APPLE__
const char *const kCurl = "/usr/bin/curl";
#else
const char *const kCurl = "curl";
#endif

// Chemin de QRProtec.app : QRPROTEC_UPDATE_BUNDLE (essais), sinon l'executable .../X.app/Contents/MacOS/...
std::string find_bundle(std::string &reason) {
  if (const char *forced = std::getenv("QRPROTEC_UPDATE_BUNDLE"); forced && *forced)
    return forced;
#ifdef __APPLE__
  char     buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0)
    return "";
  std::error_code  error;
  const fs::path   executable = fs::canonical(buffer, error);
  if (error)
    return "";
  const fs::path macos    = executable.parent_path();
  const fs::path contents = macos.parent_path();
  const fs::path bundle   = contents.parent_path();
  if (macos.filename() != "MacOS" || contents.filename() != "Contents" || bundle.extension() != ".app") {
    reason = "Application lancée hors de QRProtec.app : pas de mise à jour automatique.";
    return "";
  }
  const std::string path = bundle.string();
  if (path.rfind("/Volumes/", 0) == 0 || path.find("/AppTranslocation/") != std::string::npos) {
    reason = "Glissez QRProtec dans le dossier Applications pour recevoir les mises à jour automatiques.";
    return "";
  }
  return path;
#else
  reason = "Mises à jour par le paquet (dnf pour le RPM, Logiciels ou « flatpak update » pour le Flatpak).";
  return "";
#endif
}

std::string cache_directory() {
  const char *home = std::getenv("HOME");
  const fs::path base = home && *home ? fs::path(home) : fs::temp_directory_path();
#ifdef __APPLE__
  return (base / "Library/Caches/QRProtec/updates").string();
#else
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  return ((xdg && *xdg ? fs::path(xdg) : base / ".cache") / "qrprotec/updates").string();
#endif
}

std::string read_file(const std::string &path) {
  std::ifstream     file(path, std::ios::binary);
  std::stringstream content;
  content << file.rdbuf();
  return content.str();
}

std::string now_hhmm() {
  const std::time_t now = std::time(nullptr);
  std::tm           local{};
  localtime_r(&now, &local);
  char buffer[8];
  std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
  return buffer;
}

bool writable(const std::string &path) {
  return ::access(path.c_str(), W_OK) == 0;
}

} // namespace

std::string Updater::current_version() {
  return QRPROTEC_VERSION;
}

Updater::Updater() {
  std::string reason;
  if (!is_release_version(current_version())) {
    status_.message = "Version de développement : pas de mise à jour automatique.";
    return;
  }
  bundle_ = find_bundle(reason);
  if (bundle_.empty()) {
    status_.message = reason;
    return;
  }
  cache_dir_          = cache_directory();
  status_.state       = State::Disabled;
  status_.needs_admin = !writable(fs::path(bundle_).parent_path().string()) || !writable(bundle_);
}

Updater::~Updater() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (const int pid = child_.load(); pid > 0)
    ::kill(pid, SIGTERM);
  if (thread_.joinable())
    thread_.join();
}

void Updater::start(bool enabled) {
  std::lock_guard< std::mutex > lock(mutex_);
  if (status_.state == State::Unsupported || thread_.joinable())
    return;
  if (const char *off = std::getenv("QRPROTEC_NO_AUTO_UPDATE"); off && std::string(off) == "1")
    enabled = false;
  enabled_      = enabled;
  status_.state = enabled ? State::Idle : State::Disabled;
  thread_       = std::thread([this]() { run(); });
}

void Updater::set_enabled(bool enabled) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    if (status_.state == State::Unsupported || enabled_ == enabled)
      return;
    enabled_ = enabled;
    if (!enabled && status_.state != State::Ready)
      status_.state = State::Disabled;
    else if (enabled && status_.state == State::Disabled)
      status_.state = State::Idle;
    requested_ = enabled;
  }
  wake_.notify_all();
}

void Updater::check_now() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    if (status_.state == State::Unsupported)
      return;
    requested_ = true;
  }
  wake_.notify_all();
}

Updater::Status Updater::status() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return status_;
}

void Updater::set_state(State state, const std::string &message) {
  std::lock_guard< std::mutex > lock(mutex_);
  status_.state   = state;
  status_.message = message;
}

void Updater::run() {
  auto next = std::chrono::steady_clock::now() + kFirstCheck;
  for (;;) {
    {
      std::unique_lock< std::mutex > lock(mutex_);
      wake_.wait_until(lock, next, [this]() { return stop_ || requested_; });
      if (stop_)
        return;
      const bool requested = requested_;
      requested_           = false;
      if (!requested && std::chrono::steady_clock::now() < next)
        continue;
      // automatique : seulement si active (« Rechercher maintenant » verifie meme desactive)
      if (!enabled_ && !requested) {
        next = std::chrono::steady_clock::now() + kInterval;
        continue;
      }
      if (status_.state == State::Ready || status_.launched) {
        next = std::chrono::steady_clock::now() + kInterval;
        continue;
      }
    }
    check();
    const bool failed = status().state == State::Error;
    next              = std::chrono::steady_clock::now() + (failed ? kRetry : kInterval);
  }
}

bool Updater::run_curl(const std::string &url, const std::string &output, int max_seconds, std::string &error) {
  const std::string errors     = output + ".err";
  const std::string time_limit = std::to_string(max_seconds);
  const std::string agent      = "User-Agent: QRProtec/" + current_version();
  std::vector< std::string > args = {
    kCurl, "-sSfL", "--proto", "=https", "--proto-redir", "=https", "--retry", "2", "--connect-timeout", "20",
    "--max-time", time_limit, "-H", agent, "-H", "Accept: application/vnd.github+json", "-o", output,
    "--stderr", errors, url,
  };
  std::vector< char * > argv;
  for (std::string &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, kCurl, nullptr, nullptr, argv.data(), environ) != 0) {
    error = "curl introuvable";
    return false;
  }
  child_ = pid;
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  child_ = 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    std::error_code ignored;
    fs::remove(errors, ignored);
    return true;
  }
  error = read_file(errors);
  while (!error.empty() && (error.back() == '\n' || error.back() == '\r'))
    error.pop_back();
  if (error.empty())
    error = "Téléchargement interrompu";
  std::error_code ignored;
  fs::remove(errors, ignored);
  return false;
}

void Updater::check() {
  set_state(State::Checking);
  std::error_code fs_error;
  fs::create_directories(cache_dir_, fs_error);
  if (fs_error) {
    set_state(State::Error, "Dossier " + cache_dir_ + " impossible à créer");
    return;
  }
  const std::string latest = cache_dir_ + "/latest.json";
  std::string       error;
  if (!run_curl("https://api.github.com/repos/" QRPROTEC_UPDATE_REPO "/releases/latest", latest, 60, error)) {
    set_state(State::Error, "Vérification impossible : " + error);
    return;
  }
  ReleaseAsset asset;
  const bool   parsed = parse_latest_release(read_file(latest), QRPROTEC_UPDATE_ASSET, asset, error);
  fs::remove(latest, fs_error);
  if (!parsed) {
    set_state(State::Error, error);
    return;
  }
  if (compare_versions(asset.version, current_version()) <= 0) {
    std::lock_guard< std::mutex > lock(mutex_);
    status_.state      = State::UpToDate;
    status_.message.clear();
    status_.available.clear();
    status_.checked_at = now_hhmm();
    return;
  }
  {
    std::lock_guard< std::mutex > lock(mutex_);
    status_.state     = State::Downloading;
    status_.available = asset.version;
    status_.message.clear();
  }
  // anciens telechargements (versions depassees, fichiers partiels)
  const std::string file = cache_dir_ + "/QRProtec-" + asset.version + ".dmg";
  for (const auto &entry : fs::directory_iterator(cache_dir_, fs_error))
    if (entry.path().string() != file && entry.path().extension() != ".sh" && entry.path().extension() != ".log")
      fs::remove(entry.path(), fs_error);
  if (!fs::exists(file, fs_error) || sha256_hex(read_file(file)) != asset.sha256) {
    const std::string part = file + ".part";
    if (!run_curl(asset.url, part, 1800, error)) {
      fs::remove(part, fs_error);
      set_state(State::Error, "Téléchargement de la version " + asset.version + " impossible : " + error);
      return;
    }
    if (sha256_hex(read_file(part)) != asset.sha256) {
      fs::remove(part, fs_error);
      set_state(State::Error, "Version " + asset.version + " téléchargée corrompue (empreinte SHA-256 différente)");
      return;
    }
    fs::rename(part, file, fs_error);
    if (fs_error) {
      set_state(State::Error, "Enregistrement de la mise à jour impossible");
      return;
    }
  }
  std::lock_guard< std::mutex > lock(mutex_);
  ready_             = asset;
  ready_file_        = file;
  status_.state      = State::Ready;
  status_.checked_at = now_hhmm();
}

bool Updater::install(bool restart, std::string &error) {
  std::string file, bundle, needs_admin;
  {
    std::lock_guard< std::mutex > lock(mutex_);
    if (status_.state != State::Ready || status_.launched) {
      error = "Aucune mise à jour prête";
      return false;
    }
    file        = ready_file_;
    bundle      = bundle_;
    needs_admin = status_.needs_admin ? "1" : "0";
  }
  // copie du script hors du .app (remplace pendant qu'il s'execute, et sh le lit au fur et a mesure)
  const std::string source = bundle + "/Contents/Resources/qrprotec-update.sh";
  const std::string script = cache_dir_ + "/qrprotec-update.sh";
  std::error_code   fs_error;
  fs::copy_file(source, script, fs::copy_options::overwrite_existing, fs_error);
  if (fs_error) {
    error = "Script de mise à jour introuvable : " + source;
    return false;
  }
  const std::string pid = std::to_string(::getpid());
  const std::string log = cache_dir_ + "/update.log";
  const pid_t       child = ::fork();
  if (child < 0) {
    error = "Lancement de la mise à jour impossible";
    return false;
  }
  if (child == 0) {
    // detache de l'application (nouvelle session) : survit a sa fermeture
    ::setsid();
    ::execl(
      "/bin/sh", "sh", script.c_str(), "install", pid.c_str(), file.c_str(), bundle.c_str(), restart ? "1" : "0",
      needs_admin.c_str(), log.c_str(), static_cast< char * >(nullptr)
    );
    ::_exit(127);
  }
  std::lock_guard< std::mutex > lock(mutex_);
  status_.launched = true;
  return true;
}

void Updater::on_exit() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    // pas de mot de passe demande apres la fermeture sans que l'utilisateur l'ait voulu
    if (!enabled_ || status_.state != State::Ready || status_.launched || status_.needs_admin)
      return;
  }
  std::string error;
  install(false, error);
}

} // namespace qrprotec
