#include "inateck_worker.hpp"

#include <cctype>
#include <cstring>
#include <utility>

#ifdef QRPROTEC_HAS_INATECK
#include "inateck_scanner_ble_cpp.h"
#endif

namespace qrprotec {

namespace {
std::mutex callback_mutex;
InateckWorker* callback_owner = nullptr;

std::string json_string(const char* json, const char* key) {
    if (!json || !key)
        return {};
    const std::string needle = std::string("\"") + key + "\"";
    const char* start = std::strstr(json, needle.c_str());
    if (!start)
        return {};
    start = std::strchr(start + needle.size(), ':');
    if (!start || !std::strchr(start, '"'))
        return {};
    start = std::strchr(start, '"') + 1;
    const char* end = std::strchr(start, '"');
    return end ? std::string(start, end) : std::string(start);
}

bool json_success(const char* json) {
    if (!json)
        return false;
    const char* status = std::strstr(json, "\"status\"");
    if (!status)
        return false;
    status = std::strchr(status, ':');
    if (!status)
        return false;
    ++status;
    while (*status && std::isspace(static_cast<unsigned char>(*status)))
        ++status;
    return *status == '0';
}

#ifdef QRPROTEC_HAS_INATECK
void discover_callback(const char* json) {
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

void InateckWorker::on_discovery(const char* json) {
    const std::string id = json_string(json, "id");
    if (id.empty())
        return;
    const std::string name = json_string(json, "device_name");
    std::lock_guard<std::mutex> lock(mutex_);
    for (InateckDevice& device : snapshot_.devices) {
        if (device.id == id) {
            device.name = name;
            return;
        }
    }
    snapshot_.devices.push_back({id, name, false});
}

void InateckWorker::on_scan(const char* json) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.last_scan = json_string(json, "code");
    if (snapshot_.last_scan.empty() && json)
        snapshot_.last_scan = json;
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
            condition_.wait(lock, [this] { return !commands_.empty(); });
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

void InateckWorker::execute(const Command& command) {
#ifndef QRPROTEC_HAS_INATECK
    (void)command;
    set_error("SDK Inateck non lie : fournissez INATECK_SDK_LIBRARY dans CMake");
    return;
#else
    const std::string id = snapshot().selected_id;
    const char* result = nullptr;
    switch (command.type) {
    case CommandType::StartDiscovery:
        result = inateck_scanner_ble_init();
        if (!json_success(result)) { set_error(result ? result : "Initialisation impossible"); return; }
        result = inateck_scanner_ble_wait_available();
        if (!json_success(result)) { set_error(result ? result : "Bluetooth indisponible"); return; }
        inateck_scanner_ble_set_discover_callback(discover_callback);
        result = inateck_scanner_ble_start_discover();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.initialized = json_success(result);
            snapshot_.discovering = snapshot_.initialized;
        }
        break;
    case CommandType::StopDiscovery:
        result = inateck_scanner_ble_stop_discover();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.discovering = false;
        }
        break;
    case CommandType::Connect: {
        const std::size_t separator = command.value.find('\n');
        const std::string device_id = command.value.substr(0, separator);
        const std::string device_name = separator == std::string::npos ? std::string() : command.value.substr(separator + 1);
        result = inateck_scanner_ble_connect(device_id.c_str());
        if (!json_success(result)) { set_error(result ? result : "Connexion impossible"); return; }
        inateck_scanner_ble_check_communication(device_id.c_str());
        result = inateck_scanner_ble_auth(device_id.c_str());
        if (!json_success(result)) { set_error(result ? result : "Authentification impossible"); return; }
        inateck_scanner_ble_set_code_callback(device_id.c_str(), code_callback);
        inateck_scanner_ble_set_disconnect_callback(device_id.c_str(), disconnect_callback);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.connected = true;
        snapshot_.authenticated = true;
        snapshot_.selected_id = device_id;
        snapshot_.selected_name = device_name;
        break;
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
    case CommandType::Shutdown:
        return;
    }
    if (result && !json_success(result))
        set_error(result);
#endif
}

} // namespace qrprotec