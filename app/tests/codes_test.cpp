#include "core/code128.hpp"
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
#include <set>
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
  assert((Date{ 2026, 10, 15 }.weekday() == "jeudi"));
  assert((Date{ 2024, 2, 29 }.weekday() == "jeudi"));
  assert((Date{ 2027, 1, 3 }.weekday() == "dimanche"));
  const Date today{ 2026, 10, 15 };
  assert(relative_datetime("2026-10-15T09:05:00+02:00", today) == "aujourd'hui à 09:05");
  assert(relative_datetime("2026-10-14T23:59:00+02:00", today, true, true) == "hier à 23:59");
  assert(relative_datetime("2026-10-12T08:00:00+02:00", today) == "12/10/2026 à 08:00");
  assert(relative_datetime("2026-10-12T08:00:00+02:00", today, true, true) == "le lundi 12/10/2026 à 08:00");
  assert(relative_datetime("", today) == "-");
}

static void test_douchette() {
  assert(is_douchette("douchette"));
  assert(is_douchette("  La DOUCHETTE du poste 2"));
  assert(!is_douchette("https://example.com/badge?m=1&key=x"));
  assert(douchette_phrases().size() >= 2);
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

  // nouveau format des etiquettes d'item : URL du front web, l'ancien (iid seul) reste lu
  const ParsedScan item_url = parse_scan("https://example.com/item?id=compre20261231000000A1");
  assert(item_url.kind == ScanKind::Item && item_url.id == "compre20261231000000A1" && item_url.item_type == "compre");
  assert(item_url.peremption && item_url.peremption->iso() == "2026-12-31");
  assert(parse_scan("https://autre.fr/sous/item/?id=garrot00000000000000A1").kind == ScanKind::Item);
  assert(parse_scan("https://example.com/item?id=pas-un-iid").kind == ScanKind::Unknown);
  const ParsedScan checklist = parse_scan("https://example.com/liste?id=Ab12Cd34Ef56");
  assert(checklist.kind == ScanKind::Checklist && checklist.id == "Ab12Cd34Ef56");
  assert(!item_url.task);
  // etiquette de taches (TaskItem) : l'item, avec ses taches a cocher
  const ParsedScan task = parse_scan("https://example.com/task?id=defibr00000000000000A1");
  assert(task.kind == ScanKind::Item && task.task && task.id == "defibr00000000000000A1" && !task.peremption);
  assert(parse_scan("https://example.com/task?id=pas-un-iid").kind == ScanKind::Unknown);
  assert(describe_scan(task) == "Tâches de l'item defibr00000000000000A1");

  const ParsedScan lot = parse_scan("https://example.com/verif?lot=sacpse00000001&key=abc%2Bd");
  assert(lot.kind == ScanKind::Lot && lot.id == "sacpse00000001" && lot.key == "abc+d");
  const ParsedScan lot_public = parse_scan("https://autre-domaine.fr/app/verif/?lot=sacpse00000001");
  assert(lot_public.kind == ScanKind::Lot && lot_public.key.empty());
  const ParsedScan badge = parse_scan("https://example.com/badge?m=M001&key=K");
  assert(badge.kind == ScanKind::User && badge.id == "M001" && badge.key == "K" && badge.temp.empty());
  // QR code de connexion temporaire (depannage du TECH)
  const ParsedScan temp = parse_scan("https://example.com/badge?m=M001&t=eyJtIjoiTTAwMSJ9%3A1vW%3Aab-c_d");
  assert(temp.kind == ScanKind::User && temp.id == "M001" && temp.key.empty() && temp.temp == "eyJtIjoiTTAwMSJ9:1vW:ab-c_d");
  assert(temp.otp.empty());
  // QR code du telephone connecte avec sa passkey : cle OTP et code de 30 s
  const ParsedScan otp = parse_scan("https://example.com/badge?m=M001&o=eyJvIjoiYWIifQ%3A1vW%3Axy&c=012345");
  assert(otp.kind == ScanKind::User && otp.id == "M001" && otp.temp == "eyJvIjoiYWIifQ:1vW:xy" && otp.otp == "012345");
  const ParsedScan pack = parse_scan("https://example.com/pack?id=0000002B");
  assert(pack.kind == ScanKind::SealedPack && pack.id == "0000002B");
  assert(pack.key.empty());
  // etiquette de paquet ouvert (paquet pre-etiquete) : sa cle
  const ParsedScan open_pack = parse_scan("https://example.com/pack?id=0000002B&o=Ab12Cd34");
  assert(open_pack.kind == ScanKind::SealedPack && open_pack.id == "0000002B" && open_pack.key == "Ab12Cd34");
  const ParsedScan seal = parse_scan("https://example.com/seal?lot=sacpse00000001&s=AbC123");
  assert(seal.kind == ScanKind::LotSeal && seal.id == "sacpse00000001" && seal.key == "AbC123");
  assert(parse_scan("https://example.com/seal?lot=sacpse00000001").kind == ScanKind::Unknown);
  const ParsedScan opening = parse_scan("https://example.com/unseal?lot=sacpse00000001&c=Gh34");
  assert(opening.kind == ScanKind::LotSealOpen && opening.id == "sacpse00000001" && opening.key == "Gh34");
  assert(parse_scan("https://example.com/unseal?lot=sacpse00000001").kind == ScanKind::Unknown);
  assert(parse_scan("n'importe quoi").kind == ScanKind::Unknown);
  assert(parse_scan("compre2026123100000!A1").kind == ScanKind::Unknown);

  // affichage : elements distinctifs seulement, jamais l'URL ni la cle
  assert(describe_scan(item) == "Item compre20261231000000A1");
  assert(describe_scan(item_url) == "Item compre20261231000000A1");
  assert(describe_scan(lot) == "Lot sacpse00000001 · étiquette clé");
  assert(describe_scan(lot_public) == "Lot sacpse00000001");
  assert(describe_scan(badge) == "Badge M001");
  assert(describe_scan(pack) == "Paquet 0000002B");
  assert(describe_scan(seal) == "Scellé du lot sacpse00000001");
  assert(describe_scan(opening) == "Ouverture du scellé du lot sacpse00000001");
  assert(describe_scan(parse_scan("https://example.com/scanner/?s=SESSION")) == "QR de session douchette");
  assert(describe_scan(parse_scan("https://user:pw@autre.fr/badge?key=K")) == "Lien autre.fr");
  assert(describe_scan(parse_scan("https://autre.fr")) == "Lien autre.fr");
  assert(describe_scan(parse_scan("n'importe quoi")) == "n'importe quoi");
  assert(describe_scan(parse_scan("abcdefghijklmnopqrstuvwxyz")) == "abcdefghijklmnopqrstuvwx…");
}

static void test_http() {
  HttpUrl     url;
  std::string error;
  assert(parse_http_url("http://127.0.0.1:8001", url, error) && url.host == "127.0.0.1" && url.port == 8001);
  assert(parse_http_url("http://serveur/qrprotec/", url, error) && url.port == 80 && url.base_path == "/qrprotec");
  assert(!url.tls && host_header(url) == "serveur");
  assert(parse_http_url("HTTPS://serveur.example.org", url, error) && url.tls && url.port == 443);
  assert(host_header(url) == "serveur.example.org");
  assert(parse_http_url("https://[::1]:8443/api", url, error) && url.tls && url.host == "::1" && url.port == 8443);
  assert(host_header(url) == "[::1]:8443" && url.base_path == "/api");
  assert(!parse_http_url("ftp://serveur", url, error));
  assert(!parse_http_url("serveur:8001", url, error));
  ApiEndpoint endpoint{ "https://serveur", "", "qrpf_cle", "/etc/ca.pem" };
  assert(endpoint.parse(url, error) && url.ca_file == "/etc/ca.pem");
  // http:// : API locale de cette machine seulement (cle, jeton et session jamais en clair sur le reseau)
  for (const char *local : { "http://127.0.0.1:8001", "http://localhost:8001", "http://[::1]:8001" }) {
    ApiEndpoint loopback{ local, "", "", "" };
    assert(loopback.parse(url, error));
  }
  ApiEndpoint remote_http{ "http://serveur.example.org", "", "qrpf_cle", "" };
  assert(!remote_http.parse(url, error) && error.find("https://") != std::string::npos);
  assert(is_loopback_host("LOCALHOST") && is_loopback_host("127.0.1.1"));
  assert(!is_loopback_host("127.example.org") && !is_loopback_host("127.0.0.1.nip.io") && !is_loopback_host("10.0.0.1"));
  const HttpHeaders headers = endpoint.headers();
  assert(headers.size() == 1 && headers[0].first == "X-QRProtec-Key" && headers[0].second == "qrpf_cle");
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
  assert(category_from_id("lot_storage") == TemplateCategory::LotStorage);
  assert(category_from_id("lot_location") == TemplateCategory::LotLocation);
  assert(example_parameters(TemplateCategory::LotLocation).count("emplacement") == 1);
  assert(category_from_id("guest_card") == TemplateCategory::GuestCard);
  assert(example_parameters(TemplateCategory::GuestCard).count("guest_hours") == 1);
}

static void test_compact_iid() {
  // forme courte : D sans date, N a la place des zeros de tete du compteur (au moins deux)
  assert(compact_iid("serphy2026102900000007") == "serphy20261029N7");
  assert(compact_iid("lecgly0000000000000006") == "lecglyDN6");
  assert(compact_iid("compre20271231000000A1") == "compre20271231NA1");
  assert(compact_iid("abcdef202712310zzzzzzz") == "abcdef202712310zzzzzzz"); // un seul zero : rien a gagner
  assert(compact_iid("abcdef00000000zzzzzzzz") == "abcdefDzzzzzzzz");
  for (const char *iid : { "serphy2026102900000007", "lecgly0000000000000006", "abcdef202712310zzzzzzz",
                           "abcdef00000000zzzzzzzz", "compre20271231000000A1" })
    assert(expand_iid(compact_iid(iid)) == iid);
  assert(expand_iid("lecglyDN6") == "lecgly0000000000000006");
  assert(expand_iid("serphy20261029N7") == "serphy2026102900000007");
  // un compteur de 8 caracteres est toujours litteral (N est aussi un chiffre base62)
  assert(expand_iid("serphyDN1234567") == "serphy00000000N1234567");
  // pas un iid : date incomplete, compteur incomplet
  assert(expand_iid("serphy2026N7").empty());
  assert(expand_iid("serphyD1234").empty() && expand_iid("serphyDN").empty() && expand_iid("PSE1-1").empty());
  const ParsedScan scan = parse_scan("serphy20261029N7");
  assert(scan.kind == ScanKind::Item && scan.id == "serphy2026102900000007" && scan.item_type == "serphy");
  assert(scan.peremption && scan.peremption->iso() == "2026-10-29");
  const ParsedScan undated = parse_scan("lecglyDN6");
  assert(undated.kind == ScanKind::Item && undated.id == "lecgly0000000000000006" && !undated.peremption);
}

static void test_bare_matricule() {
  // matricule seul (sans l'URL du badge) : badge sans cle, le serveur demandera le PIN
  const ParsedScan scan = parse_scan("M001");
  assert(scan.kind == ScanKind::User && scan.id == "M001" && scan.key.empty());
  assert(parse_scan("A-12_b").kind == ScanKind::User);
  assert(parse_scan("matricule trop long 1234").kind == ScanKind::Unknown);
  assert(parse_scan("bad code!").kind == ScanKind::Unknown);
  // un iid (meme en forme courte) reste un item
  assert(parse_scan("lecglyDN6").kind == ScanKind::Item);
  // URL d'item en forme courte
  const ParsedScan url = parse_scan("https://example.com/item?id=serphy20261029N7");
  assert(url.kind == ScanKind::Item && url.id == "serphy2026102900000007");
  assert(parse_scan("https://example.com/task?id=lecglyDN6").task);
}

// Code de l'antenne devant les codes imprimes : le notre est retire, sans code l'etiquette est a nous (imprimee avant),
// celui d'une autre antenne est refuse (Unknown, foreign)
static void test_code_prefix() {
  set_code_prefix("PC");
  assert(code_prefix() == "PC");
  assert(item_code("serphy2026102900000007") == "PCserphy20261029N7");
  assert(expand_iid("PCserphy20261029N7") == "serphy2026102900000007");
  assert(expand_iid("PCserphy2026102900000007") == "serphy2026102900000007");
  assert(expand_iid("serphy20261029N7") == "serphy2026102900000007"); // ancienne etiquette : a nous
  std::string foreign;
  assert(expand_iid("12serphy20261029N7", &foreign).empty() && foreign == "12");
  assert(expand_iid("12serphyDN7").empty());

  const ParsedScan mine = parse_scan("PClecglyDN6");
  assert(mine.kind == ScanKind::Item && mine.id == "lecgly0000000000000006" && mine.foreign.empty());
  const ParsedScan other = parse_scan("12lecglyDN6"); // aurait ressemble a un matricule
  assert(other.kind == ScanKind::Unknown && other.foreign == "12");
  assert(describe_scan(other) == "Étiquette d'une autre antenne (12)");
  assert(parse_scan("https://example.com/item?id=PCserphy20261029N7").id == "serphy2026102900000007");
  assert(parse_scan("https://example.com/task?id=12serphy20261029N7").foreign == "12");

  // lots, rangements, scelles : id de 14 caracteres (type de lot + compteur)
  const ParsedScan lot = parse_scan("https://example.com/verif?lot=PCsacpse00000003&key=abc");
  assert(lot.kind == ScanKind::Lot && lot.id == "sacpse00000003" && lot.key == "abc");
  assert(parse_scan("https://example.com/verif?lot=sacpse00000003").id == "sacpse00000003");
  assert(parse_scan("https://example.com/verif?lot=12sacpse00000003").foreign == "12");
  assert(parse_scan("https://example.com/seal?lot=PCsacpse00000003&s=X").id == "sacpse00000003");
  assert(parse_scan("https://example.com/unseal?lot=AZsacpse00000003&c=X").kind == ScanKind::Unknown);
  // paquets fermes : id de 8 caracteres
  assert(parse_scan("https://example.com/pack?id=PC0000002B").id == "0000002B");
  assert(parse_scan("https://example.com/pack?id=0000002B").id == "0000002B");
  assert(parse_scan("https://example.com/pack?id=AB0000002B").foreign == "AB");
  assert(read_id("L1", kLotIdLength) == "L1"); // autre longueur : tel quel

  // sans code d'antenne (serveur pas encore regle) : toute etiquette avec un code est d'une autre antenne
  set_code_prefix("");
  assert(parse_scan("PCserphy20261029N7").foreign == "PC");
  assert(parse_scan("serphy20261029N7").kind == ScanKind::Item);
  set_code_prefix("pc"); // invalide : aucun
  assert(code_prefix().empty());
}

static void test_barcode() {
  // jeu C pour les suites de chiffres : depart C pour 4 chiffres, depart B puis C au milieu d'un iid
  assert((code128_symbols("1234") == std::vector< int >{ 105, 12, 34, 82, 106 }));
  const std::vector< int > iid = code128_symbols("compre20271231000000A1");
  assert(iid.front() == 104 && iid[7] == 99 && iid.back() == 106);
  assert(code128_modules("compre20271231000000A1").size() == 222);
  assert(code128_modules("abcdef20271231zzzzzzzz").size() == 255); // iid le plus long
  assert(code128_modules("").empty() && code128_modules("café").empty());
  for (const char *pattern : kCode128Patterns) {
    int sum = 0;
    for (const char *width = pattern; *width; ++width)
      sum += *width - '0';
    assert(sum == (pattern[6] ? 13 : 11));
  }

  // modele fourni : valide, le code tient sur sa largeur a 1 px par module au moins, avec ses marges blanches
  // (valeurs relues dans item_codebarre.qr : le modele peut changer sans casser le test)
  TemplateDocument document;
  std::string      error;
  assert(load_template(document, std::string(QRPROTEC_TEMPLATES_SOURCE) + "/item_codebarre.qr", error));
  assert(document.category == TemplateCategory::ItemBarcode);
  assert(category_from_id("item_barcode") == TemplateCategory::ItemBarcode);
  const auto barcode_element = std::find_if(document.elements.begin(), document.elements.end(),
                                            [](const TemplateElement &element) { return element.kind == ElementKind::Barcode; });
  assert(barcode_element != document.elements.end());
  const std::size_t     barcode_index = static_cast< std::size_t >(barcode_element - document.elements.begin());
  const BarcodeElement  barcode       = std::get< BarcodeElement >(barcode_element->content);
  document.parameters["iid_short"] = "abcdef20271231zzzzzzzz"; // compteur sans zero : forme courte = iid
  assert(validate(document).empty());
  assert(barcode_module_pixels(barcode, "abcdef20271231zzzzzzzz", document.media.pixels_per_mm) >= 1);
  // avec le code de l'antenne devant (2 caracteres, 22 modules de plus) : tient encore
  assert(barcode_module_pixels(barcode, "ZZabcdef20271231zzzzzzzz", document.media.pixels_per_mm) >= 1);

  // relecture du rendu sur une ligne : memes symboles que l'encodeur (le code seul, sans les autres elements)
  TemplateDocument alone = document;
  alone.elements         = { document.elements[barcode_index] };
  alone.elements[0].rotation = 0;
  alone.parameters["iid_short"] = "compre20271231000000A1";
  const RasterImage image  = render_template(alone);
  const int         module = barcode_module_pixels(barcode, "compre20271231000000A1", alone.media.pixels_per_mm);
  assert(module >= 1);
  const int row = static_cast< int >((barcode.y_mm + barcode.height_mm / 2.0f) * alone.media.pixels_per_mm);
  std::vector< int > runs;
  int                first = -1, last = -1;
  for (int x = 0; x < image.width; ++x)
    if (image.at(x, row) == 0) {
      first = first < 0 ? x : first;
      last  = x;
    }
  // marge blanche de quiet_zone modules a gauche
  assert(first >= static_cast< int >(barcode.x_mm * alone.media.pixels_per_mm) + barcode.quiet_zone * module);
  for (int x = first; x <= last;) {
    const std::uint8_t value = image.at(x, row);
    int                width = 0;
    for (; x <= last && image.at(x, row) == value; ++x)
      ++width;
    assert(width % module == 0);
    runs.push_back(width / module);
  }
  std::vector< int > decoded;
  for (std::size_t start = 0; start + 6 <= runs.size(); start += 6) {
    std::string widths;
    for (std::size_t index = start; index < std::min(runs.size(), start + (runs.size() - start == 7 ? 7 : 6)); ++index)
      widths += static_cast< char >('0' + runs[index]);
    const auto found = std::find_if(std::begin(kCode128Patterns), std::end(kCode128Patterns),
                                    [&widths](const char *pattern) { return widths == pattern; });
    assert(found != std::end(kCode128Patterns));
    decoded.push_back(static_cast< int >(found - std::begin(kCode128Patterns)));
    if (decoded.back() == 106)
      break;
  }
  assert(decoded == code128_symbols("compre20271231000000A1"));

  // persistance de l'element : relu tel qu'il est dans le modele
  assert(save_template(document, "/tmp/qrprotec-barcode.qr", error));
  TemplateDocument loaded;
  assert(load_template(loaded, "/tmp/qrprotec-barcode.qr", error));
  assert(loaded.elements.size() == document.elements.size());
  assert(loaded.elements[barcode_index].kind == ElementKind::Barcode);
  const BarcodeElement &reloaded = std::get< BarcodeElement >(loaded.elements[barcode_index].content);
  assert(reloaded.payload == barcode.payload && reloaded.x_mm == barcode.x_mm && reloaded.y_mm == barcode.y_mm);
  assert(reloaded.width_mm == barcode.width_mm && reloaded.height_mm == barcode.height_mm);
  assert(reloaded.quiet_zone == barcode.quiet_zone);
  std::remove("/tmp/qrprotec-barcode.qr");

  // modeles fournis : valides avec leurs valeurs d'exemple, un pour chacun des 9 usages d'etiquette d'item
  std::set< std::string > item_categories;
  for (const auto &entry : std::filesystem::directory_iterator(QRPROTEC_TEMPLATES_SOURCE)) {
    if (entry.path().extension() != ".qr")
      continue;
    TemplateDocument bundled;
    assert(load_template(bundled, entry.path().string(), error));
    if (!validate(bundled).empty())
      std::fprintf(stderr, "%s : %s\n", entry.path().filename().c_str(), validate(bundled)[0].message.c_str());
    assert(validate(bundled).empty());
    const std::string id = category_id(bundled.category);
    if (id.find("item") != std::string::npos && id != "itempack")
      item_categories.insert(id);
  }
  assert((item_categories == std::set< std::string >{ "item", "item_small", "item_barcode", "task_item", "task_item_small",
                                                      "task_item_barcode", "item_tear", "item_small_tear",
                                                      "item_barcode_tear", "item_sheet" }));

  // trop etroit ou contenu non encodable : signale
  document.parameters["iid_short"] = "https://example.com/item?id=compre20271231000000A1";
  assert(!validate(document).empty());
}

// Planche (item_planche.qr) : plusieurs items par etiquette, autant que le plus grand numero de ses placeholders ;
// les places vides de la derniere planche ne gardent jamais les valeurs d'exemple du modele
static void test_item_sheet() {
  TemplateDocument document;
  std::string      error;
  assert(load_template(document, std::string(QRPROTEC_TEMPLATES_SOURCE) + "/item_planche.qr", error));
  assert(document.category == TemplateCategory::ItemSheet);
  int expected = 0; // nombre d'items lu dans le modele : plus grand numero de {{codeN}}
  for (const std::string &name : find_placeholders(document))
    if (name.rfind("code", 0) == 0 && name.size() > 4)
      expected = std::max(expected, std::stoi(name.substr(4)));
  assert(expected > 1);
  const auto item = [](const std::string &iid, const std::string &date) {
    return Parameters{ { "code", compact_iid(iid) }, { "iid", iid }, { "type_name", "Dosette" }, { "exp", date } };
  };
  assert(sheet_slots(document, item("serdos2028091200000001", "12/09/2028")) == expected);
  std::vector< Parameters > items;
  for (int index = 1; index < expected; ++index) // une place vide
    items.push_back(item("serdos202809120000000" + std::to_string(index), "12/09/2028"));
  const Parameters filled = sheet_parameters(document, items, expected);
  assert(filled.at("code1") == compact_iid("serdos2028091200000001"));
  assert(filled.at("type_name1") == "Dosette" && filled.at("exp1") == "12/09/2028");
  const std::string last = std::to_string(expected);
  assert(filled.at("code" + last).empty() && filled.at("type_name" + last).empty() && filled.at("exp" + last).empty());
  assert(filled.at("type_name") == "Dosette" && filled.at("sheet_count") == std::to_string(expected - 1));
  // rendu : la place vide n'a ni QR ni texte, le modele reste valide avec de vrais iid
  TemplateDocument printed = document;
  for (const auto &[name, value] : filled)
    printed.parameters[name] = value;
  assert(validate(printed).empty());
  // modele sans numero : un item par etiquette
  TemplateDocument plain;
  plain.elements.push_back({ "qr", ElementKind::QrCode, QrElement{ "{{code}}" } });
  assert(sheet_slots(plain, item("serdos2028091200000001", "")) == 1);
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
  // images des etiquettes : sous-dossier images/ d'abord, puis la racine
  std::filesystem::create_directories(dir / LABEL_IMAGES_DIR);
  std::ofstream(dir / LABEL_IMAGES_DIR / "protec.png") << "x";
  std::ofstream(dir / LABEL_IMAGES_DIR / "autre.JPG") << "x";
  const auto images = label_images();
  assert(images.size() == 3 && images[0] == "images/autre.JPG" && images[1] == "images/protec.png" && images[2] == "logo.png");
  assert(resolve_template_path(images[1]) == dir / "images" / "protec.png");
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
  test_douchette();
  test_http();
  test_compact_iid();
  test_code_prefix();
  test_bare_matricule();
  test_barcode();
  test_qr_and_categories();
  test_item_sheet();
  test_text_and_label_fit();
  test_templates_dir();
  test_search();
  test_user_dates();
  std::puts("codes_test OK");
  return 0;
}
