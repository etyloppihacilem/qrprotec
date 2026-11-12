/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          print_counter.hpp
        -\-    _|__
         |\___/  . \        Created on 12 Nov. 2026 at 17:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "json.hpp"
#include "template.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace qrprotec {

// Etiquettes envoyees a l'imprimante, par jour et par categorie (largeur x hauteur de l'etiquette physique, sans
// l'orientation du modele, et support direct / transfert), pas encore recues par le serveur.
struct PrintTally {
    std::string  day; // AAAA-MM-JJ, heure locale
    double       width_mm  = 0.0;
    double       height_mm = 0.0;
    LabelSupport support   = LabelSupport::Direct;
    int          count     = 0;
};

// Compteur d'impressions du front (inventory/print_counts.py) : le fil d'impression y ajoute chaque etiquette
// acceptee par l'imprimante ; l'application envoie les compteurs au serveur par lots identifies. Un lot en cours
// d'envoi ne change plus (renvoye tel quel apres une reponse perdue : le serveur ne le recompte pas) ; les nouvelles
// impressions vont dans le lot suivant. Garde dans un fichier : hors connexion ou au redemarrage, rien n'est perdu.
// Utilisable depuis plusieurs fils.
class PrintCounter {
  public:
    void load(const std::filesystem::path &file);
    void add(const std::string &day, double width_mm, double height_mm, LabelSupport support, int count = 1);
    // Lot a envoyer (le meme tant qu'il n'est pas acquitte) : {"batch", "counts": [...]} ; null s'il n'y a rien
    Json next_batch();
    // Lot recu par le serveur (ou refuse comme invalide : il ne passerait jamais) : retire
    void batch_done(const std::string &id);
    int  pending() const; // etiquettes pas encore recues par le serveur

  private:
    bool save() const; // sous mutex_

    mutable std::mutex          mutex_;
    std::filesystem::path       file_;
    std::vector< PrintTally >   open_;  // pas encore dans un lot
    std::string                 batch_id_;
    std::vector< PrintTally >   batch_; // lot en cours d'envoi
};

std::string local_day_iso(); // AAAA-MM-JJ du jour, heure locale

} // namespace qrprotec
