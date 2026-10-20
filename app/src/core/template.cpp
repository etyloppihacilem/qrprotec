#include "template.hpp"
#include "core/code128.hpp"
#include "core/template_io.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace qrprotec {

namespace {
int pixels(double millimetres, double pixels_per_mm) {
  return std::max(1, static_cast< int >(std::lround(millimetres * pixels_per_mm)));
}

bool inside(double x, double y, double width, double height, const MediaSettings &media) {
  return x >= 0.0 && y >= 0.0 && width > 0.0 && height > 0.0 && x + width <= media.oriented_width_mm()
      && y + height <= media.oriented_height_mm();
}
} // namespace

int barcode_module_pixels(const BarcodeElement &barcode, const std::string &payload, double pixels_per_mm) {
  const std::size_t modules = code128_modules(payload).size();
  if (modules == 0)
    return 0;
  const int width = std::max(1, static_cast< int >(barcode.width_mm * pixels_per_mm));
  return width / static_cast< int >(modules + 2 * static_cast< std::size_t >(std::max(0, barcode.quiet_zone)));
}

int MediaSettings::width_pixels() const {
  return pixels(oriented_width_mm(), pixels_per_mm);
}

int MediaSettings::height_pixels() const {
  return pixels(oriented_height_mm(), pixels_per_mm);
}

double MediaSettings::oriented_width_mm() const {
  return orientation == Orientation::Portrait ? height_mm : width_mm;
}

double MediaSettings::oriented_height_mm() const {
  return orientation == Orientation::Portrait ? width_mm : height_mm;
}

std::vector< ValidationIssue > validate(const TemplateDocument &document) {
  std::vector< ValidationIssue > issues;
  if (document.media.width_mm <= 0.0 || document.media.height_mm <= 0.0 || document.media.pixels_per_mm <= 0.0)
    issues.push_back({ "Les dimensions du media doivent être positives.", {} });

  for (const TemplateElement &element : document.elements) {
    if (element.id.empty())
      issues.push_back({ "Chaque élément doit avoir un identifiant.", {} });

    if (element.kind == ElementKind::Text) {
      const TextElement &text = std::get< TextElement >(element.content);
      if (!inside(text.x_mm, text.y_mm, text.width_mm, text.height_mm, document.media))
        issues.push_back({ "Le texte dépasse les limites du media.", element.id });
      if (text.font_size_mm <= 0.0f)
        issues.push_back({ "La taille du texte doit être positive.", element.id });
    } else if (element.kind == ElementKind::QrCode) {
      const QrElement &qr = std::get< QrElement >(element.content);
      if (qr.payload.empty())
        issues.push_back({ "Le payload QR ne peut pas être vide.", element.id });
      if (!inside(qr.x_mm, qr.y_mm, qr.size_mm, qr.size_mm, document.media))
        issues.push_back({ "Le QR code dépasse les limites du media.", element.id });
      const int module = qr_module_pixels(qr, resolve_parameters(qr.payload, document.parameters),
                                          document.media.pixels_per_mm);
      if (!qr.payload.empty() && module < 2)
        issues.push_back({ "QR code trop petit pour son contenu (" + std::to_string(module)
                             + " px par module, 2 minimum) : agrandissez-le, réduisez la marge ou la correction.",
                           element.id });
    } else if (element.kind == ElementKind::Barcode) {
      const BarcodeElement &barcode = std::get< BarcodeElement >(element.content);
      const std::string     payload = resolve_parameters(barcode.payload, document.parameters);
      if (barcode.payload.empty())
        issues.push_back({ "Le contenu du code-barres ne peut pas être vide.", element.id });
      if (!inside(barcode.x_mm, barcode.y_mm, barcode.width_mm, barcode.height_mm, document.media))
        issues.push_back({ "Le code-barres dépasse les limites du media.", element.id });
      if (!payload.empty() && code128_modules(payload).empty())
        issues.push_back({ "Code-barres : seuls les caractères ASCII imprimables passent (pas d'accents).",
                           element.id });
      else if (!payload.empty() && barcode_module_pixels(barcode, payload, document.media.pixels_per_mm) < 1)
        issues.push_back({ "Code-barres trop étroit pour son contenu (" + std::to_string(code128_modules(payload).size())
                             + " modules + marges, 1 px minimum chacun) : élargissez-le ou réduisez la marge.",
                           element.id });
      if (barcode.height_mm < 4.0f)
        issues.push_back({ "Code-barres trop bas (4 mm minimum pour être lu).", element.id });
    } else {
      const ImageElement &image = std::get< ImageElement >(element.content);
      if (image.path.empty())
        issues.push_back({ "Choisissez un fichier image.", element.id });
      if (!inside(image.x_mm, image.y_mm, image.width_mm, image.height_mm, document.media))
        issues.push_back({ "L'image dépasse les limites du media.", element.id });
    }
  }
  return issues;
}

std::string
resolve_parameters(const std::string &input, const std::unordered_map< std::string, std::string > &parameters) {
  std::string result = input;
  for (const auto &[key, value] : parameters) {
    const std::string token    = "{{" + key + "}}";
    std::size_t       position = 0;
    while ((position = result.find(token, position)) != std::string::npos) {
      result.replace(position, token.size(), value);
      position += value.size();
    }
  }
  return result;
}

std::vector< std::string > find_placeholders(const TemplateDocument &document) {
  std::set< std::string > names;
  const auto              collect = [&names](const std::string &value) {
    std::size_t position = 0;
    while ((position = value.find("{{", position)) != std::string::npos) {
      const std::size_t end = value.find("}}", position + 2);
      if (std::string::npos == end)
        break;
      const std::string name = value.substr(position + 2, end - position - 2);
      if (!name.empty())
        names.insert(name);
      position = end + 2;
    }
  };
  for (const TemplateElement &element : document.elements)
    if (element.kind == ElementKind::Text)
      collect(std::get< TextElement >(element.content).text);
    else if (element.kind == ElementKind::QrCode)
      collect(std::get< QrElement >(element.content).payload);
    else if (element.kind == ElementKind::Barcode)
      collect(std::get< BarcodeElement >(element.content).payload);
  std::vector< std::string > result(names.begin(), names.end());

  // Tri standard (ordre lexicographique ASCII)
  std::sort(result.begin(), result.end());

  return result;
}

// std::vector< std::string > extract_placeholders(const std::string &str) {
//   std::set< std::string > names;
//   const auto              collect = [&names](const std::string &value) {
//     std::size_t position = 0;
//     while ((position = value.find("{{", position)) != std::string::npos) {
//       const std::size_t end = value.find("}}", position + 2);
//       if (std::string::npos == end)
//         break;
//       const std::string name = value.substr(position + 2, end - position - 2);
//       if (!name.empty())
//         names.insert(name);
//       position = end + 2;
//     }
//   };
//   collect(str);
//   return { names.begin(), names.end() };
// }

} // namespace qrprotec
