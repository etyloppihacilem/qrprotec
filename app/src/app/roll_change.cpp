// Assistant « Changer de rouleau » : les gestes dans l'ordre, un ecran par etape, puis les commandes a
// l'imprimante a la fin (reglages d'usine si demande, calibrage si le format change, lecture de l'etat) et le
// rouleau declare en place. Ouvert depuis les Reglages, la fenetre « Changez de rouleau » de la file en pause,
// le menu de l'impression dans la barre du haut et la fenetre Impressions et consommables.

#include "../core/paths.hpp"
#include "../core/template_io.hpp"
#include "../printer/label_languages.hpp"
#include "../render/label_types.hpp"
#include "../ui/widgets.hpp"
#include "app.hpp"

#include <cmath>

namespace qrprotec {

namespace {
constexpr const char *kTitle = "Changer de rouleau###roll_wizard";

enum Step { Choose, Before, Remove, Insert, Ribbon, Close, Send, Done, StepCount };

bool same_format(const LabelType &a, const LabelType &b) {
  return std::fabs(a.width_mm - b.width_mm) < 0.05 && std::fabs(a.height_mm - b.height_mm) < 0.05
         && a.support == b.support;
}

const LabelType *find_type(const std::vector< LabelType > &types, const std::string &name) {
  for (const LabelType &type : types)
    if (type.name == name)
      return &type;
  return nullptr;
}

void bullet(const char *text) {
  ImGui::Bullet();
  ImGui::TextWrapped("%s", text);
}
} // namespace

labels::RawJob printer_media_job(const PrintSettings &print, double width_mm, double height_mm, bool ribbon,
                                 double dots_per_mm) {
  labels::RawJob job;
  job.dots_per_mm   = dots_per_mm > 0 ? dots_per_mm : 8.0;
  job.width_dots    = static_cast< int >(std::lround(width_mm * job.dots_per_mm));
  job.height_dots   = static_cast< int >(std::lround(height_mm * job.dots_per_mm));
  job.gap_mm        = print.raw.gap_mm;
  job.ribbon        = ribbon;
  job.darkness      = print.raw.darkness;
  job.offset_x_dots = static_cast< int >(std::lround(print.raw.offset_x_mm * job.dots_per_mm));
  job.offset_y_dots = static_cast< int >(std::lround(print.raw.offset_y_mm * job.dots_per_mm));
  job.tear_off_dots = static_cast< int >(std::lround(print.raw.tear_off_mm * job.dots_per_mm));
  return job;
}

void App::open_roll_wizard(const std::string &type_name) {
  RollWizard wizard;
  std::vector< TemplateDocument > documents;
  for (const auto &path : glob_templates("*.qr")) {
    TemplateDocument document;
    std::string      error;
    if (load_template(document, path.string(), error))
      documents.push_back(std::move(document));
  }
  wizard.types        = collect_label_types(documents);
  wizard.chosen       = !type_name.empty() ? type_name : settings.print.loaded_label_type;
  wizard.open_pending = true;
  const LabelType *chosen = find_type(wizard.types, wizard.chosen);
  const LabelType *loaded = find_type(wizard.types, settings.print.loaded_label_type);
  // autre format que le rouleau en place (ou format inconnu) : l'imprimante doit le mesurer
  wizard.calibrate = chosen == nullptr || loaded == nullptr || !same_format(*chosen, *loaded);
  roll_wizard_     = std::move(wizard);
}

void App::draw_roll_wizard() {
  RollWizard &wizard = roll_wizard_;
  if (wizard.open_pending && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    wizard.open_pending = false;
    ImGui::OpenPopup(kTitle);
  }
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;
  const float      width  = ImGui::GetFontSize() * 32.0f;
  const bool       raw    = settings.print.kind != PrinterKind::NiimbotB1;
  const LabelType *chosen = find_type(wizard.types, wizard.chosen);
  const bool       ribbon = chosen != nullptr ? chosen->support == LabelSupport::Transfer : settings.print.raw.ribbon;
  // la Niimbot n'a ni capot a loquets ni commandes : choix du rouleau puis confirmation
  const auto next = [&](int step) {
    if (!raw && step > Choose && step < Send)
      return static_cast< int >(Send);
    return step;
  };
  const int step_count = raw ? StepCount - 1 : 2; // ecran final « Fait » non compte
  const int shown_step = raw ? std::min(wizard.step, static_cast< int >(Send)) + 1 : (wizard.step == Choose ? 1 : 2);

  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  if (wizard.step != Done)
    ImGui::TextDisabled("Étape %d sur %d", shown_step, step_count);
  const auto title = [](const char *text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextColored(colors::orange, "%s", text);
    ImGui::PopFont();
  };

  switch (wizard.step) {
  case Choose: {
    title("Quel rouleau mettez-vous ?");
    ImGui::SetNextItemWidth(width * 0.7f);
    if (ImGui::BeginCombo("##roll", chosen != nullptr ? chosen->name.c_str() : "choisir le rouleau")) {
      for (const LabelType &type : wizard.types) {
        if (ImGui::Selectable(type.name.c_str(), type.name == wizard.chosen)) {
          wizard.chosen        = type.name;
          const LabelType *now = find_type(wizard.types, settings.print.loaded_label_type);
          wizard.calibrate     = now == nullptr || !same_format(type, *now);
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", type.summary().c_str());
      }
      ImGui::EndCombo();
    }
    chosen = find_type(wizard.types, wizard.chosen);
    if (chosen != nullptr)
      ImGui::TextDisabled("%s", chosen->summary().c_str());
    const std::string &loaded = settings.print.loaded_label_type;
    ImGui::TextDisabled("En place : %s", loaded.empty() ? "format libre" : ("« " + loaded + " »").c_str());
    if (raw) {
      ImGui::Spacing();
      ImGui::Checkbox("Calibrer l'imprimante sur ce rouleau", &wizard.calibrate);
      help_marker("Coché d'office quand la taille ou le support (ruban ou non) change. À cocher aussi pour un "
                  "rouleau d'un autre fabricant, ou si des étiquettes blanches sortaient entre deux impressions.");
      ImGui::Checkbox("Remettre d'abord l'imprimante aux réglages d'usine", &wizard.factory);
      help_marker("Seulement si l'imprimante a été réglée pour autre chose (imprimante d'occasion, essais) ou se "
                  "comporte bizarrement. Le réseau n'est pas touché. Le calibrage suit toujours.");
      if (wizard.factory)
        wizard.calibrate = true;
    }
    break;
  }
  case Before:
    title("Avant d'ouvrir");
    bullet("Laissez l'imprimante allumée.");
    {
      const PrintQueueStatus print = printer.status();
      if (print.busy && !print.paused)
        bullet("Une étiquette est en train de partir : attendez la fin de l'impression.");
      else if (print.pending > 0)
        bullet(("Les étiquettes en attente (" + std::to_string(print.pending)
                + ") restent dans la file : elles partiront sur le nouveau rouleau à la fin.")
                 .c_str());
      else
        bullet("La file d'impression est vide : rien ne part pendant le changement.");
    }
    break;
  case Remove:
    title("Retirer l'ancien rouleau");
    bullet("Ouvrez le capot : tirez vers vous les deux loquets, sur les côtés, et levez le capot.");
    bullet("Écartez les deux supports du rouleau et retirez le mandrin vide (ou ce qui reste du rouleau).");
    bullet("Décollez toute étiquette restée collée sous la tête ou sur le rouleau noir d'entraînement.");
    break;
  case Insert:
    title("Mettre le nouveau rouleau");
    bullet("Écartez les supports et placez le rouleau entre eux, étiquettes vers le haut, le papier partant "
           "par-dessus le rouleau vers l'avant. Relâchez : le rouleau doit tourner librement.");
    bullet("Faites passer le papier sous les deux guides et rapprochez-les jusqu'à toucher le bord du papier, "
           "sans le serrer.");
    bullet("Tirez le papier jusqu'à ce qu'il dépasse de quelques centimètres à l'avant.");
    break;
  case Ribbon:
    title(ribbon ? "Ruban : il en faut un" : "Ruban : il n'en faut pas");
    if (ribbon) {
      bullet("Ce rouleau s'imprime avec un ruban (transfert thermique). Vérifiez qu'il en reste ; sinon, mettez-en "
             "un neuf en suivant le schéma collé dans le capot : rouleau neuf et mandrin vide sur leurs axes, "
             "ruban passé sous la tête, scotché sur le mandrin vide, tendu en tournant la molette.");
      bullet("Le ruban doit être au moins aussi large que l'étiquette, encrage extérieur.");
      bullet("Ruban changé : nettoyez la tête avec un stylo ou un coton-tige à l'alcool isopropylique et laissez "
             "sécher une minute.");
    } else {
      bullet("Ce rouleau est en thermique direct : retirez le ruban s'il y en a un, il s'userait pour rien.");
    }
    break;
  case Close:
    title("Fermer");
    bullet("Fermez le capot en appuyant des deux côtés jusqu'au clic.");
    bullet("Le voyant doit passer au vert fixe. Rouge clignotant : capot mal fermé ou papier mal engagé.");
    break;
  case Send:
    title(raw ? "Envoyer à l'imprimante" : "Confirmer");
    ImGui::TextWrapped("En cliquant sur Terminer :");
    if (raw && wizard.factory)
      bullet("l'imprimante revient à ses réglages d'usine ;");
    if (raw && wizard.calibrate)
      bullet("elle reçoit le format du rouleau, le mesure et règle ses capteurs en avançant 2 à 4 étiquettes, puis "
             "enregistre le tout ;");
    if (raw)
      bullet("son état est relu (prête, capot ouvert, plus d'étiquettes...) ;");
    bullet(("le poste note « " + wizard.chosen + " » comme rouleau en place, et les étiquettes qui l'attendaient "
            "repartent.")
             .c_str());
    break;
  case Done: {
    title("Rouleau en place");
    const PrintQueueStatus print = printer.status();
    if (raw) {
      ImGui::TextUnformatted("État de l'imprimante :");
      ImGui::SameLine();
      if (print.printer_state_blocking)
        ImGui::TextColored(colors::red, "%s", print.printer_state.c_str());
      else if (print.printer_state_known)
        ImGui::TextColored(colors::green, "%s", print.printer_state.c_str());
      else
        ImGui::TextDisabled("%s", print.printer_state.empty() ? "lecture…" : print.printer_state.c_str());
      if (print.printer_state_blocking)
        bullet("Réglez ce qui est indiqué, puis Relire l'état.");
      if (wizard.calibrate)
        bullet("Attendez que l'imprimante s'arrête. Pour garder les étiquettes sorties pendant le calibrage, "
               "ouvrez le capot, réenroulez-les à la main jusqu'à la sortie et refermez.");
      if (wizard.calibrate)
        bullet("Imprimez l'étiquette d'étalonnage : les quatre cadres pleins doivent apparaître. Sinon, réglez les "
               "décalages dans Réglages > Étiquettes et impression.");
    }
    if (print.paused && print.needed_label_type.empty() && !print.error.empty())
      bullet("La file est en pause : une fois l'imprimante prête, Reprendre l'impression.");
    break;
  }
  default: break;
  }
  ImGui::PopTextWrapPos();

  ImGui::Spacing();
  ImGui::Separator();
  // pas de validation par Entree : la douchette en mode clavier en envoie une a chaque scan
  if (wizard.step == Done) {
    const PrintQueueStatus print = printer.status();
    if (raw) {
      if (button("Relire l'état"))
        printer.request_printer_state(settings.print);
      ImGui::SameLine();
      if (button("Étiquette d'étalonnage") && chosen != nullptr) {
        PrintJob job;
        job.description = "Étiquette d'étalonnage";
        job.document    = calibration_document(*chosen, true);
        job.image       = calibration_image(job.document, chosen->effective_margin_mm());
        print_documents({ job });
      }
      ImGui::SameLine();
    }
    if (print.paused && print.needed_label_type.empty() && button("Reprendre l'impression"))
      printer.resume();
    if (print.paused && print.needed_label_type.empty())
      ImGui::SameLine();
    if (primary_button("Fermer"))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  if (wizard.step > Choose) {
    if (button("Précédent")) {
      int step = wizard.step - 1;
      if (!raw && step > Choose)
        step = Choose;
      wizard.step = step;
    }
    ImGui::SameLine();
  }
  if (wizard.step < Send) {
    ImGui::BeginDisabled(chosen == nullptr);
    if (primary_button("Suivant"))
      wizard.step = next(wizard.step + 1);
    ImGui::EndDisabled();
  } else if (primary_button("Terminer") && chosen != nullptr) {
    if (raw) {
      PrintSettings print = settings.print;
      print.raw.ribbon    = ribbon;
      const double ppm    = settings.physical_label().pixels_per_mm;
      const labels::RawJob job =
        printer_media_job(print, chosen->width_mm, chosen->height_mm, ribbon, ppm);
      const bool zpl = print.kind == PrinterKind::Zpl;
      if (wizard.factory)
        printer.send_command("Réglages d'usine", labels::maintenance_command(zpl, labels::Maintenance::Factory, job),
                             print);
      if (wizard.calibrate)
        printer.send_command("Calibrage", labels::maintenance_command(zpl, labels::Maintenance::Calibrate, job),
                             print);
      printer.request_printer_state(print);
    }
    set_loaded_label_type(wizard.chosen); // reprend la file si elle attendait ce rouleau
    wizard.step = Done;
  }
  ImGui::SameLine();
  if (button("Annuler"))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

} // namespace qrprotec
