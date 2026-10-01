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

namespace qrprotec {

namespace {

void add_peremption(Parameters &parameters, const Json &value) {
  const auto date = Date::parse(value.str());
  parameters["peremption"]       = date ? date->display() : "";
  parameters["peremption_iso"]   = date ? date->iso() : "";
  parameters["peremption_short"] = date ? date->display().substr(3) : "";
}

std::string label_date(const Json &value) {
  const auto date = Date::parse(value.str());
  return date ? date->display() : "";
}

} // namespace

Parameters item_parameters(const Json &item, int index, int count) {
  Parameters parameters;
  parameters["iid"]       = item["iid"].str();
  parameters["type"]      = item["type"].str();
  parameters["type_name"] = item["type_name"].str();
  parameters["index"]     = std::to_string(index);
  parameters["count"]     = std::to_string(count);
  parameters["pack_id"]   = item["sealed_pack"].str();
  add_peremption(parameters, item["peremption"]);
  return parameters;
}

Parameters sealed_pack_parameters(const Json &pack) {
  Parameters parameters;
  parameters["pack_id"]   = pack["id"].str();
  parameters["pack_url"]  = pack["url"].str();
  parameters["count"]     = pack["count"].str();
  parameters["type"]      = pack["type"].str();
  parameters["type_name"] = pack["type_name"].str();
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
  parameters["seal_number"]     = lot["seal_number"].str();
  parameters["sealed_date"]     = label_date(lot["sealed"]);
  parameters["valid_until"]     = label_date(lot["valid_until"]);
  return parameters;
}

Parameters user_parameters(const Json &user) {
  Parameters parameters;
  parameters["matricule"]   = user["matricule"].str();
  parameters["nom"]         = user["nom"].str();
  parameters["prenom"]      = user["prenom"].str();
  parameters["full_name"]   = user["prenom"].str() + " " + user["nom"].str();
  parameters["role"]        = user["role_label"].str(user["privileged"].boolean() ? "Administrateur" : "Secouriste");
  parameters["badge_url"]   = user["badge_url"].str();
  parameters["key_expires"] = label_date(user["key_expires"]);
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
