#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qrprotec {

// Liaison « brute » vers une imprimante TSPL / ZPL : fichier de peripherique (USB classe imprimante,
// /dev/usb/lp0, ou port serie) ou reseau (« 192.168.1.50 » ou « 192.168.1.50:9100 », port 9100 par defaut).
class RawTransport {
public:
    RawTransport() = default;
    ~RawTransport();
    RawTransport(const RawTransport&) = delete;
    RawTransport& operator=(const RawTransport&) = delete;

    bool open(const std::string& address, int timeout_ms, std::string& error);
    void close();
    bool is_open() const { return file_descriptor_ >= 0; }
    bool write_bytes(const std::vector<std::uint8_t>& bytes, std::string& error);

    // true si l'adresse designe l'imprimante par le reseau (pas un chemin de fichier)
    static bool is_network_address(const std::string& address);
    static bool split_host_port(const std::string& address, std::string& host, std::string& port);

private:
    int file_descriptor_ = -1;
    int timeout_ms_ = 5000;
};

} // namespace qrprotec
