#include "raster.hpp"

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

void draw_text(RasterImage& image, const TextElement& text, const std::string& value, double scale)
{
    const int left = static_cast<int>(text.x_mm * scale);
    const int top = static_cast<int>(text.y_mm * scale);
    const int text_width = std::max(1, static_cast<int>(text.width_mm * scale));
    const std::array<const char*, 4> font_paths = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&library) != 0) return;
    for (const char* font_path : font_paths) {
        if (FT_New_Face(library, font_path, 0, &face) == 0) break;
    }
    if (face == nullptr) {
        if (library) FT_Done_FreeType(library);
        return;
    }
    const int pixel_size = std::max(1, static_cast<int>(text.font_size_mm * scale));
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));
    int pen_x = left;
    int pen_y = top + pixel_size;
    const int line_height = pixel_size + 2;
    for (const unsigned char character : value) {
        if (character == '\n') { pen_x = left; pen_y += pixel_size + 2; continue; }
        if (FT_Load_Char(face, character, FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot glyph = face->glyph;
        if (pen_x + glyph->bitmap.width > left + text_width) {
            pen_x = left;
            pen_y += line_height;
        }
        if (pen_y - glyph->bitmap_top >= top + static_cast<int>(text.height_mm * scale)) break;
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

void draw_diagnostic_qr(RasterImage& image, const QrElement& qr, const std::string& payload, double scale)
{
    const int left = static_cast<int>(qr.x_mm * scale);
    const int top = static_cast<int>(qr.y_mm * scale);
    const int size = std::max(1, static_cast<int>(qr.size_mm * scale));
    const int modules = 21;
    const int module_size = std::max(1, size / modules);
    const auto module = [&](int x, int y) { return ((x * 31 + y * 17 + static_cast<int>(payload.size()) * 13) % 7) < 3; };
    const auto finder = [&](int origin_x, int origin_y) {
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                if (x == 0 || y == 0 || x == 6 || y == 6 || (x >= 2 && x <= 4 && y >= 2 && y <= 4))
                    fill_rect(image, left + (origin_x + x) * module_size, top + (origin_y + y) * module_size,
                              left + (origin_x + x + 1) * module_size, top + (origin_y + y + 1) * module_size, 0);
    };
    finder(0, 0);
    finder(modules - 7, 0);
    finder(0, modules - 7);
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (module(x, y) && !(x < 8 && y < 8) && !(x >= modules - 8 && y < 8) && !(x < 8 && y >= modules - 8))
                fill_rect(image, left + x * module_size, top + y * module_size,
                          left + (x + 1) * module_size, top + (y + 1) * module_size, 0);
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
        } else {
            const QrElement& qr = std::get<QrElement>(element.content);
            draw_diagnostic_qr(image, qr, resolve_parameters(qr.payload, document.parameters), scale);
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
        error = "Erreur d'ecriture PNG.";
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
