#include "label_languages.hpp"

#include <algorithm>
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
    // decalage negatif (impression trop a droite / trop bas) : SHIFT, les positifs passent par la position du BITMAP
    if (job.offset_x_dots < 0 || job.offset_y_dots < 0)
        append(out, "SHIFT " + std::to_string(std::min(0, job.offset_x_dots)) + "," +
                        std::to_string(std::min(0, job.offset_y_dots)) + "\r\n");
    append(out, "CLS\r\n");
    append(out, "BITMAP " + std::to_string(std::max(0, job.offset_x_dots)) + "," +
                    std::to_string(std::max(0, job.offset_y_dots)) + "," + std::to_string(bytes_per_row) + "," +
                    std::to_string(image.height) + ",0,");
    const std::vector<std::uint8_t> bits = pack_rows(image, false);
    out.insert(out.end(), bits.begin(), bits.end());
    append(out, "\r\n");
    append(out, "PRINT 1," + std::to_string(std::max(1, job.copies)) + "\r\n");
    return out;
}

namespace {
// Les Zebra de bureau centrent le rouleau et la zone d'impression (^PW) sur la tete : une zone juste a la taille de
// l'etiquette ne bouge pas avec ^FO / ^LS, le decalage horizontal serait coupe par le bord de la zone. On l'elargit
// de `zpl_side_margin` points de chaque cote (sans depasser une tete 4 pouces) et l'image y est placee au milieu,
// decalage compris : sans decalage, elle tombe exactement au meme endroit.
constexpr int kZplHeadDots = 832; // 104 mm a 8 points/mm
int zpl_side_margin(int width_dots)
{
    return std::clamp((kZplHeadDots - width_dots) / 2, 0, 64);
}
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
    append(out, "^XA\n");
    append(out, std::string("^MT") + (job.ribbon ? "T" : "D") + "\n");
    append(out, "^MNY\n"); // etiquettes separees par un espace (capteur d'interstice)
    const int side = zpl_side_margin(job.width_dots);
    append(out, "^PW" + std::to_string(job.width_dots + 2 * side) + "\n");
    append(out, "^LL" + std::to_string(job.height_dots) + "\n");
    append(out, "^LH0,0\n");
    // decalage vertical negatif : ^LT remonte l'etiquette (-120 a 120)
    if (job.offset_y_dots < 0) append(out, "^LT" + std::to_string(std::max(-120, job.offset_y_dots)) + "\n");
    append(out, "^FO" + std::to_string(std::clamp(side + job.offset_x_dots, 0, 2 * side)) + "," +
                    std::to_string(std::max(0, job.offset_y_dots)));
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
            // format de l'etiquette chargee enregistre (^JUS), puis ~JC : l'imprimante avance quelques
            // etiquettes et mesure l'interstice ; ^JUS final garde la mesure apres extinction
            return std::string("^XA^MNY^MT") + (job.ribbon ? "T" : "D") + "^PW" +
                   std::to_string(job.width_dots + 2 * zpl_side_margin(job.width_dots)) +
                   "^LL" + std::to_string(job.height_dots) + "^JUS^XZ\n~JC\n^XA^JUS^XZ\n";
        case Maintenance::Config: return "~WC\n";
        case Maintenance::Feed: return "^XA^MNY^LL" + std::to_string(job.height_dots) + "^XZ\n";
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
    }
    return {};
}

} // namespace qrprotec::labels
