#include "../src/core/french.hpp"
#include "../src/core/placeholders.hpp"
#include "../src/core/template.hpp"
#include "../src/core/template_io.hpp"
#include "../src/core/update_check.hpp"
#include "../src/core/codes.hpp"
#include "../src/core/print_counter.hpp"
#include "../src/core/verif_cache.hpp"
#include "../src/net/front_config.hpp"
#include "../src/net/websocket.hpp"
#include "../src/render/label_types.hpp"
#include "../src/render/raster.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

int main()
{
    // mise a jour automatique (macOS) : versions et derniere release GitHub
    {
        using qrprotec::compare_versions;
        assert(compare_versions("1.2.10", "1.2.9") > 0);
        assert(compare_versions("v1.2.4", "1.2.4") == 0);
        assert(compare_versions("1.2", "1.2.0") == 0);
        assert(compare_versions("1.3.0", "1.10.0") < 0);
        assert(compare_versions("2.0.0", "1.99.99") > 0);
        assert(qrprotec::is_release_version("1.2.4"));
        assert(!qrprotec::is_release_version("0.0.0"));
        assert(!qrprotec::is_release_version(""));
        assert(!qrprotec::is_release_version("1.2.0^3.gabc1234"));
        assert(!qrprotec::is_release_version("1..2"));

        const std::string dmg_sha = "6cf464d7cb9c006ebbe0c9fecb90fb67b209ca45175e8bd4d21bd53ab88d2b8d";
        const std::string release = R"({"tag_name": "v1.2.5", "draft": false, "prerelease": false, "assets": [
          {"name": "QRProtec-front.flatpak", "size": 4171072, "digest": "sha256:9d5434a46dbb5cb442af72e058adc0647559fb47314c758202f862b38374e0d4",
           "browser_download_url": "https://github.com/etyloppihacilem/qrprotec/releases/download/v1.2.5/QRProtec-front.flatpak"},
          {"name": "QRProtec-macos-arm64.dmg", "size": 8094954, "digest": "sha256:)" + dmg_sha + R"(",
           "browser_download_url": "https://github.com/etyloppihacilem/qrprotec/releases/download/v1.2.5/QRProtec-macos-arm64.dmg"}]})";
        qrprotec::ReleaseAsset asset;
        std::string            error;
        assert(qrprotec::parse_latest_release(release, "QRProtec-macos-arm64.dmg", asset, error));
        assert(asset.version == "1.2.5");
        assert(asset.sha256 == dmg_sha);
        assert(asset.size == 8094954);
        assert(asset.url.find("/v1.2.5/QRProtec-macos-arm64.dmg") != std::string::npos);
        assert(!qrprotec::parse_latest_release(release, "QRProtec-macos-x86_64.dmg", asset, error));
        assert(error.find("Pas de QRProtec-macos-x86_64.dmg") == 0);
        // sans empreinte : refuse (rien ne garantirait le fichier telecharge)
        std::string no_digest = release;
        no_digest.replace(no_digest.find("sha256:" + dmg_sha), 7 + 64, "");
        assert(!qrprotec::parse_latest_release(no_digest, "QRProtec-macos-arm64.dmg", asset, error));
        std::string pre = release;
        pre.replace(pre.find("\"prerelease\": false"), 19, "\"prerelease\": true");
        assert(!qrprotec::parse_latest_release(pre, "QRProtec-macos-arm64.dmg", asset, error));
        std::string http = release;
        http.replace(http.rfind("https://github.com"), 8, "http://");
        assert(!qrprotec::parse_latest_release(http, "QRProtec-macos-arm64.dmg", asset, error));
        assert(!qrprotec::parse_latest_release("<html>", "QRProtec-macos-arm64.dmg", asset, error));
        assert(qrprotec::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    // fichier de configuration d'un front distant, chiffre par database/inventory/front_config.py
    {
        const std::string file = R"({
 "qrprotec": "front-config",
 "version": 1,
 "kdf": "PBKDF2-HMAC-SHA256",
 "iterations": 100000,
 "salt": "FiFOd3apTkjCMYBwpdgVSg==",
 "cipher": "AES-256-GCM",
 "nonce": "2zGZ7aB7Bcnv2mtv",
 "data": "y3FAgYfyg0hES03m34oc10NwiDCcP0LtW5iRhVpq8VdFuYd3qBKrEJ/Q27A3235w/mDbcdyFtpdIWokwuf7GU2DtAJcoSSxLnrIUGBnuIHPsdd526n0SfpRiy8s6AaIjHZF5LGrDxvekKjH4KSYCC+NT4u2qOtrLWPAMmMMg6G736iZcrVKI/p+EzAfDLOV1QiXibkemQLFCpSdMySvdF9Yg9vDwxoFSj0wxZ+CnLPGYz7hhZvzqzM5+iMkWqR472rJHzN00ihqW5hcIbzZbSg=="
})";
        qrprotec::FrontConfig config;
        std::string           error;
        assert(qrprotec::decrypt_front_config(file, "secret-antenne", config, error));
        assert(config.name == "accueil");
        assert(config.api_url == "https://inv.example.org");
        assert(config.api_key == "qrpf_test");
        assert(config.api_ca_pem == "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
        assert(config.created == "2026-11-07T13:00:00+01:00");
        assert(!qrprotec::decrypt_front_config(file, "mauvais", config, error));
        assert(error.find("Mot de passe incorrect") != std::string::npos);
        assert(config.api_key == "qrpf_test"); // inchange en cas d'echec
        std::string tampered = file;
        tampered.replace(tampered.find("100000"), 6, "100001");
        assert(!qrprotec::decrypt_front_config(tampered, "secret-antenne", config, error));
        tampered = file;
        tampered.replace(tampered.find("\"data\": \"y3"), 11, "\"data\": \"z3");
        assert(!qrprotec::decrypt_front_config(tampered, "secret-antenne", config, error));
        tampered = file;
        tampered.replace(tampered.find("100000"), 6, "1000");  // trop peu d'iterations : refuse avant de deriver
        assert(!qrprotec::decrypt_front_config(tampered, "secret-antenne", config, error));
        assert(error == "Fichier de configuration invalide.");
        assert(!qrprotec::decrypt_front_config("api_url=https://x", "secret-antenne", config, error));
        assert(error.find("Ce n'est pas") == 0);
    }

    // messages des verifs : le nom des lots, avec « de » accorde
    assert(de_name("Trousse de secours") == "de Trousse de secours");
    assert(de_name("Armoire 1") == "d'Armoire 1");
    assert(de_name("Étagère") == "d'Étagère");
    assert(de_name("Le sac O2") == "du sac O2");
    assert(de_names({"Sac de soin", "Sac O2"}) == "de Sac de soin ou de Sac O2");
    assert(de_names({"A1", "B", "C"}) == "d'A1, de B ou de C");

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
    // etiquette envoyee au poste par un autre ordinateur : 1 bit par pixel, aller-retour sans perte
    {
        qrprotec::RasterImage line{10, 1, std::vector<std::uint8_t>(10, 255)};
        line.pixels[0] = 0;
        line.pixels[9] = 100; // gris fonce : noir
        assert(qrprotec::encode_raster_bits(line) == "gEA="); // 0x80 0x40, meme format que label_prints.py
        qrprotec::RasterImage back;
        assert(qrprotec::decode_raster_bits("gEA=", 10, 1, back));
        assert(back.pixels[0] == 0 && back.pixels[9] == 0 && back.pixels[1] == 255);
        assert(!qrprotec::decode_raster_bits("gA==", 10, 1, back)); // trop court
        qrprotec::RasterImage again;
        assert(qrprotec::decode_raster_bits(qrprotec::encode_raster_bits(image), image.width, image.height, again));
        for (std::size_t index = 0; index < image.pixels.size(); ++index)
            assert((again.pixels[index] == 0) == (image.pixels[index] < 128));
    }
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

    // Types d'etiquettes : enregistres dans le modele, format du modele, orientation d'impression imposee
    {
        qrprotec::TemplateDocument document;
        document.name = "PET";
        document.elements.push_back({"t", qrprotec::ElementKind::Text, qrprotec::TextElement{"A", 1.0f, 1.0f, 10.0f, 5.0f, 3.0f}});
        // sans type, rien n'est ecrit : les modeles existants restent identiques
        assert(qrprotec::serialize_template(document).find("label_type") == std::string::npos);
        qrprotec::LabelType type;
        type.name = "PET \"40x30\"";
        type.width_mm = 30.0;
        type.height_mm = 40.0;
        type.rotation = 270;
        type.support = qrprotec::LabelSupport::Transfer;
        assert(type.design_width_mm() == 40.0 && type.design_height_mm() == 30.0);
        document.label_type = type;
        qrprotec::apply_label_type(document.media, type);
        assert(document.media.oriented_width_mm() == 40.0 && document.media.oriented_height_mm() == 30.0);
        assert(qrprotec::save_template(document, "/tmp/qrprotec-label-type.json", error));
        qrprotec::TemplateDocument reloaded;
        assert(qrprotec::load_template(reloaded, "/tmp/qrprotec-label-type.json", error));
        std::remove("/tmp/qrprotec-label-type.json");
        assert(reloaded.name == "PET" && reloaded.label_type && *reloaded.label_type == type);
        assert(reloaded.media.width_mm == 40.0 && reloaded.media.height_mm == 30.0); // format, pas le type
        // marge par defaut non ecrite (modeles fournis inchanges), autre marge ecrite et relue
        assert(qrprotec::serialize_template(document).find("margin_mm") == std::string::npos);
        {
            qrprotec::TemplateDocument wide = document;
            wide.label_type->margin_mm = 1.0;
            qrprotec::TemplateDocument back;
            assert(qrprotec::save_template(wide, "/tmp/qrprotec-label-margin.json", error));
            assert(qrprotec::load_template(back, "/tmp/qrprotec-label-margin.json", error));
            std::remove("/tmp/qrprotec-label-margin.json");
            assert(back.label_type && back.label_type->margin_mm == 1.0 && *back.label_type == *wide.label_type);
        }
        assert(reloaded.elements.size() == 1);
        // liste des types : un par nom, triee
        qrprotec::TemplateDocument other = reloaded;
        other.label_type->name = "B direct";
        const std::vector<qrprotec::LabelType> types =
            qrprotec::collect_label_types({reloaded, other, reloaded, qrprotec::TemplateDocument{}});
        assert(types.size() == 2 && types[0].name == "B direct" && types[1].name == type.name);
        // impression : 40 x 30 tourne d'un quart de tour anti-horaire sur une etiquette 30 x 40
        const qrprotec::RasterImage image = qrprotec::render_template(reloaded);
        assert(image.width == 320 && image.height == 240);
        qrprotec::RasterImage out;
        qrprotec::MediaSettings media;
        assert(qrprotec::fit_to_label(image, qrprotec::physical_label_for(type, 8.0), out, media, error));
        assert(out.width == 240 && out.height == 320);
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                assert(out.at(y, image.width - 1 - x) == image.at(x, y));
        // modele plus grand que son type : refuse
        type.width_mm = 20.0;
        assert(!qrprotec::fit_to_label(image, qrprotec::physical_label_for(type, 8.0), out, media, error));
        // modele sans type : type construit d'apres les etiquettes reglees, meme orientation qu'avant
        qrprotec::PhysicalLabel roll; // 40 x 30
        qrprotec::MediaSettings landscape;
        const qrprotec::LabelType straight = qrprotec::default_label_type(landscape, roll, qrprotec::LabelSupport::Direct);
        assert(straight.name == "40 x 30 mm" && straight.rotation == 0 && straight.width_mm == 40.0);
        qrprotec::MediaSettings portrait;
        portrait.orientation = qrprotec::Orientation::Portrait; // 30 x 40 : tourne d'un quart de tour
        roll.rotate_counterclockwise = true;
        const qrprotec::LabelType turned = qrprotec::default_label_type(portrait, roll, qrprotec::LabelSupport::Transfer);
        assert(turned.rotation == 270 && turned.support == qrprotec::LabelSupport::Transfer);
        assert(turned.name == "40 x 30 mm, quart de tour anti-horaire, ruban");
        roll.flip = true;
        assert(qrprotec::default_label_type(landscape, roll, qrprotec::LabelSupport::Direct).rotation == 180);
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
    // cache de vérif (inventory/verif_cache.py) : seaux complets puis seulement les seaux changes
    {
        qrprotec::VerifCache cache;
        assert(!cache.loaded());
        assert(cache.request()["buckets"].size() == 0);
        const auto first = qrprotec::Json::parse(R"({"buckets": {
          "3": {"hash": "aaa", "records": {"i:compre1": {"iid": "compre1", "type_name": "Compresses", "reported": false},
                                          "i:compre2": {"iid": "compre2", "type_name": "Compresses"}}},
          "7": {"hash": "bbb", "records": {"l:bplus1": {"id": "bplus1", "name": "B+ 1", "items": [], "missing_items": [],
              "descendants": [{"id": "soin1", "name": "Sac de soin", "items": ["compre1", "compre2"], "missing_items": [],
                               "optional_items": ["compre2"]}]},
                                          "t:compre": {"type": "compre", "name": "Compresses"}}},
          "9": {"hash": "ccc", "records": {}}}})");
        assert(cache.apply(first).size() == 4);
        assert(cache.loaded() && cache.size() == 4);
        assert(cache.request()["buckets"]["3"].str() == "aaa");
        assert(cache.root_of("soin1") == "bplus1" && cache.root_of("bplus1") == "bplus1" && cache.root_of("x").empty());
        const qrprotec::Json lot = cache.lot("bplus1");
        const qrprotec::Json &bag = lot["descendants"][0];
        assert(bag["items"].size() == 2 && bag["items"][0]["type_name"].str() == "Compresses");
        assert(!bag["items"][0]["optional"].boolean() && bag["items"][1]["optional"].boolean());
        assert(cache.type("compre") && !cache.pack("x"));
        // seau 3 change : compre1 signale, compre2 disparu ; seau 7 inchange (pas renvoye)
        const auto changed = cache.apply(qrprotec::Json::parse(R"({"buckets": {
          "3": {"hash": "ddd", "records": {"i:compre1": {"iid": "compre1", "type_name": "Compresses", "reported": true}}}}})"));
        assert(changed.size() == 2);
        assert((*cache.item("compre1"))["reported"].boolean() && !cache.item("compre2"));
        assert(cache.request()["buckets"]["3"].str() == "ddd" && cache.request()["buckets"]["7"].str() == "bbb");
        const qrprotec::Json after = cache.lot("bplus1");
        assert(after["descendants"][0]["items"][1]["iid"].str() == "compre2"); // detail perdu, iid garde

        // file d'attente des envois, gardee dans un fichier
        const auto file = std::filesystem::temp_directory_path() / "qrprotec-outbox-test.json";
        std::filesystem::remove(file);
        {
            qrprotec::VerifOutbox outbox;
            outbox.load(file);
            assert(outbox.empty());
            outbox.push({ qrprotec::Json::parse(R"({"lots": [{"id": "soin1"}], "step": true})"), "Sac de soin", "13:02" });
            outbox.push({ qrprotec::Json::parse(R"({"lots": [{"id": "bplus1"}]})"), "B+ 1", "13:05" });
        }
        qrprotec::VerifOutbox again;
        again.load(file);
        assert(again.size() == 2 && again.front().label == "Sac de soin" && again.front().body["step"].boolean());
        again.pop_front();
        qrprotec::VerifOutbox last;
        last.load(file);
        assert(last.size() == 1 && last.front().label == "B+ 1");
        std::filesystem::remove(file);
    }

    {
        // compteur d'impressions : par jour et categorie, lot fige pendant l'envoi, garde dans un fichier
        const auto file = std::filesystem::temp_directory_path() / "qrprotec-print-counts-test.json";
        std::filesystem::remove(file);
        using qrprotec::LabelSupport;
        std::string id;
        {
            qrprotec::PrintCounter counter;
            counter.load(file);
            assert(counter.pending() == 0 && counter.next_batch().is_null());
            counter.add("2026-11-12", 40.0, 30.0, LabelSupport::Direct);
            counter.add("2026-11-12", 40.04, 30.0, LabelSupport::Direct, 2); // meme categorie (au dixieme)
            counter.add("2026-11-12", 60.0, 40.0, LabelSupport::Transfer);
            assert(counter.pending() == 4);
            const qrprotec::Json batch = counter.next_batch();
            id                         = batch["batch"].str();
            assert(id.size() == 32 && batch["counts"].size() == 2);
            assert(batch["counts"][0]["count"].integer() == 3 && batch["counts"][1]["support"].str() == "transfer");
            counter.add("2026-11-13", 40.0, 30.0, LabelSupport::Direct); // pendant l'envoi : lot suivant
            assert(counter.next_batch()["batch"].str() == id && counter.next_batch()["counts"].size() == 2);
        }
        qrprotec::PrintCounter again; // redemarrage : meme lot a renvoyer, impression d'apres gardee
        again.load(file);
        assert(again.pending() == 5 && again.next_batch()["batch"].str() == id);
        again.batch_done("autre");
        assert(again.pending() == 5);
        again.batch_done(id);
        assert(again.pending() == 1);
        const qrprotec::Json next = again.next_batch();
        assert(next["batch"].str() != id && next["counts"][0]["day"].str() == "2026-11-13");
        again.batch_done(next["batch"].str());
        assert(again.pending() == 0 && again.next_batch().is_null());
        assert(qrprotec::local_day_iso().size() == 10);
        std::filesystem::remove(file);
    }

    {
        // codes d'action rapide des postes : rouleau en place, ignores par le web (pas une URL)
        qrprotec::LabelType roll;
        roll.name      = "60 x 40 mm, transfert";
        roll.width_mm  = 60.0;
        roll.height_mm = 40.0;
        roll.support   = qrprotec::LabelSupport::Transfer;
        const std::string code = qrprotec::roll_action_code(roll);
        assert(code == "qrprotec:poste/rouleau?t=60%20x%2040%20mm%2C%20transfert&w=60&h=40&s=transfer");
        const qrprotec::ParsedScan scan = qrprotec::parse_scan(code);
        assert(scan.kind == qrprotec::ScanKind::PosteAction && scan.id == "rouleau");
        assert(scan.params.at("t") == roll.name && scan.params.at("s") == "transfer" && scan.params.at("w") == "60");
        assert(qrprotec::describe_scan(scan) == "Rouleau en place : « 60 x 40 mm, transfert »");
        assert(qrprotec::parse_scan("QRPROTEC:Poste/Rouleau").id == "rouleau");
        assert(qrprotec::parse_scan("qrprotec:poste/").kind == qrprotec::ScanKind::Unknown);
        // etiquette du code, imprimee sur un autre rouleau choisi a la main
        qrprotec::LabelType small;
        small.name      = "40 x 30 mm";
        const qrprotec::TemplateDocument document = qrprotec::roll_code_document(roll, small);
        assert(document.label_type && document.label_type->name == "40 x 30 mm");
        assert(std::get< qrprotec::QrElement >(document.elements[0].content).payload == code);
        assert(qrprotec::validate(document).empty());
        const qrprotec::RasterImage image = qrprotec::render_template(document);
        assert(image.width == 320 && image.height == 240);
        if (const char *out = std::getenv("QRPROTEC_ROLL_PNG")) {
            std::string error;
            qrprotec::write_png(image, std::string(out) + "-40x30.png", error);
            qrprotec::write_png(qrprotec::render_template(qrprotec::roll_code_document(small, roll)),
                                std::string(out) + "-60x40.png", error);
        }
    }

    // etiquette d'etalonnage : cadre au bord et croix au centre, a la taille du rouleau
    {
        qrprotec::LabelType roll;
        roll.name = "60 x 40 mm";
        roll.width_mm = 60.0;
        roll.height_mm = 40.0;
        const qrprotec::TemplateDocument typed = qrprotec::calibration_document(roll, true);
        assert(typed.label_type && typed.label_type->name == "60 x 40 mm");
        assert(!qrprotec::calibration_document(roll, false).label_type);
        assert(qrprotec::validate(typed).empty());
        const qrprotec::RasterImage image = qrprotec::calibration_image(typed, roll.effective_margin_mm());
        assert(image.width == 480 && image.height == 320);
        // marge de 0,5 mm par defaut : bord blanc, cadre plein a 4 points du bord
        assert(roll.margin_mm == 0.5 && image.at(0, 0) >= 128 && image.at(2, 80) >= 128);
        assert(image.at(4, 4) < 128 && image.at(475, 315) < 128 && image.at(475, 4) < 128 && image.at(4, 315) < 128);
        // sans marge : cadre au ras du bord
        const qrprotec::RasterImage edge = qrprotec::calibration_image(typed, 0.0);
        assert(edge.at(0, 0) < 128 && edge.at(479, 319) < 128);
        // impression sur le type : rien en dehors du cadre des marges, meme un modele noir jusqu'au bord
        {
            qrprotec::RasterImage black{480, 320, std::vector<std::uint8_t>(480 * 320, 0)};
            qrprotec::RasterImage out;
            qrprotec::MediaSettings media;
            std::string error;
            assert(qrprotec::fit_to_label(black, qrprotec::physical_label_for(roll, 8.0), out, media, error));
            assert(out.at(0, 0) == 255 && out.at(3, 160) == 255 && out.at(476, 160) == 255 && out.at(240, 319) == 255);
            assert(out.at(4, 4) == 0 && out.at(475, 315) == 0);
            qrprotec::LabelType none = roll;
            none.margin_mm = 0.0;
            assert(qrprotec::fit_to_label(black, qrprotec::physical_label_for(none, 8.0), out, media, error));
            assert(out.at(0, 0) == 0);
        }
        assert(image.at(240, 160) < 128 && image.at(240, 5) < 128 && image.at(5, 160) < 128); // croix
        assert(image.at(360, 80) >= 128 && image.at(420, 240) >= 128); // quarts vides (mode d'emploi en bas a gauche)
        roll.rotation = 90; // rouleau imprime en portrait : croix au centre du modele tourne
        const qrprotec::RasterImage turned = qrprotec::calibration_image(qrprotec::calibration_document(roll, true), 0.5);
        assert(turned.width == 320 && turned.height == 480 && turned.at(160, 240) < 128);
        if (const char *out = std::getenv("QRPROTEC_CALIBRATION_PNG")) {
            std::string error;
            qrprotec::write_png(image, std::string(out) + "-60x40.png", error);
            qrprotec::LabelType small;
            small.name = "40 x 30 mm";
            qrprotec::write_png(qrprotec::calibration_image(qrprotec::calibration_document(small, true), 0.5),
                                std::string(out) + "-40x30.png", error);
        }
    }

    return 0;
}
