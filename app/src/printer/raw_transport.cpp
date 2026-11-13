#include "raw_transport.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// macOS (front de bureau) : ni SOCK_CLOEXEC ni MSG_NOSIGNAL, on passe par fcntl et SO_NOSIGPIPE
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace qrprotec {

RawTransport::~RawTransport()
{
    close();
}

bool RawTransport::is_network_address(const std::string& address)
{
    return !address.empty() && address.front() != '/' && address.front() != '.';
}

bool RawTransport::split_host_port(const std::string& address, std::string& host, std::string& port)
{
    std::string text = address;
    const std::string scheme = "tcp://";
    if (text.rfind(scheme, 0) == 0) text = text.substr(scheme.size());
    while (!text.empty() && (text.back() == '/' || text.back() == ' ')) text.pop_back();
    port = "9100";
    if (!text.empty() && text.front() == '[') { // IPv6 : [fe80::1]:9100
        const std::size_t end = text.find(']');
        if (end == std::string::npos) return false;
        host = text.substr(1, end - 1);
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':') return false;
            port = text.substr(end + 2);
        }
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon != std::string::npos && text.find(':') == colon) {
            host = text.substr(0, colon);
            port = text.substr(colon + 1);
        } else {
            host = text;
        }
    }
    if (host.empty() || port.empty()) return false;
    for (const char c : port)
        if (c < '0' || c > '9') return false;
    return true;
}

bool RawTransport::open(const std::string& address, int timeout_ms, std::string& error)
{
    close();
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 5000;
    if (address.empty()) {
        error = "Adresse de l'imprimante vide.";
        return false;
    }
    if (!is_network_address(address)) {
        // lecture + ecriture pour lire l'etat (~HS, <ESC>!?) ; ecriture seule si le peripherique la refuse
        file_descriptor_ = ::open(address.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
        readable_ = file_descriptor_ >= 0;
        if (file_descriptor_ < 0)
            file_descriptor_ = ::open(address.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (file_descriptor_ < 0) {
            const int code = errno;
            error = address + " : " + std::strerror(code);
            if (code == EACCES)
                error += " (l'utilisateur doit être dans le groupe lp : sudo usermod -aG lp $USER)";
            else if (code == ENOENT)
                error += " (imprimante débranchée ou éteinte ?)";
            return false;
        }
        return true;
    }

    std::string host;
    std::string port;
    if (!split_host_port(address, host, port)) {
        error = "Adresse réseau invalide : " + address + " (ex : 192.168.1.50 ou 192.168.1.50:9100).";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int resolved = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (resolved != 0) {
        error = host + " : " + gai_strerror(resolved);
        return false;
    }
    error = "Connexion impossible à " + host + ":" + port + ".";
    for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
        const int socket_fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket_fd < 0) continue;
        ::fcntl(socket_fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        ::setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        const int flags = ::fcntl(socket_fd, F_GETFL, 0);
        ::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
        int result = ::connect(socket_fd, entry->ai_addr, entry->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS) {
            pollfd descriptor{socket_fd, POLLOUT, 0};
            result = ::poll(&descriptor, 1, timeout_ms_);
            if (result > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                ::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
                result = socket_error == 0 ? 0 : -1;
                if (socket_error != 0) error = host + ":" + port + " : " + std::strerror(socket_error);
            } else {
                if (result == 0) error = host + ":" + port + " : délai de connexion dépassé.";
                result = -1;
            }
        } else if (result < 0) {
            error = host + ":" + port + " : " + std::strerror(errno);
        }
        if (result == 0) {
            ::fcntl(socket_fd, F_SETFL, flags);
            file_descriptor_ = socket_fd;
            readable_ = true;
            break;
        }
        ::close(socket_fd);
    }
    ::freeaddrinfo(results);
    if (file_descriptor_ < 0) return false;
    error.clear();
    return true;
}

void RawTransport::close()
{
    if (file_descriptor_ >= 0) {
        ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
    readable_ = false;
}

std::vector<std::uint8_t> RawTransport::read_reply(int timeout_ms)
{
    std::vector<std::uint8_t> reply;
    if (!is_open() || !readable_) return reply;
    int wait = timeout_ms;
    for (;;) {
        pollfd descriptor{file_descriptor_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, wait);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) break;
        std::uint8_t buffer[256];
        const ssize_t count = ::read(file_descriptor_, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        reply.insert(reply.end(), buffer, buffer + count);
        wait = 150;
    }
    return reply;
}

bool RawTransport::write_bytes(const std::vector<std::uint8_t>& bytes, std::string& error)
{
    if (!is_open()) {
        error = "Imprimante non connectée.";
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        pollfd descriptor{file_descriptor_, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, timeout_ms_);
        if (ready == 0) {
            error = "L'imprimante ne reçoit plus les données (papier, ruban ou couvercle ?).";
            return false;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            error = std::strerror(errno);
            return false;
        }
        const ssize_t written = ::send(file_descriptor_, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == ENOTSOCK) {
            const ssize_t file_written = ::write(file_descriptor_, bytes.data() + offset, bytes.size() - offset);
            if (file_written < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                error = std::strerror(errno);
                return false;
            }
            offset += static_cast<std::size_t>(file_written);
            continue;
        }
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            error = std::strerror(errno);
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace qrprotec
