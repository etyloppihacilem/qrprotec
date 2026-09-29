#include "../src/core/template.hpp"
#include "../src/core/template_io.hpp"
#include "../src/net/websocket.hpp"
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

    // WebSocket : base64 (cle de la poignee de main) et trames masquees / non masquees
    assert(qrprotec::base64_encode("") == "");
    assert(qrprotec::base64_encode("f") == "Zg==");
    assert(qrprotec::base64_encode("fo") == "Zm8=");
    assert(qrprotec::base64_encode("foo") == "Zm9v");
    const std::uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (const std::size_t length : {std::size_t(5), std::size_t(300), std::size_t(70000)}) {
        const std::string payload(length, 'a');
        const std::string encoded = qrprotec::ws_encode_frame(1, payload, mask);
        qrprotec::WebSocketFrame frame;
        assert(qrprotec::ws_decode_frame(encoded.substr(0, encoded.size() - 1), frame) == 0); // incomplete
        assert(qrprotec::ws_decode_frame(encoded, frame) == static_cast<long>(encoded.size()));
        assert(frame.final && frame.opcode == 1 && frame.payload == payload);
    }
    const std::string server_frame = std::string("\x81\x05", 2) + "hello"; // serveur : non masquee
    qrprotec::WebSocketFrame frame;
    assert(qrprotec::ws_decode_frame(server_frame + "rest", frame) == 7 && frame.payload == "hello");
    return 0;
}
