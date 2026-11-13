/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          settings_window.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "../../core/paths.hpp"
#include "../../core/plural.hpp"
#include "../../core/template_io.hpp"
#include "../../inateck/scanner_settings.hpp"
#include "../../printer/label_languages.hpp"
#include "../../render/label_types.hpp"
#include "../editor.hpp"
#include "../file_picker.hpp"
#include "../widgets.hpp"
#include "imgui_stdlib.h"
#include "windows.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace qrprotec {

namespace {

// Champ de connexion au back, en lecture seule s'il est impose par l'environnement (kiosk : front-api.conf)
void api_field(const char *label, const char *env_name, std::string &value, ImGuiInputTextFlags flags,
               const char *help) {
  if (const char *forced = AppSettings::api_env(env_name)) {
    std::string shown = (flags & ImGuiInputTextFlags_Password) ? std::string("********") : std::string(forced);
    ImGui::BeginDisabled();
    ImGui::InputText(label, &shown, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    help_marker((std::string(help) + "\n\nImposé par " + env_name
                 + " (kiosk : /etc/qrprotec/front-api.conf, sudo qrprotec-setup).").c_str());
    return;
  }
  ImGui::InputText(label, &value, flags);
  help_marker(help);
}

} // namespace

// Fichier cree sur le serveur : sudo qrprotec-manage frontkey add NOM --config NOM.qrprotec
static void draw_front_config_import(App &app) {
  static std::string password;
  ImGui::SeparatorText("Importer un fichier de configuration");
  if (AppSettings::api_env("QRPROTEC_KIOSK") || AppSettings::api_env("QRPROTEC_API_URL")
      || AppSettings::api_env("QRPROTEC_API_KEY") || AppSettings::api_env("QRPROTEC_API_CA_FILE")) {
    ImGui::TextDisabled("Connexion imposée par l'environnement (borne : sudo qrprotec-setup).");
    return;
  }
  static FilePicker picker("Choisir le fichier de configuration", ".qrprotec");
  static bool       focus_password = false;
  if (picker.draw("Choisir le fichier…", app.front_config_path))
    focus_password = true;
  help_marker("Ouvre une fenêtre pour retrouver le fichier sur cet ordinateur ou sur une clé USB "
              "(Téléchargements, Bureau...).");
  ImGui::InputTextWithHint("Fichier", "chemin du fichier .qrprotec",
                           &app.front_config_path);
  help_marker("Créé sur le serveur avec :\n  sudo qrprotec-manage frontkey add NOM --config NOM.qrprotec\n"
              "Il contient l'URL du serveur, la clé du front et, si besoin, le certificat de l'autorité, "
              "chiffrés (AES) avec un mot de passe. Il remplace les champs ci-dessous.\n"
              "Le fichier peut aussi être glissé sur la fenêtre de QRProtec.");
  if (focus_password) {
    ImGui::SetKeyboardFocusHere();
    focus_password = false;
  }
  const bool submit = ImGui::InputText("Mot de passe du fichier", &password,
                                       ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::BeginDisabled(app.front_config_path.empty() || password.empty());
  if ((button("Importer") || submit) && !app.front_config_path.empty() && !password.empty()) {
    if (app.import_front_config(app.front_config_path, password))
      app.front_config_path.clear();
    password.clear(); // vide apres chaque tentative
  }
  ImGui::EndDisabled();
  ImGui::SeparatorText("Saisie manuelle");
}

void draw_server_settings(App &app, bool save_on_success) {
  draw_front_config_import(app);
  api_field("URL du serveur", "QRPROTEC_API_URL", app.settings.api_url, 0,
            "Même machine : http://127.0.0.1:8001 (API locale, port non exposé).\n"
            "Serveur distant : https://nom-du-serveur (avec une clé de front).");
  api_field("Clé du front (serveur distant)", "QRPROTEC_API_KEY", app.settings.api_key,
            ImGuiInputTextFlags_Password,
            "Créée sur le serveur : sudo qrprotec-manage frontkey add NOM.\n"
            "Inutile pour l'API locale de la même machine.");
  api_field("Certificat de l'autorité (HTTPS)", "QRPROTEC_API_CA_FILE", app.settings.api_ca_file, 0,
            "Fichier PEM, nécessaire si le serveur utilise l'autorité interne de Caddy (QRPROTEC_TLS=internal) :\n"
            "copier /var/lib/caddy/.local/share/caddy/pki/authorities/local/root.crt du serveur.\n"
            "Vide = autorités du système (Let's Encrypt...).");
  api_field("Jeton de l'API locale", "QRPROTEC_API_TOKEN", app.settings.api_token, ImGuiInputTextFlags_Password,
            "Optionnel : doit correspondre à QRPROTEC_LOCAL_API_TOKEN côté serveur.");
  if (button("Tester la connexion")) {
    app.apply_settings();
    app.api.get("/api/health/", [&app, save_on_success](const ApiResult &result) {
      if (!result.ok) {
        app.notify("Connexion impossible : " + result.error, true);
        return;
      }
      const std::string front = result.data["front"].str();
      app.poste_key           = result.data["poste"].boolean();
      app.notify("Connecté à l'API " + (front.empty() ? result.data["api"].str() : "distante (front " + front + ")")
                 + ", URLs publiques : " + result.data["public_base_url"].str());
      if (result.data["api"].str() != "local") {
        app.notify("Attention : ce port est l'API publique, la gestion ne fonctionnera pas.", true);
        return;
      }
      // premier lancement (fenetre « Connexion au serveur ») : la connexion qui marche est gardee tout de suite
      if (save_on_success) {
        app.save_settings();
        app.refresh_item_types();
        app.refresh_lots();
      }
    });
  }
}

namespace {

// macOS : mise a jour automatique de QRProtec.app (ailleurs : version et canal de mise a jour du paquet)
void draw_update_settings(App &app) {
  const Updater::Status update  = app.updater.status();
  const std::string     version = Updater::current_version();
  ImGui::Text("Version installée : %s", version.empty() ? "construction de développement" : version.c_str());
  if (update.state == Updater::State::Unsupported) {
    ImGui::TextWrapped("%s", update.message.c_str());
    return;
  }
  ImGui::Checkbox("Mises à jour automatiques", &app.settings.auto_update);
  help_marker("Toutes les 6 heures, QRProtec regarde s'il existe une nouvelle version sur GitHub et la télécharge "
              "en arrière-plan. Elle s'installe à la fermeture de l'application, ou tout de suite depuis la barre "
              "de menu (« ↑ Mise à jour »).\nPrend effet après « Enregistrer les réglages ».");
  using State = Updater::State;
  switch (update.state) {
    case State::Checking: ImGui::TextDisabled("Recherche d'une nouvelle version..."); break;
    case State::Downloading: ImGui::TextDisabled("Téléchargement de la version %s...", update.available.c_str()); break;
    case State::UpToDate: ImGui::TextColored(colors::green, "À jour (vérifié à %s).", update.checked_at.c_str()); break;
    case State::Ready:
      ImGui::TextColored(colors::blue, "Version %s téléchargée : installée à la fermeture.", update.available.c_str());
      break;
    case State::Error: ImGui::TextColored(colors::red, "%s", update.message.c_str()); break;
    default: break;
  }
  if (update.needs_admin)
    ImGui::TextDisabled("Dossier de l'application protégé : le mot de passe d'un administrateur du Mac sera demandé.");
  const bool busy = update.state == State::Checking || update.state == State::Downloading;
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Rechercher maintenant"))
    app.updater.check_now();
  ImGui::EndDisabled();
  if (update.state == State::Ready && !update.launched) {
    ImGui::SameLine();
    if (ImGui::Button("Installer et redémarrer")) {
      std::string error;
      if (app.updater.install(true, error))
        app.quit_requested = true;
      else
        app.notify("Mise à jour impossible : " + error, true);
    }
  }
}

class SettingsWindow final : public AppWindow {
  public:
    SettingsWindow() : AppWindow("settings", "Réglages", true, true) {
      admin_only     = true;
      reload_on_wake = false; // reglages en cours de saisie
    }

    void on_open(App &app) override {
      scan_templates();
      load_notifications(app);
      load_server_rules(app);
    }

    void draw(App &app) override {
      AppSettings &settings = app.settings;
      if (primary_button("Enregistrer les réglages")) {
        app.apply_settings();
        app.save_settings();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", AppSettings::file_path().string().c_str());

      if (ImGui::CollapsingHeader("Serveur")) {
        draw_server_settings(app);
      }

      if (ImGui::CollapsingHeader("Règles du serveur (tous les fronts)", ImGuiTreeNodeFlags_DefaultOpen))
        draw_server_rules(app);

      if (ImGui::CollapsingHeader("Session et affichage", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Machine partagée", &settings.shared_machine);
        help_marker("Cochée (poste commun, kiosk) : réinitialisation après inactivité et nouvelle connexion à "
                    "chaque démarrage.\nDécochée (poste personnel) : pas de réinitialisation, et l'utilisateur "
                    "reste connecté jusqu'à « Se déconnecter », même après un redémarrage de l'application ; seul "
                    "son code PIN est redemandé à l'ouverture s'il en a un.");
        ImGui::BeginDisabled(!settings.shared_machine);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Réinitialisation après inactivité (min)", &settings.inactivity_minutes);
        settings.inactivity_minutes = std::clamp(settings.inactivity_minutes, 0, 24 * 60);
        ImGui::EndDisabled();
        help_marker("La pile est vidée, l'utilisateur déconnecté et les fenêtres remises en place. 0 = jamais. "
                    "Seulement sur une machine partagée.");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Alerte péremption proche (jours)", &settings.expiring_soon_days);
        settings.expiring_soon_days = std::clamp(settings.expiring_soon_days, 0, 3650);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Délai de livraison d'une commande (jours)", &settings.order_lead_days);
        settings.order_lead_days = std::clamp(settings.order_lead_days, 0, 365);
        help_marker("Prévisions de stock : une commande est suggérée assez tôt pour arriver avant que le stock passe "
                    "sous son minimum.");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Historique de consommation (mois)", &settings.forecast_history_months);
        settings.forecast_history_months = std::clamp(settings.forecast_history_months, 1, 36);
        help_marker("Prévisions de stock : la consommation de chaque lot est la moyenne des items utilisés sur cette "
                    "période (items absents à une vérif, sauf s'ils ont été retrouvés depuis).");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("Taille du texte", &settings.font_size, 12.0f, 32.0f, "%.0f px");
        ImGui::GetStyle().FontSizeBase = settings.font_size;
      }

      if (ImGui::CollapsingHeader("Mises à jour"))
        draw_update_settings(app);

      if (ImGui::CollapsingHeader("Débug")) {
        ImGui::Checkbox("Activer les options de débug", &settings.debug_options);
        help_marker("Pour les administrateurs seulement (le serveur le vérifie) : autorise à construire des lots "
                    "inacceptables en vrai, pour tester. Items périmés, signalés inutilisables, désactivés, "
                    "supprimés ou remplacés ajoutés à un lot, retirés d'un autre lot... sont acceptés au lieu "
                    "d'être refusés.\nÀ laisser décoché en service.");
        if (settings.debug_options)
          ImGui::TextColored(colors::orange, "Options de débug actives sur ce poste.");
      }

      if (ImGui::CollapsingHeader("Disposition par défaut")) {
        ImGui::TextWrapped("Fenêtres ouvertes au démarrage et après une réinitialisation. La pile de scans est "
                           "toujours ouverte.");
        for (auto &window : app.windows) {
          if (window->id == "scan")
            continue;
          ImGui::Checkbox(window->title.c_str(), &settings.layout[window->id].open);
        }
        if (button("Utiliser la disposition actuelle"))
          app.save_current_layout();
        help_marker("Enregistre la position, la taille et l'ouverture de chaque fenêtre telles qu'elles sont "
                    "affichées maintenant.");
        ImGui::SameLine();
        if (button("Disposition d'origine")) {
          settings.layout = AppSettings::defaults().layout;
          app.request_layout_reset();
        }
        ImGui::SameLine();
        if (button("Appliquer maintenant"))
          app.request_layout_reset();
      }

      if (ImGui::CollapsingHeader("Notifications"))
        draw_notifications(app);

      if (ImGui::CollapsingHeader("Étiquettes et impression")) {
        ImGui::TextUnformatted("Dossier des modèles (fichiers .qr et images, à commiter avec le dépôt) :");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##templates_dir", default_templates_dir().c_str(), &settings.templates_dir,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
          set_templates_dir(settings.templates_dir);
          scan_templates();
        }
        ImGui::TextDisabled("Utilisé : %s (vide = app/templates du dépôt)", templates_dir().string().c_str());
        if (button("Relire les modèles"))
          scan_templates();
        ImGui::SameLine();
        ImGui::TextDisabled("Modèles *.qr du dossier ci-dessus (créés dans l'Éditeur d'étiquettes)");
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
        {
          const std::string loaded  = settings.print.loaded_label_type;
          const std::string preview = loaded.empty() ? std::string("(format libre, réglé ci-dessous)") : loaded;
          ImGui::SetNextItemWidth(320.0f);
          if (ImGui::BeginCombo("Type chargé", preview.c_str())) {
            if (ImGui::Selectable("(format libre, réglé ci-dessous)", loaded.empty()))
              app.set_loaded_label_type("");
            for (const LabelType &type : label_types_) {
              const std::string label = type.name + "  (" + type.summary() + ")";
              if (ImGui::Selectable(label.c_str(), type.name == loaded))
                app.set_loaded_label_type(type.name);
            }
            ImGui::EndCombo();
          }
          help_marker("Types d'étiquettes des modèles (Éditeur d'étiquettes > Types d'étiquettes). Un modèle d'un "
                      "autre type met la file en pause jusqu'à ce que son rouleau soit chargé. Un modèle sans "
                      "type reçoit à sa lecture un type construit d'après l'étiquette réglée ci-dessous.");
          const bool known = loaded.empty() || std::any_of(label_types_.begin(), label_types_.end(),
                                                           [&](const LabelType &type) { return type.name == loaded; });
          if (!known)
            ImGui::TextColored(colors::orange, "Aucun modèle n'utilise le type « %s ».", loaded.c_str());
        }
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Largeur (mm, sens de la tête)", &settings.label_width_mm, 1.0f, 5.0f, "%.1f");
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputFloat("Hauteur (mm, sens du défilement)", &settings.label_height_mm, 1.0f, 5.0f, "%.1f");
        settings.label_width_mm =
            std::clamp(settings.label_width_mm, 5.0f, static_cast< float >(printer_max_width_mm(settings.print.kind)));
        settings.label_height_mm = std::clamp(settings.label_height_mm, 5.0f, 500.0f);
        help_marker("La Niimbot B1 imprime au plus 48 mm de large, une imprimante 4 pouces (TSPL / ZPL) 104 mm. Un "
                    "modèle dessiné dans l'autre sens (portrait / paysage) est tourné d'un quart de tour à "
                    "l'impression pour tenir sur l'étiquette.");
        int rotation = settings.rotate_counterclockwise ? 1 : 0;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Sens du quart de tour", &rotation, "Horaire\0Anti-horaire\0"))
          settings.rotate_counterclockwise = rotation == 1;
        ImGui::Checkbox("Retourner les étiquettes (180°)", &settings.flip_labels);
        ImGui::SeparatorText("Titre des étiquettes");
        ImGui::TextUnformatted("Nom de l'antenne, imprimé par {{titre}} (bouton « Nom de l'antenne » de l'éditeur) :");
        ImGui::InputTextMultiline("##titre", &settings.label_title, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f));

        ImGui::SeparatorText("Imprimante");
        if (app.main_poste() && !settings.api_endpoint().key.empty()) {
          ImGui::TextWrapped("Poste principal : les étiquettes s'impriment ici, avec celles demandées depuis le web "
                             "ou d'autres ordinateurs (« Impressions en attente »).");
        } else if (!settings.api_endpoint().key.empty()) {
          ImGui::Checkbox("Imprimante branchée sur cet ordinateur", &settings.print_here);
          help_marker("Décochée (par défaut) : cet ordinateur se connecte au serveur avec une clé de front, sans "
                      "imprimante. « Imprimer » envoie les étiquettes au poste, qui les propose dans sa fenêtre « "
                      "Impressions en attente » (même sans personne de connecté). Cochée : impression ici. La borne "
                      "avec l'imprimante se déclare plutôt « poste principal » sur le serveur (Utilisateurs > "
                      "Ordinateurs connectés).");
        }
        int kind = static_cast< int >(settings.print.kind);
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::Combo("Modèle", &kind,
                         "Niimbot B1 (secours, debug)\0TSPL (TSC, IDPRT, Munbyn, Phomemo...)\0ZPL (Zebra)\0"))
          settings.print.kind = static_cast< PrinterKind >(kind);
        help_marker("TSPL et ZPL : imprimantes d'étiquettes de bureau. Choisir une tête 203 dpi (8 points/mm), la "
                    "résolution des modèles d'étiquettes ; en 300 dpi l'étiquette sortirait plus petite. La Niimbot "
                    "B1 reste utilisable en secours.");
        if (settings.print.kind == PrinterKind::NiimbotB1) {
          ImGui::InputText("Port série", &settings.print.serial.device);
          ImGui::SetNextItemWidth(150.0f);
          ImGui::InputInt("Débit", &settings.print.serial.baud_rate);
          ImGui::SetNextItemWidth(150.0f);
          ImGui::SliderInt("Densité", &settings.print.density, 1, 5);
          ImGui::SetNextItemWidth(150.0f);
          ImGui::SliderInt("Type d'étiquette", &settings.print.label_type, 1, 3);
        } else {
          RawPrinterSettings &raw = settings.print.raw;
          ImGui::InputTextWithHint("Adresse", "/dev/usb/lp0 ou 192.168.1.50", &raw.address);
          help_marker("USB : /dev/usb/lp0 (l'utilisateur du poste doit être dans le groupe lp). Réseau : adresse IP "
                      "de l'imprimante, port 9100 par défaut (192.168.1.50:9100) ; elle peut alors servir à "
                      "plusieurs ordinateurs.");
          ImGui::Checkbox("Transfert thermique (ruban)", &raw.ribbon);
          help_marker("Décoché : thermique direct, sans ruban (étiquettes thermiques). Coché : ruban encreur, pour "
                      "des étiquettes papier vélin ou synthétiques qui durent.");
          ImGui::SetNextItemWidth(150.0f);
          ImGui::SliderInt("Noirceur", &raw.darkness, 0, 15);
          float gap = static_cast< float >(raw.gap_mm);
          ImGui::SetNextItemWidth(150.0f);
          if (ImGui::InputFloat("Espace entre étiquettes (mm)", &gap, 0.5f, 1.0f, "%.1f"))
            raw.gap_mm = std::clamp(gap, 0.0f, 20.0f);
          ImGui::SeparatorText("Position de l'impression");
          float offset_x = static_cast< float >(raw.offset_x_mm);
          ImGui::SetNextItemWidth(150.0f);
          if (ImGui::InputFloat("Décalage horizontal (mm)", &offset_x, 0.5f, 1.0f, "%.1f"))
            raw.offset_x_mm = std::clamp(offset_x, -15.0f, 15.0f);
          help_marker("Déplace toute l'impression sur l'étiquette. Positif : vers la droite ; négatif : vers la "
                      "gauche. ZPL : ^LS (« Left Position » de l'imprimante) ; TSPL : SHIFT x.");
          float offset_y = static_cast< float >(raw.offset_y_mm);
          ImGui::SetNextItemWidth(150.0f);
          if (ImGui::InputFloat("Décalage vertical (mm)", &offset_y, 0.5f, 1.0f, "%.1f"))
            raw.offset_y_mm = std::clamp(offset_y, -15.0f, 15.0f);
          help_marker("Déplace toute l'impression sur l'étiquette. Positif : vers le bas (loin du haut de "
                      "l'étiquette) ; négatif : vers le haut. ZPL : ^LT (« Label Top »), 120 points au plus soit "
                      "15 mm ; TSPL : SHIFT y. Une impression qui déborde de plusieurs millimètres sur l'étiquette "
                      "suivante ne se règle pas ici : calibrer.");
          float tear_off = static_cast< float >(raw.tear_off_mm);
          ImGui::SetNextItemWidth(150.0f);
          if (ImGui::InputFloat("Position de découpe (mm)", &tear_off, 0.5f, 1.0f, "%.1f"))
            raw.tear_off_mm = std::clamp(tear_off, -15.0f, 15.0f);
          help_marker("Où l'étiquette s'arrête après impression, pour la détacher sur la barre de découpe. Positif : "
                      "elle sort plus loin ; négatif : elle s'arrête plus tôt. Ne déplace pas l'impression sur "
                      "l'étiquette. ZPL : ~TA (« Tear Off ») ; TSPL : OFFSET.");
          ImGui::TextDisabled("Imprimer l'étiquette d'étalonnage (ci-dessous) pour lire les valeurs à régler.");
          draw_printer_maintenance(app, settings);
        }
        const PrintQueueStatus status = app.printer.status();
        ImGui::Text("File : %zu en attente, %s%s", status.pending, nb(status.printed, "imprimée").c_str(),
                    status.busy ? " - en cours" : "");
        if (!status.current.empty())
          ImGui::Text("En cours : %s", status.current.c_str());
        if (status.paused) {
          ImGui::TextColored(colors::red, "Pause : %s", status.error.c_str());
          if (!status.needed_label_type.empty()) {
            if (button(("Les étiquettes « " + status.needed_label_type + " » sont chargées").c_str()))
              app.set_loaded_label_type(status.needed_label_type);
          } else if (button("Reprendre"))
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
        help_marker("Code couleur passe à inateck_scanner_set_led : 2 vert, 3 bleu, 4 orange (pas de rouge).");
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
        if (button("Tester le signal d'erreur"))
          app.feedback.test(settings);
      }
    }

  private:
    void scan_templates() {
      templates_.clear();
      std::vector< TemplateDocument > documents;
      for (const auto &path : glob_templates("*.qr")) {
        TemplateDocument document;
        std::string      error;
        if (load_template(document, path.string(), error)) {
          templates_.emplace_back(path.filename().string(), document.category);
          documents.push_back(std::move(document));
        }
      }
      std::sort(templates_.begin(), templates_.end());
      label_types_ = collect_label_types(documents);
    }

    // -----------------------------------------------------------------------------------------------------------
    // Maintenance d'une imprimante TSPL / ZPL : calibrage sur l'etiquette chargee, page de configuration, avance.
    // Les commandes passent par la file d'impression, apres les etiquettes en attente.
    void draw_printer_maintenance(App &app, AppSettings &settings) {
      ImGui::SeparatorText("Maintenance de l'imprimante");
      const PhysicalLabel label  = settings.physical_label();
      double              width  = label.width_mm;
      double              height = label.height_mm;
      bool                ribbon = settings.print.raw.ribbon;
      LabelType           loaded;
      bool                typed = false;
      for (const LabelType &type : label_types_)
        if (!settings.print.loaded_label_type.empty() && type.name == settings.print.loaded_label_type) {
          width  = type.width_mm;
          height = type.height_mm;
          ribbon = type.support == LabelSupport::Transfer;
          loaded = type;
          typed  = true;
        }
      if (!typed) { // format libre : etiquette des Reglages, droite
        loaded.width_mm  = width;
        loaded.height_mm = height;
        loaded.rotation  = 0;
        loaded.support   = ribbon ? LabelSupport::Transfer : LabelSupport::Direct;
      }
      const labels::RawJob job = printer_media_job(settings.print, width, height, ribbon, label.pixels_per_mm);
      const bool zpl    = settings.print.kind == PrinterKind::Zpl;
      const auto send   = [&](labels::Maintenance action, const char *description) {
        app.printer.send_command(description, labels::maintenance_command(zpl, action, job), settings.print);
        printer_message_ = std::string(description) + " : envoyé à l'imprimante.";
      };
      ImGui::TextDisabled("Étiquette chargée : %.0f x %.0f mm, %s", width, height,
                          ribbon ? "transfert (ruban)" : "thermique direct");

      if (primary_button("Changer de rouleau..."))
        app.open_roll_wizard();
      help_marker("Assistant pas à pas : retirer l'ancien rouleau, mettre le nouveau, le ruban, puis à la fin il envoie "
                  "les commandes (calibrage si le format change, lecture de l'état) et note le rouleau en place.");

      const PrintQueueStatus status = app.printer.status();
      if (button("Lire l'état"))
        app.printer.request_printer_state(settings.print);
      help_marker("Demande à l'imprimante si elle est prête : tête ouverte, plus d'étiquettes, plus de ruban, pause. "
                  "Le même contrôle est fait avant chaque étiquette : la file s'arrête avec la raison. Une imprimante "
                  "qui ne répond pas (liaison USB à sens unique) imprime sans contrôle.");
      if (!status.printer_state.empty()) {
        ImGui::SameLine();
        if (status.printer_state_blocking)
          ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.2f, 1.0f), "%s", status.printer_state.c_str());
        else if (status.printer_state_known)
          ImGui::TextColored(ImVec4(0.2f, 0.65f, 0.3f, 1.0f), "%s", status.printer_state.c_str());
        else
          ImGui::TextDisabled("%s", status.printer_state.c_str());
      }

      if (button("Calibrer sur les étiquettes chargées"))
        send(labels::Maintenance::Calibrate, "Calibrage");
      help_marker("À chaque nouveau format de rouleau, après un retour aux réglages d'usine, ou si des étiquettes "
                  "blanches sortent entre deux impressions. L'imprimante reçoit le format (support, largeur, "
                  "longueur, positions), mesure la longueur d'étiquette et règle ses capteurs en avançant quelques "
                  "étiquettes, puis enregistre le tout (gardé après extinction). ZPL : ^MT ^MNY ^PW ^LL ^LS ^LT ~TA, "
                  "~JC, ^JUS ; TSPL : SIZE, GAP, SET RIBBON, GAPDETECT.");
      ImGui::SameLine();
      if (button("Avancer d'une étiquette"))
        send(labels::Maintenance::Feed, "Avance");
      ImGui::SameLine();
      if (button("Page de configuration"))
        send(labels::Maintenance::Config, "Page de configuration");
      help_marker("Imprime les réglages de l'imprimante (largeur d'impression, longueur, capteur, décalages, réseau) : "
                  "utile pour vérifier un calibrage ou retrouver son adresse IP. ZPL : ~WC ; TSPL : SELFTEST.");
      if (button("Étiquette d'étalonnage")) {
        PrintJob calibration;
        calibration.description = "Étiquette d'étalonnage";
        calibration.document    = calibration_document(loaded, typed);
        calibration.image       = calibration_image(calibration.document);
        app.print_documents({ calibration });
        printer_message_ = "Étiquette d'étalonnage : envoyée à l'imprimante.";
      }
      help_marker("Cadre au ras du bord, cadre pointillé à 1 mm et croix graduée, à la taille de l'étiquette "
                  "chargée. Au bout des axes, chiffres en mm depuis chaque bord : si un bord est coupé, le chiffre "
                  "au ras de ce bord, avec son signe, s'ajoute au Décalage horizontal / vertical.");
      if (zpl) {
        ImGui::SameLine();
        if (button("Diagnostic des décalages"))
          send(labels::Maintenance::Diagnostic, "Diagnostic des décalages");
        help_marker("Quatre étiquettes dessinées par l'imprimante elle-même (cadre au bord et texte, sans image) : "
                    "1 sans décalage, 2 origine déplacée par ^LH, 3 et 4 décalage par ^LS et ^LT, de 5 mm vers la "
                    "droite et vers le bas. Une étiquette identique à la 1 montre que l'imprimante ignore cette "
                    "commande ; la 4 montre si elle ne s'applique qu'à l'étiquette suivante.");
      }
      ImGui::SameLine();
      if (!confirm_factory_) {
        if (button("Réglages d'usine"))
          confirm_factory_ = true;
      } else {
        if (button("Confirmer : tout remettre à zéro")) {
          send(labels::Maintenance::Factory, "Réglages d'usine");
          confirm_factory_ = false;
        }
        ImGui::SameLine();
        if (button("Annuler"))
          confirm_factory_ = false;
      }
      help_marker("Efface tous les réglages enregistrés dans l'imprimante (largeur, longueur, capteur, décalages, "
                  "noirceur...) et remet ceux de l'usine ; le réseau n'est pas touché. À faire si l'imprimante a été "
                  "réglée pour autre chose. Ensuite, toujours Calibrer. ZPL : ^JUF puis ^JUS ; TSPL : "
                  "INITIALPRINTER.");
      if (!printer_message_.empty())
        ImGui::TextUnformatted(printer_message_.c_str());
    }

    // -----------------------------------------------------------------------------------------------------------
    // Regles du serveur : enregistrees sur le serveur, appliquees immediatement a tous les fronts (web compris).
    void load_server_rules(App &app) {
      app.api.get("/api/server-settings/", [this, &app](const ApiResult &result) {
        if (!result.ok)
          return;
        server_rules_          = result.data;
        app.declared_identity = server_rules_["declared_identity"].boolean();
      });
    }

    void draw_server_rules(App &app) {
      if (server_rules_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        if (button("Recharger##server_rules"))
          load_server_rules(app);
        return;
      }
      bool declared = server_rules_["declared_identity"].boolean();
      if (ImGui::Checkbox("Autoriser les vérifs et les ajouts sans badge (nom déclaré)", &declared)) {
        Json body;
        body["declared_identity"] = declared;
        app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify("Règles du serveur : " + result.error, true);
            return;
          }
          server_rules_          = result.data;
          app.declared_identity = server_rules_["declared_identity"].boolean();
          app.notify(app.declared_identity ? "Nom déclaré sans badge autorisé sur tous les fronts."
                                           : "Badge obligatoire sur tous les fronts.");
        });
      }
      help_marker("Désactivé (par défaut) : une vérif, un ajout d'items ou l'ouverture d'un scellé exige un "
                  "badge.\nActivé : à la place du badge, la personne peut saisir son nom, enregistré sans "
                  "vérification (« D:nom »). L'étiquette clé du lot reste exigée.");
      // Code PIN obligatoire par role (toujours pour les admins) : une case par role. Obligatoire ne veut pas dire
      // demande a chaque connexion : c'est la case « Demander a chaque connexion » de chacun, cochee par defaut.
      // « Code a la connexion obligatoire » : obligatoire ET demande apres le badge a chaque connexion (case imposee).
      ImGui::TextUnformatted("Code PIN obligatoire pour :");
      ImGui::SameLine();
      help_marker("Les administrateurs ont toujours un code PIN, demandé à chaque connexion.\nRôle coché : chacun doit "
                  "avoir un code, sauf ceux dont le PIN est désactivé (fenêtre Utilisateurs) ; ceux qui n'en ont pas le "
                  "choisissent à leur prochaine connexion. La case « Demander à chaque connexion » y est cochée par "
                  "défaut ; décochée, le code ne sert qu'à la connexion sans badge (nom, prénom et code).\nRôle "
                  "décoché : le code est facultatif.");
      draw_pin_roles(app, "pin_required_roles", "pin_login_roles");
      ImGui::TextUnformatted("Code PIN à la connexion obligatoire pour :");
      ImGui::SameLine();
      help_marker("Rôle coché : le code est obligatoire et demandé après le badge à chaque connexion ; la case "
                  "« Demander à chaque connexion » ne peut plus être décochée. Ceux qui n'ont pas de code le choisissent "
                  "à leur prochaine connexion, sauf ceux dont le PIN est désactivé.\nToujours le cas des "
                  "administrateurs.");
      draw_pin_roles(app, "pin_login_roles", "");
      bool web_barcodes = server_rules_["web_barcodes"].boolean();
      if (ImGui::Checkbox("Lire les codes-barres sur le web (téléphones)", &web_barcodes)) {
        Json body;
        body["web_barcodes"] = web_barcodes;
        app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify("Règles du serveur : " + result.error, true);
            return;
          }
          server_rules_    = result.data;
          app.web_barcodes = server_rules_["web_barcodes"].boolean();
          app.notify(app.web_barcodes ? "Le front web lit les codes-barres des étiquettes d'items."
                                      : "Le front web ne charge plus le lecteur de codes-barres.");
        });
      }
      help_marker("Activé d'office à la première impression d'une étiquette à code-barres. Désactivé : le front "
                  "web et le téléphone-douchette ne chargent pas le lecteur de codes-barres (page plus légère) et "
                  "ne lisent que les QR codes. À désactiver quand plus aucune étiquette à code-barres ne circule ; "
                  "la douchette du poste lit les codes-barres dans tous les cas.");
      bool full_verif = server_rules_["departure_full_verif"].boolean();
      if (ImGui::Checkbox("Vérif complète à chaque départ", &full_verif)) {
        Json body;
        body["departure_full_verif"] = full_verif;
        app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify("Règles du serveur : " + result.error, true);
            return;
          }
          server_rules_ = result.data;
          app.notify(server_rules_["departure_full_verif"].boolean()
                         ? "Une vérif complète de chaque lot non scellé est demandée avant de partir."
                         : "Départ sans vérif : l'étiquette de chaque lot suffit.");
        });
      }
      help_marker("Listes de départ (« Partir avec un lot ») : décoché (par défaut), on fait confiance aux secouristes, "
                  "l'étiquette de chaque lot suffit et on touche « Vérif recommandée » au retour sur ce qui a servi. "
                  "Coché, chaque lot non scellé pris doit être vérifié (complet) avant de valider le départ. Jamais "
                  "pour un lot scellé.");
      bool restock_recommended = server_rules_["restock_verif_recommended"].boolean();
      if (ImGui::Checkbox("Réassort : vérif recommandée pour les suivants", &restock_recommended)) {
        Json body;
        body["restock_verif_recommended"] = restock_recommended;
        app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
          if (!result.ok) {
            app.notify("Règles du serveur : " + result.error, true);
            return;
          }
          server_rules_                 = result.data;
          app.restock_verif_recommended = server_rules_["restock_verif_recommended"].boolean();
          app.notify(app.restock_verif_recommended
                         ? "Après un réassort, le lot passe en vérif recommandée pour les suivants."
                         : "Crédit de confiance : un réassort garde le lot au vert jusqu'à la limite.");
        });
      }
      help_marker("Coché (par défaut) : tout réassort (« Ajouter au lot », vérif partielle), sur le web comme au "
                  "poste, passe le lot en « Vérif recommandée » (orange) jusqu'à la prochaine vérif complète, "
                  "pour que les suivants sachent qu'il faut tout recompter. Décoché : crédit de confiance, le "
                  "réassort remet le lot au vert (même rentré d'une liste ou marqué utilisé) tant qu'il n'a pas eu "
                  "plus de sorties ni plus de jours que les limites ci-dessous depuis sa dernière vérif complète ; "
                  "au-delà, vérif recommandée. Une sortie = liste de départ validée ou lot marqué utilisé. Jamais "
                  "pour un rangement du stock.");
      if (!restock_recommended) {
        const auto credit_input = [this, &app](const char *label, const char *field, int top) {
          // valeur en cours de saisie gardee d'une image a l'autre, envoyee a la fin de la saisie
          static std::map<std::string, int> editing;
          static std::string                active;
          int                              &value = editing[field];
          if (active != field)
            value = server_rules_[field].integer(0);
          ImGui::SetNextItemWidth(120.0f);
          ImGui::InputInt(label, &value, 0, 0);
          if (ImGui::IsItemActive())
            active = field;
          else if (active == field)
            active.clear();
          if (ImGui::IsItemDeactivatedAfterEdit()) {
            Json body;
            body[field] = std::clamp(value, 0, top);
            app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
              if (!result.ok) {
                app.notify("Règles du serveur : " + result.error, true);
                return;
              }
              server_rules_ = result.data;
              app.notify("Crédit de confiance : " + nb(server_rules_["restock_credit_outings"].integer(), "sortie")
                         + ", " + nb(server_rules_["restock_credit_days"].integer(), "jour")
                         + " depuis la dernière vérif complète.");
            });
          }
        };
        ImGui::Indent();
        credit_input("Sorties sans vérif complète", "restock_credit_outings", 50);
        credit_input("Jours depuis la dernière vérif complète", "restock_credit_days", 365);
        ImGui::Unindent();
      }
      {
        // valeur en cours de saisie gardee d'une image a l'autre, envoyee a la fin de la saisie
        static int  storage_outings = 10;
        static bool storage_editing = false;
        if (!storage_editing)
          storage_outings = server_rules_["storage_verif_outings"].integer(10);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Sorties avant de proposer la vérif d'un rangement", &storage_outings, 0, 0);
        storage_editing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          Json body;
          body["storage_verif_outings"] = std::clamp(storage_outings, 1, 100);
          app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Règles du serveur : " + result.error, true);
              return;
            }
            server_rules_ = result.data;
            app.notify("Vérif d'un rangement du stock proposée au TECH toutes les "
                       + nb(server_rules_["storage_verif_outings"].integer(10), "sortie") + ".");
          });
        }
        help_marker("Au retour d'une liste, chaque item revient dans le rangement du stock d'où il est sorti (on fait "
                    "confiance aux secouristes pour le remettre à sa place). La Gestion de la TECH propose une vérif "
                    "d'un rangement (ou du stock sans rangement) au bout de ce nombre de sorties depuis sa dernière "
                    "vérif, de 1 (à chaque sortie) à 100. Une sortie = les items sortis ensemble (liste ou sortie à "
                    "la main).");
      }
      const int volume = server_rules_["scanner_volume_default"].integer(4);
      const ScannerSettingInfo volume_info = scanner_setting_info(1001, "volume");
      std::string preview = "Valeur " + std::to_string(volume);
      for (const ScannerSettingOption &option : volume_info.options)
        if (option.value == volume)
          preview = option.label;
      if (ImGui::BeginCombo("Volume de la douchette par défaut", preview.c_str())) {
        for (const ScannerSettingOption &option : volume_info.options) {
          if (!ImGui::Selectable(option.label, option.value == volume) || option.value == volume)
            continue;
          Json body;
          body["scanner_volume_default"] = option.value;
          app.api.patch("/api/server-settings/", body, [this, &app](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Règles du serveur : " + result.error, true);
              return;
            }
            server_rules_              = result.data;
            app.scanner_volume_default = server_rules_["scanner_volume_default"].integer(4);
            app.notify("Volume de la douchette par défaut enregistré.");
          });
        }
        ImGui::EndCombo();
      }
      help_marker("Volume de la douchette pour les utilisateurs qui n'ont pas choisi le leur, et quand personne "
                  "n'est connecté. Chaque utilisateur retrouve le dernier volume qu'il a choisi.");
    }

    // -----------------------------------------------------------------------------------------------------------
    // Notifications : reglages enregistres sur le serveur, appliques immediatement. Les alertes partent en
    // notifications web (front web) ; l'API SMS de Free Mobile n'est plus maintenue (voir notifications.py).
    // Une case par role pour le reglage `field` (liste de roles), plus la case Administrateur toujours cochee. Un
    // role coche dans `implied_by` (code a la connexion obligatoire) l'est aussi ici, case grisee.
    void draw_pin_roles(App &app, const std::string &field, const std::string &implied_by) {
      const Json &roles   = server_rules_[field];
      const Json &choices = server_rules_["pin_role_choices"];
      auto        has     = [](const Json &list, const std::string &role) {
        for (std::size_t j = 0; j < list.size(); ++j)
          if (list[j].str() == role)
            return true;
        return false;
      };
      for (std::size_t i = 0; i < choices.size(); ++i) {
        const std::string role    = choices[i]["value"].str();
        const bool        implied = !implied_by.empty() && has(server_rules_[implied_by], role);
        bool              on      = implied || has(roles, role);
        if (i > 0)
          ImGui::SameLine();
        ImGui::BeginDisabled(implied);
        if (ImGui::Checkbox((choices[i]["label"].str() + "##" + field + "_" + role).c_str(), &on)) {
          Json list = Json::array();
          for (std::size_t j = 0; j < choices.size(); ++j) {
            const std::string other = choices[j]["value"].str();
            if (other == role ? on : has(roles, other))
              list.push_back(Json(other));
          }
          Json body;
          body[field] = list;
          const bool at_login = field == "pin_login_roles";
          app.api.patch("/api/server-settings/", body, [this, &app, on, at_login](const ApiResult &result) {
            if (!result.ok) {
              app.notify("Règles du serveur : " + result.error, true);
              return;
            }
            server_rules_ = result.data;
            app.notify(at_login ? (on ? "Code PIN obligatoire et demandé à chaque connexion pour ce rôle."
                                      : "Code PIN plus imposé à chaque connexion pour ce rôle.")
                       : on     ? "Code PIN obligatoire pour ce rôle : choisi à la prochaine connexion par ceux qui "
                                  "n'en ont pas."
                                : "Code PIN facultatif pour ce rôle.");
            app.refresh_users();
          });
        }
        ImGui::EndDisabled();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled();
      bool admins = true;
      ImGui::Checkbox(("Administrateur##" + field + "_admin").c_str(), &admins);
      ImGui::EndDisabled();
    }

    void load_notifications(App &app) {
      app.api.get("/api/notifications/", [this](const ApiResult &result) {
        if (!result.ok)
          return;
        notifications_ = result.data;
        warning_days_  = notifications_["key_expiry_warning_days"].integer();
      });
    }

    void notifications_request(App &app, const std::string &method, const std::string &path, const Json &body,
                               const std::string &done) {
      const auto callback = [this, &app, done](const ApiResult &result) {
        if (!result.ok) {
          app.notify("Notifications : " + result.error, true);
          return;
        }
        notifications_ = result.data;
        warning_days_  = notifications_["key_expiry_warning_days"].integer();
        if (!done.empty())
          app.notify(done);
      };
      if (method == "PATCH")
        app.api.patch(path, body, callback);
      else if (method == "POST")
        app.api.post(path, body, callback);
      else
        app.api.remove(path, callback);
    }

    void draw_notifications(App &app) {
      if (notifications_.is_null()) {
        ImGui::TextDisabled("Chargement...");
        if (button("Recharger"))
          load_notifications(app);
        return;
      }
      ImGui::TextWrapped("Les alertes sont envoyées en notifications web : chaque admin ou gestionnaire les active "
                         "et choisit leurs types sur le front web.");
      /* API SMS plus maintenue : activation et choix des evenements SMS, gardes en commentaire.
      bool enabled = notifications_["enabled"].boolean();
      if (ImGui::Checkbox("Activer les notifications SMS", &enabled)) {
        Json body;
        body["enabled"] = enabled;
        notifications_request(app, "PATCH", "/api/notifications/", body,
                              enabled ? "Notifications SMS activées." : "Notifications SMS désactivées.");
      }
      help_marker("Envoyées par le serveur avec l'API SMS de Free Mobile. Chaque destinataire active l'option "
                  "« Notifications par SMS » dans son espace abonné Free, qui donne son identifiant et sa clé.");
      ImGui::BeginDisabled(!enabled);
      static const std::pair< const char *, const char * > events[] = {
        { "stock_low", "Stock sous le minimum d'un type d'item" },
        { "verif_problem", "Vérif de lot incomplète (manquants, périmés, disparus)" },
        { "seal_broken", "Scellé d'un lot ouvert (étiquette d'ouverture, vérif, ajout ou retrait)" },
        { "expired_daily", "Résumé quotidien des lots contenant des périmés (commande check_alerts)" },
        { "pin_blocked", "PIN d'un utilisateur bloqué (trop d'essais faux ou code oublié), une fois par blocage" },
        { "lot_key_renewed", "Étiquette clé d'un lot renouvelée" },
        { "lot_key_expiring", "Étiquette clé de lot qui expire bientôt ou a expiré (commande check_alerts)" },
        { "badge_renewed", "Badge d'un utilisateur renouvelé" },
        { "badge_expiring", "Badge qui expire bientôt ou a expiré (commande check_alerts)" },
        { "order_due", "Commande à passer d'après les prévisions de stock (commande check_alerts)" },
        { "problem_report", "Problème signalé sur un item ou un lot" },
      };
      for (const auto &[event, label] : events) {
        bool value = notifications_["events"][event].boolean();
        if (ImGui::Checkbox(label, &value)) {
          Json body;
          body["events"][event] = value;
          notifications_request(app, "PATCH", "/api/notifications/", body, "");
        }
      }
      ImGui::EndDisabled();
      */

      // delai de l'alerte « expire bientot » des notifications web, envoye en quittant le champ
      ImGui::SetNextItemWidth(80.0f);
      ImGui::InputInt("Alerte d'expiration (jours avant)", &warning_days_, 0, 0);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Json body;
        body["key_expiry_warning_days"] = std::clamp(warning_days_, 1, 365);
        notifications_request(app, "PATCH", "/api/notifications/", body, "");
      }
      help_marker("Étiquettes clés de lot et badges : alerte « expire bientôt » ce nombre de jours avant "
                  "l'expiration (notifications web), puis une alerte le jour de l'expiration.");

      /* API SMS plus maintenue : destinataires et SMS de test, gardes en commentaire.
      ImGui::SeparatorText("Destinataires");
      if (ImGui::BeginTable("sms_recipients", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Identifiant", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Dernier envoi", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableHeadersRow();
        for (const Json &recipient : notifications_["recipients"].items()) {
          const std::string id   = recipient["id"].str();
          const std::string path = "/api/notifications/recipients/" + id + "/";
          ImGui::PushID(id.c_str());
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          bool active = recipient["active"].boolean();
          if (ImGui::Checkbox("##active", &active)) {
            Json body;
            body["active"] = active;
            notifications_request(app, "PATCH", path, body, "");
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(recipient["name"].str().c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(recipient["user"].str().c_str());
          ImGui::TableNextColumn();
          const std::string status = recipient["last_status"].str();
          if (status.empty())
            ImGui::TextDisabled("-");
          else
            ImGui::TextColored(status == "Envoyé" ? colors::green : colors::red, "%s (%s)", status.c_str(),
                               display_datetime(recipient["last_sent"]).c_str());
          ImGui::TableNextColumn();
          if (small_button("SMS de test")) {
            Json body;
            body["recipient"] = recipient["id"];
            body["user"]      = app.user_ref();
            app.api.post("/api/notifications/test/", body, [this, &app](const ApiResult &result) {
              app.notify(result.ok ? "SMS de test envoyé : résultat dans la colonne « Dernier envoi »." : result.error,
                         !result.ok);
              reload_at_ = ImGui::GetTime() + 4.0; // le resultat de l'envoi arrive apres coup
            });
          }
          ImGui::SameLine();
          if (confirm_button("Supprimer", "Supprimer ce destinataire ?", "delete_recipient"))
            notifications_request(app, "DELETE", path, Json(), "Destinataire supprimé.");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      if (reload_at_ > 0.0 && ImGui::GetTime() > reload_at_) {
        reload_at_ = 0.0;
        load_notifications(app);
      }
      if (button("Rafraîchir"))
        load_notifications(app);

      ImGui::SeparatorText("Ajouter un destinataire");
      ImGui::SetNextItemWidth(200.0f);
      input_limited("Nom##sms", new_name_, 64, "ex : Responsable matériel");
      ImGui::SetNextItemWidth(200.0f);
      input_limited("Identifiant Free##sms", new_user_, 32, "8 chiffres");
      ImGui::SetNextItemWidth(200.0f);
      input_limited("Clé d'identification##sms", new_password_, 64, "clé de l'espace abonné", ImGuiInputTextFlags_Password);
      ImGui::BeginDisabled(new_name_.empty() || new_user_.empty() || new_password_.empty());
      if (button("Ajouter")) {
        Json body;
        body["name"]     = new_name_;
        body["user"]     = new_user_;
        body["password"] = new_password_;
        notifications_request(app, "POST", "/api/notifications/recipients/", body, "Destinataire ajouté.");
        new_name_.clear();
        new_user_.clear();
        new_password_.clear();
      }
      ImGui::EndDisabled();
      */
    }

    std::vector< std::pair< std::string, TemplateCategory > > templates_;
    std::vector< LabelType >                                  label_types_; // types des modeles du dossier
    std::string                                               printer_message_; // derniere commande de maintenance envoyee
    bool                                                      confirm_factory_ = false; // « Réglages d'usine » cliqué une fois
    Json                                                      notifications_;
    Json                                                      server_rules_;
    int                                                       warning_days_ = 30;
    // API SMS plus maintenue (destinataires, voir draw_notifications) :
    // std::string                                               new_name_;
    // std::string                                               new_user_;
    // std::string                                               new_password_;
    // double                                                    reload_at_ = 0.0;
};

class EditorWindow final : public AppWindow {
  public:
    EditorWindow() : AppWindow("editor", "Éditeur d'étiquettes", true, true) { admin_only = true; }

    void             draw(App &app) override {
      if (!connected_) {
        editor_.set_print_callback([&app](const TemplateDocument &document) {
          app.print_documents({ PrintJob{ "Test " + document.name, document } });
        });
        connected_ = true;
      }
      editor_.set_label_title(app.settings.label_title);
      editor_.set_default_media(app.settings.label_width_mm, app.settings.label_height_mm);
      editor_.set_default_label(app.settings.physical_label(),
                                app.settings.print.kind != PrinterKind::NiimbotB1 && app.settings.print.raw.ribbon
                                  ? LabelSupport::Transfer
                                  : LabelSupport::Direct);
      editor_.set_selected_templates(app.settings.label_templates);
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
