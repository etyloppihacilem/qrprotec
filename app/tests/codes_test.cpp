#include "core/codes.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/placeholders.hpp"
#include "core/search.hpp"
#include "core/template.hpp"
#include "core/template_io.hpp"
#include "net/http.hpp"
#include "render/raster.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace qrprotec;

static void test_json() {
  std::string error;
  const Json  value = Json::parse(R"({"a": 1, "b": [true, null, "xé\n"], "c": {"d": -2.5}})", &error);
  assert(error.empty());
  assert(value["a"].integer() == 1);
  assert(value["b"].size() == 3);
  assert(value["b"][0].boolean());
  assert(value["b"][1].is_null());
  assert(value["b"][2].str() == "x\xc3\xa9\n");
  assert(value["c"]["d"].num() == -2.5);
  assert(value["missing"]["deep"].str("def") == "def");
  Json object;
  object["name"] = "S\xc3\xa9rum \"phy\"";
  object["count"] = 3;
  object["list"].push_back("a");
  const Json round = Json::parse(object.dump(), &error);
  assert(error.empty());
  assert(round["name"].str() == object["name"].str());
  assert(round["count"].integer() == 3);
  assert(round["list"][0].str() == "a");
  Json::parse("{\"a\":", &error);
  assert(!error.empty());
}

static void test_dates() {
  const auto date = Date::parse("2027-02-28");
  assert(date && date->plus_days(1) == (Date{ 2027, 3, 1 }));
  assert(Date::parse("31/12/2026")->iso() == "2026-12-31");
  assert(Date::parse("20261231")->display() == "31/12/2026");
  assert(!Date::parse("2026-02-30"));
  assert(Date::parse("2026-09-29T10:00:00Z")->iso() == "2026-09-29");
  assert((Date{ 2026, 1, 1 }.plus_days(-1) == Date{ 2025, 12, 31 }));
}

static void test_scans() {
  const ParsedScan item = parse_scan("compre20261231000000A1\n");
  assert(item.kind == ScanKind::Item);
  assert(item.item_type == "compre");
  assert(item.peremption && item.peremption->iso() == "2026-12-31");
  assert(is_expired(item, Date{ 2027, 1, 1 }));
  assert(!is_expired(item, Date{ 2026, 12, 31 }));

  const ParsedScan garrot = parse_scan("garrot00000000000000A1");
  assert(garrot.kind == ScanKind::Item && !garrot.peremption);
  assert(!is_expired(garrot, Date{ 2100, 1, 1 }));

  const ParsedScan lot = parse_scan("https://example.com/verif?lot=sacpse00000001&key=abc%2Bd");
  assert(lot.kind == ScanKind::Lot && lot.id == "sacpse00000001" && lot.key == "abc+d");
  const ParsedScan lot_public = parse_scan("https://autre-domaine.fr/app/verif/?lot=sacpse00000001");
  assert(lot_public.kind == ScanKind::Lot && lot_public.key.empty());
  const ParsedScan badge = parse_scan("https://example.com/badge?m=M001&key=K");
  assert(badge.kind == ScanKind::User && badge.id == "M001" && badge.key == "K");
  const ParsedScan pack = parse_scan("https://example.com/pack?id=0000002B");
  assert(pack.kind == ScanKind::SealedPack && pack.id == "0000002B");
  assert(parse_scan("n'importe quoi").kind == ScanKind::Unknown);
  assert(parse_scan("compre2026123100000!A1").kind == ScanKind::Unknown);
}

static void test_http() {
  HttpUrl     url;
  std::string error;
  assert(parse_http_url("http://127.0.0.1:8001", url, error) && url.host == "127.0.0.1" && url.port == 8001);
  assert(parse_http_url("http://serveur/qrprotec/", url, error) && url.port == 80 && url.base_path == "/qrprotec");
  assert(!parse_http_url("https://serveur", url, error));
  HttpResponse response;
  assert(parse_http_response("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\n{}", response));
  assert(response.status == 201 && response.body == "{}");
  assert(parse_http_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", response));
  assert(response.body == "abcde");
}

static void test_qr_and_categories() {
  TemplateDocument document;
  document.category = TemplateCategory::Item;
  document.parameters = example_parameters(TemplateCategory::Item);
  document.elements.push_back({ "qr", ElementKind::QrCode, QrElement{ "{{iid}}", 2.0f, 2.0f, 20.0f } });
  const RasterImage image = render_template(document);
  // QR dans sa zone (20 mm) : de l'encre au centre, et une marge blanche de 2 modules sur le bord
  const int origin = static_cast< int >(2.0 * document.media.pixels_per_mm);
  const int size   = static_cast< int >(20.0 * document.media.pixels_per_mm);
  const int module = qr_module_pixels(std::get< QrElement >(document.elements[0].content),
                                      document.parameters["iid"], document.media.pixels_per_mm);
  assert(module >= 3);
  bool ink = false, margin_white = true;
  for (int y = origin; y < origin + size; ++y)
    for (int x = origin; x < origin + size; ++x) {
      ink = ink || image.at(x, y) == 0;
      if (x < origin + 2 * module || y < origin + 2 * module)
        margin_white = margin_white && image.at(x, y) == 255;
    }
  assert(ink && margin_white);

  std::string error;
  document.elements.push_back({ "logo", ElementKind::Image, ImageElement{ "logo.png", 1, 1, 5, 5, false, 100 } });
  assert(save_template(document, "/tmp/qrprotec-category.qr", error));
  TemplateDocument loaded;
  assert(load_template(loaded, "/tmp/qrprotec-category.qr", error));
  assert(loaded.category == TemplateCategory::Item);
  assert(loaded.elements.size() == 2);
  assert(loaded.elements[1].kind == ElementKind::Image);
  const ImageElement &logo = std::get< ImageElement >(loaded.elements[1].content);
  assert(logo.path == "logo.png" && !logo.dither && logo.threshold == 100);
  std::remove("/tmp/qrprotec-category.qr");
  assert(category_from_id("lot_private") == TemplateCategory::LotPrivate);
}

static void test_text_and_label_fit() {
  // gras + centre : persistance et rendu
  TemplateDocument document;
  TextElement      title{ "PROTECTION CIVILE\nPARIS CENTRE", 1.0f, 1.0f, 38.0f, 10.0f, 3.0f };
  title.bold  = true;
  title.align = TextAlign::Center;
  document.elements.push_back({ "titre", ElementKind::Text, title });
  std::string error;
  assert(save_template(document, "/tmp/qrprotec-bold.qr", error));
  TemplateDocument loaded;
  assert(load_template(loaded, "/tmp/qrprotec-bold.qr", error));
  const TextElement &text = std::get< TextElement >(loaded.elements[0].content);
  assert(text.bold && text.align == TextAlign::Center);
  // plusieurs valeurs d'apercu (dont une sur deux lignes) relues sans melange
  document.parameters = { { "iid", "compre20271231000000A1" }, { "titre", "A\nB" }, { "type_name", "Sérum \"phy\"" } };
  assert(save_template(document, "/tmp/qrprotec-bold.qr", error));
  assert(load_template(loaded, "/tmp/qrprotec-bold.qr", error));
  assert(loaded.parameters.size() == 3 && loaded.parameters["titre"] == "A\nB");
  assert(loaded.parameters["type_name"] == "Sérum \"phy\"" && loaded.parameters["iid"] == "compre20271231000000A1");
  std::remove("/tmp/qrprotec-bold.qr");
  const RasterImage image = render_template(document);
  // texte centre : de l'encre des deux cotes du milieu, marges vides a gauche
  int leftmost = image.width;
  for (int y = 0; y < 60; ++y)
    for (int x = 0; x < image.width; ++x)
      if (image.at(x, y) == 0)
        leftmost = std::min(leftmost, x);
  assert(leftmost > 20 && leftmost < image.width / 2);
  // bold plus epais que normal
  TemplateDocument regular = document;
  std::get< TextElement >(regular.elements[0].content).bold = false;
  const RasterImage thin = render_template(regular);
  int ink_bold = 0, ink_regular = 0;
  for (std::size_t i = 0; i < image.pixels.size(); ++i) {
    ink_bold += image.pixels[i] == 0;
    ink_regular += thin.pixels[i] == 0;
  }
  assert(ink_bold > ink_regular);

  // modele portrait (30x40) sur etiquette paysage 40x30 : quart de tour automatique
  TemplateDocument portrait;
  portrait.media.orientation = Orientation::Portrait;
  RasterImage marker = render_template(portrait);
  assert(marker.width == 240 && marker.height == 320);
  marker.at(0, 0) = 0; // coin haut gauche du modele
  PhysicalLabel label; // 40 x 30 mm
  RasterImage   out;
  MediaSettings media;
  assert(fit_to_label(marker, label, out, media, error));
  assert(out.width == 320 && out.height == 240 && media.width_pixels() == 320 && media.height_pixels() == 240);
  assert(out.at(319, 0) == 0); // horaire : le haut gauche passe en haut a droite
  label.rotate_counterclockwise = true;
  assert(fit_to_label(marker, label, out, media, error) && out.at(0, 239) == 0);
  // modele deja dans le bon sens : inchange
  TemplateDocument landscape;
  RasterImage      straight = render_template(landscape);
  straight.at(5, 5) = 0;
  label.rotate_counterclockwise = false;
  assert(fit_to_label(straight, label, out, media, error) && out.at(5, 5) == 0);
  // modele trop grand
  TemplateDocument big;
  big.media.width_mm = 80.0;
  assert(!fit_to_label(render_template(big), label, out, media, error) && !error.empty());
}

static void test_templates_dir() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "qrprotec-templates-test";
  std::filesystem::remove_all(dir);
  set_templates_dir(dir.string()); // cree le dossier
  assert(std::filesystem::is_directory(dir));
  assert(resolve_template_path("item.qr") == dir / "item.qr");
  assert(resolve_template_path("/abs/item.qr") == std::filesystem::path("/abs/item.qr"));
  TemplateDocument document;
  std::string      error;
  assert(save_template(document, resolve_template_path("b.qr").string(), error));
  assert(save_template(document, resolve_template_path("a.QR").string(), error));
  std::ofstream(dir / "logo.png") << "x";
  const auto found = glob_templates("*.qr");
  assert(found.size() == 2 && found[0].filename() == "a.QR" && found[1].filename() == "b.qr");
  std::filesystem::remove_all(dir);
  set_templates_dir("");
}

static void test_user_dates() {
  const auto iso = [](const char *text) {
    const auto date = parse_user_date(text);
    return date ? date->iso() : std::string("invalide");
  };
  assert(iso("02/09/2026") == "2026-09-02");
  assert(iso("2/9/26") == "2026-09-02");
  assert(iso("02092026") == "2026-09-02");
  assert(iso("020926") == "2026-09-02");
  assert(iso("02-09-2026") == "2026-09-02");
  assert(iso("02.09.26") == "2026-09-02");
  assert(iso("09/2026") == "2026-09-30");
  assert(iso("092026") == "2026-09-30");
  assert(iso("09/26") == "2026-09-30");
  assert(iso("0926") == "2026-09-30");
  assert(iso("02/2028") == "2028-02-29");
  assert(iso("2026-09-02") == "2026-09-02");
  assert(iso("20260902") == "2026-09-02");
  assert(iso(" 12 2027 ") == "2027-12-31");
  assert(iso("31/02/2026") == "invalide");
  assert(iso("13/2026") == "invalide");
  assert(iso("abc") == "invalide");
  assert(iso("") == "invalide");
}

static void test_search() {
  assert(normalize_search("Sérum Physiologique Œil") == "serum physiologique oeil");
  assert(search_score("ser ph", "Sérum physiologique (serphy)") > 0);
  assert(search_score("physio", "Sérum physiologique (serphy)") > 0);
  assert(search_score("serphy", "Sérum physiologique (serphy)") > 0);
  assert(search_score("compresse", "Sérum physiologique (serphy)") < 0);
  assert(search_score("", "n'importe quoi") == 0);
  // le debut du nom l'emporte sur un mot plus loin, un mot entier sur un morceau de mot
  assert(search_score("ga", "Gants nitrile M") > search_score("ga", "Compresses de gaze"));
  assert(search_score("gaz", "Compresses de gaze") > search_score("aze", "Compresses de gaze"));
}

int main() {
  test_json();
  test_dates();
  test_scans();
  test_http();
  test_qr_and_categories();
  test_text_and_label_fit();
  test_templates_dir();
  test_search();
  test_user_dates();
  std::puts("codes_test OK");
  return 0;
}
