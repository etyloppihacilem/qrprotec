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

} // namespace qrprotec
