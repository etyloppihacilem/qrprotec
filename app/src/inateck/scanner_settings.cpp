#include "scanner_settings.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace qrprotec {

namespace {

constexpr const char* kSound = "Son, vibration et lumière";
constexpr const char* kScan = "Lecture";
constexpr const char* kOutput = "Sortie des codes";
constexpr const char* kPower = "Alimentation, connexion et mémoire";
constexpr const char* kCodes = "Types de codes lus";
constexpr const char* kCodeOptions = "Options des types de codes";
constexpr const char* kFactory = "Usine (ne pas modifier)";
constexpr const char* kOthers = "Autres";

ScannerSettingInfo toggle(int flag, const char* label, const char* group, const char* help = "") {
    ScannerSettingInfo info;
    info.flag = flag;
    info.label = label;
    info.group = group;
    info.kind = ScannerSettingKind::Toggle;
    info.maximum = 1;
    info.help = help;
    return info;
}

ScannerSettingInfo number(int flag, const char* label, const char* group, int minimum, int maximum,
                          const char* help = "") {
    ScannerSettingInfo info;
    info.flag = flag;
    info.label = label;
    info.group = group;
    info.kind = ScannerSettingKind::Number;
    info.minimum = minimum;
    info.maximum = maximum;
    info.help = help;
    return info;
}

ScannerSettingInfo choice(int flag, const char* label, const char* group, std::vector<ScannerSettingOption> options,
                          const char* help = "") {
    ScannerSettingInfo info;
    info.flag = flag;
    info.label = label;
    info.group = group;
    info.kind = ScannerSettingKind::Choice;
    info.options = std::move(options);
    info.help = help;
    return info;
}

ScannerSettingInfo character(int flag, const char* label, const char* group, const char* help = "") {
    ScannerSettingInfo info;
    info.flag = flag;
    info.label = label;
    info.group = group;
    info.kind = ScannerSettingKind::Character;
    info.help = help;
    return info;
}

// Valeurs documentees par Inateck (docs.inateck.com/scanner-sdk-en/ble/desktop_setting_list) ; tailles
// des champs tirees des tables de la bibliotheque (scanner_lib).
std::map<int, ScannerSettingInfo> build_catalogue() {
    std::vector<ScannerSettingInfo> list = {
        // Son, vibration, lumiere
        choice(1001, "Volume du bip", kSound, {{0, "Muet"}, {2, "Faible"}, {4, "Moyen"}, {8, "Fort"}}),
        choice(1047, "Vibration à chaque scan", kSound, {{0, "Désactivée"}, {1, "Activée"}}),
        number(1035, "Force de la vibration (moteur)", kSound, 1, 8, "Niveau de vibration du moteur, de 1 à 8."),
        choice(1003, "Éclairage", kSound,
               {{0, "Allumé pendant la lecture"}, {1, "Toujours allumé"}, {2, "Toujours éteint"}}),
        choice(1004, "Viseur", kSound,
               {{0, "Allumé pendant la lecture"}, {1, "Toujours allumé"}, {2, "Toujours éteint"}}),
        toggle(1046, "Viseur clignotant", kSound),
        number(1058, "Luminosité de l'écran", kSound, 0, 15),
        number(1061, "Mise en veille de l'écran", kSound, 0, 255),
        // Lecture
        choice(1006, "Mode de lecture", kScan,
               {{0, "Gâchette"}, {1, "Lecture continue"}, {2, "Extinction automatique du laser"},
                {3, "Détection automatique"}, {5, "Mains libres"}}),
        number(1022, "Pause en lecture continue", kScan, 0, 255,
               "Unités d'environ 200 ms. Seulement en mode « Lecture continue »."),
        number(1023, "Délai d'extinction automatique du laser", kScan, 0, 255,
               "Seulement en mode « Extinction automatique du laser »."),
        number(1024, "Distance de détection automatique", kScan, 0, 255,
               "Seulement en mode « Détection automatique »."),
        number(1036, "Niveau de sécurité de lecture", kScan, 0, 3,
               "Plus le niveau est haut, plus la douchette relit le code avant de le valider."),
        toggle(1020, "Lire les codes en couleurs inversées", kScan),
        toggle(1019, "Ne lire que les codes commençant par un caractère choisi", kScan,
               "Caractères choisis : « Caractère particulier 1 à 6 »."),
        character(1064, "Caractère particulier 1", kScan),
        character(1065, "Caractère particulier 2", kScan),
        character(1066, "Caractère particulier 3", kScan),
        character(1067, "Caractère particulier 4", kScan),
        character(1068, "Caractère particulier 5", kScan),
        character(1069, "Caractère particulier 6", kScan),
        toggle(1049, "Lecture de plusieurs codes à la fois", kScan),
        number(1050, "Nombre de codes lus à la fois", kScan, 0, 3),
        toggle(1083, "Codes adaptatifs", kScan),
        toggle(1044, "Détection sur le support", kScan),
        toggle(3052, "Détection sur le support", kScan),
        number(1060, "Choix du lecteur", kScan, 0, 3),
        number(1085, "RFID", kScan, 0, 3),
        number(1086, "Puissance RFID (dBm)", kScan, 0, 31),
        // Sortie des codes
        toggle(1028, "Entrée après chaque code", kOutput),
        toggle(1055, "Tabulation après chaque code", kOutput, "Désactive « Entrée après chaque code »."),
        toggle(1029, "Préfixe personnalisé", kOutput, "Texte du préfixe : champ « Préfixe » ci-dessus."),
        toggle(1030, "Suffixe personnalisé", kOutput, "Texte du suffixe : champ « Suffixe » ci-dessus."),
        number(1063, "Types de codes concernés par le préfixe et le suffixe", kOutput, 0, 255),
        toggle(1027, "Identifiant du type de code avant le code", kOutput),
        choice(1052, "Identifiant ajouté", kOutput, {{0, "AIM ID"}, {1, "Code ID"}}),
        number(1025, "Caractères masqués au début", kOutput, 0, 255),
        number(1026, "Caractères masqués à la fin", kOutput, 0, 255),
        number(1033, "Types de codes concernés par le masquage", kOutput, 0, 255),
        choice(1008, "Casse des lettres", kOutput, {{0, "Inchangée"}, {1, "Minuscules"}, {2, "Majuscules"}}),
        choice(1005, "Clavier de l'ordinateur (mode HID)", kOutput,
               {{1, "Anglais US (Windows)"}, {3, "Allemand (Windows)"}, {5, "Français (Windows)"},
                {6, "Anglais UK (Windows)"}, {17, "Polonais (Windows)"}, {33, "Anglais US (Mac)"},
                {35, "Allemand (Mac)"}, {37, "Français (Mac)"}, {50, "Belge français (Mac)"},
                {51, "Belge allemand (Mac)"}, {52, "Hongrois (Mac)"}, {53, "Tchèque (Mac)"}},
               "Doit correspondre au clavier de l'ordinateur, sinon les codes tapés en mode HID sont faux."),
        toggle(1051, "Gérer la touche Verr. Maj", kOutput),
        number(1037, "Type de caractères envoyés", kOutput, 0, 3),
        number(1084, "Encodage de saisie", kOutput, 0, 3),
        number(1081, "Type de clavier installé", kOutput, 0, 255),
        toggle(1082, "Utiliser le clavier installé", kOutput),
        toggle(1042, "Trémas allemands", kOutput),
        toggle(1041, "Jeu de caractères ISO 8859", kOutput),
        choice(1031, "Délai entre deux caractères envoyés", kOutput,
               {{0, "0 ms (rapide)"}, {8, "8 ms"}, {16, "16 ms"}, {32, "32 ms"}, {64, "64 ms"}, {96, "96 ms"},
                {128, "128 ms"}, {255, "256 ms"}}),
        toggle(1009, "Ajouter la date", kOutput),
        toggle(1010, "Ajouter l'heure", kOutput),
        toggle(1056, "Tiret avant le code additionnel (ISBN, ISSN, EAN)", kOutput),
        toggle(1057, "Masquer les symboles de début spéciaux", kOutput),
        toggle(1038, "Remplacer le caractère GS", kOutput),
        character(1034, "Caractère de remplacement de GS", kOutput),
        toggle(3036, "FUNC1 envoyé comme caractère ASCII 29", kOutput),
        number(1062, "Langue / gestion des données", kOutput, 0, 255),
        // Alimentation, connexion, memoire
        toggle(1012, "Extinction automatique", kPower),
        choice(1021, "Délai d'extinction automatique", kPower,
               {{1, "1 min"}, {3, "3 min"}, {5, "5 min"}, {10, "10 min"}, {30, "30 min"}, {60, "60 min"}}),
        number(1007, "Mode Bluetooth", kPower, 0, 3),
        toggle(1045, "Mode sans fil", kPower),
        toggle(1054, "Port série virtuel USB", kPower),
        toggle(1013, "Mode inventaire (codes gardés en mémoire)", kPower),
        toggle(1043, "Refuser les doublons en mode inventaire", kPower),
        toggle(1014, "Double clic pour envoyer la mémoire", kPower),
        number(1071, "Double clic en mode inventaire", kPower, 0, 3),
        number(1059, "Codes en double", kPower, 0, 3),
        toggle(1015, "Vider la mémoire à l'allumage", kPower),
        toggle(1016, "Envoyer la mémoire automatiquement", kPower),
        toggle(1048, "Raccourcis clavier", kPower),
        toggle(1053, "Fonctions personnalisées", kPower),
        // Types de codes
        toggle(2001, "Codabar", kCodes),
        toggle(2002, "IATA 2 parmi 5", kCodes),
        toggle(2003, "2 parmi 5 entrelacé", kCodes),
        toggle(2004, "Matrix 2 parmi 5", kCodes),
        toggle(2005, "2 parmi 5 standard", kCodes),
        toggle(2006, "Code 39", kCodes),
        toggle(2007, "Code 93", kCodes),
        toggle(2008, "Code 128", kCodes),
        toggle(2009, "EAN-8", kCodes),
        toggle(2010, "EAN-13", kCodes),
        toggle(2011, "UPC-A", kCodes),
        toggle(2012, "UPC-E0", kCodes),
        toggle(2016, "UPC-E1", kCodes),
        toggle(2013, "MSI", kCodes),
        toggle(2014, "Code 11", kCodes),
        toggle(2015, "Code postal chinois", kCodes),
        toggle(2018, "Aztec", kCodes),
        toggle(2019, "MaxiCode", kCodes),
        toggle(2020, "Han Xin", kCodes),
        toggle(2021, "DataMatrix", kCodes),
        toggle(2022, "QR Code", kCodes, "Indispensable pour les étiquettes QRProtec."),
        toggle(2023, "PDF417", kCodes),
        toggle(2024, "GS1-128", kCodes),
        toggle(2025, "GS1 DataBar (RSS-14) composite", kCodes),
        toggle(2028, "GS1 DataBar (RSS-14)", kCodes),
        toggle(3038, "GS1 DataBar Expanded", kCodes),
        toggle(1070, "GS1 DataBar Limited", kCodes),
        toggle(1072, "GS1 DataBar Limited", kCodes),
        toggle(2026, "Plessey", kCodes),
        toggle(2027, "Telepen", kCodes),
        toggle(3016, "Numéro d'identification de véhicule (VIN)", kCodes),
        toggle(3019, "USPS et FedEx", kCodes),
        // Options des types de codes
        toggle(3001, "Codabar : caractères de début et de fin", kCodeOptions),
        toggle(3002, "Code 39 : caractères de début et de fin", kCodeOptions),
        toggle(3003, "Code 39 : ASCII complet", kCodeOptions),
        number(3009, "Code 39 : clé de contrôle", kCodeOptions, 0, 3),
        toggle(3017, "Code 39 converti en Code 32", kCodeOptions),
        toggle(3018, "Code 32 : préfixe A", kCodeOptions),
        toggle(3004, "UPC-A : envoyer le 0 initial", kCodeOptions),
        toggle(3005, "UPC-E : envoyer le premier caractère", kCodeOptions),
        toggle(3042, "UPC-A : envoyer le System Number", kCodeOptions),
        toggle(3047, "UPC-E converti en UPC-A", kCodeOptions),
        toggle(3012, "UPC-A : envoyer la clé de contrôle", kCodeOptions),
        toggle(3013, "UPC-E : envoyer la clé de contrôle", kCodeOptions),
        toggle(3010, "EAN-8 : envoyer la clé de contrôle", kCodeOptions),
        toggle(3011, "EAN-13 : envoyer la clé de contrôle", kCodeOptions),
        toggle(3030, "EAN-8 étendu en EAN-13", kCodeOptions),
        toggle(3032, "EAN-13 converti en ISBN", kCodeOptions),
        toggle(3033, "EAN-13 converti en ISSN", kCodeOptions),
        toggle(3045, "EAN : code additionnel à 2 chiffres", kCodeOptions),
        toggle(3046, "EAN : code additionnel à 5 chiffres", kCodeOptions),
        toggle(3029, "Code additionnel obligatoire", kCodeOptions),
        toggle(3039, "IATA 2 parmi 5 : vérifier la clé", kCodeOptions),
        toggle(3006, "IATA 2 parmi 5 : envoyer la clé", kCodeOptions),
        toggle(3040, "Matrix 2 parmi 5 : vérifier la clé", kCodeOptions),
        toggle(3007, "Matrix 2 parmi 5 : envoyer la clé", kCodeOptions),
        toggle(3041, "2 parmi 5 standard : vérifier la clé", kCodeOptions),
        toggle(3008, "2 parmi 5 standard : envoyer la clé", kCodeOptions),
        number(3043, "Code 11 : nombre de clés de contrôle", kCodeOptions, 0, 3),
        toggle(3014, "Code 11 : envoyer la clé", kCodeOptions),
        number(3044, "MSI : nombre de clés de contrôle", kCodeOptions, 0, 3),
        toggle(3015, "MSI : envoyer la clé", kCodeOptions),
        toggle(3034, "Code postal chinois : vérifier la clé", kCodeOptions),
        toggle(3035, "Code postal chinois : envoyer la clé", kCodeOptions),
        toggle(3027, "Plessey : vérifier la clé", kCodeOptions),
        toggle(3028, "Telepen : vérifier la clé", kCodeOptions),
        toggle(3037, "Telepen alphanumérique", kCodeOptions),
        toggle(3031, "Symbologie (symbolo)", kCodeOptions),
        toggle(3048, "Ignorer les codes courts", kCodeOptions),
        number(3020, "QR Code : couleurs normales / inversées", kCodeOptions, 0, 3),
        number(3024, "QR Code : plusieurs codes", kCodeOptions, 0, 3),
        number(3022, "DataMatrix : plusieurs codes", kCodeOptions, 0, 3),
        number(3023, "DataMatrix : couleurs normales / inversées", kCodeOptions, 0, 3),
        number(3025, "PDF417 : plusieurs codes", kCodeOptions, 0, 3),
        number(3026, "PDF417 : couleurs normales / inversées", kCodeOptions, 0, 3),
        number(3021, "Han Xin : couleurs normales / inversées", kCodeOptions, 0, 3),
        // Usine
        toggle(1017, "Mode vieillissement (usine)", kFactory),
        toggle(1018, "Mode test (usine)", kFactory),
        toggle(1039, "Mise à jour du lecteur 2D en cours", kFactory),
        toggle(1040, "Mise à jour du lecteur 2D par port série", kFactory),
    };
    std::map<int, ScannerSettingInfo> catalogue;
    for (ScannerSettingInfo& info : list)
        catalogue.emplace(info.flag, std::move(info));
    return catalogue;
}

const std::map<int, ScannerSettingInfo>& catalogue() {
    static const std::map<int, ScannerSettingInfo> instance = build_catalogue();
    return instance;
}

std::string compact_upper(const std::string& value) {
    std::string result;
    for (const char c : value)
        if (std::isalnum(static_cast<unsigned char>(c)))
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

} // namespace

const std::vector<ScannerModel>& scanner_models() {
    static const std::vector<ScannerModel> models = {
        {1, "Pro 8"}, {2, "BCST-45"}, {3, "BCST-23"}, {4, "BCST-91"}, {5, "BCST-42"}, {6, "BCST-54"},
        {7, "BCST-55"}, {8, "BCST-73"}, {9, "BCST-75"}, {10, "BCST-43"}, {11, "P7"}, {12, "BCST-21"},
        {13, "BCST-60"}, {14, "BCST-70"}, {15, "P6"}, {16, "BCST-35"}, {17, "Pro 8 AI"}, {18, "BCST-45 AI"},
        {19, "BCST-23 AI"}, {20, "BCST-91 AI"}, {21, "BCST-42 AI"}, {22, "BCST-54 AI"}, {23, "BCST-55 AI"},
        {24, "BCST-73 AI"}, {25, "BCST-75 AI"}, {26, "BCST-43 AI"}, {27, "P7 AI"}, {28, "BCST-21 AI"},
        {29, "BCST-60 AI"}, {30, "BCST-70 AI"}, {31, "P6 AI"}, {32, "BCST-35 AI"}, {33, "BCST-46"},
        {34, "BCST-46 AI"}, {35, "BCST-72"}, {36, "BCST-72 AI"}, {37, "BCST-75S"}, {38, "BCST-75S AI"},
        {39, "BCST-36"}, {40, "BCST-36 AI"}, {41, "BCST-61"}, {42, "BCST-61 AI"}, {43, "BCST-47"},
        {44, "BCST-47 AI"}, {45, "PBS002"}, {46, "Pro 8 SE"}, {47, "Pro 8 SE AI"}, {48, "BCST-49"},
        {49, "BCST-49 AI"}, {50, "B560B"}, {51, "PBS005 AI"},
    };
    return models;
}

const char* scanner_model_label(int type) {
    for (const ScannerModel& model : scanner_models())
        if (model.type == type)
            return model.label;
    return "";
}

int detect_scanner_model(const std::string& device_name) {
    const std::string name = compact_upper(device_name);
    // Le libelle le plus long d'abord : "BCST-75S" avant "BCST-75", "Pro 8 SE" avant "Pro 8"
    int best = 0;
    std::size_t best_length = 0;
    for (const ScannerModel& model : scanner_models()) {
        const std::string label = model.label;
        if (label.size() > 3 && label.compare(label.size() - 3, 3, " AI") == 0)
            continue;
        const std::string key = compact_upper(label);
        const std::size_t position = name.find(key);
        if (position == std::string::npos || key.size() <= best_length)
            continue;
        // "BCST-75" ne doit pas reconnaitre "BCST-751" : le chiffre suivant fait partie du modele
        const std::size_t end = position + key.size();
        if (end < name.size() && std::isdigit(static_cast<unsigned char>(name[end])) &&
            std::isdigit(static_cast<unsigned char>(key.back())))
            continue;
        best = model.type;
        best_length = key.size();
    }
    return best;
}

const std::vector<const char*>& scanner_setting_groups() {
    static const std::vector<const char*> groups = {kSound, kScan, kOutput, kPower, kCodes, kCodeOptions,
                                                    kOthers, kFactory};
    return groups;
}

ScannerSettingInfo scanner_setting_info(int flag, const std::string& name) {
    // 1002 : interrupteur d'intensite (0 faible, 1 forte) ou rappel par vibration sur 2 bits selon le modele
    if (flag == 1002) {
        if (name == "shake_Reminder")
            return choice(1002, "Vibration à chaque scan", kSound,
                          {{0, "Désactivée"}, {1, "Faible"}, {2, "Moyenne"}, {3, "Forte"}});
        return choice(1002, "Intensité de la vibration", kSound, {{0, "Faible"}, {1, "Forte"}});
    }
    const auto found = catalogue().find(flag);
    if (found != catalogue().end())
        return found->second;
    ScannerSettingInfo info = number(flag, "", kOthers, 0, 255);
    return info;
}

std::string scanner_setting_command(const std::vector<ScannerSettingValue>& values) {
    std::string command = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index)
            command += ',';
        command += "{\"flag\":" + std::to_string(values[index].flag) + ",\"value\":" +
                   std::to_string(values[index].value) + "}";
    }
    return command + "]";
}

} // namespace qrprotec
