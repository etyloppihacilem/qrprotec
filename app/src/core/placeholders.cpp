/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          placeholders.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "placeholders.hpp"

namespace qrprotec {

namespace {

std::vector< PlaceholderInfo > item_type_placeholders() {
  return {
    { "type", "Code du type d'item (6 caractères)", "compre" },
    { "type_name", "Nom du type d'item", "Compresses stériles" },
    { "peremption", "Date de péremption JJ/MM/AAAA (vide si non périssable)", "31/12/2027" },
    { "peremption_iso", "Date de péremption AAAA-MM-JJ", "2027-12-31" },
    { "peremption_short", "Date de péremption MM/AAAA", "12/2027" },
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

  CategoryInfo item{ TemplateCategory::Item, "item", "Item",
                     "Étiquette collée sur chaque item. Le QR code doit contenir {{iid}}.", {} };
  item.placeholders.push_back({ "iid", "Identifiant unique de l'item (contenu du QR code)", "compre20271231000000A1" });
  for (const PlaceholderInfo &info : item_type_placeholders())
    item.placeholders.push_back(info);
  item.placeholders.push_back({ "index", "Numéro de l'étiquette dans la série imprimée", "3" });
  item.placeholders.push_back({ "count", "Nombre d'étiquettes de la série", "10" });
  item.placeholders.push_back({ "pack_id", "Paquet scellé d'origine (vide sinon)", "0000002B" });
  categories.push_back(item);

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

  CategoryInfo lot_private{ TemplateCategory::LotPrivate, "lot_private", "Lot - étiquette privée",
                            "Étiquette contenant la clé du lot (à garder à l'abri). Le QR code doit contenir "
                            "{{lot_private_url}}.",
                            lot_placeholders() };
  lot_private.placeholders.push_back({ "lot_key", "Clé de vérification du lot", "a1B2c3D4e5F6g7H8i9J0k1L2" });
  lot_private.placeholders.push_back(
    { "lot_private_url", "URL de vérif avec la clé (QR de l'étiquette privée)",
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

  CategoryInfo user{ TemplateCategory::User, "user", "Badge utilisateur",
                     "Badge de connexion d'un secouriste. Le QR code doit contenir {{badge_url}}.", {} };
  user.placeholders = {
    { "matricule", "Matricule", "M0042" },
    { "nom", "Nom", "Dupont" },
    { "prenom", "Prénom", "Jeanne" },
    { "full_name", "Prénom et nom", "Jeanne Dupont" },
    { "role", "Rôle (Secouriste / Gestion / Administrateur)", "Secouriste" },
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
    { "titre", "Titre défini dans les Réglages (bouton « Ajouter le titre » de l'éditeur)", "PROTECTION CIVILE\nPARIS CENTRE" },
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

} // namespace qrprotec
