#include "template.hpp"
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
    issues.push_back({ "Les dimensions du media doivent etre positives.", {} });

  for (const TemplateElement &element : document.elements) {
    if (element.id.empty())
      issues.push_back({ "Chaque element doit avoir un identifiant.", {} });

    if (element.kind == ElementKind::Text) {
      const TextElement &text = std::get< TextElement >(element.content);
      if (!inside(text.x_mm, text.y_mm, text.width_mm, text.height_mm, document.media))
        issues.push_back({ "Le texte depasse les limites du media.", element.id });
      if (text.font_size_mm <= 0.0f)
        issues.push_back({ "La taille du texte doit etre positive.", element.id });
    } else {
      const QrElement &qr = std::get< QrElement >(element.content);
      if (qr.payload.empty())
        issues.push_back({ "Le payload QR ne peut pas etre vide.", element.id });
      if (!inside(qr.x_mm, qr.y_mm, qr.size_mm, qr.size_mm, document.media))
        issues.push_back({ "Le QR code depasse les limites du media.", element.id });
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
    else
      collect(std::get< QrElement >(element.content).payload);
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
