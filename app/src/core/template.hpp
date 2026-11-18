#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace qrprotec {

enum class Orientation { Portrait, Landscape };

enum class ElementKind { Text, QrCode, Image, Barcode };

// Usage d'un modele : determine les placeholders disponibles (voir placeholders.hpp).
enum class TemplateCategory { Generic, Item, ItemPack, LotPublic, LotPrivate, LotSeal, LotSealOpen, LotStorage, User, TaskItem, ItemBarcode,
                              ItemSmall, ItemTear, ItemSmallTear, ItemBarcodeTear, TaskItemSmall, TaskItemBarcode,
                              ItemSheet, LotLocation, GuestCard };

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

// Support d'une etiquette : thermique direct (papier qui noircit, sans ruban) ou transfert thermique (ruban)
enum class LabelSupport { Direct, Transfer };

const char*  label_support_id(LabelSupport support);    // "direct", "transfer" (fichier .qr)
const char*  label_support_label(LabelSupport support); // texte affiche
LabelSupport label_support_from_id(const std::string& id);

// Marges d'une etiquette (mm), une par cote. Celles d'un type d'etiquette sont donnees sur l'etiquette telle qu'elle
// sort de l'imprimante (haut = premier bord sorti, gauche/droite dans le sens de la tete), comme les decalages.
struct LabelMargins {
    double top = 0.0;
    double bottom = 0.0;
    double left = 0.0;
    double right = 0.0;

    static LabelMargins all(double value) { return {value, value, value, value}; }
    bool uniform() const; // les quatre egales (a 0,01 mm pres)
    bool operator==(const LabelMargins& other) const;
    bool operator!=(const LabelMargins& other) const { return !(*this == other); }
    // Memes marges vues sur un modele pose sur l'etiquette apres un quart de tour horaire `rotation` (0, 90, 180,
    // 270) : marges du modele a partir de celles de l'etiquette.
    LabelMargins rotated_to_design(int rotation) const;
};

// Type d'etiquette (rouleau) : enregistre dans chaque modele qui l'utilise, la liste des types est reconstruite
// a la lecture des modeles. Largeur dans le sens de la tete d'impression, hauteur dans le sens du defilement.
struct LabelType {
    std::string name;
    double width_mm = 40.0;
    double height_mm = 30.0;
    // Orientation d'impression : quart de tour (sens horaire) applique au modele pour le poser sur l'etiquette,
    // 0, 90, 180 ou 270. A 90 et 270, le modele est dessine en largeur = hauteur de l'etiquette.
    int rotation = 0;
    LabelSupport support = LabelSupport::Direct;
    // Marges d'impression (mm, une par cote, sur l'etiquette qui sort de l'imprimante) : cadre dans lequel le
    // modele est imprime. Ce qui depasse du cadre n'est pas imprime : l'imprimante peut se decaler de quelques
    // dixiemes de mm sans rien couper. Ecrites dans le .qr seulement si elles different de la valeur par defaut
    // (« margin_mm » si les quatre sont egales, sinon une cle par cote).
    static constexpr double kDefaultMarginMm = 1.0;
    LabelMargins margins = LabelMargins::all(kDefaultMarginMm);

    bool operator==(const LabelType& other) const;
    bool operator!=(const LabelType& other) const { return !(*this == other); }
    // Format du modele (canevas de l'editeur) qui correspond a ce type
    double design_width_mm() const;
    double design_height_mm() const;
    // Marges utilisables, sur l'etiquette : chacune au plus le tiers de la largeur (gauche, droite) ou de la
    // hauteur (haut, bas)
    LabelMargins effective_margins() const;
    // Marges utilisables vues sur le modele (orientation d'impression appliquee) : cadre de l'editeur et de
    // l'etiquette d'etalonnage
    LabelMargins design_margins() const;
    std::string summary() const; // ex : « 40 x 30 mm, droite, thermique direct »
};

const char* label_rotation_label(int rotation); // « droite », « quart de tour horaire »...
// Donne au modele le format de son type (largeur, hauteur, sens du canevas)
void apply_label_type(MediaSettings& media, const LabelType& type);

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
    bool dither = false;     // tramage Floyd-Steinberg (sinon seuil simple), desactive par defaut
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
    std::optional<LabelType> label_type; // absent : format libre, imprime selon les Reglages (etiquettes chargees)
    std::vector<TemplateElement> elements;
    std::unordered_map<std::string, std::string> parameters;
};

struct ValidationIssue {
    std::string message;
    std::string element_id;
};

std::vector<ValidationIssue> validate(const TemplateDocument& document);

// Types d'etiquettes utilises par des modeles, tries par nom (un par nom : le premier modele lu l'emporte)
std::vector<LabelType> collect_label_types(const std::vector<TemplateDocument>& documents);
std::string resolve_parameters(const std::string& input, const std::unordered_map<std::string, std::string>& parameters);
std::vector<std::string> find_placeholders(const TemplateDocument& document);
// std::vector<std::string> extract_placeholders(const std::string &str);

} // namespace qrprotec
