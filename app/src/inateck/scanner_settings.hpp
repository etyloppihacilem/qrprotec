#pragma once

#include <string>
#include <vector>

namespace qrprotec {

// Modeles de douchette du SDK Inateck (valeurs de l'enum INATECK_DEVICE_TYPE de inateck_sdk). Le SDK
// lit et ecrit les parametres selon une table d'adresses propre a chaque modele : un mauvais modele
// ecrit certains reglages au mauvais endroit et ne connait pas la liste des parametres du modele.
struct ScannerModel {
    int type;          // valeur passee a inateck_scanner_ble_get/set_setting_info
    const char* label; // nom commercial, ex. "BCST-75S"
};
const std::vector<ScannerModel>& scanner_models();
const char* scanner_model_label(int type); // "" si inconnu
// Modele deduit du nom Bluetooth ("BCST-75S", "Inateck P7"...), 0 si non reconnu. Les variantes AI
// ne se distinguent pas par le nom : elles se choisissent a la main.
int detect_scanner_model(const std::string& device_name);

// Valeur d'un parametre telle que la renvoie inateck_scanner_ble_get_setting_info.
struct ScannerSettingValue {
    int flag = 0;
    std::string name;
    int value = 0;
};

enum class ScannerSettingKind { Toggle, Choice, Number, Character };

struct ScannerSettingOption {
    int value;
    const char* label;
};

// Description d'un parametre (flag du SDK) pour l'affichage.
struct ScannerSettingInfo {
    int flag = 0;
    const char* label = "";
    const char* group = "";
    ScannerSettingKind kind = ScannerSettingKind::Number;
    int minimum = 0;
    int maximum = 255;
    std::vector<ScannerSettingOption> options; // kind == Choice
    const char* help = "";
};

// Groupes d'affichage, dans l'ordre.
const std::vector<const char*>& scanner_setting_groups();
// Description d'un parametre ; `name` (nom renvoye par le SDK) sert pour les flags dont le sens change
// selon le modele (1002 : intensite ou rappel par vibration). Flag inconnu : nombre 0-255, groupe "Autres".
ScannerSettingInfo scanner_setting_info(int flag, const std::string& name);

// Commande JSON de inateck_scanner_ble_set_setting_info, ex. [{"flag":1001,"value":4}].
std::string scanner_setting_command(const std::vector<ScannerSettingValue>& values);

} // namespace qrprotec
