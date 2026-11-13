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

TemplateDocument calibration_document(const LabelType& type, bool with_type) {
    TemplateDocument document;
    document.name = "Étalonnage " + type.summary();
    document.category = TemplateCategory::Generic;
    if (with_type)
        document.label_type = type;
    apply_label_type(document.media, type);
    const float width = static_cast<float>(type.design_width_mm());
    const float height = static_cast<float>(type.design_height_mm());
    // texte dans le quart haut-gauche, entre le cadre et la croix
    const float margin = 1.5f;
    const float size = std::clamp(std::min(width, height) / 12.0f, 1.4f, 3.0f);
    char format[64];
    std::snprintf(format, sizeof(format), "%g × %g mm", std::round(type.width_mm * 10.0) / 10.0,
                  std::round(type.height_mm * 10.0) / 10.0);
    const std::vector<std::pair<std::string, bool>> lines = {{"ÉTALONNAGE", true}, {format, false}};
    float y = margin;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        TextElement text;
        text.text = lines[index].first;
        text.bold = lines[index].second;
        text.x_mm = margin;
        text.y_mm = y;
        text.width_mm = std::max(1.0f, width / 2.0f - margin - 1.0f);
        text.height_mm = size + 0.4f;
        text.font_size_mm = size;
        y += text.height_mm;
        TemplateElement element;
        element.id = index == 0 ? "title" : "format";
        element.kind = ElementKind::Text;
        element.content = text;
        document.elements.push_back(element);
    }
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
    return image;
}

} // namespace qrprotec
