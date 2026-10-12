#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "scanner_settings.hpp"

namespace qrprotec {

struct InateckDevice {
    std::string id;
    std::string name;
    bool connected = false;
};

// Phone : telephone utilise comme douchette (relais WebSocket, voir net/remote_scanner_link.hpp)
enum class ScanSource { Sdk, Hid, Manual, Phone };

struct ScanEvent {
    std::string code;
    ScanSource source = ScanSource::Manual;
};

// Parametres du signal "mauvais scan" envoye a la douchette (unites du SDK Inateck).
struct ScannerErrorSignal {
    int beep_on = 3;
    int beep_off = 1;
    int beep_count = 3;
    int led_color = 4; // 2 vert, 3 bleu, 4 orange (pas de rouge sur la douchette)
    int led_on = 5;
    int led_off = 2;
    int led_count = 3;
};

struct InateckSnapshot {
    bool sdk_available = false;
    bool initialized = false;
    bool discovering = false;
    bool connecting = false;
    bool connected = false;
    bool authenticated = false;
    std::string selected_id;
    std::string selected_name;
    std::string preferred_id; // derniere douchette connectee (reconnexion automatique)
    std::string last_scan;
    std::string error;
    std::vector<InateckDevice> devices;

    // Modele utilise pour lire et ecrire les parametres : choisi a la main (model_choice) ou deduit
    // du nom de la douchette (model_choice == 0). 0 = inconnu.
    int model_choice = 0;
    int model = 0;
    bool settings_loading = false;
    std::vector<ScannerSettingValue> settings; // vide tant que non lus
    // Informations lues sur la douchette (texte renvoye par le SDK)
    std::string battery;
    std::string software_version;
    std::string hardware_version;
    std::string mac;
    std::string prefix;
    std::string suffix;
    std::string cache_count; // nombre de codes en memoire renvoye par la douchette (vide : inconnu)
    // Mode inventaire : la douchette renvoie les codes gardes en memoire (ou leur nombre) comme des scans.
    // Pendant la capture, ils sont captes ici au lieu d'aller dans la pile de scans.
    bool inventory_reading = false;
    bool inventory_waiting = false; // capture en cours, rien recu pour l'instant
    std::vector<std::string> inventory_codes;
    std::string sdk_version;
    std::string message; // resultat de la derniere action (ex. "Heure synchronisee")
};

class InateckWorker {
public:
    InateckWorker();
    ~InateckWorker();
    InateckWorker(const InateckWorker&) = delete;
    InateckWorker& operator=(const InateckWorker&) = delete;

    InateckSnapshot snapshot() const;
    // Douchette a privilegier lors de la connexion automatique qui suit une recherche.
    void set_preferred_device(const std::string& device_id);
    std::string preferred_device() const;
    void start_discovery();
    void stop_discovery();
    void connect(const std::string& device_id, const std::string& device_name);
    void disconnect();
    // 0 : modele deduit du nom de la douchette
    void set_model(int model);
    // Relit tous les parametres et les informations de la douchette (fait aussi a la connexion)
    void refresh_settings();
    void set_setting(int flag, int value);
    void beep();
    void sync_time();
    void restart();
    void factory_reset();
    void open_all_codes();
    void close_all_codes();
    void reset_all_codes();
    // Capte ce que la douchette renvoie de sa memoire : les codes, ou leur nombre (« Total Counters = N »).
    // Les commandes d'envoi du SDK (upload_cache, upload_cache_number) font envoyer la memoire sur le cable
    // USB, jamais en Bluetooth (documentation Inateck, confirme par btmon) : l'envoi se declenche en
    // scannant le QR « Data Upload » (/*SetFun84*/) ou « Upload the Number » (/*SetFun85*/) affiche a l'ecran.
    enum class InventoryCapture { Codes, Count };
    void capture_inventory(InventoryCapture capture);
    void stop_inventory_capture();
    void clear_cache(); // commande du SDK, marche en Bluetooth
    // Envoie les codes recuperes de la memoire dans la pile de scans, comme s'ils venaient d'etre lus
    void process_inventory_codes();
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
        StartDiscovery, StopDiscovery, Connect, Disconnect, RefreshSettings, SetSetting, Beep, SyncTime,
        Restart, FactoryReset, OpenAllCodes, CloseAllCodes, ResetAllCodes, ClearCache, SetPrefix, SetSuffix, SetName, SetSdkOutput, SignalError, Shutdown
    };
    struct Command {
        CommandType type;
        std::string value;
        int number = 0;
        int flag = 0;
        ScannerErrorSignal signal{};
    };

    void enqueue(Command command);
    void run();
    void execute(const Command& command);
    void set_error(const std::string& error);
    void finish_discovery();
    bool connect_device(const std::string& device_id, const std::string& device_name);
    void reject_device(const std::string& device_id);
    void read_settings(const std::string& device_id);
    void set_message(const std::string& message);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Command> commands_;
    InateckSnapshot snapshot_;
    std::vector<ScanEvent> scans_;
    std::thread thread_;
    bool stopping_ = false;
    bool sdk_ready_ = false;     // init + wait_available deja faits (une seule fois)
    bool auto_connect_ = false;  // se connecter a la douchette connue a la fin de la recherche
    bool stop_requested_ = false;
    std::chrono::steady_clock::time_point discovery_deadline_{};
    std::vector<std::string> rejected_ids_; // appareils qui ne repondent pas comme une douchette
    std::optional<InventoryCapture> inventory_capture_;
    std::chrono::steady_clock::time_point inventory_deadline_{};
    void expire_inventory_read(); // mutex_ verrouille
};

// Duree de la recherche avant la connexion automatique (si la douchette connue n'apparait pas avant).
inline constexpr std::chrono::seconds kInateckDiscoveryDuration{8};
// Capture de la memoire (mode inventaire) : attente du scan du QR d'envoi, puis fin apres ce delai sans
// nouveau code.
inline constexpr std::chrono::seconds kInventoryWaitTimeout{90};
inline constexpr std::chrono::seconds kInventoryReadTimeout{4};
// Delai entre deux tentatives de reconnexion automatique a la douchette connue (base rallumee...).
inline constexpr std::chrono::seconds kInateckReconnectInterval{15};

} // namespace qrprotec