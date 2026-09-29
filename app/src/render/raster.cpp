#include "raster.hpp"
#include "image_loader.hpp"
#include "../core/fonts.hpp"

#include "qrcodegen.hpp"

#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <array>
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
    const int text_width = std::max(1, static_cast<int>(text.width_mm * scale));
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&library) != 0) return;
    const std::string& font_path = ui_font_path();
    if (font_path.empty() || FT_New_Face(library, font_path.c_str(), 0, &face) != 0) face = nullptr;
    if (face == nullptr) {
        if (library) FT_Done_FreeType(library);
        return;
    }
    const int pixel_size = std::max(1, static_cast<int>(text.font_size_mm * scale));
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));
    int pen_x = left;
    int pen_y = top + pixel_size;
    const int line_height = pixel_size + 2;
    const int right = left + text_width;
    const int bottom = top + static_cast<int>(text.height_mm * scale);
    const std::vector<char32_t> characters = decode_utf8(value);
    const auto advance = [&](char32_t character) {
        return FT_Load_Char(face, character, FT_LOAD_DEFAULT) == 0 ? static_cast<int>(face->glyph->advance.x >> 6) : 0;
    };
    for (std::size_t index = 0; index < characters.size(); ++index) {
        const char32_t character = characters[index];
        if (character == '\n') { pen_x = left; pen_y += line_height; continue; }
        // retour a la ligne avant un mot qui ne tient pas (coupure au milieu seulement s'il est trop long)
        if (character != ' ' && (index == 0 || characters[index - 1] == ' ' || characters[index - 1] == '\n')) {
            int word = 0;
            for (std::size_t end = index; end < characters.size() && characters[end] != ' ' && characters[end] != '\n'; ++end)
                word += advance(characters[end]);
            if (pen_x > left && pen_x + word > right) { pen_x = left; pen_y += line_height; }
        }
        if (character == ' ' && pen_x == left) continue; // pas d'espace en debut de ligne
        if (FT_Load_Char(face, character, FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot glyph = face->glyph;
        if (pen_x + static_cast<int>(glyph->bitmap.width) > right && pen_x > left) {
            pen_x = left;
            pen_y += line_height;
        }
        if (pen_y - glyph->bitmap_top >= bottom) break;
        for (unsigned int y = 0; y < glyph->bitmap.rows; ++y)
            for (unsigned int x = 0; x < glyph->bitmap.width; ++x)
                if (glyph->bitmap.buffer[y * glyph->bitmap.pitch + x] >= 128)
                    fill_rect(image, pen_x + glyph->bitmap_left + static_cast<int>(x),
                              pen_y - glyph->bitmap_top + static_cast<int>(y),
                              pen_x + glyph->bitmap_left + static_cast<int>(x) + 1,
                              pen_y - glyph->bitmap_top + static_cast<int>(y) + 1, 0);
        pen_x += glyph->advance.x >> 6;
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
}

void draw_qr(RasterImage& image, const QrElement& qr, const std::string& payload, double scale)
{
    if (payload.empty()) return;
    const int left = static_cast<int>(qr.x_mm * scale);
    const int top = static_cast<int>(qr.y_mm * scale);
    const int size = std::max(1, static_cast<int>(qr.size_mm * scale));
    qrcodegen::QrCode code = qrcodegen::QrCode::encodeText("", qrcodegen::QrCode::Ecc::LOW);
    try {
        code = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    } catch (const std::exception&) {
        return; // payload trop long pour un QR code
    }
    const int modules = code.getSize();
    const int module_size = std::max(1, size / modules);
    // centre le QR dans la zone reservee (le reste sert de marge blanche)
    const int offset = std::max(0, (size - module_size * modules) / 2);
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (code.getModule(x, y))
                fill_rect(image, left + offset + x * module_size, top + offset + y * module_size,
                          left + offset + (x + 1) * module_size, top + offset + (y + 1) * module_size, 0);
}

void draw_image(RasterImage& image, const ImageElement& element, double scale)
{
    std::string error;
    const std::shared_ptr<const GrayImage> source = load_gray_image(element.path, error);
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
