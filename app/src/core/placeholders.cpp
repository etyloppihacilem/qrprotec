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
    { "type", "Code du type d'item (6 caracteres)", "compre" },
    { "type_name", "Nom du type d'item", "Compresses steriles" },
    { "peremption", "Date de peremption JJ/MM/AAAA (vide si non perissable)", "31/12/2027" },
    { "peremption_iso", "Date de peremption AAAA-MM-JJ", "2027-12-31" },
    { "peremption_short", "Date de peremption MM/AAAA", "12/2027" },
  };
}

std::vector< PlaceholderInfo > lot_placeholders() {
  return {
    { "lot_id", "Identifiant du lot", "sacpse00000001" },
    { "lot_name", "Nom du lot", "Sac PSE1 n°1" },
    { "lot_short", "Nom court du lot", "PSE1-1" },
    { "lot_type", "Code du type de lot", "sacpse" },
    { "lot_type_name", "Nom du type de lot", "Sac PSE1" },
    { "lot_url", "URL publique de verif (QR de l'etiquette publique)", "https://example.com/verif?lot=sacpse00000001" },
  };
}

std::vector< CategoryInfo > build_categories() {
  std::vector< CategoryInfo > categories;
  categories.push_back({ TemplateCategory::Generic, "generic", "Generique", "Modele libre, sans donnees de la base.", {} });

  CategoryInfo item{ TemplateCategory::Item, "item", "Item",
                     "Etiquette collee sur chaque item. Le QR code doit contenir {{iid}}.", {} };
  item.placeholders.push_back({ "iid", "Identifiant unique de l'item (contenu du QR code)", "compre20271231000000A1" });
  for (const PlaceholderInfo &info : item_type_placeholders())
    item.placeholders.push_back(info);
  item.placeholders.push_back({ "index", "Numero de l'etiquette dans la serie imprimee", "3" });
  item.placeholders.push_back({ "count", "Nombre d'etiquettes de la serie", "10" });
  item.placeholders.push_back({ "pack_id", "Paquet scelle d'origine (vide sinon)", "0000002B" });
  categories.push_back(item);

  CategoryInfo pack{ TemplateCategory::ItemPack, "itempack", "Paquet ferme (ItemPack)",
                     "Etiquette d'un paquet ferme : les etiquettes individuelles seront imprimees a l'ouverture. "
                     "Le QR code doit contenir {{pack_url}}.",
                     {} };
  pack.placeholders.push_back({ "pack_id", "Identifiant du paquet", "0000002B" });
  pack.placeholders.push_back({ "pack_url", "URL du paquet (contenu du QR code)", "https://example.com/pack?id=0000002B" });
  pack.placeholders.push_back({ "count", "Nombre d'items dans le paquet", "25" });
  for (const PlaceholderInfo &info : item_type_placeholders())
    pack.placeholders.push_back(info);
  categories.push_back(pack);

  CategoryInfo lot_public{ TemplateCategory::LotPublic, "lot_public", "Lot - etiquette publique",
                           "Etiquette visible du lot. Le QR code doit contenir {{lot_url}}.", lot_placeholders() };
  categories.push_back(lot_public);

  CategoryInfo lot_private{ TemplateCategory::LotPrivate, "lot_private", "Lot - etiquette privee",
                            "Etiquette contenant la cle du lot (a garder a l'abri). Le QR code doit contenir "
                            "{{lot_private_url}}.",
                            lot_placeholders() };
  lot_private.placeholders.push_back({ "lot_key", "Cle de verification du lot", "a1B2c3D4e5F6g7H8i9J0k1L2" });
  lot_private.placeholders.push_back(
    { "lot_private_url", "URL de verif avec la cle (QR de l'etiquette privee)",
      "https://example.com/verif?lot=sacpse00000001&key=a1B2c3D4e5F6g7H8i9J0k1L2" }
  );
  lot_private.placeholders.push_back({ "key_expires", "Date d'expiration de la cle JJ/MM/AAAA", "29/09/2036" });
  categories.push_back(lot_private);

  CategoryInfo user{ TemplateCategory::User, "user", "Badge utilisateur",
                     "Badge de connexion d'un secouriste. Le QR code doit contenir {{badge_url}}.", {} };
  user.placeholders = {
    { "matricule", "Matricule", "M0042" },
    { "nom", "Nom", "Dupont" },
    { "prenom", "Prenom", "Jeanne" },
    { "full_name", "Prenom et nom", "Jeanne Dupont" },
    { "role", "Role (Secouriste / Responsable)", "Secouriste" },
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
    { "today", "Date d'impression JJ/MM/AAAA", "29/09/2026" },
    { "printed_by", "Utilisateur connecte au moment de l'impression", "Jeanne Dupont" },
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
