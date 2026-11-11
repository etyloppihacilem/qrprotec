/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################
               """          verif_cache.hpp
        -\-    _|__
         |\___/  . \        Created on 11 Nov. 2026 at 13:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr
##################################################################################################################### */

#pragma once

#include "json.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace qrprotec {

// Cache de vérif (serveur : inventory/verif_cache.py, meme fonctionnement que vcache du front web). Une partie de la
// reserve n'a ni reseau ni wifi : tout ce qu'une verif lit au serveur (lots globaux avec leurs sous-lots, items,
// paquets fermes, types d'items) est charge en arriere-plan et chaque scan se traite aussitot depuis ce cache.
// Enregistrements « i:<iid> », « l:<lot global> », « p:<paquet> », « t:<type> » repartis en seaux : on envoie
// l'empreinte de chaque seau, le serveur ne renvoie que les seaux changes (avec tout leur contenu).
class VerifCache {
  public:
    // Corps de POST /api/verifs/cache/ : {"buckets": {seau: empreinte}}
    Json request() const;
    // Reponse du serveur appliquee ; renvoie les cles des enregistrements changes (ajoutes, modifies, retires)
    std::vector< std::string > apply(const Json &response);

    bool        loaded() const { return loaded_; }
    std::size_t size() const { return records_.size(); }
    const Json *item(const std::string &iid) const { return find("i:" + iid); }
    const Json *pack(const std::string &id) const { return find("p:" + id); }
    const Json *type(const std::string &code) const { return find("t:" + code); }
    // lot global (ou lot seul) qui contient le lot `id`, vide s'il n'est pas dans le cache
    std::string root_of(const std::string &id) const;
    // detail du lot global, sous-lots compris, comme GET /api/lots/<id>/ (items repris des enregistrements d'items)
    Json lot(const std::string &root) const;

  private:
    const Json *find(const std::string &key) const;
    Json        with_items(const Json &row) const;

    std::unordered_map< std::string, Json >              records_;
    std::map< std::string, std::string >                 hashes_; // seau -> empreinte
    std::map< std::string, std::vector< std::string > >  keys_;   // seau -> cles
    std::unordered_map< std::string, std::string >       roots_;  // lot -> lot global
    bool                                                 loaded_ = false;
};

// Envois de verif en attente (hors connexion) : verif finale, etapes de la verif en cascade, scelles intacts. Gardes
// dans un fichier (redemarrage du poste) et renvoyes dans l'ordre au retour du reseau.
struct OutboxEntry {
    Json        body;
    std::string label; // lot, pour les messages
    std::string at;    // heure de la verif (affichage)
};

class VerifOutbox {
  public:
    void                      load(const std::filesystem::path &file);
    bool                      save() const;
    void                      push(OutboxEntry entry);
    void                      pop_front();
    bool                      empty() const { return entries_.empty(); }
    std::size_t               size() const { return entries_.size(); }
    const OutboxEntry        &front() const { return entries_.front(); }

  private:
    std::filesystem::path      file_;
    std::vector< OutboxEntry > entries_;
};

} // namespace qrprotec
