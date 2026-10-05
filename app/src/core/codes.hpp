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
    Date                       plus_days(int days) const;

    bool operator<(const Date &other) const;
    bool operator==(const Date &other) const;
    bool operator<=(const Date &other) const { return *this < other || *this == other; }
};

// Date saisie librement au clavier (peremption...). Separateurs / - . espace, ou aucun :
//   02/09/2026, 2/9/26, 02092026, 020926    -> 02/09/2026
//   09/2026, 9/26, 092026, 0926             -> 30/09/2026 (dernier jour du mois)
//   2026-09-02, 20260902                    -> 02/09/2026
// Les annees sur 2 chiffres sont en 20xx.
std::optional< Date > parse_user_date(const std::string &text);

enum class ScanKind { Item, Lot, User, SealedPack, LotSeal, LotSealOpen, Unknown };

// Contenu d'un QR code interprete localement (sans appel reseau).
//   item      : iid seul (TYPE 6 + AAAAMMJJ + compteur base62 8)
//   lot       : <base>/verif?lot=ID[&key=CLE]
//   user      : <base>/badge?m=MATRICULE&key=CLE
//   sealed    : <base>/pack?id=ID
//   lot seal  : <base>/seal?lot=ID&s=CODE (lot scelle : valide sans verif)
//   seal open : <base>/unseal?lot=ID&c=CODE (etiquette d'ouverture, dans le lot : son scan ouvre le scelle)
// La base (https://example.com/ par defaut) n'est pas verifiee : changer de domaine ne casse pas
// les etiquettes deja imprimees.
struct ParsedScan {
    ScanKind              kind = ScanKind::Unknown;
    std::string           raw;
    std::string           id;  // iid, id de lot, matricule ou id de paquet
    std::string           key; // cle du lot (etiquette privee), du badge ou code du scelle (ou de son etiquette d'ouverture)
    std::string           item_type;
    std::optional< Date > peremption; // items dates uniquement
};

constexpr std::size_t kTypeLength    = 6;
constexpr std::size_t kDateLength    = 8;
constexpr std::size_t kCounterLength = 8;
constexpr std::size_t kIidLength     = kTypeLength + kDateLength + kCounterLength;

bool        is_iid(const std::string &code);
ParsedScan  parse_scan(const std::string &raw);
const char *scan_kind_name(ScanKind kind);
bool        is_expired(const ParsedScan &scan, const Date &today);

std::map< std::string, std::string > parse_query(const std::string &query);
std::string                          url_decode(const std::string &value);

} // namespace qrprotec
