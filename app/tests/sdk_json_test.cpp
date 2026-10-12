#include "../src/inateck/inateck_worker.hpp"
#include "../src/inateck/scanner_settings.hpp"
#include "../src/inateck/sdk_json.hpp"

#include <cassert>
#include <string>
#include <vector>

using namespace qrprotec;

int main() {
    // Le SDK echappe le retour a la ligne final du code ; il est decode puis retire
    const char* scan = "{\"code\":\"PRODUIT-42\\n\",\"status\":0}";
    assert(sdk_json_string(scan, "code") == "PRODUIT-42\n");
    assert(clean_scan_code(sdk_json_string(scan, "code")) == "PRODUIT-42");
    assert(clean_scan_code("ABC\r\n") == "ABC");
    assert(clean_scan_code("ABC") == "ABC");
    assert(sdk_json_string("{\"code\": \"a\\\"b\\\\c\\u00e9\"}", "code") == "a\"b\\c\xC3\xA9");
    assert(sdk_json_string("{\"code\":12}", "code").empty());
    assert(sdk_json_string("{}", "code").empty());

    assert(sdk_json_success("{\"status\": 0}"));
    assert(!sdk_json_success("{\"status\":1,\"error\":\"x\"}"));

    const auto objects = sdk_json_device_objects(
        "{\"devices\":[{\"id\":\"AB:2B:00:07:C4:FF\",\"device_name\":\"BCST-75S\"},{\"id\":\"11\"}],\"status\":0}");
    assert(objects.size() == 2);
    assert(sdk_json_string(objects[0].c_str(), "id") == "AB:2B:00:07:C4:FF");

    assert(looks_like_scanner("BCST-75S"));
    assert(looks_like_scanner("Inateck P7"));
    assert(!looks_like_scanner("JBL Flip"));

    const std::vector<InateckDevice> devices = {
        {"1", "JBL Flip", false}, {"2", "Inateck P7", false}, {"3", "Douchette", false}, {"4", "Clavier", false}};
    // Douchette memorisee en premier (meme renommee), puis noms Inateck, puis le reste ; refuses ignores
    auto candidates = ordered_devices(devices, "3", {"4"});
    assert(candidates.size() == 3);
    assert(candidates[0].id == "3" && candidates[1].id == "2" && candidates[2].id == "1");
    candidates = ordered_devices(devices, "", {});
    assert(candidates.size() == 4 && candidates[0].id == "2" && candidates[1].id == "1");

    // Reponse de inateck_scanner_ble_get_setting_info (format de la bibliotheque scanner_lib)
    const auto settings = sdk_json_settings(
        "{\"status\":0,\"data\":[{\"name\":\"auto_off\",\"flag\":1012,\"value\":1},"
        "{\"name\":\"volume\",\"flag\":1001,\"value\":4},{\"name\":\"a]b\",\"flag\":2022,\"value\":0}]}");
    assert(settings.size() == 3);
    assert(settings[1].flag == 1001 && settings[1].name == "volume" && settings[1].value == 4);
    assert(settings[2].name == "a]b" && settings[2].flag == 2022);
    assert(sdk_json_settings("{\"status\":2,\"data\":[]}").empty());
    assert(sdk_json_settings(nullptr).empty());

    assert(sdk_json_data_text("{\"status\":0,\"data\":\"85\"}") == "85");
    assert(sdk_json_data_text("{\"status\":0,\"data\":85}") == "85");
    assert(sdk_json_data_text("{\"data\": [1, 2],\"status\":0}") == "[1, 2]");
    assert(sdk_json_data_text("{\"status\":0}").empty());

    // Modele deduit du nom Bluetooth (valeurs de INATECK_DEVICE_TYPE)
    assert(detect_scanner_model("BCST-75S") == 37);
    assert(detect_scanner_model("Inateck BCST-75") == 9);
    assert(detect_scanner_model("bcst 73") == 8);
    assert(detect_scanner_model("BCST-751") == 0);
    assert(detect_scanner_model("Inateck P7") == 11);
    assert(detect_scanner_model("Pro 8 SE") == 46);
    assert(detect_scanner_model("JBL Flip") == 0);
    assert(std::string(scanner_model_label(37)) == "BCST-75S");

    assert(scanner_setting_command({{1001, "volume", 8}, {2022, "", 1}}) ==
           "[{\"flag\":1001,\"value\":8},{\"flag\":2022,\"value\":1}]");
    // Le volume se regle en 0/2/4/8, pas en 0-3
    const ScannerSettingInfo volume = scanner_setting_info(1001, "volume");
    assert(volume.kind == ScannerSettingKind::Choice && volume.options.size() == 4 && volume.options[3].value == 8);
    assert(scanner_setting_info(1002, "shake_Reminder").options.size() == 4);
    assert(scanner_setting_info(1002, "shake_intensity").options.size() == 2);
    assert(scanner_setting_info(2022, "qrcode_on").kind == ScannerSettingKind::Toggle);
    assert(std::string(scanner_setting_info(9999, "x").group) == "Autres");
    return 0;
}
