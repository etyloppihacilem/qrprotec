#include "label_types.hpp"
#include "../core/codes.hpp"

#include "../core/paths.hpp"
#include "../core/template_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qrprotec {

LabelType default_label_type(const MediaSettings& media, const PhysicalLabel& label, LabelSupport support)
{
    // meme choix que fit_to_label en format libre : on garde le sens du modele s'il remplit l'etiquette
    const double width = media.oriented_width_mm();
    const double height = media.oriented_height_mm();
    const double tolerance = 1.0;
    const auto fits = [&](double w, double h) {
        return w <= label.width_mm + tolerance && h <= label.height_mm + tolerance;
    };
    const auto fill_ratio = [&](double w, double h) {
        return std::min(w, label.width_mm) * std::min(h, label.height_mm) / (label.width_mm * label.height_mm);
    };
    const bool straight_ok = fits(width, height);
    const bool rotated_ok = fits(height, width);
    const bool rotate = (!straight_ok && rotated_ok)
                     || (straight_ok && rotated_ok && fill_ratio(height, width) > fill_ratio(width, height) + 1e-6);
    int rotation = rotate ? (label.rotate_counterclockwise ? 270 : 90) : 0;
    if (label.flip) rotation += 180;

    LabelType type;
    type.width_mm = label.width_mm;
    type.height_mm = label.height_mm;
    type.rotation = normalize_rotation(rotation);
    type.support = support;
    char size[64];
    std::snprintf(size, sizeof(size), "%g x %g mm", std::round(label.width_mm * 10.0) / 10.0,
                  std::round(label.height_mm * 10.0) / 10.0);
    type.name = size;
    if (type.rotation != 0) type.name += std::string(", ") + label_rotation_label(type.rotation);
    if (support == LabelSupport::Transfer) type.name += ", ruban";
    return type;
}

int add_missing_label_types(const PhysicalLabel& label, LabelSupport support, std::vector<LabelType>* types,
                            std::vector<std::string>* errors)
{
    int updated = 0;
    for (const auto& path : glob_templates("*.qr")) {
        TemplateDocument document;
        std::string error;
        if (!load_template(document, path.string(), error) || document.label_type) continue;
        document.label_type = default_label_type(document.media, label, support);
        if (types) types->push_back(*document.label_type);
        if (!save_template(document, path.string(), error)) {
            if (errors) errors->push_back(path.filename().string() + " : " + error);
            continue;
        }
        ++updated;
    }
    return updated;
}

namespace {
// Nombre de caracteres d'un texte UTF-8 (octets de continuation non comptes)
std::size_t decode_length(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char c : text)
        if ((c & 0xC0) != 0x80)
            ++count;
    return count;
}
} // namespace

std::string roll_action_code(const LabelType& roll) {
    const auto mm = [](double value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0) / 10.0);
        return std::string(text);
    };
    return poste_action_code("rouleau", {{"t", roll.name}, {"w", mm(roll.width_mm)}, {"h", mm(roll.height_mm)},
                                         {"s", label_support_id(roll.support)}});
}

TemplateDocument roll_code_document(const LabelType& roll, const LabelType& print_on) {
    TemplateDocument document;
    document.name = "Rouleau " + roll.name;
    document.category = TemplateCategory::Generic;
    document.label_type = print_on;
    apply_label_type(document.media, print_on);
    const float width = static_cast<float>(print_on.design_width_mm());
    const float height = static_cast<float>(print_on.design_height_mm());
    const float margin = 1.5f;
    const bool tall = height > width * 1.15f;
    QrElement qr;
    qr.payload = roll_action_code(roll);
    qr.ecc = 0; // modules plus gros : lisible de plus loin sur une petite etiquette
    float text_x, text_y, text_w, text_h;
    if (tall) {
        qr.size_mm = std::min(width - 2 * margin, height * 0.55f);
        qr.x_mm = (width - qr.size_mm) / 2;
        qr.y_mm = margin;
        text_x = margin;
        text_y = qr.y_mm + qr.size_mm + 0.5f;
        text_w = width - 2 * margin;
        text_h = height - text_y - margin;
    } else {
        qr.size_mm = std::min(height - 2 * margin, width * 0.5f);
        qr.x_mm = margin;
        qr.y_mm = (height - qr.size_mm) / 2;
        text_x = qr.x_mm + qr.size_mm + 1.0f;
        text_y = margin;
        text_w = width - text_x - margin;
        text_h = height - 2 * margin;
    }
    // textes : titre, nom du rouleau (le plus gros), format en clair s'il n'est pas deja dans le nom, mode d'emploi
    // s'il reste de la place. Taille reduite jusqu'a ce que tout tienne (nombre de lignes estime a la largeur
    // moyenne des caracteres).
    char format[96];
    std::snprintf(format, sizeof(format), "%g × %g mm, %s", std::round(roll.width_mm * 10.0) / 10.0,
                  std::round(roll.height_mm * 10.0) / 10.0,
                  roll.support == LabelSupport::Transfer ? "transfert thermique" : "thermique direct");
    struct Line { std::string id, value; float scale; bool bold; };
    std::vector<Line> lines = {{"title", "ROULEAU", 0.8f, true}, {"name", roll.name, 1.15f, true}};
    const std::string support_word = roll.support == LabelSupport::Transfer ? "transfert" : "direct";
    if (roll.name.find(support_word) == std::string::npos)
        lines.push_back({"format", format, 0.75f, false});
    const auto line_count = [&](const Line& line, float size) {
        const float per_char = size * (line.bold ? 0.72f : 0.6f);
        const int chars_per_line = std::max(1, static_cast<int>(text_w / per_char));
        return static_cast<int>((decode_length(line.value) + chars_per_line - 1) / chars_per_line);
    };
    // plus long mot d'une ligne : il doit tenir sur la largeur (sinon coupe au milieu)
    const auto longest_word = [](const std::string& value) {
        std::size_t best = 0, current = 0;
        for (const unsigned char c : value) {
            if (c == ' ') { best = std::max(best, current); current = 0; }
            else if ((c & 0xC0) != 0x80) ++current;
        }
        return std::max(best, current);
    };
    const auto total_height = [&](const std::vector<Line>& rows, float unit) {
        float total = 0;
        for (const Line& line : rows) {
            const float size = unit * line.scale;
            if (longest_word(line.value) * size * (line.bold ? 0.72f : 0.62f) > text_w)
                return 1.0e9f;
            total += line_count(line, size) * (size + 0.3f) + 0.4f;
        }
        return total;
    };
    std::vector<Line> with_hint = lines;
    with_hint.push_back({"hint", "Scanner à la douchette quand ce rouleau est mis dans l'imprimante", 0.6f, false});
    float unit = 4.0f;
    while (unit > 1.6f && total_height(lines, unit) > text_h)
        unit -= 0.1f;
    if (total_height(with_hint, unit) <= text_h && unit * 0.6f >= 1.5f)
        lines = with_hint;
    std::vector<TemplateElement> elements;
    TemplateElement qr_element;
    qr_element.id = "qr";
    qr_element.kind = ElementKind::QrCode;
    qr_element.content = qr;
    elements.push_back(qr_element);
    for (const Line& line : lines) {
        const float size = unit * line.scale;
        TextElement text;
        text.text = line.value;
        text.x_mm = text_x;
        text.y_mm = text_y;
        text.width_mm = text_w;
        text.height_mm = line_count(line, size) * (size + 0.3f) + 0.3f;
        text.font_size_mm = size;
        text.bold = line.bold;
        text_y += text.height_mm + 0.1f;
        TemplateElement element;
        element.id = line.id;
        element.kind = ElementKind::Text;
        element.content = text;
        elements.push_back(element);
    }
    document.elements = std::move(elements);
    return document;
}

namespace {
// Graduations chiffrees depuis chaque bord, au bout des axes de la croix (decalage a lire au ras du bord)
int calibration_edge_marks(double length_mm) {
    return std::clamp(static_cast<int>(length_mm / 2.0 - 4.0), 0, 4);
}

void add_text(TemplateDocument& document, const std::string& id, const std::string& value, float x, float y,
              float width, float size, bool bold, TextAlign align) {
    TextElement text;
    text.text = value;
    text.bold = bold;
    text.width_mm = std::max(0.5f, width);
    text.height_mm = size + 0.4f;
    // reste dans l'etiquette (chiffres au ras du bord)
    text.x_mm = std::clamp(x, 0.0f, std::max(0.0f, static_cast<float>(document.media.width_mm) - text.width_mm - 0.01f));
    text.y_mm = std::clamp(y, 0.0f, std::max(0.0f, static_cast<float>(document.media.height_mm) - text.height_mm - 0.01f));
    text.font_size_mm = size;
    text.align = align;
    TemplateElement element;
    element.id = id;
    element.kind = ElementKind::Text;
    element.content = text;
    document.elements.push_back(element);
}
} // namespace

TemplateDocument calibration_document(const LabelType& type, bool with_type) {
    TemplateDocument document;
    document.name = "Étalonnage " + type.summary();
    document.category = TemplateCategory::Generic;
    if (with_type)
        document.label_type = type;
    apply_label_type(document.media, type);
    const float width = static_cast<float>(type.design_width_mm());
    const float height = static_cast<float>(type.design_height_mm());
    const float ppm = static_cast<float>(document.media.pixels_per_mm > 0 ? document.media.pixels_per_mm : 8.0);
    // centre de la croix tel que calibration_image le trace
    const float cx = static_cast<float>(document.media.width_pixels() / 2) / ppm;
    const float cy = static_cast<float>(document.media.height_pixels() / 2) / ppm;
    const float margin = 1.5f;
    // titre et format dans le quart haut-gauche, entre le cadre et la croix ; taille telle que le titre tienne
    const float quarter = std::max(1.0f, cx - margin - 3.0f);
    const float size = std::clamp(std::min({std::min(width, height) / 12.0f, quarter / 7.6f}), 1.2f, 3.0f);
    char format[64];
    std::snprintf(format, sizeof(format), "%g × %g mm", std::round(type.width_mm * 10.0) / 10.0,
                  std::round(type.height_mm * 10.0) / 10.0);
    add_text(document, "title", "ÉTALONNAGE", margin, margin, quarter, size, true, TextAlign::Left);
    add_text(document, "format", format, margin, margin + size + 0.4f, quarter, size * 0.85f, false, TextAlign::Left);

    // chiffres 1..n mm depuis chaque bord, au bout des axes (impairs d'un cote, pairs de l'autre pour avoir la
    // place), et signe a ajouter au decalage avec la fleche du sens dans lequel il deplace l'impression
    const float digit = 1.7f;
    const float gap = 1.1f; // entre l'axe et les chiffres (au-dela des graduations)
    const int marks_x = calibration_edge_marks(width);
    const int marks_y = calibration_edge_marks(height);
    for (int k = 1; k <= marks_x; ++k) {
        const std::string value = std::to_string(k);
        const float y = k % 2 == 1 ? cy - gap - digit - 0.3f : cy + gap;
        add_text(document, "left" + value, value, k - 0.8f, y, 1.6f, digit, true, TextAlign::Center);
        add_text(document, "right" + value, value, width - k - 0.8f, y, 1.6f, digit, true, TextAlign::Center);
    }
    for (int k = 1; k <= marks_y; ++k) {
        const std::string value = std::to_string(k);
        const float x = k % 2 == 1 ? cx + gap : cx - gap - 1.2f;
        add_text(document, "top" + value, value, x, k - digit / 2 - 0.2f, 1.2f, digit, true, TextAlign::Center);
        add_text(document, "bottom" + value, value, x, height - k - digit / 2 - 0.2f, 1.2f, digit, true,
                 TextAlign::Center);
    }
    const float sign = 2.0f;
    const float sign_w = 5.5f;
    if (marks_x > 0) {
        const float y = cy - 1.7f - sign - 0.4f;
        add_text(document, "left_sign", "+ →", marks_x + 0.8f, y, sign_w, sign, true, TextAlign::Left);
        add_text(document, "right_sign", "← −", width - marks_x - 0.8f - sign_w, y, sign_w, sign, true,
                 TextAlign::Right);
    }
    if (marks_y > 0) {
        add_text(document, "top_sign", "+ ↓", cx + 1.7f, marks_y + 0.6f, sign_w, sign, true, TextAlign::Left);
        add_text(document, "bottom_sign", "− ↑", cx + 1.7f, height - marks_y - 0.6f - sign - 0.4f, sign_w, sign,
                 true, TextAlign::Left);
    }
    // mode d'emploi dans le quart bas-gauche s'il y a la place
    const float legend_y = cy + gap + digit + 1.0f;
    const std::vector<std::string> legend = {"Bord coupé : lire", "le chiffre au bord,", "l'ajouter (signe)",
                                             "au décalage (mm)."};
    const float legend_size = std::min(1.6f, (quarter + 2.0f) / (0.55f * 19.0f));
    if (legend_y + legend.size() * (legend_size + 0.4f) < height - 1.5f && legend_size >= 1.0f)
        for (std::size_t index = 0; index < legend.size(); ++index)
            add_text(document, "legend" + std::to_string(index), legend[index], margin,
                     legend_y + index * (legend_size + 0.4f), quarter + 2.0f, legend_size, false, TextAlign::Left);
    return document;
}

RasterImage calibration_image(const TemplateDocument& document) {
    RasterImage image = render_template(document);
    const int width = image.width;
    const int height = image.height;
    if (width <= 0 || height <= 0)
        return image;
    const double ppm = document.media.pixels_per_mm > 0 ? document.media.pixels_per_mm : 8.0;
    const auto dot = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < width && y < height)
            image.at(x, y) = 0;
    };
    const int mm1 = std::max(1, static_cast<int>(std::lround(ppm)));
    // cadre plein au ras du bord (2 points) et cadre pointille a 1 mm
    for (int x = 0; x < width; ++x) {
        for (const int y : {0, 1, height - 2, height - 1})
            dot(x, y);
        if (x % 4 == 0) {
            dot(x, mm1);
            dot(x, height - 1 - mm1);
        }
    }
    for (int y = 0; y < height; ++y) {
        for (const int x : {0, 1, width - 2, width - 1})
            dot(x, y);
        if (y % 4 == 0) {
            dot(mm1, y);
            dot(width - 1 - mm1, y);
        }
    }
    // croix au centre (2 points d'epaisseur), graduee tous les millimetres depuis le centre
    const int cx = width / 2;
    const int cy = height / 2;
    for (int x = 0; x < width; ++x) {
        dot(x, cy - 1);
        dot(x, cy);
    }
    for (int y = 0; y < height; ++y) {
        dot(cx - 1, y);
        dot(cx, y);
    }
    const auto tick = [&](int step) {
        const int length = step % 10 == 0 ? 3 * mm1 / 2 : step % 5 == 0 ? mm1 : mm1 / 2;
        return std::max(2, length);
    };
    for (int step = 1;; ++step) {
        const int offset = static_cast<int>(std::lround(step * ppm));
        if (offset > cx && offset > cy && offset > width - cx && offset > height - cy)
            break;
        const int length = tick(step);
        for (int d = -length; d <= length; ++d) {
            for (const int x : {cx - offset, cx + offset - 1}) // horizontale
                dot(x, cy + d);
            for (const int y : {cy - offset, cy + offset - 1}) // verticale
                dot(cx + d, y);
        }
    }
    // graduations depuis chaque bord (chiffrees par calibration_document) : un decalage se lit au ras du bord
    const int marks_x = calibration_edge_marks(width / ppm);
    const int marks_y = calibration_edge_marks(height / ppm);
    const int half = std::max(2, mm1 / 2);
    for (int k = 1; k <= marks_x; ++k) {
        const int offset = static_cast<int>(std::lround(k * ppm));
        for (int d = -half; d <= half; ++d) {
            dot(offset, cy + d);
            dot(offset - 1, cy + d);
            dot(width - offset, cy + d);
            dot(width - 1 - offset, cy + d);
        }
    }
    for (int k = 1; k <= marks_y; ++k) {
        const int offset = static_cast<int>(std::lround(k * ppm));
        for (int d = -half; d <= half; ++d) {
            dot(cx + d, offset);
            dot(cx + d, offset - 1);
            dot(cx + d, height - offset);
            dot(cx + d, height - 1 - offset);
        }
    }
    return image;
}

} // namespace qrprotec
