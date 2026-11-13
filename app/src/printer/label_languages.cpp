#include "label_languages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace qrprotec::labels {

namespace {
void append(std::vector<std::uint8_t>& out, const std::string& text)
{
    out.insert(out.end(), text.begin(), text.end());
}

// millimetres avec au plus une decimale, point decimal quelle que soit la locale
std::string mm(double value)
{
    const long tenths = static_cast<long>(value * 10.0 + (value >= 0 ? 0.5 : -0.5));
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) text += "." + std::to_string(std::labs(tenths % 10));
    return text;
}
}

std::vector<std::uint8_t> pack_rows(const RasterImage& image, bool one_is_black)
{
    const int bytes_per_row = (image.width + 7) / 8;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes_per_row) * static_cast<std::size_t>(image.height),
                                  one_is_black ? 0x00 : 0xff);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.at(x, y) >= 128) continue;
            std::uint8_t& byte = out[static_cast<std::size_t>(y * bytes_per_row + x / 8)];
            const std::uint8_t mask = static_cast<std::uint8_t>(0x80U >> (x % 8));
            if (one_is_black) byte |= mask;
            else byte &= static_cast<std::uint8_t>(~mask);
        }
    }
    return out;
}

std::vector<std::uint8_t> encode_tspl(const RasterImage& image, const RawJob& job)
{
    const double ppm = job.dots_per_mm > 0 ? job.dots_per_mm : 8.0;
    const int bytes_per_row = (image.width + 7) / 8;
    std::vector<std::uint8_t> out;
    append(out, "SIZE " + mm(job.width_dots / ppm) + " mm," + mm(job.height_dots / ppm) + " mm\r\n");
    append(out, "GAP " + mm(std::max(0.0, job.gap_mm)) + " mm,0 mm\r\n");
    append(out, "DIRECTION 0,0\r\n");
    append(out, "REFERENCE 0,0\r\n");
    append(out, "DENSITY " + std::to_string(std::clamp(job.darkness, 0, 15)) + "\r\n");
    append(out, std::string("SET RIBBON ") + (job.ribbon ? "ON" : "OFF") + "\r\n");
    append(out, "SET TEAR ON\r\n");
    // SHIFT [x,] y : deplace l'impression par rapport a l'etiquette detectee (1 pouce au plus de chaque cote)
    const int inch = static_cast<int>(std::lround(25.4 * ppm));
    append(out, "SHIFT " + std::to_string(std::clamp(job.offset_x_dots, -inch, inch)) + "," +
                    std::to_string(std::clamp(job.offset_y_dots, -inch, inch)) + "\r\n");
    // OFFSET : avance en plus apres chaque etiquette (position d'arret pour la decoupe), reprise avant la suivante
    append(out, "OFFSET " + mm(std::clamp(job.tear_off_dots, -inch, inch) / ppm) + " mm\r\n");
    append(out, "CLS\r\n");
    append(out, "BITMAP 0,0," + std::to_string(bytes_per_row) + "," +
                    std::to_string(image.height) + ",0,");
    const std::vector<std::uint8_t> bits = pack_rows(image, false);
    out.insert(out.end(), bits.begin(), bits.end());
    append(out, "\r\n");
    append(out, "PRINT 1," + std::to_string(std::max(1, job.copies)) + "\r\n");
    return out;
}

namespace {
// ^LT et ~TA : 120 rangees de points au plus, dans un sens ou dans l'autre (guide ZPL)
constexpr int kZplMaxShiftRows = 120;

std::string zpl_signed(int value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%03d", value);
    return text;
}

// Marge de chaque cote de l'etiquette dans la zone d'impression : les imprimantes de bureau Zebra (GK420t)
// centrent la zone ^PW sur la tete, comme le rouleau ; une zone a la largeur exacte de l'etiquette ne laisse pas
// aller a gauche du 0 (« les objets s'arretent a la position 0 »). Zone elargie de cette marge de chaque cote,
// sans depasser la tete (104 mm), 15 mm au plus : l'image y est placee au milieu, decalage compris.
int zpl_side_margin(const RawJob& job)
{
    const double ppm = job.dots_per_mm > 0 ? job.dots_per_mm : 8.0;
    const int head = static_cast<int>(std::lround(104.0 * ppm));
    const int most = static_cast<int>(std::lround(15.0 * ppm));
    return std::clamp((head - job.width_dots) / 2, 0, most);
}

// Reglages de position, dans le format (avant le premier ^FS) :
// - horizontal : ^LHx (origine du format, « Label Home ») = marge + decalage, dans la zone elargie ; ^LS0 ;
// - vertical vers le bas : ^LH x,y. ^LT seul n'a aucun effet a l'impression sur une GK420t (firmware V61.17.17Z,
//   essai du 13/11/2026) bien qu'il soit enregistre, alors que ^LH deplace le dessin ;
// - vertical vers le haut : ^LT negatif (« Label Top »), seule commande prevue pour remonter le dessin (^LH ne
//   prend pas de valeur negative), borne a 120 points.
std::string zpl_position(const RawJob& job)
{
    const int side = zpl_side_margin(job);
    const int left = std::clamp(side + job.offset_x_dots, 0, 2 * side);
    const int down = std::max(0, job.offset_y_dots);
    const int up = std::clamp(std::min(0, job.offset_y_dots), -kZplMaxShiftRows, 0);
    return "^LH" + std::to_string(left) + "," + std::to_string(down) + "\n^LS0\n^LT" + std::to_string(up) + "\n";
}

// ~TA : position d'arret apres impression (barre de decoupe), commande immediate hors format
std::string zpl_tear_off(const RawJob& job)
{
    return "~TA" + zpl_signed(std::clamp(job.tear_off_dots, -kZplMaxShiftRows, kZplMaxShiftRows)) + "\n";
}

// Format de l'etiquette chargee : support, type de suivi, largeur d'impression, longueur, position
std::string zpl_media(const RawJob& job)
{
    return std::string("^MT") + (job.ribbon ? "T" : "D") + "\n^MNY\n^PW" + std::to_string(zpl_print_width(job)) +
           "\n^LL" + std::to_string(job.height_dots) + "\n" + zpl_position(job);
}
}

int zpl_print_width(const RawJob& job)
{
    return job.width_dots + 2 * zpl_side_margin(job);
}

std::vector<std::uint8_t> encode_zpl(const RasterImage& image, const RawJob& job)
{
    const int bytes_per_row = (image.width + 7) / 8;
    const std::vector<std::uint8_t> bits = pack_rows(image, true);
    std::vector<std::uint8_t> out;
    // ~SD : noirceur 0-30, commande immediate (avant ^XA)
    char darkness[8];
    std::snprintf(darkness, sizeof(darkness), "%02d", std::clamp(job.darkness * 2, 0, 30));
    append(out, std::string("~SD") + darkness + "\n");
    append(out, zpl_tear_off(job));
    append(out, "^XA\n");
    append(out, zpl_media(job));
    append(out, "^FO0,0");
    append(out, "^GFA," + std::to_string(bits.size()) + "," + std::to_string(bits.size()) + "," +
                    std::to_string(bytes_per_row) + ",");
    static const char* digits = "0123456789ABCDEF";
    for (std::size_t index = 0; index < bits.size(); ++index) {
        if (index > 0 && index % static_cast<std::size_t>(bytes_per_row) == 0) out.push_back('\n');
        out.push_back(static_cast<std::uint8_t>(digits[bits[index] >> 4]));
        out.push_back(static_cast<std::uint8_t>(digits[bits[index] & 0x0f]));
    }
    append(out, "^FS\n");
    append(out, "^PQ" + std::to_string(std::max(1, job.copies)) + "\n");
    append(out, "^XZ\n");
    return out;
}

std::string maintenance_command(bool zpl, Maintenance action, const RawJob& job)
{
    const double ppm = job.dots_per_mm > 0 ? job.dots_per_mm : 8.0;
    if (zpl) {
        switch (action) {
        case Maintenance::Calibrate:
            // format de l'etiquette chargee (sans rien a imprimer), ~JC : mesure de la longueur d'etiquette et
            // reglage des capteurs (l'imprimante avance quelques etiquettes), ^JUS : tout est enregistre
            return zpl_tear_off(job) + "^XA\n" + zpl_media(job) + "^XZ\n~JC\n^XA^JUS^XZ\n";
        case Maintenance::Config: return "~WC\n";
        case Maintenance::Feed: return "~PH\n";
        // ^JUF : reglages d'usine (le reseau n'est pas touche), ^JUS : enregistres
        case Maintenance::Factory: return "^XA^JUF^XZ\n^XA^JUS^XZ\n";
        case Maintenance::Diagnostic: {
            // trois etiquettes au format charge, dessinees par l'imprimante (^GB, ^A0) : on voit directement si
            // ^LH (origine), ^LS et ^LT deplacent le dessin ; 5 mm dans chaque cas
            const int shift = static_cast<int>(std::lround(5.0 * ppm));
            const int border = std::max(2, static_cast<int>(std::lround(0.5 * ppm)));
            const int font = std::max(16, static_cast<int>(std::lround(3.0 * ppm)));
            const auto label = [&](const std::string& position, const std::string& first, const std::string& second) {
                return "^XA^CI28^MNY^PW" + std::to_string(job.width_dots) + "^LL" + std::to_string(job.height_dots) +
                       position + "^FO0,0^GB" + std::to_string(job.width_dots) + "," +
                       std::to_string(job.height_dots) + "," + std::to_string(border) + "^FS^FO" +
                       std::to_string(2 * border + font / 2) + "," + std::to_string(2 * border + font / 2) + "^A0N," +
                       std::to_string(font) + "," + std::to_string(font) + "^FD" + first + "^FS^FO" +
                       std::to_string(2 * border + font / 2) + "," + std::to_string(2 * border + font * 2) + "^A0N," +
                       std::to_string(font) + "," + std::to_string(font) + "^FD" + second + "^FS^XZ\n";
            };
            const std::string s = std::to_string(shift);
            // 4 = 3 renvoyee telle quelle : montre si ^LT ne prend effet qu'a l'etiquette suivante
            return label("^LH0,0^LS0^LT0", "1 REFERENCE", "cadre au bord") +
                   label("^LH" + s + "," + s + "^LS0^LT0", "2 ^LH" + s + "," + s, "5 mm droite+bas ?") +
                   label("^LH0,0^LS-" + s + "^LT" + s, "3 ^LS-" + s + " ^LT" + s, "5 mm droite+bas ?") +
                   label("^LH0,0^LS-" + s + "^LT" + s, "4 = 3 une 2e fois", "5 mm droite+bas ?") +
                   "^XA^LS0^LT0^XZ\n";
        }
        }
        return {};
    }
    switch (action) {
    case Maintenance::Calibrate:
        return "SIZE " + mm(job.width_dots / ppm) + " mm," + mm(job.height_dots / ppm) + " mm\r\nGAP " +
               mm(std::max(0.0, job.gap_mm)) + " mm,0 mm\r\nSET RIBBON " + (job.ribbon ? "ON" : "OFF") +
               "\r\nGAPDETECT\r\n";
    case Maintenance::Config: return "SELFTEST\r\n";
    case Maintenance::Feed: return "FORMFEED\r\n";
    case Maintenance::Factory: return "INITIALPRINTER\r\n";
    case Maintenance::Diagnostic: return {}; // ZPL seulement
    }
    return {};
}

std::string zpl_send_settings_and_read(const RawJob& job)
{
    char darkness[8];
    std::snprintf(darkness, sizeof(darkness), "%02d", std::clamp(job.darkness * 2, 0, 30));
    return std::string("~SD") + darkness + "\n" + zpl_tear_off(job) + "^XA\n" + zpl_media(job) + "^XZ\n^XA^HH^XZ\n";
}

std::vector<ConfigEntry> parse_zpl_config(const std::string& reply)
{
    // derniere reponse <STX>...<ETX> seulement (une reponse plus ancienne a pu rester en attente)
    std::string text = reply;
    const std::size_t end = text.rfind('\x03');
    if (end != std::string::npos) {
        const std::size_t start = text.rfind('\x02', end);
        text = text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
    }
    const auto trim = [](std::string value) {
        const std::size_t first = value.find_first_not_of(" \t");
        const std::size_t last = value.find_last_not_of(" \t");
        return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
    };
    std::vector<ConfigEntry> entries;
    std::size_t position = 0;
    while (position <= text.size()) {
        std::size_t next = text.find_first_of("\r\n", position);
        if (next == std::string::npos) next = text.size();
        const std::string line = trim(text.substr(position, next - position));
        position = next + 1;
        if (line.empty()) continue;
        // « valeur      NOM » : colonnes separees par au moins deux espaces (la derniere separation, la valeur
        // peut en contenir : « 39.0IN   989MM ») ; « 2104k......R: RAM » : separees par des points
        ConfigEntry entry;
        const std::size_t gap = line.rfind("  ");
        const std::size_t dots = line.find("..");
        if (gap != std::string::npos) {
            entry.value = trim(line.substr(0, gap));
            entry.name = trim(line.substr(gap));
        } else if (dots != std::string::npos) {
            const std::size_t name = line.find_first_not_of('.', dots);
            entry.value = trim(line.substr(0, dots));
            if (name != std::string::npos) entry.name = trim(line.substr(name));
        }
        if (!entry.name.empty() && !entry.value.empty()) entries.push_back(entry);
    }
    return entries;
}

std::string config_value(const std::vector<ConfigEntry>& entries, const std::string& name)
{
    for (const ConfigEntry& entry : entries)
        if (entry.name.rfind(name, 0) == 0) return entry.value;
    return {};
}

std::vector<std::uint8_t> status_request(bool zpl)
{
    if (zpl) return {'~', 'H', 'S', '\n'};
    return {0x1b, '!', '?'};
}

namespace {
// champs separes par des virgules d'une chaine de ~HS (entre STX et ETX)
std::vector<std::string> split_fields(const std::string& text)
{
    std::vector<std::string> fields(1);
    for (const char c : text) {
        if (c == ',') fields.emplace_back();
        else fields.back() += c;
    }
    return fields;
}

bool flag(const std::vector<std::string>& fields, std::size_t index)
{
    return index < fields.size() && fields[index] == "1";
}
}

PrinterState parse_status(bool zpl, const std::vector<std::uint8_t>& reply)
{
    PrinterState state;
    if (!zpl) {
        // <ESC>!? : un octet, chaque bit un etat (0 = pret)
        if (reply.empty()) return state;
        const std::uint8_t bits = reply.back();
        state.known = true;
        state.head_open = (bits & 0x01) != 0;
        state.paper_jam = (bits & 0x02) != 0;
        state.paper_out = (bits & 0x04) != 0;
        state.ribbon_out = (bits & 0x08) != 0;
        state.paused = (bits & 0x10) != 0;
        state.printing = (bits & 0x20) != 0;
        state.other_error = (bits & 0x80) != 0;
        return state;
    }
    // ~HS : trois chaines <STX>...<ETX><CR><LF>
    std::vector<std::string> strings;
    std::string current;
    bool inside = false;
    for (const std::uint8_t byte : reply) {
        if (byte == 0x02) {
            inside = true;
            current.clear();
        } else if (byte == 0x03 && inside) {
            strings.push_back(current);
            inside = false;
        } else if (inside) {
            current += static_cast<char>(byte);
        }
    }
    if (strings.size() < 2) return state;
    const std::vector<std::string> first = split_fields(strings[0]);
    const std::vector<std::string> second = split_fields(strings[1]);
    if (first.size() < 12 || second.size() < 11) return state;
    state.known = true;
    state.paper_out = flag(first, 1);
    state.paused = flag(first, 2);
    state.buffer_full = flag(first, 5);
    state.under_temperature = flag(first, 10);
    state.over_temperature = flag(first, 11);
    state.head_open = flag(second, 2);
    state.ribbon_out = flag(second, 3);
    try {
        state.formats_waiting = std::stoi(first[4]);
        state.labels_remaining = std::stoi(second[8]);
    } catch (...) {
    }
    return state;
}

bool PrinterState::blocking() const
{
    return known && (head_open || paper_out || paper_jam || ribbon_out || paused || over_temperature ||
                     under_temperature || other_error);
}

std::string PrinterState::description() const
{
    if (!known) return "état inconnu (l'imprimante ne répond pas)";
    std::vector<std::string> parts;
    if (head_open) parts.emplace_back("tête ouverte (refermer le capot jusqu'au clic)");
    if (paper_out) parts.emplace_back("plus d'étiquettes");
    if (paper_jam) parts.emplace_back("bourrage");
    if (ribbon_out) parts.emplace_back("plus de ruban (ou ruban coché sans ruban chargé)");
    if (paused) parts.emplace_back("en pause (appuyer une fois sur le bouton de l'imprimante)");
    if (over_temperature) parts.emplace_back("tête trop chaude");
    if (under_temperature) parts.emplace_back("tête trop froide");
    if (other_error) parts.emplace_back("erreur de l'imprimante");
    if (parts.empty()) return printing || formats_waiting > 0 || labels_remaining > 0 ? "prête, impression en cours" : "prête";
    std::string text;
    for (const std::string& part : parts) text += (text.empty() ? "" : ", ") + part;
    return text;
}

} // namespace qrprotec::labels
