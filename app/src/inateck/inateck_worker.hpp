#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qrprotec {

struct InateckDevice {
    std::string id;
    std::string name;
    bool connected = false;
};

enum class ScanSource { Sdk, Hid, Manual };

struct ScanEvent {
    std::string code;
    ScanSource source = ScanSource::Manual;
};

// Parametres du signal "mauvais scan" envoye a la douchette (unites du SDK Inateck).
struct ScannerErrorSignal {
    int beep_on = 3;
    int beep_off = 1;
    int beep_count = 3;
    int led_color = 1;
    int led_on = 5;
    int led_off = 2;
    int led_count = 3;
};

struct InateckSnapshot {
    bool sdk_available = false;
    bool initialized = false;
    bool discovering = false;
    bool connected = false;
    bool authenticated = false;
    std::string selected_id;
    std::string selected_name;
    std::string last_scan;
    std::string error;
    std::vector<InateckDevice> devices;
};

class InateckWorker {
public:
    InateckWorker();
    ~InateckWorker();
    InateckWorker(const InateckWorker&) = delete;
    InateckWorker& operator=(const InateckWorker&) = delete;

    InateckSnapshot snapshot() const;
    void start_discovery();
    void stop_discovery();
    void connect(const std::string& device_id, const std::string& device_name);
    void disconnect();
    void refresh_settings();
    void set_volume(int volume);
    void set_vibration(bool enabled);
    void set_prefix(const std::string& value);
    void set_suffix(const std::string& value);
    void set_name(const std::string& value);
    void set_sdk_output(bool enabled);
    void signal_error(const ScannerErrorSignal& signal);

    // Scans recus depuis le dernier appel (SDK ou clavier HID), dans l'ordre.
    std::vector<ScanEvent> take_scans();

    void on_discovery(const char* json);
    void on_scan(const char* json);
    void on_scan_text(const std::string& code, ScanSource source);
    void on_disconnect();

private:
    enum class CommandType {
        StartDiscovery, StopDiscovery, Connect, Disconnect, RefreshSettings,
        SetVolume, SetVibration, SetPrefix, SetSuffix, SetName, SetSdkOutput,
        SignalError, Shutdown
    };
    struct Command {
        CommandType type;
        std::string value;
        int number = 0;
        ScannerErrorSignal signal{};
    };

    void enqueue(Command command);
    void run();
    void execute(const Command& command);
    void set_error(const std::string& error);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Command> commands_;
    InateckSnapshot snapshot_;
    std::vector<ScanEvent> scans_;
    std::thread thread_;
    bool stopping_ = false;
};

} // namespace qrprotec