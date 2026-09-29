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

    void on_discovery(const char* json);
    void on_scan(const char* json);
    void on_disconnect();

private:
    enum class CommandType {
        StartDiscovery, StopDiscovery, Connect, Disconnect, RefreshSettings,
        SetVolume, SetVibration, SetPrefix, SetSuffix, SetName, SetSdkOutput,
        Shutdown
    };
    struct Command {
        CommandType type;
        std::string value;
        int number = 0;
    };

    void enqueue(Command command);
    void run();
    void execute(const Command& command);
    void set_error(const std::string& error);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Command> commands_;
    InateckSnapshot snapshot_;
    std::thread thread_;
    bool stopping_ = false;
};

} // namespace qrprotec