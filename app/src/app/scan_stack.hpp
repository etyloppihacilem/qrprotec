/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          scan_stack.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/codes.hpp"
#include "../core/json.hpp"
#include "../inateck/inateck_worker.hpp"

#include <string>
#include <vector>

namespace qrprotec {

enum class EntryState { Pending, Ok, Error };

struct ScanEntry {
    int                        id = 0;
    ParsedScan                 scan;
    ScanSource                 source = ScanSource::Manual;
    EntryState                 state  = EntryState::Pending;
    std::string                title;  // ex: "Compresses steriles"
    std::string                detail; // ex: "Lot Sac A" / "Inconnu en base"
    bool                       expired   = false;
    bool                       duplicate = false;
    bool                       warning   = false; // item signale disparu/supprime...
    std::vector< std::string > pack_items;       // iids contenus dans un paquet scelle
    Json                       data;             // reponse de l'API
};

// Lot cible, defini en scannant l'etiquette privee d'un lot.
struct TargetLot {
    std::string id;
    std::string key;
    std::string name;
    bool        valid() const { return !id.empty(); }
};

class ScanStack {
  public:
    ScanEntry &add(const ParsedScan &scan, ScanSource source);
    ScanEntry *find(int id);
    void       remove(int id);
    void       remove_duplicates();
    void       remove_errors();
    void       undo_last();
    void       clear();

    const std::vector< ScanEntry > &entries() const { return entries_; }
    bool                            empty() const { return entries_.empty(); }

    // iids a envoyer a l'API : items scannes + contenu des paquets, sans doublons ni erreurs.
    std::vector< std::string > iids() const;
    bool                       contains_iid(const std::string &iid) const;
    std::size_t                expired_count() const;

    TargetLot target;

  private:
    void refresh_duplicates();

    std::vector< ScanEntry > entries_;
    int                      next_id_ = 1;
};

} // namespace qrprotec
