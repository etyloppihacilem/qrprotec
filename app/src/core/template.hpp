#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace qrprotec {

enum class Orientation { Portrait, Landscape };

enum class ElementKind { Text, QrCode, Image, Barcode };

// Usage d'un modele : determine les placeholders disponibles (voir placeholders.hpp).
enum class TemplateCategory { Generic, Item, ItemPack, LotPublic, LotPrivate, LotSeal, LotSealOpen, LotStorage, User, TaskItem, ItemBarcode,
                              ItemSmall, ItemTear, ItemSmallTear, ItemBarcodeTear, TaskItemSmall, TaskItemBarcode };

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

// Code-barres Code 128 (voir code128.hpp) : lisible sur un objet rond ou un QR code se deforme. Les barres ont
// toutes la meme largeur entiere de pixels (sinon illisibles) : le code est centre dans sa zone, au plus large.
struct BarcodeElement {
    std::string payload;
    float x_mm = 2.0f;
    float y_mm = 2.0f;
    float width_mm = 36.0f;
    float height_mm = 10.0f;
    int quiet_zone = 10; // marge blanche de chaque cote, en modules (la norme en demande 10)
};

// Nombre de pixels par module (barre la plus fine) une fois rendu, 0 si le contenu ne tient pas dans la zone
// (marges comprises) ou n'est pas encodable.
int barcode_module_pixels(const BarcodeElement& barcode, const std::string& payload, double pixels_per_mm);

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
    std::variant<TextElement, QrElement, ImageElement, BarcodeElement> content;
    // Quart de tour en degres, sens horaire : 0, 90, 180 ou 270. x_mm et y_mm restent le coin haut-gauche de la
    // zone occupee sur l'etiquette ; largeur et hauteur sont celles de l'element avant rotation (echangees sur
    // l'etiquette a 90 et 270 degres).
    int rotation = 0;
};

// Rotation ramenee a 0, 90, 180 ou 270 (arrondie au quart de tour le plus proche)
int normalize_rotation(int degrees);
// Vrai si l'element est couche (90 ou 270 degres) : sa largeur et sa hauteur s'echangent sur l'etiquette
bool quarter_turned(const TemplateElement& element);

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
