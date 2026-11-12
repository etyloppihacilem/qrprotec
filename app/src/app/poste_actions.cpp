/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          poste_actions.cpp
        -\-    _|__
         |\___/  . \        Created on 12 Nov. 2026 at 18:30
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

// Codes d'action rapide des postes (« qrprotec:poste/ACTION?param=valeur », voir core/codes.hpp) : scannes a la
// douchette (ou au telephone utilise comme douchette du poste), ils agissent tout de suite sur ce poste, sans
// passer par la pile de scans. Le front web les ignore.
//
// Ajouter une action : une ligne dans kActions (nom dans le code, qui peut s'en servir, fonction), la fonction
// ci-dessous, et un generateur de code a imprimer si besoin (ex. roll_action_code). Une action inconnue de ce poste
// (poste pas a jour) est signalee sans rien faire. Les actions doivent rester sans danger si le code est scanne par
// erreur ou par n'importe qui : une action sensible demande un role (Access) et la personne connectee.

#include "../core/paths.hpp"
#include "../core/template_io.hpp"
#include "../render/label_types.hpp"
#include "app.hpp"

#include <cmath>

namespace qrprotec {

namespace {

enum class Access { Anyone, Privileged, Admin };

struct PosteAction {
    const char *name;   // dans le code : qrprotec:poste/<name>
    const char *label;  // pour les messages
    Access      access;
    void (*run)(App &app, const ParsedScan &scan, ScanSource source);
};

// Types d'etiquettes des modeles de ce poste
std::vector< LabelType > local_label_types() {
  std::vector< TemplateDocument > documents;
  for (const auto &path : glob_templates("*.qr")) {
    TemplateDocument document;
    std::string      error;
    if (load_template(document, path.string(), error))
      documents.push_back(std::move(document));
  }
  return collect_label_types(documents);
}

std::string param(const ParsedScan &scan, const char *key) {
  const auto found = scan.params.find(key);
  return found == scan.params.end() ? std::string() : found->second;
}

// rouleau?t=NOM&w=&h=&s= : rouleau en place dans l'imprimante. Le nom d'abord ; un poste dont les modeles nomment
// ce format autrement le retrouve par sa taille et son support.
void loaded_roll(App &app, const ParsedScan &scan, ScanSource source) {
  const std::string name = param(scan, "t");
  if (name.empty()) {
    app.feedback.error(source, app.settings);
    app.notify("Code de rouleau sans nom : réimprimez-le depuis Impressions et consommables.", true);
    return;
  }
  const std::vector< LabelType > types = local_label_types();
  std::string                    chosen;
  for (const LabelType &type : types)
    if (type.name == name)
      chosen = type.name;
  if (chosen.empty()) {
    const double      width   = std::atof(param(scan, "w").c_str());
    const double      height  = std::atof(param(scan, "h").c_str());
    const std::string support = param(scan, "s");
    for (const LabelType &type : types)
      if (std::fabs(type.width_mm - width) < 0.05 && std::fabs(type.height_mm - height) < 0.05
          && support == label_support_id(type.support)) {
        chosen = type.name;
        break;
      }
  }
  if (chosen.empty()) {
    app.feedback.error(source, app.settings);
    app.notify("Rouleau « " + name + " » inconnu sur ce poste (aucun modèle de ce type d'étiquette).", true);
    return;
  }
  app.feedback.notice(source, app.settings);
  if (chosen == app.settings.print.loaded_label_type) {
    app.notify("Le rouleau « " + chosen + " » est déjà en place.");
    return;
  }
  app.set_loaded_label_type(chosen); // reprend la file si elle attendait ce rouleau
}

const PosteAction kActions[] = {
  { "rouleau", "Rouleau en place", Access::Anyone, &loaded_roll },
};

} // namespace

void App::handle_poste_action(const ParsedScan &scan, ScanSource source) {
  for (const PosteAction &action : kActions) {
    if (scan.id != action.name)
      continue;
    const bool allowed = action.access == Access::Anyone || (action.access == Access::Privileged && privileged())
                      || (action.access == Access::Admin && admin());
    if (!allowed) {
      feedback.error(source, settings);
      notify(std::string(action.label) + " : réservé "
               + (action.access == Access::Admin ? "aux administrateurs" : "aux rôles gestion et admin")
               + ", scannez d'abord votre badge.",
             true);
      return;
    }
    action.run(*this, scan, source);
    return;
  }
  feedback.error(source, settings);
  notify("Code d'action « " + scan.id + " » inconnu de ce poste : mettez QRProtec à jour.", true);
}

void App::print_roll_code(const LabelType &roll, const LabelType &print_on) {
  PrintJob job;
  job.description = "Code du rouleau " + roll.name;
  job.document    = roll_code_document(roll, print_on);
  print_documents({ job });
}

} // namespace qrprotec
