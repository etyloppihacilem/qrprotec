#include "raster.hpp"
#include "image_loader.hpp"
#include "../core/fonts.hpp"
#include "../core/paths.hpp"
#include <filesystem>

#include "qrcodegen.hpp"

#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <algorithm>
#include <cstdio>
#include <array>
#include <optional>
#include <cmath>
#include <string>

namespace qrprotec {

namespace {
void fill_rect(RasterImage& image, int left, int top, int right, int bottom, std::uint8_t value)
{
    left = std::max(0, left);
    top = std::max(0, top);
    right = std::min(image.width, right);
    bottom = std::min(image.height, bottom);
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x)
            image.at(x, y) = value;
}

std::vector<char32_t> decode_utf8(const std::string& value)
{
    std::vector<char32_t> output;
    for (std::size_t index = 0; index < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[index]);
        int length = 1;
        char32_t code = lead;
        if (lead >= 0xf0) { length = 4; code = lead & 0x07; }
        else if (lead >= 0xe0) { length = 3; code = lead & 0x0f; }
        else if (lead >= 0xc0) { length = 2; code = lead & 0x1f; }
        if (index + static_cast<std::size_t>(length) > value.size()) break;
        for (int extra = 1; extra < length; ++extra)
            code = (code << 6) | (static_cast<unsigned char>(value[index + static_cast<std::size_t>(extra)]) & 0x3f);
        output.push_back(code);
        index += static_cast<std::size_t>(length);
    }
    return output;
}

void draw_text(RasterImage& image, const TextElement& text, const std::string& value, double scale)
{
    const int left = static_cast<int>(text.x_mm * scale);
    const int top = static_cast<int>(text.y_mm * scale);
    const int max_width = std::max(1, static_cast<int>(text.width_mm * scale));
    const int bottom = top + static_cast<int>(text.height_mm * scale);
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&library) != 0) return;
    // gras : police grasse si disponible, sinon gras synthetique sur la police normale
    const std::string& bold_path = text.bold ? bold_font_path() : std::string();
    const bool synthetic_bold = text.bold && bold_path.empty();
    const std::string& font_path = bold_path.empty() ? ui_font_path() : bold_path;
    if (font_path.empty() || FT_New_Face(library, font_path.c_str(), 0, &face) != 0) face = nullptr;
    if (face == nullptr) {
        if (library) FT_Done_FreeType(library);
        return;
    }
    const int pixel_size = std::max(1, static_cast<int>(text.font_size_mm * scale));
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));
    const int line_height = pixel_size + 2;
    const auto load = [&](char32_t character) {
        if (FT_Load_Char(face, character, FT_LOAD_DEFAULT) != 0) return false;
        if (synthetic_bold) FT_GlyphSlot_Embolden(face->glyph);
        return true;
    };
    const auto advance = [&](char32_t character) {
        return load(character) ? static_cast<int>(face->glyph->advance.x >> 6) : 0;
    };

    // 1. mise en page : coupure aux espaces (au milieu d'un mot seulement s'il est trop long)
    struct Line { std::vector<char32_t> characters; int width = 0; };
    std::vector<Line> lines(1);
    const int space = advance(' ');
    const std::vector<char32_t> characters = decode_utf8(value);
    std::size_t index = 0;
    while (index < characters.size()) {
        if (characters[index] == '\n') { lines.emplace_back(); ++index; continue; }
        if (characters[index] == ' ') { ++index; continue; }
        std::vector<char32_t> word;
        int word_width = 0;
        while (index < characters.size() && characters[index] != ' ' && characters[index] != '\n') {
            word.push_back(characters[index]);
            word_width += advance(characters[index]);
            ++index;
        }
        Line* line = &lines.back();
        if (!line->characters.empty() && line->width + space + word_width > max_width) {
            lines.emplace_back();
            line = &lines.back();
        }
        if (!line->characters.empty()) { line->characters.push_back(' '); line->width += space; }
        for (const char32_t character : word) {
            const int width = advance(character);
            if (!line->characters.empty() && line->width + width > max_width && word_width > max_width) {
                lines.emplace_back();
                line = &lines.back();
            }
            line->characters.push_back(character);
            line->width += width;
        }
    }

    // 2. dessin ligne par ligne selon l'alignement
    for (std::size_t row = 0; row < lines.size(); ++row) {
        const Line& line = lines[row];
        const int pen_y = top + pixel_size + static_cast<int>(row) * line_height;
        if (pen_y - pixel_size >= bottom) break;
        int pen_x = left;
        if (text.align == TextAlign::Center) pen_x += std::max(0, (max_width - line.width) / 2);
        else if (text.align == TextAlign::Right) pen_x += std::max(0, max_width - line.width);
        for (const char32_t character : line.characters) {
            if (!load(character) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
            const FT_GlyphSlot glyph = face->glyph;
            for (unsigned int y = 0; y < glyph->bitmap.rows; ++y)
                for (unsigned int x = 0; x < glyph->bitmap.width; ++x)
                    if (glyph->bitmap.buffer[y * glyph->bitmap.pitch + x] >= 128) {
                        const int px = pen_x + glyph->bitmap_left + static_cast<int>(x);
                        const int py = pen_y - glyph->bitmap_top + static_cast<int>(y);
                        if (py < bottom) fill_rect(image, px, py, px + 1, py + 1, 0);
                    }
            pen_x += glyph->advance.x >> 6;
        }
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
}

std::optional<qrcodegen::QrCode> encode_qr(const QrElement& qr, const std::string& payload)
{
    static const qrcodegen::QrCode::Ecc levels[] = {qrcodegen::QrCode::Ecc::LOW, qrcodegen::QrCode::Ecc::MEDIUM,
                                                    qrcodegen::QrCode::Ecc::QUARTILE, qrcodegen::QrCode::Ecc::HIGH};
    try {
        return qrcodegen::QrCode::encodeText(payload.c_str(), levels[std::clamp(qr.ecc, 0, 3)]);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void draw_qr(RasterImage& image, const QrElement& qr, const std::string& payload, double scale)
{
    if (payload.empty()) return;
    const int left = static_cast<int>(qr.x_mm * scale);
    const int top = static_cast<int>(qr.y_mm * scale);
    const int size = std::max(1, static_cast<int>(qr.size_mm * scale));
    // le masque est choisi automatiquement par qrcodegen parmi les 8 de la norme (penalite minimale)
    const std::optional<qrcodegen::QrCode> encoded = encode_qr(qr, payload);
    if (!encoded) return; // payload trop long pour un QR code
    const qrcodegen::QrCode& code = *encoded;
    const int modules = code.getSize();
    // marge blanche (zone de silence) comprise dans la zone reservee : modules entiers de pixels
    const int quiet = std::max(0, qr.quiet_zone);
    const int module_size = std::max(1, size / (modules + 2 * quiet));
    const int offset = std::max(0, (size - module_size * modules) / 2);
    fill_rect(image, left, top, left + size, top + size, 255); // efface ce qui chevaucherait le QR
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (code.getModule(x, y))
                fill_rect(image, left + offset + x * module_size, top + offset + y * module_size,
                          left + offset + (x + 1) * module_size, top + offset + (y + 1) * module_size, 0);
}

void draw_image(RasterImage& image, const ImageElement& element, double scale)
{
    std::string error;
    // chemin relatif : cherche d'abord dans le dossier des modeles (logo commite avec les modeles)
    std::error_code exists_error;
    const std::filesystem::path in_templates = resolve_template_path(element.path);
    const std::string image_path = std::filesystem::exists(in_templates, exists_error) ? in_templates.string() : element.path;
    const std::shared_ptr<const GrayImage> source = load_gray_image(image_path, error);
    if (!source || source->width <= 0 || source->height <= 0) return;
    const int box_left = static_cast<int>(element.x_mm * scale);
    const int box_top = static_cast<int>(element.y_mm * scale);
    const int box_width = std::max(1, static_cast<int>(element.width_mm * scale));
    const int box_height = std::max(1, static_cast<int>(element.height_mm * scale));
    // conserve les proportions et centre dans la zone
    const double ratio = std::min(static_cast<double>(box_width) / source->width,
                                  static_cast<double>(box_height) / source->height);
    const int width = std::max(1, static_cast<int>(source->width * ratio));
    const int height = std::max(1, static_cast<int>(source->height * ratio));
    const int left = box_left + (box_width - width) / 2;
    const int top = box_top + (box_height - height) / 2;
    // reechantillonnage par moyenne de zone
    std::vector<float> scaled(static_cast<std::size_t>(width * height), 255.0f);
    for (int y = 0; y < height; ++y) {
        const int source_top = y * source->height / height;
        const int source_bottom = std::max(source_top + 1, (y + 1) * source->height / height);
        for (int x = 0; x < width; ++x) {
            const int source_left = x * source->width / width;
            const int source_right = std::max(source_left + 1, (x + 1) * source->width / width);
            float sum = 0.0f;
            int count = 0;
            for (int sy = source_top; sy < source_bottom && sy < source->height; ++sy)
                for (int sx = source_left; sx < source_right && sx < source->width; ++sx) {
                    sum += source->pixels[static_cast<std::size_t>(sy * source->width + sx)];
                    ++count;
                }
            scaled[static_cast<std::size_t>(y * width + x)] = count ? sum / static_cast<float>(count) : 255.0f;
        }
    }
    const float threshold = static_cast<float>(std::clamp(element.threshold, 0, 255));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float value = scaled[static_cast<std::size_t>(y * width + x)];
            const float output = value < threshold ? 0.0f : 255.0f;
            if (element.dither) {
                const float error_value = value - output;
                auto spread = [&](int dx, int dy, float weight) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && nx < width && ny < height)
                        scaled[static_cast<std::size_t>(ny * width + nx)] += error_value * weight;
                };
                spread(1, 0, 7.0f / 16.0f);
                spread(-1, 1, 3.0f / 16.0f);
                spread(0, 1, 5.0f / 16.0f);
                spread(1, 1, 1.0f / 16.0f);
            }
            if (output == 0.0f)
                fill_rect(image, left + x, top + y, left + x + 1, top + y + 1, 0);
        }
}
}

int qr_module_pixels(const QrElement& qr, const std::string& payload, double pixels_per_mm)
{
    const std::optional<qrcodegen::QrCode> code = encode_qr(qr, payload);
    if (!code) return 0;
    const int size = std::max(1, static_cast<int>(qr.size_mm * pixels_per_mm));
    return size / (code->getSize() + 2 * std::max(0, qr.quiet_zone));
}

RasterImage render_template(const TemplateDocument& document)
{
    RasterImage image{document.media.width_pixels(), document.media.height_pixels(), {}};
    image.pixels.assign(static_cast<std::size_t>(image.width * image.height), 255);
    const double scale = document.media.pixels_per_mm;
    for (const TemplateElement& element : document.elements) {
        if (element.kind == ElementKind::Text) {
            const TextElement& text = std::get<TextElement>(element.content);
            draw_text(image, text, resolve_parameters(text.text, document.parameters), scale);
        } else if (element.kind == ElementKind::QrCode) {
            const QrElement& qr = std::get<QrElement>(element.content);
            draw_qr(image, qr, resolve_parameters(qr.payload, document.parameters), scale);
        } else {
            draw_image(image, std::get<ImageElement>(element.content), scale);
        }
    }
    return image;
}

bool fit_to_label(const RasterImage& image, const PhysicalLabel& label, RasterImage& out, MediaSettings& media,
                  std::string& error)
{
    // l'etiquette physique est exprimee dans le sens de la tete d'impression (largeur) et du defilement
    const double ppmm = label.pixels_per_mm > 0.0 ? label.pixels_per_mm : 8.0;
    const int width = std::max(1, static_cast<int>(std::lround(label.width_mm * ppmm)));
    const int height = std::max(1, static_cast<int>(std::lround(label.height_mm * ppmm)));
    const int tolerance = static_cast<int>(ppmm); // 1 mm
    const auto fits = [&](int w, int h) { return w <= width + tolerance && h <= height + tolerance; };
    const auto fill_ratio = [&](int w, int h) {
        return static_cast<double>(std::min(w, width) * std::min(h, height)) / (static_cast<double>(width) * height);
    };
    // on garde le sens du modele s'il remplit l'etiquette, sinon on le tourne d'un quart de tour
    const bool straight_ok = fits(image.width, image.height);
    const bool rotated_ok = fits(image.height, image.width);
    if (!straight_ok && !rotated_ok) {
        error = "Le modèle (" + std::to_string(image.width) + "x" + std::to_string(image.height)
              + " px) est plus grand que l'étiquette réglée (" + std::to_string(width) + "x" + std::to_string(height) + " px).";
        return false;
    }
    const bool rotate = !straight_ok || (rotated_ok && fill_ratio(image.height, image.width) > fill_ratio(image.width, image.height) + 1e-6);
    RasterImage source = image;
    if (rotate) {
        RasterImage turned{image.height, image.width, std::vector<std::uint8_t>(image.pixels.size(), 255)};
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                // quart de tour horaire : (x, y) -> (H-1-y, x) ; anti-horaire : (x, y) -> (y, W-1-x)
                if (label.rotate_counterclockwise) turned.at(y, image.width - 1 - x) = image.at(x, y);
                else turned.at(image.height - 1 - y, x) = image.at(x, y);
            }
        source = std::move(turned);
    }
    if (label.flip) {
        RasterImage flipped = source;
        for (int y = 0; y < source.height; ++y)
            for (int x = 0; x < source.width; ++x)
                flipped.at(source.width - 1 - x, source.height - 1 - y) = source.at(x, y);
        source = std::move(flipped);
    }
    // centre sur l'etiquette (marges blanches si le modele est plus petit)
    out = RasterImage{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width * height), 255)};
    const int offset_x = (width - source.width) / 2;
    const int offset_y = (height - source.height) / 2;
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x) {
            const int tx = x + offset_x;
            const int ty = y + offset_y;
            if (tx >= 0 && ty >= 0 && tx < width && ty < height)
                out.at(tx, ty) = source.at(x, y);
        }
    media = MediaSettings{};
    media.width_mm = static_cast<double>(width) / ppmm;
    media.height_mm = static_cast<double>(height) / ppmm;
    media.pixels_per_mm = ppmm;
    media.orientation = Orientation::Landscape;
    return true;
}

bool write_png(const RasterImage& image, const std::string& path, std::string& error)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = "Impossible d'ouvrir le fichier PNG.";
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (!png || !info) {
        error = "Initialisation libpng impossible.";
        std::fclose(file);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        error = "Erreur d'écriture PNG.";
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        return false;
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::vector<png_bytep> rows(static_cast<std::size_t>(image.height));
    for (int y = 0; y < image.height; ++y)
        rows[static_cast<std::size_t>(y)] = const_cast<png_bytep>(&image.pixels[static_cast<std::size_t>(y * image.width)]);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    return true;
}

} // namespace qrprotec
