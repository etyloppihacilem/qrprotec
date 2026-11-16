// Scenarios de vérif communs (tests/verif-scenarios/*.json), joues par le poste : plan_verif et discard_check de
// src/core/verif_plan.cpp. Le serveur (database/inventory/tests/test_verif_scenarios.py) et le front web
// (tests/verif-scenarios/web.test.js) jouent les memes fichiers : voir tests/verif-scenarios/LISEZMOI.md.
#include "../src/core/codes.hpp"
#include "../src/core/json.hpp"
#include "../src/core/verif_plan.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using qrprotec::Json;

namespace {

std::string quote(const std::string &text) { return Json(text).dump(); }

std::string read_file(const std::filesystem::path &path) {
    std::ifstream      file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Lancement d'un scenario : erreurs (vide si le poste donne le resultat attendu)
std::vector< std::string > play(const Json &scenario) {
    const Json &types = scenario["types"];
    // iids : meme construction que web.test.js (type, date, compteur dans l'ordre du fichier)
    std::map< std::string, std::string > iid, label;
    std::set< std::string >              expired;
    int                                  counter = 0;
    const auto                           name_item = [&](const Json &item) {
        const Json       &type = types[item["type"].str()];
        const std::string date = item["expired"].boolean()           ? "20200101"
                                 : !type["perissable"].boolean(true) ? "00000000"
                                                                     : "20991231";
        std::string       count = std::to_string(++counter);
        const std::string value = item["type"].str() + date + std::string(8 - count.size(), '0') + count;
        iid[item["id"].str()]   = value;
        label[value]            = item["id"].str();
        if (item["expired"].boolean())
            expired.insert(item["id"].str());
    };
    for (const Json &lot : scenario["lots"].items())
        for (const Json &item : lot["items"].items())
            name_item(item);
    for (const Json &item : scenario["elsewhere"].items())
        name_item(item);

    // lots tels que les envoie le serveur (serializers) : requirements, items
    std::string text = "[";
    bool        cascade = false;
    for (const Json &lot : scenario["lots"].items()) {
        cascade = cascade || !lot["parent"].str().empty();
        text += std::string(text.size() > 1 ? "," : "") + "{\"id\":" + quote(lot["id"].str()) + ",\"parent\":"
                + quote(lot["parent"].str()) + ",\"storage\":" + (lot["storage"].boolean() ? "true" : "false")
                + ",\"requirements\":[";
        bool first = true;
        for (const auto &[type, required] : lot["requirements"].members()) {
            text += std::string(first ? "" : ",") + "{\"type\":" + quote(type) + ",\"required\":"
                    + std::to_string(required.integer()) + ",\"check\":"
                    + (types[type]["physical"].boolean(true) ? "false" : "true") + "}";
            first = false;
        }
        text += "],\"items\":[";
        first = true;
        for (const Json &item : lot["items"].items()) {
            const Json &type = types[item["type"].str()];
            text += std::string(first ? "" : ",") + "{\"iid\":" + quote(iid[item["id"].str()]) + ",\"tear_off\":"
                    + (type["tear_off"].boolean() ? "true" : "false") + ",\"consumable\":"
                    + (type["consumable"].boolean(true) ? "true" : "false") + "}";
            first = false;
        }
        text += "]}";
    }
    text += "]";
    std::string error;
    const Json  lots_json = Json::parse(text, &error);
    if (!error.empty())
        return { "lots illisibles : " + error };
    std::vector< const Json * > lots;
    for (const Json &lot : lots_json.items())
        lots.push_back(&lot);

    // scans : items avec le lot de la derniere etiquette scannee, cases cochees
    const qrprotec::Date                                today{ 2026, 11, 16 };
    std::vector< qrprotec::VerifPlanScan >               scanned;
    std::map< std::string, std::set< std::string > >     checked;
    for (const Json &scan : scenario["scans"].items()) {
        if (scan.contains("check")) {
            checked[scan["lot"].str()].insert(scan["check"].str());
            continue;
        }
        const std::string          value  = iid[scan["item"].str()];
        const qrprotec::ParsedScan parsed = qrprotec::parse_scan(value);
        scanned.push_back({ value, parsed.item_type, scan["place"].str(), qrprotec::is_expired(parsed, today) });
    }
    const std::vector< qrprotec::VerifPlanLot > plan = qrprotec::plan_verif(lots, scanned, checked, cascade);

    std::vector< std::string > errors;
    const Json                &expected = scenario["expected"];
    std::map< std::string, std::string > target;
    for (const qrprotec::VerifPlanLot &row : plan)
        for (const std::string &value : row.iids)
            target[label[value]] = (*row.lot)["id"].str();
    std::map< std::string, std::string > wanted;
    for (const auto &[item, lot] : expected["target"].members())
        wanted[item] = lot.str();
    for (const auto &[item, lot] : wanted)
        if (target[item] != lot)
            errors.push_back(item + " range dans " + (target[item].empty() ? "aucun lot" : target[item])
                             + ", attendu " + lot);
    if (target.size() != wanted.size())
        errors.push_back("items ranges : " + std::to_string(target.size()) + ", attendus "
                         + std::to_string(wanted.size()));

    std::set< std::string > complete, wanted_complete;
    for (const qrprotec::VerifPlanLot &row : plan)
        if (row.complete)
            complete.insert((*row.lot)["id"].str());
    for (const Json &lot : expected["complete"].items())
        wanted_complete.insert(lot.str());
    for (const std::string &lot : wanted_complete)
        if (!complete.count(lot))
            errors.push_back(lot + " devrait etre complet");
    for (const std::string &lot : complete)
        if (!wanted_complete.count(lot))
            errors.push_back(lot + " ne devrait pas etre complet");

    std::set< std::string > discard, wanted_discard;
    for (const qrprotec::VerifPlanScan &scan : scanned)
        if (scan.expired && qrprotec::discard_check(plan, scan.iid, scan.type).discard())
            discard.insert(label[scan.iid]);
    for (const Json &item : expected["discard"].items())
        wanted_discard.insert(item.str());
    if (discard != wanted_discard) {
        std::string got;
        for (const std::string &item : discard)
            got += " " + item;
        errors.push_back("perimes a jeter :" + (got.empty() ? std::string(" aucun") : got));
    }
    return errors;
}

} // namespace

int main() {
    std::vector< std::filesystem::path > files;
    for (const auto &entry : std::filesystem::directory_iterator(QRPROTEC_SCENARIOS_DIR))
        if (entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    int failures = 0;
    for (const auto &path : files) {
        std::string error;
        const Json  scenario = Json::parse(read_file(path), &error);
        std::vector< std::string > errors = error.empty() ? play(scenario) : std::vector< std::string >{ error };
        std::cout << (errors.empty() ? "ok     " : "ECHEC  ") << path.filename().string() << "\n";
        for (const std::string &message : errors)
            std::cout << "         " << message << "\n";
        failures += !errors.empty();
    }
    if (files.empty()) {
        std::cout << "aucun scenario dans " << QRPROTEC_SCENARIOS_DIR << "\n";
        return 1;
    }
    return failures ? 1 : 0;
}
