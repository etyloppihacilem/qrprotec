/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          settings_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../core/glob_utils.hpp"
#include "../../core/template_io.hpp"
#include "../editor.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>

namespace qrprotec {

namespace {

class SettingsWindow final : public AppWindow {
  public:
    SettingsWindow() : AppWindow("settings", "Réglages", true, true) {}

    void on_open(App &) override { scan_templates(); }

    void draw(App &app) override {
      AppSettings &settings = app.settings;
      if (primary_button("Enregistrer les réglages")) {
        app.apply_settings();
        app.save_settings();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", AppSettings::file_path().string().c_str());

      if (ImGui::CollapsingHeader("Serveur", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText("URL de l'API locale", &settings.api_url);
        help_marker("Port local du back (manage.py serve), ex: http://127.0.0.1:8001");
        ImGui::InputText("Jeton de l'API locale", &settings.api_token, ImGuiInputTextFlags_Password);
        help_marker("Optionnel : doit correspondre à QRPROTEC_LOCAL_API_TOKEN côté serveur.");
        if (ImGui::Button("Tester la connexion")) {
          app.apply_settings();
          app.api.get("/api/health/", [&app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Connexion impossible : " + result.error, true);
              return;
            }
            app.notify("Connecté à l'API " + result.data["api"].str() + ", URLs publiques : "
                       + result.data["public_base_url"].str());
            if (result.data["api"].str() != "local")
              app.notify("Attention : ce port est l'API publique, la gestion ne fonctionnera pas.", true);
          });
        }
      }

      if (ImGui::CollapsingHeader("Session et affichage", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Réinitialisation après inactivité (min)", &settings.inactivity_minutes);
        settings.inactivity_minutes = std::clamp(settings.inactivity_minutes, 0, 24 * 60);
        help_marker("La pile est vidée, l'utilisateur déconnecté et les fenêtres remises en place. 0 = jamais.");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Alerte péremption proche (jours)", &settings.expiring_soon_days);
        settings.expiring_soon_days = std::clamp(settings.expiring_soon_days, 0, 3650);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("Taille du texte", &settings.font_size, 12.0f, 32.0f, "%.0f px");
        ImGui::GetStyle().FontSizeBase = settings.font_size;
        ImGui::Checkbox("Exiger l'étiquette privée pour valider une vérif (hors responsable)",
                        &settings.require_private_label);
      }

      if (ImGui::CollapsingHeader("Disposition par défaut")) {
        ImGui::TextWrapped("Fenêtres ouvertes au démarrage et après une réinitialisation. La pile de scans est "
                           "toujours ouverte.");
        for (auto &window : app.windows) {
          if (window->id == "scan")
            continue;
          ImGui::Checkbox(window->title.c_str(), &settings.layout[window->id].open);
        }
        if (ImGui::Button("Utiliser la disposition actuelle"))
          app.save_current_layout();
        help_marker("Enregistre la position, la taille et l'ouverture de chaque fenêtre telles qu'elles sont "
                    "affichées maintenant.");
        ImGui::SameLine();
        if (ImGui::Button("Disposition d'origine")) {
          settings.layout = AppSettings::defaults().layout;
          app.request_layout_reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Appliquer maintenant"))
          app.request_layout_reset();
      }

      if (ImGui::CollapsingHeader("Étiquettes et impression", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Relire les modèles"))
          scan_templates();
        ImGui::SameLine();
        ImGui::TextDisabled("Modèles *.qr du dossier courant (créés dans l'Éditeur d'étiquettes)");
        for (const CategoryInfo &info : template_categories()) {
          if (info.category == TemplateCategory::Generic)
            continue;
          std::string &path    = settings.label_templates[info.id];
          const std::string preview = path.empty() ? "(aucun)" : path;
          if (ImGui::BeginCombo(info.label.c_str(), preview.c_str())) {
            if (ImGui::Selectable("(aucun)", path.empty()))
              path.clear();
            for (const auto &[file, category] : templates_) {
              std::string label = file;
              if (category != info.category)
                label += "  [usage : " + category_info(category).label + "]";
              if (ImGui::Selectable(label.c_str(), path == file))
                path = file;
            }
            ImGui::EndCombo();
          }
        }
        ImGui::SeparatorText("Étiquettes chargées dans l'imprimante");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Largeur (mm, sens de la tête)", &settings.label_width_mm, 1.0f, 5.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Hauteur (mm, sens du défilement)", &settings.label_height_mm, 1.0f, 5.0f, "%.1f");
        settings.label_width_mm  = std::clamp(settings.label_width_mm, 5.0f, 48.0f);
        settings.label_height_mm = std::clamp(settings.label_height_mm, 5.0f, 500.0f);
        help_marker("La Niimbot B1 imprime au plus 48 mm de large. Un modèle dessiné dans l'autre sens (portrait / "
                    "paysage) est tourné d'un quart de tour à l'impression pour tenir sur l'étiquette.");
        int rotation = settings.rotate_counterclockwise ? 1 : 0;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Sens du quart de tour", &rotation, "Horaire\0Anti-horaire\0"))
          settings.rotate_counterclockwise = rotation == 1;
        ImGui::Checkbox("Retourner les étiquettes (180°)", &settings.flip_labels);
        ImGui::SeparatorText("Titre des étiquettes");
        ImGui::TextUnformatted("Texte de {{titre}} (bouton « Ajouter le titre » de l'éditeur) :");
        ImGui::InputTextMultiline("##titre", &settings.label_title, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f));

        ImGui::SeparatorText("Imprimante Niimbot B1");
        ImGui::InputText("Port série", &settings.print.serial.device);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Débit", &settings.print.serial.baud_rate);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderInt("Densité", &settings.print.density, 1, 5);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderInt("Type d'étiquette", &settings.print.label_type, 1, 3);
        const PrintQueueStatus status = app.printer.status();
        ImGui::Text("File : %zu en attente, %zu imprimée(s)%s", status.pending, status.printed,
                    status.busy ? " - en cours" : "");
        if (!status.current.empty())
          ImGui::Text("En cours : %s", status.current.c_str());
        if (status.paused) {
          ImGui::TextColored(colors::red, "Pause : %s", status.error.c_str());
          if (ImGui::Button("Reprendre"))
            app.printer.resume();
          ImGui::SameLine();
        }
        if (status.pending > 0 && danger_button("Vider la file"))
          app.printer.cancel();
      }

      if (ImGui::CollapsingHeader("Signal de mauvais scan")) {
        ImGui::Checkbox("Bip sur l'ordinateur (mode HID)", &settings.sound_enabled);
        ImGui::InputTextWithHint("Commande de bip", "vide = bip intégré (aplay/paplay)", &settings.beep_command);
        help_marker("Commande shell, %f est remplacé par le fichier wav du bip. Ex: paplay %f");
        ImGui::Checkbox("Clignotement rouge de l'écran", &settings.flash_enabled);
        ImGui::Checkbox("Clignoter aussi quand la douchette est en mode SDK", &settings.flash_in_sdk_mode);
        ImGui::SeparatorText("Douchette (SDK Inateck)");
        ScannerErrorSignal &signal = settings.scanner_signal;
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Couleur LED (code SDK)", &signal.led_color);
        help_marker("Code couleur passe à inateck_scanner_set_led. Ajustez si la LED n'est pas rouge.");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("LED allumée", &signal.led_on);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("LED éteinte", &signal.led_off);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Clignotements LED", &signal.led_count);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Durée du bip", &signal.beep_on);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Silence entre bips", &signal.beep_off);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Nombre de bips", &signal.beep_count);
        if (ImGui::Button("Tester le signal d'erreur"))
          app.feedback.test(settings);
      }
    }

  private:
    void scan_templates() {
      templates_.clear();
      for (const auto &path : glob_current_dir("*.qr")) {
        TemplateDocument document;
        std::string      error;
        if (load_template(document, path.string(), error))
          templates_.emplace_back(path.filename().string(), document.category);
      }
      std::sort(templates_.begin(), templates_.end());
    }

    std::vector< std::pair< std::string, TemplateCategory > > templates_;
};

class EditorWindow final : public AppWindow {
  public:
    EditorWindow() : AppWindow("editor", "Éditeur d'étiquettes", true, true) {}

    ImGuiWindowFlags flags() const override { return ImGuiWindowFlags_MenuBar; }
    void             draw(App &app) override {
      if (!connected_) {
        editor_.set_print_callback([&app](const TemplateDocument &document) {
          app.print_documents({ PrintJob{ "Test " + document.name, document } });
        });
        connected_ = true;
      }
      editor_.set_label_title(app.settings.label_title);
      editor_.set_default_media(app.settings.label_width_mm, app.settings.label_height_mm);
      editor_.draw_contents();
    }

  private:
    Editor editor_;
    bool   connected_ = false;
};

} // namespace

std::unique_ptr< AppWindow > make_settings_window() {
  return std::make_unique< SettingsWindow >();
}

std::unique_ptr< AppWindow > make_editor_window() {
  return std::make_unique< EditorWindow >();
}

} // namespace qrprotec
