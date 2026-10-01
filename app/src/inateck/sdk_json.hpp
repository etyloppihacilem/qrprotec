#pragma once

#include <string>
#include <vector>

namespace qrprotec {

struct InateckDevice;

// Lecture minimale des reponses JSON du SDK Inateck (pas de dependance JSON).

// Valeur texte de "key", sequences d'echappement JSON decodees ("" si absente).
std::string sdk_json_string(const char* json, const char* key);
// true si "status" vaut 0.
bool sdk_json_success(const char* json);
// Objets du tableau "devices" (ou "device_list"), chacun sous forme de texte JSON.
std::vector<std::string> sdk_json_device_objects(const char* json);

// Code lu par la douchette sans le retour a la ligne final que le SDK ajoute.
std::string clean_scan_code(std::string code);

// Le nom ressemble-t-il a celui d'une douchette Inateck ?
bool looks_like_scanner(const std::string& name);

// Appareils a essayer pour la connexion automatique, dans l'ordre : la derniere douchette
// connectee, puis les noms de douchette Inateck, puis les autres. Les appareils refuses sont ignores.
std::vector<InateckDevice> connection_candidates(const std::vector<InateckDevice>& devices,
                                                 const std::string& preferred_id,
                                                 const std::vector<std::string>& rejected_ids);

} // namespace qrprotec
