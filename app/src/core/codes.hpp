/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          codes.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace qrprotec {

// Date calendaire simple (sans fuseau), comparable.
struct Date {
    int year  = 0;
    int month = 0;
    int day   = 0;

    static Date                today();
    static std::optional< Date > parse(const std::string &text); // AAAA-MM-JJ, JJ/MM/AAAA ou AAAAMMJJ
    bool                       valid() const;
    std::string                iso() const;     // AAAA-MM-JJ
    std::string                display() const; // JJ/MM/AAAA
    std::string                weekday() const; // lundi, mardi...
    Date                       plus_days(int days) const;

    bool operator<(const Date &other) const;
    bool operator==(const Date &other) const;
    bool operator<=(const Date &other) const { return *this < other || *this == other; }
};

// Horodatage d'une action passee (verif, signalement...) : « aujourd'hui à HH:MM » / « hier à HH:MM », sinon
// « JJ/MM/AAAA à HH:MM » ; long_form ajoute le jour de la semaine devant une date ecrite en entier, on ajoute
// « le » devant (« scellé le 12/10/2026 à 09:00 » mais « scellé aujourd'hui à 09:00 »). Jamais pour les peremptions.
std::string relative_datetime(const std::string &timestamp, const Date &today, bool long_form = false,
                              bool on = false);

// Date saisie librement au clavier (peremption...). Separateurs / - . espace, ou aucun :
//   02/09/2026, 2/9/26, 02092026, 020926    -> 02/09/2026
//   09/2026, 9/26, 092026, 0926             -> 30/09/2026 (dernier jour du mois)
//   2026-09-02, 20260902                    -> 02/09/2026
// Les annees sur 2 chiffres sont en 20xx.
std::optional< Date > parse_user_date(const std::string &text);

enum class ScanKind { Item, Lot, User, SealedPack, LotSeal, LotSealOpen, Checklist, PosteAction, Unknown };

// Contenu d'un QR code interprete localement (sans appel reseau).
//   item      : iid seul (TYPE 6 + AAAAMMJJ + compteur base62 8), complet ou en forme courte (compact_iid)
//   taches    : <base>/task?id=IID (etiquette de taches : item dont les taches sont a cocher a la verif)
//   lot       : <base>/verif?lot=ID[&key=CLE]
//   user      : <base>/badge?m=MATRICULE&key=CLE, ou le matricule seul (sans cle : PIN obligatoire) ;
//               <base>/badge?m=MATRICULE&t=JETON : QR code de connexion temporaire (depannage du TECH, ou montre par
//               un secouriste connecte au web avec sa passkey), envoye a /api/auth/temp/
//   sealed    : <base>/pack?id=ID ; <base>/pack?id=ID&o=CLE : etiquette de paquet ouvert (paquet pre-etiquete,
//               contenu deja etiquete : elle compte en verif les items qu'elle couvre tant qu'elle est valable)
//   lot seal  : <base>/seal?lot=ID&s=CODE (lot scelle : valide sans verif)
//   seal open : <base>/unseal?lot=ID&c=CODE (etiquette d'ouverture, dans le lot : son scan ouvre le scelle)
//   checklist : <base>/liste?id=ID (liste de materiel partagee par QR code)
//   action    : qrprotec:poste/ACTION?param=valeur&... (code d'action rapide des postes, voir poste_action_code)
// La base (https://example.com/ par defaut) n'est pas verifiee : changer de domaine ne casse pas
// les etiquettes deja imprimees.
struct ParsedScan {
    ScanKind              kind = ScanKind::Unknown;
    std::string           raw;
    std::string           id;  // iid, id de lot, matricule, id de paquet ou de liste
    std::string           key; // cle du lot (etiquette privee), du badge, code du scelle (ou de son etiquette
                               // d'ouverture), ou cle de l'etiquette de paquet ouvert
    std::string           temp; // badge : jeton du QR code de connexion temporaire (badge?m=...&t=...)
    std::string           item_type;
    std::optional< Date > peremption; // items dates uniquement
    bool                  task = false; // item scanne par son etiquette de taches (TaskItem)
    std::map< std::string, std::string > params; // code d'action : parametres (id = nom de l'action)
    std::string           foreign; // etiquette d'une autre antenne : son code (kind Unknown, rien a faire ici)
};

// Codes d'action rapide des postes : « qrprotec:poste/ACTION?param=valeur&... ». Pas une URL web : le telephone
// (front web, appareil photo) les ignore ; seuls les postes les executent, scannes a la douchette (ou au telephone
// utilise comme douchette du poste). Chaque action est declaree dans app/poste_actions.cpp (nom, droits, effet) ;
// une action inconnue (poste pas a jour) est signalee sans rien faire. Actions :
//   rouleau?t=NOM&w=LARGEUR&h=HAUTEUR&s=direct|transfer : rouleau d'etiquettes en place dans l'imprimante
// Code PIN (meme regle que le serveur, models.PIN_RE / NEW_PIN_RE) : 6 chiffres au moins pour un nouveau code, pas de
// vraie limite haute (64) ; a la connexion, les anciens codes de 4 ou 5 chiffres restent valables
inline constexpr std::size_t kPinLoginMinLength = 4;
inline constexpr std::size_t kPinMinLength      = 6;
inline constexpr std::size_t kPinMaxLength      = 64;
inline bool                  new_pin_valid(const std::string &pin) {
  return pin.size() >= kPinMinLength && pin.size() <= kPinMaxLength;
}
inline bool login_pin_valid(const std::string &pin) {
  return pin.size() >= kPinLoginMinLength && pin.size() <= kPinMaxLength;
}

inline constexpr const char *kPosteActionPrefix = "qrprotec:poste/";
std::string poste_action_code(const std::string &action, const std::vector< std::pair< std::string, std::string > > &params);

constexpr std::size_t kTypeLength    = 6;
constexpr std::size_t kDateLength    = 8;
constexpr std::size_t kCounterLength = 8;
constexpr std::size_t kIidLength     = kTypeLength + kDateLength + kCounterLength;

bool        is_iid(const std::string &code);
// Forme courte d'un iid, plus courte a imprimer (code-barres, petit QR) et a taper : date « 00000000 » (sans
// peremption) ecrite D, zeros de tete du compteur remplaces par N quand cela en retire au moins deux.
//   serphy20261029N7 = serphy2026102900000007, lecglyDN6 = lecgly0000000000000006
// La base garde l'iid complet ; chaque front deplie la forme courte (meme regle que expandIid de web/app/state.js et
// scanner.js). Sans ambiguite : un iid complet a 22 caracteres et 8 chiffres de date.
std::string compact_iid(const std::string &iid);
// Majuscules d'un texte UTF-8, lettres accentuees du francais comprises (« pièce » → « PIÈCE ») : placeholders
// d'etiquette ecrits en majuscules ({{RANGEMENT}})
std::string utf8_upper(const std::string &text);
std::string utf8_capitalize(const std::string &text); // premiere lettre seulement (« armoire » → « Armoire »)
// iid complet d'un code d'item de cette antenne (avec ou sans son code, complet ou en forme courte), vide sinon ; item
// d'une autre antenne : vide, et son code dans *foreign
std::string expand_iid(const std::string &code, std::string *foreign = nullptr);

// Code de l'antenne (2 lettres majuscules ou chiffres, ex : PC, 12) devant l'iid et l'id du lot ou du paquet dans
// chaque code imprime : deux antennes peuvent avoir les memes types, dates et compteurs. Donne par le serveur
// (/api/health/ code_prefix), garde dans les reglages du poste pour lire les etiquettes hors connexion. A la lecture :
// notre code est retire, un code sans prefixe (etiquette imprimee avant) est a nous, celui d'une autre antenne est
// refuse. Meme regle que read_iid / read_id de models.py et readIid / readId de web/app/state.js.
constexpr std::size_t kCodePrefixLength = 2;
constexpr std::size_t kLotIdLength      = kTypeLength + kCounterLength; // type de lot + compteur base62
constexpr std::size_t kPackIdLength     = kCounterLength;               // compteur base62 des paquets fermes
void               set_code_prefix(const std::string &prefix); // invalide (pas 2 majuscules ou chiffres) : aucun
const std::string &code_prefix();
std::string        item_code(const std::string &iid); // code imprime : code de l'antenne + forme courte
// id local d'un id de lot (kLotIdLength) ou de paquet (kPackIdLength) lu dans un code ; celui d'une autre antenne :
// vide, et son code dans *foreign. Un id d'une autre longueur est rendu tel quel (ancienne etiquette).
std::string read_id(const std::string &code, std::size_t length, std::string *foreign = nullptr);
ParsedScan  parse_scan(const std::string &raw);
const char *scan_kind_name(ScanKind kind);
// Libelle court d'un code scanne, pour l'affichage : jamais de cle (badge, etiquette privee, scelle, session),
// pas d'URL complete. Meme rendu que describe dans database/inventory/web/scanner.js.
std::string describe_scan(const ParsedScan &scan);
bool        is_expired(const ParsedScan &scan, const Date &today);

// Easter egg : un code contenant "douchette" (sans casse) n'est pas un scan, une phrase s'affiche.
// Meme liste que PHRASES dans database/inventory/web/douchette.js.
bool                              is_douchette(const std::string &code);
const std::vector< std::string > &douchette_phrases();

std::map< std::string, std::string > parse_query(const std::string &query);
std::string                          url_decode(const std::string &value);

} // namespace qrprotec
