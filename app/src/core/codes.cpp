/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          codes.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "codes.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <vector>

namespace qrprotec {

namespace {

bool all_digits(const std::string &value) {
  for (const char character : value)
    if (!std::isdigit(static_cast< unsigned char >(character)))
      return false;
  return !value.empty();
}

bool is_leap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
  static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (month == 2 && is_leap(year))
    return 29;
  return days[month - 1];
}

std::string trim(const std::string &value) {
  std::size_t start = 0;
  std::size_t end   = value.size();
  while (start < end && std::isspace(static_cast< unsigned char >(value[start])))
    ++start;
  while (end > start && std::isspace(static_cast< unsigned char >(value[end - 1])))
    --end;
  return value.substr(start, end - start);
}

} // namespace

Date Date::today() {
  const std::time_t now = std::time(nullptr);
  std::tm           local{};
  localtime_r(&now, &local);
  return { local.tm_year + 1900, local.tm_mon + 1, local.tm_mday };
}

bool Date::valid() const {
  return year >= 1900 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

std::optional< Date > Date::parse(const std::string &input) {
  const std::string text = trim(input);
  Date              date;
  if (text.size() == 8 && all_digits(text)) {
    date = { std::stoi(text.substr(0, 4)), std::stoi(text.substr(4, 2)), std::stoi(text.substr(6, 2)) };
  } else if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
    const std::string y = text.substr(0, 4), m = text.substr(5, 2), d = text.substr(8, 2);
    if (!all_digits(y) || !all_digits(m) || !all_digits(d))
      return std::nullopt;
    date = { std::stoi(y), std::stoi(m), std::stoi(d) };
  } else if (text.size() == 10 && text[2] == '/' && text[5] == '/') {
    const std::string d = text.substr(0, 2), m = text.substr(3, 2), y = text.substr(6, 4);
    if (!all_digits(y) || !all_digits(m) || !all_digits(d))
      return std::nullopt;
    date = { std::stoi(y), std::stoi(m), std::stoi(d) };
  } else if (text.size() >= 10 && text[4] == '-' && text[7] == '-') {
    // horodatage ISO complet : on ne garde que la date
    return parse(text.substr(0, 10));
  } else {
    return std::nullopt;
  }
  if (!date.valid())
    return std::nullopt;
  return date;
}

std::string Date::iso() const {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  return buffer;
}

std::string Date::display() const {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d", day, month, year);
  return buffer;
}

Date Date::plus_days(int days) const {
  Date result = *this;
  while (days > 0) {
    ++result.day;
    if (result.day > days_in_month(result.year, result.month)) {
      result.day = 1;
      if (++result.month > 12) {
        result.month = 1;
        ++result.year;
      }
    }
    --days;
  }
  while (days < 0) {
    if (--result.day < 1) {
      if (--result.month < 1) {
        result.month = 12;
        --result.year;
      }
      result.day = days_in_month(result.year, result.month);
    }
    ++days;
  }
  return result;
}

bool Date::operator<(const Date &other) const {
  if (year != other.year)
    return year < other.year;
  if (month != other.month)
    return month < other.month;
  return day < other.day;
}

bool Date::operator==(const Date &other) const {
  return year == other.year && month == other.month && day == other.day;
}

namespace {

std::optional< Date > make_date(int year, int month, int day) {
  if (year < 100)
    year += 2000;
  const Date date{ year, month, day };
  if (!date.valid() || year < 2000 || year > 2199)
    return std::nullopt;
  return date;
}

std::optional< Date > end_of_month(int year, int month) {
  if (year < 100)
    year += 2000;
  if (month < 1 || month > 12 || year < 2000 || year > 2199)
    return std::nullopt;
  return Date{ year, month, days_in_month(year, month) };
}

} // namespace

std::optional< Date > parse_user_date(const std::string &input) {
  // decoupe en groupes de chiffres
  std::vector< std::string > groups;
  std::string                current;
  for (const char character : trim(input)) {
    if (std::isdigit(static_cast< unsigned char >(character))) {
      current += character;
    } else if (character == '/' || character == '-' || character == '.' || character == ' ') {
      if (!current.empty())
        groups.push_back(current);
      current.clear();
    } else {
      return std::nullopt;
    }
  }
  if (!current.empty())
    groups.push_back(current);
  const auto number = [](const std::string &text) { return std::stoi(text); };

  if (groups.size() == 3) {
    if (groups[0].size() == 4) // 2026-09-02
      return make_date(number(groups[0]), number(groups[1]), number(groups[2]));
    if (groups[0].size() <= 2 && groups[1].size() <= 2 && (groups[2].size() == 2 || groups[2].size() == 4))
      return make_date(number(groups[2]), number(groups[1]), number(groups[0]));
    return std::nullopt;
  }
  if (groups.size() == 2) {
    if (groups[0].size() <= 2 && (groups[1].size() == 2 || groups[1].size() == 4)) // 09/2026, 9/26
      return end_of_month(number(groups[1]), number(groups[0]));
    if (groups[0].size() == 4 && groups[1].size() <= 2) // 2026-09
      return end_of_month(number(groups[0]), number(groups[1]));
    return std::nullopt;
  }
  if (groups.size() != 1)
    return std::nullopt;
  const std::string &digits = groups[0];
  switch (digits.size()) {
    case 8: { // JJMMAAAA, sinon AAAAMMJJ
      if (auto date = make_date(number(digits.substr(4, 4)), number(digits.substr(2, 2)), number(digits.substr(0, 2))))
        return date;
      return make_date(number(digits.substr(0, 4)), number(digits.substr(4, 2)), number(digits.substr(6, 2)));
    }
    case 6: { // MMAAAA (09 2026), sinon JJMMAA (02 09 26)
      if (digits.compare(2, 2, "20") == 0 || digits.compare(2, 2, "21") == 0)
        if (auto date = end_of_month(number(digits.substr(2, 4)), number(digits.substr(0, 2))))
          return date;
      return make_date(number(digits.substr(4, 2)), number(digits.substr(2, 2)), number(digits.substr(0, 2)));
    }
    case 4: // MMAA
      return end_of_month(number(digits.substr(2, 2)), number(digits.substr(0, 2)));
    default: return std::nullopt;
  }
}

bool is_iid(const std::string &code) {
  if (code.size() != kIidLength)
    return false;
  for (const char character : code)
    if (!std::isalnum(static_cast< unsigned char >(character)))
      return false;
  return all_digits(code.substr(kTypeLength, kDateLength));
}

std::string url_decode(const std::string &value) {
  std::string output;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '+') {
      output += ' ';
    } else if (character == '%' && index + 2 < value.size() && std::isxdigit(static_cast< unsigned char >(value[index + 1]))
               && std::isxdigit(static_cast< unsigned char >(value[index + 2]))) {
      output += static_cast< char >(std::stoi(value.substr(index + 1, 2), nullptr, 16));
      index += 2;
    } else {
      output += character;
    }
  }
  return output;
}

std::map< std::string, std::string > parse_query(const std::string &query) {
  std::map< std::string, std::string > params;
  std::size_t                          start = 0;
  while (start <= query.size()) {
    std::size_t end = query.find('&', start);
    if (end == std::string::npos)
      end = query.size();
    const std::string pair = query.substr(start, end - start);
    if (!pair.empty()) {
      const std::size_t equal = pair.find('=');
      if (equal == std::string::npos)
        params[url_decode(pair)] = "";
      else
        params[url_decode(pair.substr(0, equal))] = url_decode(pair.substr(equal + 1));
    }
    start = end + 1;
  }
  return params;
}

bool is_douchette(const std::string &code) {
  std::string lower(code);
  for (char &c : lower)
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  return lower.find("douchette") != std::string::npos;
}

const std::vector< std::string > &douchette_phrases() {
  static const std::vector< std::string > phrases = {
      "Pourquoi, au nom du Graal, vous avez scanné la douchette ?",
      "C'est pas faux. Enfin si : ça, c'est la douchette.",
      "Scanner la douchette avec la douchette ? On en a gros !",
      "Faut pas respirer la compote, ça fait tousser. Et faut pas scanner la douchette.",
      "Le gras, c'est la vie. La douchette, c'est pas un produit.",
      "Douchette ! Ça vaut combien au cul de chouette ? Zéro.",
      "Merlin, c'est encore vous qui avez scanné la douchette ?",
      "Sire, on a un problème : quelqu'un a scanné la douchette.",
      "Vous avez scanné la douchette. La douchette vous scanne en retour.",
      "Inventaire : 1 douchette, état : très perplexe.",
      "La douchette, c'est pour scanner. C'est pas elle qu'on scanne.",
      "Et après, on scanne le lecteur de badge ?",
  };
  return phrases;
}

ParsedScan parse_scan(const std::string &input) {
  ParsedScan scan;
  scan.raw               = trim(input);
  const std::string code = scan.raw;
  if (is_iid(code)) {
    scan.kind      = ScanKind::Item;
    scan.id        = code;
    scan.item_type = code.substr(0, kTypeLength);
    const std::string date = code.substr(kTypeLength, kDateLength);
    if (date != "00000000")
      scan.peremption = Date::parse(date);
    return scan;
  }
  const std::size_t question = code.find('?');
  if (code.find("://") == std::string::npos || question == std::string::npos)
    return scan;
  std::string path = code.substr(0, question);
  while (!path.empty() && path.back() == '/')
    path.pop_back();
  const std::string route  = path.substr(path.find_last_of('/') + 1);
  auto              params = parse_query(code.substr(question + 1));
  if (route == "verif" && !params["lot"].empty()) {
    scan.kind = ScanKind::Lot;
    scan.id   = params["lot"];
    scan.key  = params["key"];
  } else if (route == "badge" && !params["m"].empty()) {
    scan.kind = ScanKind::User;
    scan.id   = params["m"];
    scan.key  = params["key"];
  } else if (route == "seal" && !params["lot"].empty() && !params["s"].empty()) {
    scan.kind = ScanKind::LotSeal;
    scan.id   = params["lot"];
    scan.key  = params["s"];
  } else if (route == "unseal" && !params["lot"].empty() && !params["c"].empty()) {
    scan.kind = ScanKind::LotSealOpen;
    scan.id   = params["lot"];
    scan.key  = params["c"];
  } else if (route == "pack" && !params["id"].empty()) {
    scan.kind = ScanKind::SealedPack;
    scan.id   = params["id"];
  }
  return scan;
}

const char *scan_kind_name(ScanKind kind) {
  switch (kind) {
    case ScanKind::Item: return "Item";
    case ScanKind::Lot: return "Lot";
    case ScanKind::User: return "Badge";
    case ScanKind::SealedPack: return "Paquet";
    case ScanKind::LotSeal: return "Scellé";
    case ScanKind::LotSealOpen: return "Ouverture de scellé";
    case ScanKind::Unknown: break;
  }
  return "Inconnu";
}

bool is_expired(const ParsedScan &scan, const Date &today) {
  return scan.kind == ScanKind::Item && scan.peremption && *scan.peremption < today;
}

} // namespace qrprotec
