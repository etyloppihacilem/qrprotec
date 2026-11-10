#include "label_types.hpp"

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

} // namespace qrprotec
