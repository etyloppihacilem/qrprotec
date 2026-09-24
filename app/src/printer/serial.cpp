#include "serial.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace qrprotec {

namespace {
speed_t baud_constant(int baud_rate)
{
    switch (baud_rate) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return 0;
    }
}
}

SerialPort::~SerialPort()
{
    close();
}

bool SerialPort::open(const SerialSettings& settings, std::string& error)
{
    close();
    const speed_t speed = baud_constant(settings.baud_rate);
    if (speed == 0) {
        error = "Debit serie non supporte.";
        return false;
    }
    file_descriptor_ = ::open(settings.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (file_descriptor_ < 0) {
        error = std::strerror(errno);
        return false;
    }
    termios configuration{};
    if (tcgetattr(file_descriptor_, &configuration) != 0) {
        error = std::strerror(errno);
        close();
        return false;
    }
    cfmakeraw(&configuration);
    cfsetispeed(&configuration, speed);
    cfsetospeed(&configuration, speed);
    configuration.c_cflag |= CLOCAL | CREAD;
    configuration.c_cflag &= ~CSTOPB;
    configuration.c_cflag &= ~CRTSCTS;
    configuration.c_cflag &= ~PARENB;
    configuration.c_cflag &= ~CSIZE;
    configuration.c_cflag |= CS8;
    configuration.c_cc[VMIN] = 0;
    configuration.c_cc[VTIME] = 0;
    if (tcsetattr(file_descriptor_, TCSANOW, &configuration) != 0) {
        error = std::strerror(errno);
        close();
        return false;
    }
    tcflush(file_descriptor_, TCIFLUSH);
    read_timeout_ms_ = settings.read_timeout_ms;
    return true;
}

void SerialPort::close()
{
    if (file_descriptor_ >= 0) {
        ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
}

bool SerialPort::write_bytes(const std::vector<std::uint8_t>& bytes, std::string& error)
{
    if (!is_open()) {
        error = "Port serie ferme.";
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(file_descriptor_, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool SerialPort::read_bytes(std::vector<std::uint8_t>& bytes, std::size_t expected, std::string& error)
{
    if (!is_open()) {
        error = "Port serie ferme.";
        return false;
    }
    bytes.clear();
    bytes.reserve(expected);
    while (bytes.size() < expected) {
        pollfd descriptor{file_descriptor_, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, read_timeout_ms_);
        if (result == 0) {
            error = "Timeout de lecture serie.";
            return false;
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        std::uint8_t buffer[256];
        const std::size_t remaining = expected - bytes.size();
        const ssize_t read_count = ::read(file_descriptor_, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (read_count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            error = std::strerror(errno);
            return false;
        }
        if (read_count == 0) {
            error = "Le port serie a ete ferme.";
            return false;
        }
        bytes.insert(bytes.end(), buffer, buffer + read_count);
    }
    return true;
}

bool SerialPort::read_packet(std::uint8_t& command, std::vector<std::uint8_t>& payload, std::string& error)
{
    std::vector<std::uint8_t> byte;
    for (;;) {
        if (!read_bytes(byte, 1, error)) return false;
        if (byte[0] != 0x55) continue;
        if (!read_bytes(byte, 1, error)) return false;
        if (byte[0] == 0x55) break;
    }
    if (!read_bytes(byte, 1, error)) return false;
    command = byte[0];
    if (!read_bytes(byte, 1, error)) return false;
    const std::size_t length = byte[0];
    if (!read_bytes(payload, length, error)) return false;
    if (!read_bytes(byte, 1, error)) return false;
    std::uint8_t checksum = command ^ static_cast<std::uint8_t>(length);
    for (const std::uint8_t value : payload) checksum ^= value;
    if (byte[0] != checksum) {
        error = "Checksum serie invalide.";
        return false;
    }
    if (!read_bytes(byte, 2, error) || byte[0] != 0xaa || byte[1] != 0xaa) {
        error = "Trame serie invalide: fin.";
        return false;
    }
    return true;
}

} // namespace qrprotec
