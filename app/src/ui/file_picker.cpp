/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          file_picker.cpp
        -\-    _|__
         |\___/  . \        Created on 7 Nov. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "file_picker.hpp"

#include "widgets.hpp"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#include <sys/wait.h>

namespace qrprotec {

namespace fs = std::filesystem;

// Boite du systeme lancee dans un fil a part : l'interface continue de se dessiner pendant le choix
struct FilePicker::NativeState {
    std::mutex  mutex;
    bool        running  = false;
    bool        finished = false;
    bool        failed   = false; // outil absent ou en erreur : navigateur integre
    std::string result;
};

namespace {

std::string home_dir() {
  const char *home = std::getenv("HOME");
  return home && *home ? std::string(home) : std::string("/");
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char character) { return static_cast< char >(std::tolower(character)); });
  return text;
}

bool has_extension(const std::string &name, const std::string &extension) {
  return name.size() > extension.size()
      && lower(name.substr(name.size() - extension.size())) == lower(extension);
}

bool command_exists(const char *name) {
  const char *path = std::getenv("PATH");
  if (!path)
    return false;
  std::string paths(path);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = std::min(paths.find(':', start), paths.size());
    std::error_code   code;
    if (end > start && fs::exists(fs::path(paths.substr(start, end - start)) / name, code))
      return true;
    start = end + 1;
  }
  return false;
}

// Commande de la boite de dialogue du systeme, ou vide (Flatpak : pas d'outil dans le bac a sable ;
// borne : pas de bureau)
std::string native_command(const std::string &title, const std::string &extension) {
  if (std::getenv("FLATPAK_ID") || std::getenv("QRPROTEC_KIOSK"))
    return {};
#ifdef __APPLE__
  (void)extension;
  return "osascript -e 'POSIX path of (choose file with prompt \"" + title + "\")' 2>/dev/null";
#else
  const char *display = std::getenv("DISPLAY");
  const char *wayland = std::getenv("WAYLAND_DISPLAY");
  if ((!display || !*display) && (!wayland || !*wayland))
    return {};
  if (command_exists("zenity"))
    return "zenity --file-selection --title='" + title + "' --file-filter='Fichiers de configuration QRProtec | *" + extension
         + "' --file-filter='Tous les fichiers | *' 2>/dev/null";
  if (command_exists("kdialog"))
    return "kdialog --title '" + title + "' --getopenfilename \"$HOME\" '*" + extension + "' 2>/dev/null";
  return {};
#endif
}

// Dossiers de l'utilisateur dans leur langue (~/.config/user-dirs.dirs : XDG_DOWNLOAD_DIR="$HOME/Téléchargements")
std::string user_dir(const char *key, const char *fallback) {
  std::ifstream input(home_dir() + "/.config/user-dirs.dirs");
  std::string   line;
  const std::string prefix = std::string(key) + "=\"";
  while (std::getline(input, line)) {
    if (line.rfind(prefix, 0) != 0 || line.size() < prefix.size() + 1)
      continue;
    std::string value = line.substr(prefix.size(), line.size() - prefix.size() - 1);
    if (value.rfind("$HOME", 0) == 0)
      value = home_dir() + value.substr(5);
    return value;
  }
  return home_dir() + "/" + fallback;
}

} // namespace

FilePicker::FilePicker(std::string title, std::string extension)
    : title_(std::move(title)), extension_(std::move(extension)), native_(std::make_shared< NativeState >()) {}

FilePicker::~FilePicker() = default; // un fil encore en cours garde son propre etat (shared_ptr)

bool FilePicker::draw(const char *label, std::string &path) {
  bool chosen = false;
  {
    std::lock_guard< std::mutex > lock(native_->mutex);
    if (native_->finished) {
      native_->finished = false;
      if (native_->failed) {
        browser_requested_ = true;
      } else if (!native_->result.empty()) {
        path   = native_->result;
        chosen = true;
      }
    }
  }
  bool running = false;
  {
    std::lock_guard< std::mutex > lock(native_->mutex);
    running = native_->running;
  }
  ImGui::BeginDisabled(running);
  if (button(label)) {
    const std::string command = native_command(title_, extension_);
    if (command.empty()) {
      browser_requested_ = true;
    } else {
      native_->running = true;
      std::thread([state = native_, command]() {
        std::string output;
        bool        failed = true;
        if (FILE *pipe = popen(command.c_str(), "r")) {
          char buffer[512];
          while (std::fgets(buffer, sizeof(buffer), pipe))
            output += buffer;
          const int status = pclose(pipe);
          // zenity / kdialog / osascript : 1 = annule par l'utilisateur, autre code = outil inutilisable
          failed = status == -1 || (WIFEXITED(status) && WEXITSTATUS(status) > 1) || !WIFEXITED(status);
        }
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
          output.pop_back();
        std::lock_guard< std::mutex > lock(state->mutex);
        state->result   = failed ? std::string() : output;
        state->failed   = failed;
        state->running  = false;
        state->finished = true;
      }).detach();
    }
  }
  ImGui::EndDisabled();
  if (running) {
    ImGui::SameLine();
    ImGui::TextDisabled("Choix du fichier en cours…");
  }
  if (browser_requested_) {
    browser_requested_ = false;
    open_browser(path);
  }
  if (draw_browser(path))
    chosen = true;
  return chosen;
}

void FilePicker::open_browser(const std::string &start) {
  std::error_code code;
  fs::path        dir = start.empty() ? fs::path() : fs::path(start).parent_path();
  if (dir.empty() || !fs::is_directory(dir, code)) {
    dir = user_dir("XDG_DOWNLOAD_DIR", "Downloads");
    if (!fs::is_directory(dir, code))
      dir = home_dir();
  }
  selected_.clear();
  change_dir(dir);
  ImGui::OpenPopup(title_.c_str());
}

void FilePicker::change_dir(const fs::path &dir) {
  std::error_code code;
  entries_.clear();
  error_.clear();
  selected_.clear();
  fs::directory_iterator iterator(dir, fs::directory_options::skip_permission_denied, code);
  if (code) {
    error_ = "Dossier illisible : " + dir.string();
    return;
  }
  dir_ = dir;
  for (; iterator != fs::directory_iterator(); iterator.increment(code)) {
    if (code)
      break;
    const std::string name = iterator->path().filename().string();
    if (name.empty() || name[0] == '.')
      continue;
    std::error_code type_code;
    const bool      directory = iterator->is_directory(type_code);
    entries_.push_back({ name, directory });
  }
  std::sort(entries_.begin(), entries_.end(), [](const Entry &left, const Entry &right) {
    if (left.directory != right.directory)
      return left.directory;
    return lower(left.name) < lower(right.name);
  });
}

bool FilePicker::draw_browser(std::string &path) {
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(720.0f, viewport->WorkSize.x * 0.9f),
                                  std::min(520.0f, viewport->WorkSize.y * 0.85f)),
                           ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title_.c_str(), nullptr))
    return false;
  bool chosen = false;

  // raccourcis : la ou un fichier recu ou copie se trouve le plus souvent
  struct Place {
      std::string label;
      std::string path;
  };
  std::vector< Place > places = {
    { "Téléchargements", user_dir("XDG_DOWNLOAD_DIR", "Downloads") },
    { "Bureau", user_dir("XDG_DESKTOP_DIR", "Desktop") },
    { "Documents", user_dir("XDG_DOCUMENTS_DIR", "Documents") },
    { "Dossier personnel", home_dir() },
  };
  const char *user = std::getenv("USER");
  for (const std::string &root : { std::string("/run/media/") + (user ? user : ""), std::string("/media/") + (user ? user : ""),
                                   std::string("/Volumes") }) {
    std::error_code        code;
    fs::directory_iterator iterator(root, code);
    for (; !code && iterator != fs::directory_iterator(); iterator.increment(code))
      if (iterator->is_directory(code))
        places.push_back({ "Clé USB : " + iterator->path().filename().string(), iterator->path().string() });
  }
  for (std::size_t index = 0; index < places.size(); ++index) {
    std::error_code code;
    if (!fs::is_directory(places[index].path, code))
      continue;
    if (index > 0)
      ImGui::SameLine();
    if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(places[index].label.c_str()).x + 20.0f
        > ImGui::GetContentRegionMax().x)
      ImGui::NewLine();
    ImGui::PushID(static_cast< int >(index));
    if (button(places[index].label.c_str()))
      change_dir(places[index].path);
    ImGui::PopID();
  }

  ImGui::Separator();
  ImGui::BeginDisabled(dir_ == dir_.root_path() || dir_.empty());
  if (button("Dossier parent"))
    change_dir(dir_.parent_path());
  ImGui::EndDisabled();
  ImGui::SameLine();
  // chemin lisible : « Dossier personnel/Téléchargements » plutot que /home/nom/Téléchargements
  std::string shown_dir = dir_.string();
  const std::string home = home_dir();
  if (home != "/" && shown_dir.rfind(home, 0) == 0
      && (shown_dir.size() == home.size() || shown_dir[home.size()] == '/'))
    shown_dir = "Dossier personnel" + shown_dir.substr(home.size());
  ImGui::TextWrapped("%s", shown_dir.c_str());
  if (!error_.empty())
    ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1.0f), "%s", error_.c_str());

  const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
  if (ImGui::BeginChild("##files", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
    int  shown = 0;
    fs::path enter;
    for (const Entry &entry : entries_) {
      if (!entry.directory && !show_all_ && !has_extension(entry.name, extension_))
        continue;
      ++shown;
      const std::string text = entry.directory ? "[dossier]  " + entry.name : entry.name;
      if (ImGui::Selectable(text.c_str(), selected_ == entry.name, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (entry.directory) {
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || selected_ == entry.name)
            enter = dir_ / entry.name;
          else
            selected_ = entry.name;
        } else {
          selected_ = entry.name;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            path   = (dir_ / entry.name).string();
            chosen = true;
          }
        }
      }
    }
    if (shown == 0)
      ImGui::TextDisabled(show_all_ ? "Dossier vide." : "Aucun fichier %s ici.", extension_.c_str());
    if (!enter.empty())
      change_dir(enter);
  }
  ImGui::EndChild();

  ImGui::Checkbox("Afficher tous les fichiers", &show_all_);
  const bool is_file = !selected_.empty()
                    && std::any_of(entries_.begin(), entries_.end(),
                                   [this](const Entry &entry) { return entry.name == selected_ && !entry.directory; });
  ImGui::BeginDisabled(!is_file);
  if (primary_button("Ouvrir")) {
    path   = (dir_ / selected_).string();
    chosen = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (button("Annuler") || escape_pressed() || chosen)
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return chosen;
}

} // namespace qrprotec
