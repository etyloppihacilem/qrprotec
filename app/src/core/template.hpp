#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace qrprotec {

enum class Orientation { Portrait, Landscape };

enum class ElementKind { Text, QrCode, Image };

// Usage d'un modele : determine les placeholders disponibles (voir placeholders.hpp).
enum class TemplateCategory { Generic, Item, ItemPack, LotPublic, LotPrivate, LotSeal, LotSealOpen, LotStorage, User };

struct MediaSettings {
    double width_mm = 40.0;
    double height_mm = 30.0;
    double pixels_per_mm = 8.0;
    Orientation orientation = Orientation::Landscape;

    int width_pixels() const;
    int height_pixels() const;
    double oriented_width_mm() const;
    double oriented_height_mm() const;
};

enum class TextAlign { Left, Center, Right };

struct TextElement {
    std::string text;
    float x_mm = 2.0f;
    float y_mm = 2.0f;
    float width_mm = 36.0f;
    float height_mm = 8.0f;
    float font_size_mm = 3.0f;
    bool bold = false;
    TextAlign align = TextAlign::Left;
};

struct QrElement {
    std::string payload;
    float x_mm = 2.0f;
    float y_mm = 12.0f;
    float size_mm = 16.0f;
    int ecc = 1;        // correction d'erreur : 0 = L (7 %), 1 = M (15 %), 2 = Q (25 %), 3 = H (30 %)
    int quiet_zone = 2; // marge blanche garantie autour du QR, en modules (la norme en recommande 4)
};

// Nombre de pixels par module du QR une fois rendu (0 si le contenu ne tient pas dans un QR).
int qr_module_pixels(const QrElement& qr, const std::string& payload, double pixels_per_mm);

// Image (logo PNG ou JPEG), convertie en noir et blanc a l'impression.
struct ImageElement {
    std::string path;
    float x_mm = 2.0f;
    float y_mm = 2.0f;
    float width_mm = 10.0f;
    float height_mm = 10.0f;
    bool dither = true;      // tramage Floyd-Steinberg (sinon seuil simple)
    int threshold = 128;     // seuil noir/blanc 0-255
};

struct TemplateElement {
    std::string id;
    ElementKind kind = ElementKind::Text;
    std::variant<TextElement, QrElement, ImageElement> content;
};

struct TemplateDocument {
    std::string name = "Nouveau template";
    std::string path;
    TemplateCategory category = TemplateCategory::Generic;
    MediaSettings media;
    std::vector<TemplateElement> elements;
    std::unordered_map<std::string, std::string> parameters;
};

struct ValidationIssue {
    std::string message;
    std::string element_id;
};

std::vector<ValidationIssue> validate(const TemplateDocument& document);
std::string resolve_parameters(const std::string& input, const std::unordered_map<std::string, std::string>& parameters);
std::vector<std::string> find_placeholders(const TemplateDocument& document);
// std::vector<std::string> extract_placeholders(const std::string &str);

} // namespace qrprotec
