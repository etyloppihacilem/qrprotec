#include "../src/inateck/inateck_worker.hpp"
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
    return 0;
}
