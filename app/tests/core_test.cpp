#include "../src/core/template.hpp"
#include "../src/core/template_io.hpp"
#include "../src/render/raster.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    qrprotec::TemplateDocument document;
    assert(document.media.width_pixels() == 320);
    assert(document.media.height_pixels() == 240);
    document.media.orientation = qrprotec::Orientation::Portrait;
    assert(document.media.width_pixels() == 240);
    assert(document.media.height_pixels() == 320);

    document.parameters["name"] = "test";
    assert(qrprotec::resolve_parameters("Hello {{name}}", document.parameters) == "Hello test");
    document.media.orientation = qrprotec::Orientation::Portrait;
    assert(document.media.width_pixels() == 240);
    assert(document.media.height_pixels() == 320);
    document.elements.push_back({"label", qrprotec::ElementKind::Text,
                                 qrprotec::TextElement{"{{name}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f}});
    const qrprotec::RasterImage image = qrprotec::render_template(document);
    assert(image.width == 240);
    assert(image.height == 320);
    bool has_ink = false;
    for (const std::uint8_t pixel : image.pixels)
        has_ink = has_ink || pixel < 255;
    assert(has_ink);
    std::string error;
    assert(qrprotec::write_png(image, "/tmp/qrprotec-test.png", error));
    std::remove("/tmp/qrprotec-test.png");
    assert(qrprotec::save_template(document, "/tmp/qrprotec-template.json", error));
    qrprotec::TemplateDocument loaded;
    assert(qrprotec::load_template(loaded, "/tmp/qrprotec-template.json", error));
    assert(loaded.parameters["name"] == "test");
    assert(loaded.elements.size() == 1);
    assert(qrprotec::find_placeholders(document).size() == 1);
    std::remove("/tmp/qrprotec-template.json");
    return 0;
}
