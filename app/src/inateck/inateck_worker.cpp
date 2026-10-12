#include "inateck_worker.hpp"

#include "sdk_json.hpp"
#include "../printer/logging.hpp"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <utility>

#ifdef QRPROTEC_HAS_INATECK
#include "inateck_scanner_ble_cpp.h"
#endif

namespace qrprotec {

namespace {
std::mutex callback_mutex;
InateckWorker* callback_owner = nullptr;

#ifdef QRPROTEC_HAS_INATECK
void discover_callback(const char* json) {
    std::cerr << "[Inateck BLE] appareil detecte: " << (json ? json : "<null>") << std::endl;
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (callback_owner)
        callback_owner->on_discovery(json);
}

void code_callback(const char* json) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (callback_owner)
        callback_owner->on_scan(json);
}

void disconnect_callback(const char*) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (callback_owner)
        callback_owner->on_disconnect();
}
#endif
} // namespace

InateckWorker::InateckWorker() {
#ifdef QRPROTEC_HAS_INATECK
    snapshot_.sdk_available = true;
    std::lock_guard<std::mutex> lock(callback_mutex);
    callback_owner = this;
#else
    snapshot_.error = "Bibliotheque Inateck Linux indisponible";
    error_log(snapshot_.error, "douchette");
#endif
    thread_ = std::thread(&InateckWorker::run, this);
}

InateckWorker::~InateckWorker() {
    enqueue({CommandType::Shutdown});
    if (thread_.joinable())
        thread_.join();
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (callback_owner == this)
        callback_owner = nullptr;
}

InateckSnapshot InateckWorker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void InateckWorker::set_preferred_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.preferred_id = device_id;
}

std::string InateckWorker::preferred_device() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.preferred_id;
}

void InateckWorker::enqueue(Command command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        commands_.push_back(std::move(command));
    }
    condition_.notify_one();
}

void InateckWorker::start_discovery() { enqueue({CommandType::StartDiscovery}); }
void InateckWorker::stop_discovery() { enqueue({CommandType::StopDiscovery}); }
void InateckWorker::connect(const std::string& id, const std::string& name) { enqueue({CommandType::Connect, id + "\n" + name}); }
void InateckWorker::disconnect() { enqueue({CommandType::Disconnect}); }
void InateckWorker::set_model(int model) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.model_choice == model)
            return;
        snapshot_.model_choice = model;
        snapshot_.model = model ? model : detect_scanner_model(snapshot_.selected_name);
        snapshot_.settings.clear();
        if (!snapshot_.connected)
            return;
    }
    refresh_settings();
}
void InateckWorker::refresh_settings() { enqueue({CommandType::RefreshSettings}); }
void InateckWorker::set_setting(int flag, int value) { enqueue({CommandType::SetSetting, {}, value, flag}); }
void InateckWorker::beep() { enqueue({CommandType::Beep}); }
void InateckWorker::sync_time() { enqueue({CommandType::SyncTime}); }
void InateckWorker::restart() { enqueue({CommandType::Restart}); }
void InateckWorker::factory_reset() { enqueue({CommandType::FactoryReset}); }
void InateckWorker::open_all_codes() { enqueue({CommandType::OpenAllCodes}); }
void InateckWorker::close_all_codes() { enqueue({CommandType::CloseAllCodes}); }
void InateckWorker::reset_all_codes() { enqueue({CommandType::ResetAllCodes}); }
void InateckWorker::upload_cache() { enqueue({CommandType::UploadCache}); }
void InateckWorker::clear_cache() { enqueue({CommandType::ClearCache}); }

void InateckWorker::process_inventory_codes() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& code : snapshot_.inventory_codes)
        scans_.push_back({code, ScanSource::Sdk});
    snapshot_.inventory_codes.clear();
}

void InateckWorker::start_inventory_read() {
    std::lock_guard<std::mutex> lock(mutex_);
    inventory_read_ = true;
    inventory_deadline_ = std::chrono::steady_clock::now() + kInventoryReadTimeout;
    snapshot_.inventory_reading = true;
    snapshot_.inventory_codes.clear();
}

void InateckWorker::expire_inventory_read() {
    if (!inventory_read_ || std::chrono::steady_clock::now() < inventory_deadline_)
        return;
    snapshot_.cache_count = std::to_string(snapshot_.inventory_codes.size());
    snapshot_.message = snapshot_.inventory_codes.empty()
        ? "La douchette n'a renvoyé aucun code de sa mémoire (mémoire vide, ou sortie SDK désactivée ?)."
        : std::to_string(snapshot_.inventory_codes.size()) + " code(s) récupéré(s) de la mémoire.";
    info_log("Douchette : lecture de la mémoire terminée, " + snapshot_.cache_count + " code(s) reçu(s)");
    inventory_read_ = false;
    snapshot_.inventory_reading = false;
}
void InateckWorker::set_prefix(const std::string& value) { enqueue({CommandType::SetPrefix, value}); }
void InateckWorker::set_suffix(const std::string& value) { enqueue({CommandType::SetSuffix, value}); }
void InateckWorker::set_name(const std::string& value) { enqueue({CommandType::SetName, value}); }
void InateckWorker::set_sdk_output(bool enabled) { enqueue({CommandType::SetSdkOutput, {}, enabled ? 1 : 0}); }
void InateckWorker::signal_error(const ScannerErrorSignal& signal) { enqueue({CommandType::SignalError, {}, 0, 0, signal}); }

std::vector<ScanEvent> InateckWorker::take_scans() {
    std::lock_guard<std::mutex> lock(mutex_);
    expire_inventory_read();
    std::vector<ScanEvent> scans;
    scans.swap(scans_);
    return scans;
}

void InateckWorker::on_discovery(const char* json) {
    const std::string id = sdk_json_string(json, "id");
    if (id.empty())
        return;
    const std::string name = sdk_json_string(json, "device_name");
    bool stop_now = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(rejected_ids_.begin(), rejected_ids_.end(), id) != rejected_ids_.end())
            return;
        const auto known = std::find_if(snapshot_.devices.begin(), snapshot_.devices.end(),
                                        [&id](const InateckDevice& device) { return device.id == id; });
        if (known != snapshot_.devices.end())
            known->name = name;
        else
            snapshot_.devices.push_back({id, name, false});
        // La douchette connue est la : inutile d'attendre la fin de la recherche
        if (auto_connect_ && snapshot_.discovering && !stop_requested_ && !snapshot_.connected &&
            !snapshot_.preferred_id.empty() && id == snapshot_.preferred_id) {
            stop_requested_ = true;
            stop_now = true;
        }
    }
    if (stop_now)
        enqueue({CommandType::StopDiscovery});
}

void InateckWorker::on_scan(const char* json) {
    std::string code = sdk_json_string(json, "code");
    if (code.empty() && json)
        code = json;
    bool inventory = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inventory = inventory_read_;
    }
    if (inventory) {
        // Lecture de la memoire : trace brute (format non documente), plusieurs codes possibles par envoi
        info_log(std::string("Douchette : reçu pendant la lecture de la mémoire : ") + (json ? json : "<null>"));
        std::size_t start = 0;
        while (start <= code.size()) {
            const std::size_t end = code.find_first_of("\r\n", start);
            on_scan_text(code.substr(start, end == std::string::npos ? std::string::npos : end - start),
                         ScanSource::Sdk);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return;
    }
    // Le SDK termine chaque code par un retour a la ligne
    on_scan_text(clean_scan_code(code), ScanSource::Sdk);
}

void InateckWorker::on_scan_text(const std::string& code, ScanSource source) {
    if (code.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == ScanSource::Sdk && inventory_read_) {
        info_log("Douchette : code de la mémoire : " + code);
        snapshot_.inventory_codes.push_back(code);
        inventory_deadline_ = std::chrono::steady_clock::now() + kInventoryReadTimeout;
        return;
    }
    snapshot_.last_scan = code;
    scans_.push_back({code, source});
}

void InateckWorker::on_disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.authenticated = false;
    snapshot_.settings_loading = false;
}

void InateckWorker::set_error(const std::string& error) {
    error_log(error, "douchette");
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.error = error;
}

void InateckWorker::set_message(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.message = message;
}

void InateckWorker::run() {
    for (;;) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Pendant une recherche suivie d'une connexion automatique, l'arret est declenche
            // au bout de kInateckDiscoveryDuration si rien d'autre ne l'a demande avant.
            const auto timed = [this] { return snapshot_.discovering && auto_connect_ && !stop_requested_; };
            while (commands_.empty()) {
                if (!timed()) {
                    condition_.wait(lock);
                } else if (condition_.wait_until(lock, discovery_deadline_) == std::cv_status::timeout &&
                           commands_.empty() && timed()) {
                    stop_requested_ = true;
                    commands_.push_back({CommandType::StopDiscovery});
                }
            }
            command = std::move(commands_.front());
            commands_.erase(commands_.begin());
        }
        if (command.type == CommandType::Shutdown) {
#ifdef QRPROTEC_HAS_INATECK
            const InateckSnapshot state = snapshot();
            if (state.discovering)
                inateck_scanner_ble_stop_discover();
            if (state.connected && !state.selected_id.empty())
                inateck_scanner_ble_disconnect(state.selected_id.c_str());
#endif
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            return;
        }
        execute(command);
    }
}

void InateckWorker::reject_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(rejected_ids_.begin(), rejected_ids_.end(), device_id) == rejected_ids_.end())
        rejected_ids_.push_back(device_id);
    snapshot_.devices.erase(std::remove_if(snapshot_.devices.begin(), snapshot_.devices.end(),
                                           [&device_id](const InateckDevice& device) { return device.id == device_id; }),
                            snapshot_.devices.end());
}

// Connexion + authentification. Un appareil qui se connecte mais refuse l'authentification
// n'est pas une douchette Inateck : il est ecarte des recherches suivantes.
bool InateckWorker::connect_device(const std::string& device_id, const std::string& device_name) {
#ifndef QRPROTEC_HAS_INATECK
    (void)device_id;
    (void)device_name;
    return false;
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.connecting = true;
    }
    const auto fail = [this](const char* result, const char* fallback) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.connecting = false;
        snapshot_.error = result ? result : fallback;
        error_log(snapshot_.error, "douchette");
        return false;
    };
    const char* result = inateck_scanner_ble_connect(device_id.c_str());
    if (!sdk_json_success(result))
        return fail(result, "Connexion impossible");
    inateck_scanner_ble_check_communication(device_id.c_str());
    result = inateck_scanner_ble_auth(device_id.c_str());
    if (!sdk_json_success(result)) {
        std::cerr << "[Inateck BLE] " << device_id << " n'est pas une douchette: "
                  << (result ? result : "<null>") << std::endl;
        inateck_scanner_ble_disconnect(device_id.c_str());
        reject_device(device_id);
        return fail(result, "Authentification impossible");
    }
    inateck_scanner_ble_set_code_callback(device_id.c_str(), code_callback);
    inateck_scanner_ble_set_disconnect_callback(device_id.c_str(), disconnect_callback);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connecting = false;
    snapshot_.connected = true;
    snapshot_.authenticated = true;
    snapshot_.selected_id = device_id;
    snapshot_.selected_name = device_name;
    snapshot_.preferred_id = device_id;
    snapshot_.model = snapshot_.model_choice ? snapshot_.model_choice : detect_scanner_model(device_name);
    snapshot_.settings.clear();
    snapshot_.message.clear();
    snapshot_.error.clear();
    auto_connect_ = false;
    // Parametres lus des la connexion : la fenetre affiche les valeurs reelles de la douchette
    commands_.push_back({CommandType::RefreshSettings});
    return true;
#endif
}

#ifdef QRPROTEC_HAS_INATECK
namespace {
std::string data_text(const char* result) {
    return sdk_json_success(result) ? sdk_json_data_text(result) : std::string();
}
} // namespace
#endif

// Parametres (selon le modele) et informations de la douchette.
void InateckWorker::read_settings(const std::string& device_id) {
#ifndef QRPROTEC_HAS_INATECK
    (void)device_id;
#else
    int model = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.settings_loading = true;
        model = snapshot_.model;
    }
    const std::string battery = data_text(inateck_scanner_ble_get_battery(device_id.c_str()));
    const std::string software = data_text(inateck_scanner_ble_get_software_version(device_id.c_str()));
    const std::string hardware = data_text(inateck_scanner_ble_get_hardware_version(device_id.c_str()));
    const std::string mac = data_text(inateck_scanner_ble_get_mac(device_id.c_str()));
    const std::string prefix = data_text(inateck_scanner_ble_get_prefix(device_id.c_str()));
    const std::string suffix = data_text(inateck_scanner_ble_get_suffix(device_id.c_str()));
    const char* version = inateck_scanner_ble_sdk_version();
    std::vector<ScannerSettingValue> settings;
    std::string error;
    if (model == 0) {
        error = "Modèle de douchette inconnu : choisissez-le dans « Modèle » pour lire ses paramètres.";
    } else {
        const char* result = inateck_scanner_ble_get_setting_info(device_id.c_str(), model);
        std::cerr << "[Inateck BLE] parametres (" << scanner_model_label(model) << "): "
                  << (result ? result : "<null>") << std::endl;
        // Les parametres sont pris des qu'ils sont lisibles, quel que soit le "status" renvoye
        settings = sdk_json_settings(result);
        if (settings.empty())
            error = std::string("Lecture des paramètres impossible pour le modèle ") + scanner_model_label(model) +
                    " : vérifiez le modèle choisi (réponse du SDK dans journalctl).";
        if (settings.empty())
            error_log("Réponse de get_setting_info : " + std::string(result ? result : "<null>"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.settings_loading = false;
    snapshot_.battery = battery;
    snapshot_.software_version = software;
    snapshot_.hardware_version = hardware;
    snapshot_.mac = mac;
    snapshot_.prefix = prefix;
    snapshot_.suffix = suffix;
    snapshot_.sdk_version = version ? sdk_json_data_text(version) : std::string();
    if (snapshot_.sdk_version.empty() && version)
        snapshot_.sdk_version = version;
    snapshot_.settings = std::move(settings);
    if (!error.empty()) {
        snapshot_.error = error;
        error_log(error, "douchette");
    }
#endif
}

// Fin de recherche : connexion automatique seulement a la douchette deja utilisee. Les autres
// appareils (souris, casque...) ne sont jamais essayes d'office : l'utilisateur choisit dans le menu.
void InateckWorker::finish_discovery() {
    InateckDevice preferred;
    bool found = false;
    bool any_device = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!auto_connect_ || snapshot_.connected)
            return;
        auto_connect_ = false;
        any_device = !snapshot_.devices.empty();
        for (const InateckDevice& device : snapshot_.devices) {
            if (!snapshot_.preferred_id.empty() && device.id == snapshot_.preferred_id) {
                preferred = device;
                found = true;
            }
        }
    }
    if (found && connect_device(preferred.id, preferred.name))
        return;
    if (any_device) {
        set_error("Choisissez la douchette dans le menu Douchette > Connecter à.");
        return;
    }
    set_error("Aucune douchette trouvée. Vérifiez qu'elle est allumée en mode SDK et qu'elle n'est pas "
              "appairée à l'ordinateur. Déconnectez aussi les autres appareils Bluetooth (casque, souris...) : "
              "le SDK Inateck échoue sinon. Puis relancez la recherche.");
}

void InateckWorker::execute(const Command& command) {
#ifndef QRPROTEC_HAS_INATECK
    (void)command;
    set_error("SDK Inateck non lie : fournissez INATECK_SDK_LIBRARY dans CMake");
    return;
#else
    const std::string id = snapshot().selected_id;
    const char* result = nullptr;
    switch (command.type) {
    case CommandType::StartDiscovery: {
        if (snapshot().discovering)
            return;
        if (!sdk_ready_) {
            result = inateck_scanner_ble_init();
            if (!sdk_json_success(result)) { set_error(result ? result : "Initialisation impossible"); return; }
            result = inateck_scanner_ble_wait_available();
            if (!sdk_json_success(result)) { set_error(result ? result : "Bluetooth indisponible"); return; }
            inateck_scanner_ble_set_discover_callback(discover_callback);
            sdk_ready_ = true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.error.clear();
            auto_connect_ = !snapshot_.connected;
            stop_requested_ = false;
            discovery_deadline_ = std::chrono::steady_clock::now() + kInateckDiscoveryDuration;
        }
        result = inateck_scanner_ble_start_discover();
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.initialized = sdk_json_success(result);
        snapshot_.discovering = snapshot_.initialized;
        if (!snapshot_.discovering)
            auto_connect_ = false;
        break;
    }
    case CommandType::StopDiscovery:
        if (!snapshot().discovering)
            return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        result = inateck_scanner_ble_stop_discover();
        if (sdk_json_success(result)) {
            const char* devices = inateck_scanner_ble_get_devices();
            std::cerr << "[Inateck BLE] liste des appareils: "
                      << (devices ? devices : "<null>") << std::endl;
            if (!sdk_json_success(devices)) {
                set_error(devices ? devices : "Lecture des douchettes impossible");
            } else {
                for (const std::string& device : sdk_json_device_objects(devices))
                    on_discovery(device.c_str());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.discovering = false;
        }
        finish_discovery();
        break;
    case CommandType::Connect: {
        const std::size_t separator = command.value.find('\n');
        const std::string device_id = command.value.substr(0, separator);
        const std::string device_name = separator == std::string::npos ? std::string() : command.value.substr(separator + 1);
        connect_device(device_id, device_name);
        return;
    }
    case CommandType::Disconnect:
        if (!id.empty())
            result = inateck_scanner_ble_disconnect(id.c_str());
        on_disconnect();
        break;
    case CommandType::RefreshSettings:
        if (!id.empty() && snapshot().connected)
            read_settings(id);
        return;
    case CommandType::SetSetting: {
        const int model = snapshot().model;
        if (id.empty()) return;
        if (model == 0) { set_error("Choisissez le modèle de la douchette avant de modifier ses paramètres."); return; }
        const std::string setting = scanner_setting_command({{command.flag, {}, command.number}});
        result = inateck_scanner_ble_set_setting_info(id.c_str(), setting.c_str(), model);
        std::cerr << "[Inateck BLE] " << setting << " -> " << (result ? result : "<null>") << std::endl;
        if (!sdk_json_success(result)) {
            set_error(result ? result : "Modification du paramètre impossible");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.error.clear();
            const auto known = std::find_if(snapshot_.settings.begin(), snapshot_.settings.end(),
                                            [&command](const ScannerSettingValue& value) { return value.flag == command.flag; });
            if (known != snapshot_.settings.end())
                known->value = command.number;
            else
                snapshot_.settings.push_back({command.flag, {}, command.number});
        }
        // Volume ou vibration : la douchette fait entendre le nouveau reglage
        if (command.flag == 1001 || command.flag == 1002 || command.flag == 1035 || command.flag == 1047)
            inateck_scanner_ble_bee_or_shake(id.c_str());
        return;
    }
    case CommandType::Beep:
        if (!id.empty()) result = inateck_scanner_ble_bee_or_shake(id.c_str());
        break;
    case CommandType::SyncTime: {
        if (id.empty()) return;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        result = inateck_scanner_ble_set_time(id.c_str(), local.tm_hour, local.tm_min, local.tm_sec,
                                              local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
        if (sdk_json_success(result))
            set_message("Heure de la douchette synchronisée.");
        break;
    }
    case CommandType::Restart:
        if (!id.empty()) result = inateck_scanner_ble_restart(id.c_str());
        break;
    case CommandType::FactoryReset:
        if (id.empty()) return;
        result = inateck_scanner_ble_reset(id.c_str());
        if (sdk_json_success(result)) {
            set_message("Réglages d'usine restaurés.");
            read_settings(id);
        }
        break;
    case CommandType::OpenAllCodes:
    case CommandType::CloseAllCodes:
    case CommandType::ResetAllCodes:
        if (id.empty()) return;
        result = command.type == CommandType::OpenAllCodes  ? inateck_scanner_ble_open_all_code(id.c_str())
               : command.type == CommandType::CloseAllCodes ? inateck_scanner_ble_close_all_code(id.c_str())
                                                            : inateck_scanner_ble_reset_all_code(id.c_str());
        if (sdk_json_success(result))
            read_settings(id);
        break;
    case CommandType::UploadCache:
        if (id.empty()) return;
        // Les codes arrivent ensuite par le rappel des scans (on_scan_text)
        start_inventory_read();
        result = inateck_scanner_ble_inventory_upload_cache(id.c_str());
        info_log(std::string("Douchette : lecture de la mémoire demandée : ") + (result ? result : "<null>"));
        break;
    case CommandType::ClearCache:
        if (id.empty()) return;
        result = inateck_scanner_ble_inventory_clear_cache(id.c_str());
        if (sdk_json_success(result)) {
            set_message("Codes en mémoire de la douchette supprimés.");
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.cache_count = "0";
        }
        break;
    case CommandType::SetPrefix:
    case CommandType::SetSuffix: {
        if (id.empty() || command.value.size() > 31) return;
        std::vector<unsigned char> bytes(command.value.begin(), command.value.end());
        bytes.push_back(0xff);
        result = command.type == CommandType::SetPrefix
            ? inateck_scanner_ble_set_prefix(id.c_str(), bytes.data(), bytes.size())
            : inateck_scanner_ble_set_suffix(id.c_str(), bytes.data(), bytes.size());
        break;
    }
    case CommandType::SetName:
        if (!id.empty()) result = inateck_scanner_ble_set_name(id.c_str(), command.value.c_str());
        break;
    case CommandType::SetSdkOutput:
        if (!id.empty()) result = inateck_scanner_ble_set_hid_output(id.c_str(), command.number);
        break;
    case CommandType::SignalError: {
        if (id.empty() || !snapshot().authenticated) return;
        const auto byte = [](int value) { return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value); };
        const ScannerErrorSignal& signal = command.signal;
        result = inateck_scanner_set_led(id.c_str(), byte(signal.led_color), byte(signal.led_on),
                                         byte(signal.led_off), byte(signal.led_count));
        if (result && !sdk_json_success(result))
            set_error(result);
        result = inateck_scanner_set_bee(id.c_str(), byte(signal.beep_on), byte(signal.beep_off),
                                         byte(signal.beep_count));
        break;
    }
    case CommandType::Shutdown:
        return;
    }
    if (result && !sdk_json_success(result)) {
        if (command.type == CommandType::UploadCache) {
            std::lock_guard<std::mutex> lock(mutex_);
            inventory_read_ = false;
            snapshot_.inventory_reading = false;
        }
        set_error(result);
        return;
    }
    if (!result)
        return;
    switch (command.type) {
    case CommandType::SetPrefix: set_message("Préfixe appliqué."); break;
    case CommandType::SetSuffix: set_message("Suffixe appliqué."); break;
    case CommandType::SetName: set_message("Nom Bluetooth appliqué (visible après redémarrage de la douchette)."); break;
    case CommandType::SetSdkOutput: set_message("Mode de sortie appliqué."); break;
    case CommandType::Restart: set_message("Douchette redémarrée."); break;
    default: break;
    }
#endif
}

} // namespace qrprotec