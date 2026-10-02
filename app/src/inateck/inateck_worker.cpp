#include "inateck_worker.hpp"

#include "sdk_json.hpp"

#include <algorithm>
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
void InateckWorker::refresh_settings() { enqueue({CommandType::RefreshSettings}); }
void InateckWorker::set_volume(int value) { enqueue({CommandType::SetVolume, {}, value}); }
void InateckWorker::set_vibration(bool enabled) { enqueue({CommandType::SetVibration, {}, enabled ? 1 : 0}); }
void InateckWorker::set_prefix(const std::string& value) { enqueue({CommandType::SetPrefix, value}); }
void InateckWorker::set_suffix(const std::string& value) { enqueue({CommandType::SetSuffix, value}); }
void InateckWorker::set_name(const std::string& value) { enqueue({CommandType::SetName, value}); }
void InateckWorker::set_sdk_output(bool enabled) { enqueue({CommandType::SetSdkOutput, {}, enabled ? 1 : 0}); }
void InateckWorker::signal_error(const ScannerErrorSignal& signal) { enqueue({CommandType::SignalError, {}, 0, signal}); }

std::vector<ScanEvent> InateckWorker::take_scans() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    // Le SDK termine chaque code par un retour a la ligne
    on_scan_text(clean_scan_code(code), ScanSource::Sdk);
}

void InateckWorker::on_scan_text(const std::string& code, ScanSource source) {
    if (code.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.last_scan = code;
    scans_.push_back({code, source});
}

void InateckWorker::on_disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.authenticated = false;
}

void InateckWorker::set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.error = error;
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
    snapshot_.error.clear();
    auto_connect_ = false;
    return true;
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
        if (!id.empty()) {
            inateck_scanner_ble_get_battery(id.c_str());
            inateck_scanner_ble_get_prefix(id.c_str());
            inateck_scanner_ble_get_suffix(id.c_str());
        }
        break;
    case CommandType::SetVolume:
    case CommandType::SetVibration: {
        if (id.empty()) return;
        const int flag = command.type == CommandType::SetVolume ? 1001 : 1002;
        const std::string setting = "[{\"flag\":" + std::to_string(flag) + ",\"value\":" + std::to_string(command.number) + "}]";
        result = inateck_scanner_ble_set_setting_info(id.c_str(), setting.c_str(), 3);
        break;
    }
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
    if (result && !sdk_json_success(result))
        set_error(result);
#endif
}

} // namespace qrprotec