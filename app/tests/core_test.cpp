#include "../src/core/placeholders.hpp"
#include "../src/core/template.hpp"
#include "../src/core/template_io.hpp"
#include "../src/net/websocket.hpp"
#include "../src/render/raster.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

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
    // Prefixe / suffixe imprimes seulement si la valeur n'est pas vide
    {
        std::unordered_map<std::string, std::string> values{{"peremption", "31/12/2027"}, {"vide", ""}};
        assert(qrprotec::resolve_parameters("{[Exp. ]{peremption}}", values) == "Exp. 31/12/2027");
        assert(qrprotec::resolve_parameters("{[Exp. ]{vide}}", values) == "");
        assert(qrprotec::resolve_parameters("A{{peremption}[ inclus]}B", values) == "A31/12/2027 inclusB");
        assert(qrprotec::resolve_parameters("A{{vide}[ inclus]}B", values) == "AB");
        assert(qrprotec::resolve_parameters("{[(]{peremption}[)]}", values) == "(31/12/2027)");
        assert(qrprotec::resolve_parameters("{[(]{vide}[)]}", values) == "");
        assert(qrprotec::resolve_parameters("{{vide}}x", values) == "x");
        assert(qrprotec::resolve_parameters("{[Exp. ]{inconnu}}", values) == "{[Exp. ]{inconnu}}");
        assert(qrprotec::resolve_parameters("{{{peremption}}}", values) == "{31/12/2027}");
        assert(qrprotec::resolve_parameters("{a} [b] {[c]", values) == "{a} [b] {[c]");
        // ![remplacement] imprime seul quand la valeur est vide, apres le suffixe ou dans les accolades du nom
        assert(qrprotec::resolve_parameters("{{vide}![Non périssable]}", values) == "Non périssable");
        assert(qrprotec::resolve_parameters("{{peremption}![Non périssable]}", values) == "31/12/2027");
        assert(qrprotec::resolve_parameters("{[Exp. ]{vide}[ !]![-]}", values) == "-");
        assert(qrprotec::resolve_parameters("{[Exp. ]{peremption}[ !]![-]}", values) == "Exp. 31/12/2027 !");
        assert(qrprotec::resolve_parameters("{{vide[ inclus]![aucune]}}", values) == "aucune");
        assert(qrprotec::resolve_parameters("{{peremption[ inclus]![aucune]}}", values) == "31/12/2027 inclus");
        assert(qrprotec::resolve_parameters("{{vide![]}}x", values) == "x");
        assert(qrprotec::resolve_parameters("{{vide}!}", values) == "{{vide}!}");
        assert(qrprotec::resolve_parameters("{{vide[a]}[b]}", values) == "{{vide[a]}[b]}");
        assert(qrprotec::resolve_parameters("Attention ! {{peremption}}!", values) == "Attention ! 31/12/2027!");
        values["peremption"] = "{{vide}}"; // une valeur n'est jamais re-substituee
        assert(qrprotec::resolve_parameters("{{peremption}}", values) == "{{vide}}");
        qrprotec::TemplateDocument conditional;
        conditional.elements.push_back({"t", qrprotec::ElementKind::Text,
                                        qrprotec::TextElement{"{[Exp. ]{peremption}} {{lot}[ :]} {{autre![-]}}", 2.0f, 2.0f, 36.0f, 8.0f, 3.0f}});
        assert((qrprotec::find_placeholders(conditional) == std::vector<std::string>{"autre", "lot", "peremption"}));
    }
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
    {
        // crochets dans une chaine ("{[Exp. ]{peremption}}") : le tableau des elements
        // ne s'arretait plus au bon ']' et la relecture perdait tout apres le 2e element
        std::FILE* file = std::fopen("/tmp/qrprotec-template-crochets.json", "w");
        assert(file != nullptr);
        std::fputs(R"({
  "version": 2,
  "name": "Item - tâches",
  "category": "task_item",
  "media": {"width_mm": 40, "height_mm": 30, "pixels_per_mm": 8, "orientation": "landscape"},
  "parameters": {"peremption": "28/12/2026", "tasks": "□ Voyant vert [ok]\n□ Électrodes", "type_name": "DAE"},
  "elements": [
    {"id": "qr", "kind": "qr", "payload": "{{task_url}}", "x_mm": 1, "y_mm": 1, "size_mm": 15.5, "ecc": 0, "quiet_zone": 1},
    {"id": "type", "kind": "text", "text": "{{type_name}}", "x_mm": 16.5, "y_mm": 5, "width_mm": 22.5, "height_mm": 8.5, "font_size_mm": 2.6, "bold": 1, "align": "left"},
    {"id": "exp", "kind": "text", "text": "{[Exp. ]{peremption}}", "x_mm": 16.5, "y_mm": 13.5, "width_mm": 22.5, "height_mm": 3, "font_size_mm": 2.2, "bold": 0, "align": "left"},
    {"id": "tasks", "kind": "text", "text": "{{tasks}[ \"]\" ]}", "x_mm": 1, "y_mm": 19, "width_mm": 38, "height_mm": 10, "font_size_mm": 2, "bold": 0, "align": "left"},
    {"id": "image", "kind": "image", "path": "images/protection-civile.png", "x_mm": 31, "y_mm": 0.5, "width_mm": 8, "height_mm": 4.5, "dither": 1, "threshold": 128}
  ]
}
)", file);
        std::fclose(file);
        qrprotec::TemplateDocument brackets;
        assert(qrprotec::load_template(brackets, "/tmp/qrprotec-template-crochets.json", error));
        assert(brackets.elements.size() == 5);
        assert(brackets.elements[2].id == "exp");
        assert(std::get<qrprotec::TextElement>(brackets.elements[2].content).text == "{[Exp. ]{peremption}}");
        assert(std::get<qrprotec::TextElement>(brackets.elements[3].content).text == "{{tasks}[ \"]\" ]}");
        assert(brackets.elements[4].kind == qrprotec::ElementKind::Image);
        // et l'aller-retour par save_template garde tout
        assert(qrprotec::save_template(brackets, "/tmp/qrprotec-template-crochets.json", error));
        qrprotec::TemplateDocument reloaded;
        assert(qrprotec::load_template(reloaded, "/tmp/qrprotec-template-crochets.json", error));
        assert(reloaded.elements.size() == 5);
        std::remove("/tmp/qrprotec-template-crochets.json");
    }

    // valeur d'apercu de {{tasks}} : saisie ligne par ligne, carre ajoute devant chaque ligne
    assert(qrprotec::tasks_value("Voyant vert\nÉlectrodes") == "□ Voyant vert\n□ Électrodes");
    assert(qrprotec::tasks_value("Voyant vert\n") == "□ Voyant vert\n"); // Entree : pas de carre seul
    assert(qrprotec::tasks_value("□ deja\n□sans espace") == "□ deja\n□sans espace");
    assert(qrprotec::task_lines("□ Voyant vert\n□ Électrodes") == "Voyant vert\nÉlectrodes");
    assert(qrprotec::task_lines("□sans espace\nlibre") == "sans espace\nlibre");
    assert(qrprotec::tasks_value(qrprotec::task_lines("□ a\n\n□ b")) == "□ a\n\n□ b");

    // Rotation par quart de tour : le meme code-barres couche est la transposee tournee du code a plat, pose au
    // coin haut-gauche de sa zone ; la rotation survit a l'enregistrement
    {
        qrprotec::TemplateDocument flat;
        flat.media.width_mm = 40.0;
        flat.media.height_mm = 40.0;
        flat.elements.push_back({"code", qrprotec::ElementKind::Barcode,
                                 qrprotec::BarcodeElement{"ABC123", 5.0f, 10.0f, 30.0f, 10.0f, 2}});
        qrprotec::TemplateDocument turned = flat;
        turned.elements[0].rotation = 90;
        const qrprotec::RasterImage a = qrprotec::render_template(flat);
        const qrprotec::RasterImage b = qrprotec::render_template(turned);
        const int w = 30 * 8, h = 10 * 8, left = 5 * 8, top = 10 * 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                assert(a.at(left + x, top + y) == b.at(left + h - 1 - y, top + x));
        assert(qrprotec::validate(turned).empty()); // couche : 10 x 30 mm, tient dans l'etiquette
        turned.elements[0].rotation = -90;
        assert(qrprotec::normalize_rotation(turned.elements[0].rotation) == 270);
        assert(qrprotec::save_template(turned, "/tmp/qrprotec-rotation.json", error));
        qrprotec::TemplateDocument reloaded;
        assert(qrprotec::load_template(reloaded, "/tmp/qrprotec-rotation.json", error));
        assert(reloaded.elements.size() == 1 && reloaded.elements[0].rotation == 270);
        std::remove("/tmp/qrprotec-rotation.json");
        // sans rotation, rien n'est ecrit : les modeles existants restent identiques
        assert(qrprotec::serialize_template(flat).find("rotation") == std::string::npos);
    }

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
