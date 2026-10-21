/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          placeholders.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "placeholders.hpp"

#include "codes.hpp"

namespace qrprotec {

namespace {

std::vector< PlaceholderInfo > item_type_placeholders() {
  return {
    { "type", "Code du type d'item (6 caractères)", "compre" },
    { "type_name", "Nom du type d'item", "Compresses stériles" },
    { "peremption", "Date de péremption JJ/MM/AAAA (vide si non périssable)", "31/12/2027" },
    { "peremption_iso", "Date de péremption AAAA-MM-JJ", "2027-12-31" },
    { "peremption_short", "Date de péremption MM/AAAA", "12/2027" },
    { "tear_notice", "« Déchirer avant utilisation » pour un type à étiquette à déchirer (vide sinon)",
      "Déchirer avant utilisation" },
  };
}

std::vector< PlaceholderInfo > lot_placeholders() {
  return {
    { "lot_id", "Identifiant du lot", "sacpse00000001" },
    { "lot_name", "Nom du lot", "Sac PSE1 n°1" },
    { "lot_short", "Nom court du lot", "PSE1-1" },
    { "lot_type", "Code du type de lot", "sacpse" },
    { "lot_type_name", "Nom du type de lot", "Sac PSE1" },
    { "lot_parent_name", "Lot qui contient ce lot (vide si aucun)", "B+ n°1" },
    { "lot_global_name", "Lot global (le lot lui-même s'il n'est dans aucun lot)", "VPS 1" },
    { "lot_path", "Chemin du lot, du lot global au lot", "VPS 1 › B+ n°1 › Sac O2" },
    { "lot_url", "URL publique de vérif (QR de l'étiquette publique)", "https://example.com/verif?lot=sacpse00000001" },
  };
}

std::vector< CategoryInfo > build_categories() {
  std::vector< CategoryInfo > categories;
  categories.push_back({ TemplateCategory::Generic, "generic", "Générique", "Modèle libre, sans données de la base.", {} });

  // Etiquettes d'items : 3 styles (choisis par type d'item : QR de l'URL, petit QR ou code-barres de l'iid seul),
  // chacun en version normale, liste de taches (types a taches) ou a dechirer (types a etiquette a dechirer)
  struct ItemStyle {
    const char *id, *label, *code;
  };
  const ItemStyle styles[] = {
    { "", "", "Le QR code doit contenir {{item_url}} ({{iid}} seul reste lu, mais n'ouvre pas le front web)." },
    { "_small", " - QR réduit",
      "Petit QR code de {{iid}} seul, moins déformé sur un objet rond (il n'ouvre pas le front web)." },
    { "_barcode", " - code-barres",
      "Code-barres de {{iid}} seul (une URL serait trop longue), lisible sur un objet rond." },
  };
  const TemplateCategory plain[]  = { TemplateCategory::Item, TemplateCategory::ItemSmall, TemplateCategory::ItemBarcode };
  const TemplateCategory tasks[]  = { TemplateCategory::TaskItem, TemplateCategory::TaskItemSmall,
                                      TemplateCategory::TaskItemBarcode };
  const TemplateCategory torn[]   = { TemplateCategory::ItemTear, TemplateCategory::ItemSmallTear,
                                      TemplateCategory::ItemBarcodeTear };
  const auto item_common = [](CategoryInfo &info, const char *iid_example) {
    info.placeholders.push_back({ "iid", "Identifiant unique de l'item", iid_example });
    info.placeholders.push_back({ "iid_short",
                                  "Identifiant en forme courte (D : sans date, N : zéros du compteur), lu comme l'iid",
                                  compact_iid(iid_example) });
    for (const PlaceholderInfo &placeholder : item_type_placeholders())
      info.placeholders.push_back(placeholder);
    info.placeholders.push_back({ "index", "Numéro de l'étiquette dans la série imprimée", "3" });
    info.placeholders.push_back({ "count", "Nombre d'étiquettes de la série", "10" });
    info.placeholders.push_back({ "pack_id", "Paquet scellé d'origine (vide sinon)", "0000002B" });
  };
  for (int style = 0; style < 3; ++style) {
    const ItemStyle &item_style = styles[style];
    CategoryInfo item{ plain[style], std::string("item") + item_style.id, std::string("Item") + item_style.label,
                       std::string("Étiquette collée sur chaque item. ") + item_style.code, {} };
    if (style == 0)
      item.placeholders.push_back({ "item_url", "URL de l'item (contenu du QR code) : ouvre le front web comme un scan",
                                    "https://example.com/item?id=compre20271231NA1" });
    item_common(item, "compre20271231000000A1");
    categories.push_back(item);

    // etiquette de taches : remplace l'etiquette d'item des types qui ont des taches
    CategoryInfo task_item{ tasks[style], std::string("task_item") + item_style.id,
                            std::string("Item") + item_style.label + " - liste de tâches",
                            std::string("Étiquette des items dont le type a des tâches à faire à la vérif (ex : DAE), "
                                        "imprimée à la place de l'étiquette d'item, avec la liste des tâches. La "
                                        "scanner ouvre la liste à cocher. ")
                              + (style == 0 ? "Le QR code doit contenir {{task_url}}." : item_style.code),
                            {} };
    if (style == 0)
      task_item.placeholders.push_back({ "task_url", "URL des tâches de l'item (contenu du QR code)",
                                         "https://example.com/task?id=defibrDNA1" });
    task_item.placeholders.push_back({ "tasks", "Liste des tâches, une par ligne",
                                       "□ Voyant vert allumé\n□ Électrodes dans leur date" });
    task_item.placeholders.push_back({ "task_count", "Nombre de tâches", "2" });
    item_common(task_item, "defibr00000000000000A1");
    categories.push_back(task_item);

    // etiquette a dechirer : remplace l'etiquette d'item des types a etiquette a dechirer
    CategoryInfo tear_item{ torn[style], std::string("item") + item_style.id + "_tear",
                            std::string("Item") + item_style.label + " - à déchirer",
                            std::string("Étiquette des items dont le type a une étiquette à déchirer avant utilisation "
                                        "(ex : sachet de sérums phy), imprimée à la place de l'étiquette d'item. ")
                              + item_style.code,
                            {} };
    if (style == 0)
      tear_item.placeholders.push_back({ "item_url", "URL de l'item (contenu du QR code)",
                                         "https://example.com/item?id=serphy20271231NA1" });
    item_common(tear_item, "serphy20271231000000A1");
    categories.push_back(tear_item);
  }

  CategoryInfo pack{ TemplateCategory::ItemPack, "itempack", "Paquet fermé (ItemPack)",
                     "Étiquette d'un paquet fermé : les étiquettes individuelles seront imprimées à l'ouverture. "
                     "Le QR code doit contenir {{pack_url}}.",
                     {} };
  pack.placeholders.push_back({ "pack_id", "Identifiant du paquet", "0000002B" });
  pack.placeholders.push_back({ "pack_url", "URL du paquet (contenu du QR code)", "https://example.com/pack?id=0000002B" });
  pack.placeholders.push_back({ "count", "Nombre d'items dans le paquet", "25" });
  for (const PlaceholderInfo &info : item_type_placeholders())
    pack.placeholders.push_back(info);
  categories.push_back(pack);

  CategoryInfo lot_public{ TemplateCategory::LotPublic, "lot_public", "Lot - étiquette publique",
                           "Étiquette visible du lot. Le QR code doit contenir {{lot_url}}.", lot_placeholders() };
  categories.push_back(lot_public);

  CategoryInfo lot_private{ TemplateCategory::LotPrivate, "lot_private", "Lot - étiquette clé",
                            "Étiquette contenant la clé du lot (à garder à l'abri). Le QR code doit contenir "
                            "{{lot_private_url}}.",
                            lot_placeholders() };
  lot_private.placeholders.push_back({ "lot_key", "Clé de vérification du lot", "a1B2c3D4e5F6g7H8i9J0k1L2" });
  lot_private.placeholders.push_back(
    { "lot_private_url", "URL de vérif avec la clé (QR de l'étiquette clé)",
      "https://example.com/verif?lot=sacpse00000001&key=a1B2c3D4e5F6g7H8i9J0k1L2" }
  );
  lot_private.placeholders.push_back({ "key_expires", "Date d'expiration de la clé JJ/MM/AAAA", "29/09/2036" });
  categories.push_back(lot_private);

  CategoryInfo lot_seal{ TemplateCategory::LotSeal, "lot_seal", "Lot - scellé",
                         "Étiquette posée sur le scellé d'un lot : le lot est valide sans vérif tant que le scellé "
                         "est intact. Le QR code doit contenir {{seal_url}}.",
                         lot_placeholders() };
  lot_seal.placeholders.push_back(
    { "seal_url", "URL du scellé (contenu du QR code)", "https://example.com/seal?lot=sacpse00000001&s=Xy12Ab34Cd56Ef78" }
  );
  lot_seal.placeholders.push_back({ "seal_number", "Numéro du scellé physique (peut être vide)", "004512" });
  lot_seal.placeholders.push_back({ "sealed_date", "Date du scellage JJ/MM/AAAA", "30/09/2026" });
  lot_seal.placeholders.push_back(
    { "valid_until", "Première péremption du contenu JJ/MM/AAAA (vide si rien de périssable)", "31/03/2027" }
  );
  categories.push_back(lot_seal);

  CategoryInfo lot_seal_open{ TemplateCategory::LotSealOpen, "lot_seal_open", "Lot - ouverture du scellé",
                              "Étiquette rangée à l'intérieur du lot scellé : la scanner ouvre le scellé, même sans "
                              "être connecté. Le QR code doit contenir {{seal_open_url}}.",
                              lot_placeholders() };
  lot_seal_open.placeholders.push_back({ "seal_open_url", "URL d'ouverture du scellé (contenu du QR code)",
                                         "https://example.com/unseal?lot=sacpse00000001&c=Gh34Ij56Kl78Mn90" });
  lot_seal_open.placeholders.push_back({ "seal_number", "Numéro du scellé physique (peut être vide)", "004512" });
  lot_seal_open.placeholders.push_back({ "sealed_date", "Date du scellage JJ/MM/AAAA", "30/09/2026" });
  categories.push_back(lot_seal_open);

  // rangement du stock : une seule etiquette, privee, collee a l'interieur (cle sans expiration)
  CategoryInfo lot_storage{ TemplateCategory::LotStorage, "lot_storage", "Rangement du stock",
                            "Seule étiquette d'un rangement du stock (armoire, tiroir), collée à l'intérieur dans "
                            "une salle fermée : elle contient la clé du rangement, qui n'expire pas. Le QR code doit "
                            "contenir {{lot_private_url}}.",
                            lot_placeholders() };
  lot_storage.placeholders.push_back({ "lot_key", "Clé de vérification du rangement", "a1B2c3D4e5F6g7H8i9J0k1L2" });
  lot_storage.placeholders.push_back(
    { "lot_private_url", "URL de vérif avec la clé (contenu du QR code)",
      "https://example.com/verif?lot=tiroi300000001&key=a1B2c3D4e5F6g7H8i9J0k1L2" }
  );
  categories.push_back(lot_storage);

  CategoryInfo user{ TemplateCategory::User, "user", "Badge utilisateur",
                     "Badge de connexion d'un secouriste. Le QR code doit contenir {{badge_url}}.", {} };
  user.placeholders = {
    { "matricule", "Matricule", "M0042" },
    { "nom", "Nom", "Dupont" },
    { "prenom", "Prénom", "Jeanne" },
    { "full_name", "Prénom et nom", "Jeanne Dupont" },
    { "role", "Droits (Secouriste / Gestion / Administrateur)", "Secouriste" },
    { "fonction", "Rôle dans l'équipe (l'OPÉ, le TECH...)", "Secouriste" },
    { "badge_url", "URL du badge (contenu du QR code)", "https://example.com/badge?m=M0042&key=a1B2c3D4e5F6g7H8" },
    { "key_expires", "Date d'expiration du badge JJ/MM/AAAA", "29/09/2027" },
  };
  categories.push_back(user);
  return categories;
}

} // namespace

const std::vector< CategoryInfo > &template_categories() {
  static const std::vector< CategoryInfo > categories = build_categories();
  return categories;
}

const CategoryInfo &category_info(TemplateCategory category) {
  for (const CategoryInfo &info : template_categories())
    if (info.category == category)
      return info;
  return template_categories().front();
}

const char *category_id(TemplateCategory category) {
  return category_info(category).id.c_str();
}

TemplateCategory category_from_id(const std::string &id) {
  for (const CategoryInfo &info : template_categories())
    if (info.id == id)
      return info.category;
  return TemplateCategory::Generic;
}

const std::vector< PlaceholderInfo > &common_placeholders() {
  static const std::vector< PlaceholderInfo > placeholders = {
    { "titre", "Nom de l'antenne, défini dans les Réglages (bouton « Nom de l'antenne » de l'éditeur)", "PROTECTION CIVILE\nPARIS CENTRE" },
    { "today", "Date d'impression JJ/MM/AAAA", "29/09/2026" },
    { "printed_by", "Utilisateur connecté au moment de l'impression", "Jeanne Dupont" },
  };
  return placeholders;
}

Parameters example_parameters(TemplateCategory category) {
  Parameters parameters;
  for (const PlaceholderInfo &info : common_placeholders())
    parameters[info.name] = info.example;
  for (const PlaceholderInfo &info : category_info(category).placeholders)
    parameters[info.name] = info.example;
  return parameters;
}

namespace {
const std::string task_box = "\xE2\x96\xA1"; // □

template < typename Function > std::string map_lines(const std::string &text, Function line_function) {
  std::string result;
  std::size_t start = 0;
  while (true) {
    const std::size_t end = text.find('\n', start);
    result += line_function(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos)
      return result;
    result += '\n';
    start = end + 1;
  }
}
} // namespace

std::string task_lines(const std::string &tasks) {
  return map_lines(tasks, [](const std::string &line) {
    if (line.rfind(task_box, 0) != 0)
      return line;
    return line.substr(line.compare(task_box.size(), 1, " ") == 0 ? task_box.size() + 1 : task_box.size());
  });
}

std::string tasks_value(const std::string &lines) {
  // les lignes vides restent vides : Entree en fin de saisie ne doit pas afficher un carre seul
  return map_lines(lines, [](const std::string &line) {
    if (line.find_first_not_of(" \t\r") == std::string::npos || line.rfind(task_box, 0) == 0)
      return line;
    return task_box + " " + line;
  });
}

} // namespace qrprotec
