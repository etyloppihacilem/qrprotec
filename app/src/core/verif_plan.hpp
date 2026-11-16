/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################
               """          verif_plan.hpp
        -\-    _|__
         |\___/  . \        Created on 16 Nov. 2026 at 19:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr
##################################################################################################################### */

#pragma once

#include "json.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qrprotec {

// Types a unites (attentes en mL, ItemType.unit) : un attendu « Sérum phy » de 200 mL est rempli par ses sous-types
// (sachet de 25 mL, flacon de 50 mL), chacun pour ses unites (ligne de requirements : unit, fills {sous-type: unites}).
// Attendus d'un lot que remplit un item de ce type, avec ses unites (services.row_weights) : son propre type (1), et
// le type a unites dont il est un sous-type.
std::vector< std::pair< std::string, int > > row_weights(const Json &requirements, const std::string &type);
// Attendu principal parmi row_weights : le type a unites s'il y en a un, sinon le type lui-meme (services.main_row)
std::pair< std::string, int > main_row(const std::vector< std::pair< std::string, int > > &weights,
                                       const std::string                                &type);
// Repartition des items scannes entre les lots d'une verif (meme regle que le serveur,
// services.assign_items) : un item deja dans un des lots y reste, un nouvel item va dans le lot de la derniere
// etiquette scannee avant lui (ou un de ses sous-lots) qui en attend encore, sinon le premier lot qui en attend.
struct VerifPlanLot {
    const Json                   *lot = nullptr;
    int                           depth = 0;
    std::map< std::string, int >  fresh;      // items frais scannes par type
    std::map< std::string, int >  required;   // minimum par type (exigences du type de lot)
    std::map< std::string, int >  added;      // items frais venus d'ailleurs (aucun lot de la verif), par type : « + x »
    std::set< std::string >       iids;       // items scannes ranges dans ce lot
    std::set< std::string >       outsiders;  // perimes scannes qui n'etaient pas dans ce lot : a jeter, jamais ranges
    int                           touched = 0; // items scannes ranges dans ce lot
    bool                          complete = false; // complet avec ces scans (verif partielle possible)
};

// Item scanne pendant une verif, tel que le voit la repartition
struct VerifPlanScan {
    std::string iid;
    std::string type;    // type d'item (6 premiers caracteres de l'iid)
    std::string place;   // lot de la derniere etiquette scannee avant lui (vide : aucune)
    bool        expired = false; // perime, ou signale inutilisable / desactive au scan
};

// Repartition des items scannes entre les lots d'une verif, sans etat du poste : App::plan_verif lui donne les lots
// attendus (sans les scelles intacts ni les lots deja valides de la vérif en cascade) et les scans, les tests la
// jouent sur les scenarios communs au serveur et au front web (tests/verif-scenarios/). Meme regle que
// services.assign_items / _complete_lots et planVerif (web/app/verif-plan.js).
// checked : items a cocher coches, {lot: {types}} ; cascade : un item scanne dans un lot qui ne le contient pas y a
// ete trouve.
std::vector< VerifPlanLot > plan_verif(const std::vector< const Json * > &lots, const std::vector< VerifPlanScan > &scanned,
                                       const std::map< std::string, std::set< std::string > > &checked, bool cascade);

// Perime (inutilisable, desactive) scanne : outsider s'il n'etait pas dans le lot qui le recoit (a jeter, jamais
// range) ; sinon need / have, minimum et items frais de son attendu (en unites pour un type a unites) : a jeter si
// have >= need (le lot a son compte sans lui), a remplacer sinon. Meme regle que services.perform_verif.
struct DiscardCheck {
    bool outsider = false;
    int  need     = 0;
    int  have     = 0;
    bool discard() const { return outsider || have >= need; }
};
DiscardCheck discard_check(const std::vector< VerifPlanLot > &plan, const std::string &iid, const std::string &type);

} // namespace qrprotec
