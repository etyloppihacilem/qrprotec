/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          labels.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "labels.hpp"

#include "../core/codes.hpp"
#include "../core/paths.hpp"
#include "../core/template_io.hpp"

#include <algorithm>

namespace qrprotec {

namespace {

void add_peremption(Parameters &parameters, const Json &value) {
  const auto date = Date::parse(value.str());
  parameters["peremption"]       = date ? date->display() : "";
  parameters["peremption_iso"]   = date ? date->iso() : "";
  parameters["peremption_short"] = date ? date->display().substr(3) : "";
}

// Mention des etiquettes d'items a dechirer (ex : sachet de serums phy) : arrachee a l'ouverture, l'etiquette
// manquante a la verif suivante compte l'item comme utilise.
const char *tear_notice(const Json &item) {
  return item["tear_off"].boolean() ? "Déchirer avant utilisation" : "";
}

std::string label_date(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : "";
}

} // namespace

Parameters item_parameters(const Json &item, int index, int count) {
  Parameters parameters;
  parameters["iid"]       = item["iid"].str();
  // code imprime : code de l'antenne + forme courte (D sans date, N pour les zeros du compteur), donne par le serveur ;
  // code-barres et petit QR plus courts, plus facile a taper
  parameters["iid_short"] = item["iid_short"].str().empty() ? item_code(item["iid"].str()) : item["iid_short"].str();
  // QR de l'etiquette : URL du front web (l'iid seul pour un item qui n'en a pas, ex : apercu)
  parameters["item_url"]  = item["url"].str().empty() ? item["iid"].str() : item["url"].str();
  parameters["type"]      = item["type"].str();
  parameters["type_name"] = item["type_name"].str();
  parameters["index"]     = std::to_string(index);
  parameters["count"]     = std::to_string(count);
  parameters["pack_id"]     = item["sealed_pack"].str();
  parameters["tear_notice"] = tear_notice(item);
  // etiquette de taches : imprimee a la place de l'etiquette d'item quand le type a des taches
  std::string tasks;
  for (const Json &task : item["tasks"].items())
    tasks += (tasks.empty() ? "" : "\n") + std::string("□ ") + task.str();
  parameters["tasks"]      = tasks;
  parameters["task_count"] = std::to_string(item["tasks"].size());
  parameters["task_url"]   = item["task_url"].str().empty() ? item["iid"].str() : item["task_url"].str();
  // style d'etiquette du type (qr, small, barcode) : choisit le modele a l'impression (App::build_label_jobs)
  parameters["label_style"] = item["label_style"].str("qr");
  add_peremption(parameters, item["peremption"]);
  // contenu du code de l'etiquette selon le style : URL (des taches ou de l'item) pour le QR, iid court sinon (QR
  // reduit, code-barres, planche ou chaque petit QR doit rester lisible)
  const bool url = parameters["label_style"] == "qr";
  parameters["code"] = !url ? parameters["iid_short"] : item["tasks"].size() > 0 ? parameters["task_url"]
                                                                              : parameters["item_url"];
  parameters["exp"]  = parameters["peremption"];
  return parameters;
}

Parameters sealed_pack_parameters(const Json &pack) {
  Parameters parameters;
  parameters["pack_id"]   = pack["id"].str();
  parameters["pack_url"]  = pack["url"].str();
  parameters["count"]     = pack["count"].str();
  parameters["type"]      = pack["type"].str();
  parameters["type_name"] = pack["type_name"].str();
  parameters["tear_notice"] = ""; // l'etiquette du paquet ferme n'est pas a dechirer
  add_peremption(parameters, pack["peremption"]);
  return parameters;
}

// Etiquette de paquet ouvert (paquet pre-etiquete) : `pack` vient de /api/packs/<id>/prelabel/ (poste) ou d'une
// impression envoyee du web (pack_data), avec open_url (cle de l'etiquette)
Parameters pack_open_parameters(const Json &pack) {
  Parameters parameters;
  parameters["pack_id"]       = pack["id"].str();
  parameters["pack_open_url"] = pack["open_url"].str();
  parameters["count"]         = pack["open_label"]["count"].str();
  parameters["labeled_date"]  = label_date(Json(pack["open_label"]["at"].str().substr(0, 10)));
  parameters["type"]          = pack["type"].str();
  parameters["type_name"]     = pack["type_name"].str();
  parameters["tear_notice"]   = "";
  add_peremption(parameters, pack["peremption"]);
  return parameters;
}

Parameters lot_parameters(const Json &lot) {
  Parameters parameters;
  parameters["lot_id"]          = lot["id"].str();
  parameters["lot_name"]        = lot["name"].str();
  parameters["lot_short"]       = lot["name_short"].str();
  parameters["lot_type"]        = lot["lot_type"].str();
  parameters["lot_type_name"]   = lot["lot_type_name"].str();
  // lot global et chemin (sous-lots) : path va du lot global au parent direct
  std::string path;
  for (const Json &parent : lot["path"].items())
    path += parent["name"].str() + " › ";
  parameters["lot_parent_name"] = lot["parent_name"].str();
  parameters["lot_global_name"] = lot["path"].size() > 0 ? lot["path"][0]["name"].str() : lot["name"].str();
  parameters["lot_path"]        = path + lot["name"].str();
  parameters["lot_url"]         = lot["public_url"].str();
  parameters["lot_key"]         = lot["verif_key"].str();
  parameters["lot_private_url"] = lot["private_url"].str();
  parameters["key_expires"]     = label_date(lot["verif_key_expires"]);
  parameters["seal_url"]        = lot["seal_url"].str();
  parameters["seal_open_url"]   = lot["seal_open_url"].str();
  parameters["seal_number"]     = lot["seal_number"].str();
  parameters["sealed_date"]     = label_date(lot["sealed"]);
  parameters["valid_until"]     = label_date(lot["valid_until"]);
  // genre d'un rangement du stock (pièce, armoire...) : « {{rangement}} du stock », {{Rangement}} et {{RANGEMENT}}
  // en debut de phrase ou en majuscules. Vide pour un lot ordinaire.
  const std::string kind        = lot["storage_kind"].str();
  parameters["rangement"]       = kind;
  parameters["Rangement"]       = utf8_capitalize(kind);
  parameters["RANGEMENT"]       = utf8_upper(kind);
  return parameters;
}

std::vector< std::string > lot_locations(const Json &lot) {
  std::vector< std::string > locations; // le serveur envoie les attendus dans l'ordre de la TECH
  for (const Json &row : lot["requirements"].items()) {
    const std::string location = row["location"].str();
    if (!location.empty() && std::find(locations.begin(), locations.end(), location) == locations.end())
      locations.push_back(location);
  }
  return locations;
}

Parameters location_parameters(const Json &lot, const std::string &location) {
  Parameters parameters             = lot_parameters(lot);
  parameters["emplacement"]         = location;
  parameters["Emplacement"]         = utf8_capitalize(location);
  parameters["EMPLACEMENT"]         = utf8_upper(location);
  const std::vector< std::string > locations = lot_locations(lot);
  const auto rank = std::find(locations.begin(), locations.end(), location);
  parameters["emplacement_numero"] = rank == locations.end() ? std::string()
                                                              : std::to_string(rank - locations.begin() + 1);
  // contenu attendu : « 4 × Compresses », une ligne par type (les « dont » d'une ligne a unites restent dans elle)
  std::string contents;
  for (const Json &row : lot["requirements"].items()) {
    if (row["location"].str() != location || !row["within"].str().empty())
      continue;
    std::string line = row["unit"].str().empty()
                       ? std::to_string(row["required"].integer()) + " × " + row["type_name"].str()
                       : std::to_string(row["required"].integer()) + " " + row["unit"].str() + " de "
                           + row["type_name"].str();
    if (row["optional"].boolean())
      line += " (facultatif)";
    contents += (contents.empty() ? "" : "\n") + line;
  }
  parameters["contenu"] = contents;
  return parameters;
}

Parameters user_parameters(const Json &user) {
  Parameters parameters;
  parameters["matricule"]   = user["matricule"].str();
  parameters["nom"]         = user["nom"].str();
  parameters["prenom"]      = user["prenom"].str();
  parameters["full_name"]   = user["prenom"].str() + " " + user["nom"].str();
  parameters["role"]        = user["role_label"].str(user["privileged"].boolean() ? "Administrateur" : "Secouriste");
  parameters["fonction"]    = user["fonction"].str("Secouriste");
  parameters["badge_url"]   = user["badge_url"].str();
  parameters["key_expires"] = label_date(user["key_expires"]);
  return parameters;
}

// Carte d'invite : neuve (sans nom, nom donne a la premiere connexion) ou reimprimee pour un invite qui revient
// (son nom, son antenne et la fin de sa periode, toujours avec la mention invite)
Parameters guest_card_parameters(const Json &card) {
  Parameters parameters;
  const std::string hours = std::to_string(card["hours"].integer(48));
  const std::string name  = card["prenom"].str() + (card["nom"].str().empty() ? "" : " " + card["nom"].str());
  parameters["matricule"]   = card["matricule"].str();
  parameters["badge_url"]   = card["badge_url"].str();
  parameters["guest_hours"] = hours;
  parameters["key_expires"] = label_date(card["key_expires"]);
  parameters["prenom"]      = card["prenom"].str();
  parameters["nom"]         = card["nom"].str();
  parameters["full_name"]   = card["prenom"].str().empty() ? "" : name;
  parameters["antenne"]     = card["antenna"].str();
  std::string until;
  if (const std::string expires = card["expires"].str(); card["state"].str() == "active" && expires.size() >= 16)
    until = "fin le " + label_date(card["expires"]).substr(0, 5) + " à " + expires.substr(11, 5);
  // date limite de premiere connexion : seulement sur une carte neuve
  parameters["date_limite"] = parameters["full_name"].empty() ? parameters["key_expires"] : "";
  if (parameters["full_name"].empty())
    parameters["validite"] = "Valable " + hours + " h après la 1re connexion";
  else
    parameters["validite"] = (parameters["antenne"].empty() ? std::string() : "Antenne " + parameters["antenne"])
                           + (!parameters["antenne"].empty() && !until.empty() ? " · " : "") + until;
  return parameters;
}

bool build_label(const std::string &template_path, const Parameters &values, TemplateDocument &out, std::string &error) {
  if (template_path.empty()) {
    error = "aucun modèle configuré";
    return false;
  }
  TemplateDocument document;
  if (!load_template(document, resolve_template_path(template_path).string(), error)) {
    error = template_path + " : " + error;
    return false;
  }
  for (const auto &[name, value] : values)
    document.parameters[name] = value;
  out = std::move(document);
  return true;
}

} // namespace qrprotec
