#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qrprotec {

struct SerialSettings {
    std::string device = "/dev/ttyACM0";
    int baud_rate = 115200;
    int read_timeout_ms = 500;
};

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const SerialSettings& settings, std::string& error);
    void close();
    bool is_open() const { return file_descriptor_ >= 0; }
    bool write_bytes(const std::vector<std::uint8_t>& bytes, std::string& error);
    bool read_bytes(std::vector<std::uint8_t>& bytes, std::size_t expected, std::string& error);
    bool read_packet(std::uint8_t& command, std::vector<std::uint8_t>& payload, std::string& error);

private:
    int file_descriptor_ = -1;
    int read_timeout_ms_ = 500;
};

} // namespace qrprotec
