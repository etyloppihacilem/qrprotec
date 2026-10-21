#include "template.hpp"
#include "core/code128.hpp"
#include "core/template_io.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qrprotec {

namespace {
int pixels(double millimetres, double pixels_per_mm) {
  return std::max(1, static_cast< int >(std::lround(millimetres * pixels_per_mm)));
}

bool inside(double x, double y, double width, double height, const MediaSettings &media, bool turned = false) {
  if (turned)
    std::swap(width, height);
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

int normalize_rotation(int degrees) {
  const int turns = static_cast< int >(std::lround(degrees / 90.0));
  return ((turns % 4) + 4) % 4 * 90;
}

bool quarter_turned(const TemplateElement &element) {
  const int rotation = normalize_rotation(element.rotation);
  return rotation == 90 || rotation == 270;
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
      if (!inside(text.x_mm, text.y_mm, text.width_mm, text.height_mm, document.media, quarter_turned(element)))
        issues.push_back({ "Le texte dépasse les limites du media.", element.id });
      if (text.font_size_mm <= 0.0f)
        issues.push_back({ "La taille du texte doit être positive.", element.id });
    } else if (element.kind == ElementKind::QrCode) {
      const QrElement &qr = std::get< QrElement >(element.content);
      if (qr.payload.empty())
        issues.push_back({ "Le payload QR ne peut pas être vide.", element.id });
      if (!inside(qr.x_mm, qr.y_mm, qr.size_mm, qr.size_mm, document.media, quarter_turned(element)))
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
      if (!inside(barcode.x_mm, barcode.y_mm, barcode.width_mm, barcode.height_mm, document.media, quarter_turned(element)))
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
      if (!inside(image.x_mm, image.y_mm, image.width_mm, image.height_mm, document.media, quarter_turned(element)))
        issues.push_back({ "L'image dépasse les limites du media.", element.id });
    }
  }
  return issues;
}

namespace {
// Un placeholder : {{nom}}, ou {[prefixe]{nom}[suffixe]} dont le prefixe et le suffixe (chacun facultatif)
// ne s'impriment que si la valeur n'est pas vide, ex. {[Exp. ]{peremption}} ou {{peremption}[ inclus]}.
// ![remplacement] apres le suffixe s'imprime seul quand la valeur est vide : {{peremption}![Non périssable]}.
// Suffixe et remplacement s'ecrivent aussi dans les accolades du nom : {{peremption[ inclus]![-]}}.
struct Placeholder {
    std::size_t end = 0; // juste apres l'accolade fermante
    std::string prefix;
    std::string name;
    std::string suffix;
    std::string replacement;
    bool        has_suffix      = false;
    bool        has_replacement = false;
};

// Lit un texte entre crochets a partir de position (qui pointe sur '[') ; avance position apres ']'
bool read_bracket(const std::string &value, std::size_t &position, std::string &text) {
  const std::size_t end = value.find(']', position + 1);
  if (std::string::npos == end)
    return false;
  text     = value.substr(position + 1, end - position - 1);
  position = end + 1;
  return true;
}

// Lit un [suffixe] puis un ![remplacement] facultatifs a cursor, chacun une seule fois par placeholder
bool read_suffix_and_replacement(const std::string &value, std::size_t &cursor, Placeholder &placeholder) {
  if (cursor < value.size() && value[cursor] == '[') {
    if (placeholder.has_suffix || !read_bracket(value, cursor, placeholder.suffix))
      return false;
    placeholder.has_suffix = true;
  }
  if (cursor + 1 < value.size() && value[cursor] == '!' && value[cursor + 1] == '[') {
    ++cursor;
    if (placeholder.has_replacement || !read_bracket(value, cursor, placeholder.replacement))
      return false;
    placeholder.has_replacement = true;
  }
  return true;
}

// Reconnait un placeholder commencant a position (qui pointe sur '{')
bool read_placeholder(const std::string &value, std::size_t position, Placeholder &placeholder) {
  std::size_t cursor = position + 1;
  if (cursor < value.size() && value[cursor] == '[' && !read_bracket(value, cursor, placeholder.prefix))
    return false;
  if (cursor >= value.size() || value[cursor] != '{')
    return false;
  const std::size_t name_end = value.find_first_of("{}[]!", cursor + 1);
  if (std::string::npos == name_end || name_end == cursor + 1)
    return false;
  placeholder.name = value.substr(cursor + 1, name_end - cursor - 1);
  cursor           = name_end;
  if (!read_suffix_and_replacement(value, cursor, placeholder) || cursor >= value.size() || value[cursor] != '}')
    return false;
  ++cursor;
  if (!read_suffix_and_replacement(value, cursor, placeholder))
    return false;
  if (cursor >= value.size() || value[cursor] != '}')
    return false;
  placeholder.end = cursor + 1;
  return true;
}
} // namespace

std::string
resolve_parameters(const std::string &input, const std::unordered_map< std::string, std::string > &parameters) {
  std::string result;
  std::size_t position = 0;
  while (position < input.size()) {
    const std::size_t open = input.find('{', position);
    if (std::string::npos == open) {
      result.append(input, position, std::string::npos);
      break;
    }
    result.append(input, position, open - position);
    Placeholder       placeholder;
    const auto        found = read_placeholder(input, open, placeholder) ? parameters.find(placeholder.name)
                                                                         : parameters.end();
    if (found == parameters.end()) {
      // pas un placeholder, ou sans valeur connue : laisse tel quel
      result += '{';
      position = open + 1;
      continue;
    }
    if (!found->second.empty())
      result += placeholder.prefix + found->second + placeholder.suffix;
    else
      result += placeholder.replacement;
    position = placeholder.end;
  }
  return result;
}

std::vector< std::string > find_placeholders(const TemplateDocument &document) {
  std::set< std::string > names;
  const auto              collect = [&names](const std::string &value) {
    std::size_t position = 0;
    while ((position = value.find('{', position)) != std::string::npos) {
      Placeholder placeholder;
      if (read_placeholder(value, position, placeholder)) {
        names.insert(placeholder.name);
        position = placeholder.end;
      } else
        ++position;
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
