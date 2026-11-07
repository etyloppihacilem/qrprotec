/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          file_picker.hpp
        -\-    _|__
         |\___/  . \        Created on 7 Nov. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace qrprotec {

// Bouton « Parcourir… » pour choisir un fichier sans connaitre son chemin.
// Boite de dialogue du systeme quand elle existe (macOS : Finder ; Linux : zenity ou kdialog), sinon
// (Flatpak, borne, bureau sans ces outils) navigateur integre : raccourcis Dossier personnel,
// Telechargements, Bureau, Documents, cles USB, puis dossiers et fichiers a la souris.
class FilePicker {
  public:
    FilePicker(std::string title, std::string extension); // extension avec le point (".qrprotec")
    ~FilePicker();

    // Dessine le bouton (et le navigateur integre ouvert). true quand un fichier vient d'etre choisi : `path`
    // est alors rempli. A appeler a chaque image, dans la meme fenetre ou le meme popup.
    bool draw(const char *label, std::string &path);

  private:
    struct NativeState;
    struct Entry {
        std::string name;
        bool        directory = false;
    };

    void open_browser(const std::string &start);
    void change_dir(const std::filesystem::path &dir);
    bool draw_browser(std::string &path);

    std::string                    title_;
    std::string                    extension_;
    std::shared_ptr< NativeState > native_;
    bool                           browser_requested_ = false;
    std::filesystem::path          dir_;
    std::vector< Entry >           entries_;
    std::string                    selected_;
    std::string                    error_;
    bool                           show_all_ = false;
};

} // namespace qrprotec
