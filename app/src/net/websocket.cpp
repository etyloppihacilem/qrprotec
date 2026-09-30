/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          websocket.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "websocket.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace qrprotec {

namespace {

constexpr int         kText = 0x1, kClose = 0x8, kPing = 0x9, kPong = 0xA;
constexpr std::size_t kMaxMessage = 1024 * 1024;

void random_bytes(std::uint8_t *out, std::size_t count) {
  static thread_local std::mt19937 generator(std::random_device{}());
  std::uniform_int_distribution< int > byte(0, 255);
  for (std::size_t index = 0; index < count; ++index)
    out[index] = static_cast< std::uint8_t >(byte(generator));
}

bool send_all(int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return false;
    sent += static_cast< std::size_t >(written);
  }
  return true;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast< char >(std::tolower(c)); });
  return value;
}

} // namespace

std::string base64_encode(const std::string &data) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string       output;
  std::size_t       index = 0;
  for (; index + 2 < data.size(); index += 3) {
    const unsigned value = (static_cast< unsigned char >(data[index]) << 16)
                         | (static_cast< unsigned char >(data[index + 1]) << 8) | static_cast< unsigned char >(data[index + 2]);
    output += alphabet[(value >> 18) & 63];
    output += alphabet[(value >> 12) & 63];
    output += alphabet[(value >> 6) & 63];
    output += alphabet[value & 63];
  }
  if (index + 1 == data.size()) {
    const unsigned value = static_cast< unsigned char >(data[index]) << 16;
    output += alphabet[(value >> 18) & 63];
    output += alphabet[(value >> 12) & 63];
    output += "==";
  } else if (index + 2 == data.size()) {
    const unsigned value = (static_cast< unsigned char >(data[index]) << 16) | (static_cast< unsigned char >(data[index + 1]) << 8);
    output += alphabet[(value >> 18) & 63];
    output += alphabet[(value >> 12) & 63];
    output += alphabet[(value >> 6) & 63];
    output += '=';
  }
  return output;
}

std::string ws_encode_frame(int opcode, const std::string &payload, const std::uint8_t mask[4]) {
  std::string frame;
  frame += static_cast< char >(0x80 | opcode);
  const std::size_t length = payload.size();
  if (length < 126) {
    frame += static_cast< char >(0x80 | length);
  } else if (length < 65536) {
    frame += static_cast< char >(0x80 | 126);
    frame += static_cast< char >((length >> 8) & 0xff);
    frame += static_cast< char >(length & 0xff);
  } else {
    frame += static_cast< char >(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame += static_cast< char >((static_cast< std::uint64_t >(length) >> shift) & 0xff);
  }
  frame.append(reinterpret_cast< const char * >(mask), 4);
  for (std::size_t index = 0; index < length; ++index)
    frame += static_cast< char >(payload[index] ^ mask[index % 4]);
  return frame;
}

long ws_decode_frame(const std::string &buffer, WebSocketFrame &frame) {
  if (buffer.size() < 2)
    return 0;
  const auto  byte   = [&](std::size_t index) { return static_cast< std::uint8_t >(buffer[index]); };
  const bool  masked = byte(1) & 0x80;
  std::size_t offset = 2;
  std::uint64_t length = byte(1) & 0x7f;
  if (length == 126) {
    if (buffer.size() < 4)
      return 0;
    length = (static_cast< std::uint64_t >(byte(2)) << 8) | byte(3);
    offset = 4;
  } else if (length == 127) {
    if (buffer.size() < 10)
      return 0;
    length = 0;
    for (std::size_t index = 2; index < 10; ++index)
      length = (length << 8) | byte(index);
    offset = 10;
  }
  if (length > kMaxMessage)
    return -1;
  std::uint8_t mask[4] = { 0, 0, 0, 0 };
  if (masked) {
    if (buffer.size() < offset + 4)
      return 0;
    for (int index = 0; index < 4; ++index)
      mask[index] = byte(offset + index);
    offset += 4;
  }
  if (buffer.size() < offset + length)
    return 0;
  frame.final   = byte(0) & 0x80;
  frame.opcode  = byte(0) & 0x0f;
  frame.payload = buffer.substr(offset, length);
  if (masked)
    for (std::size_t index = 0; index < frame.payload.size(); ++index)
      frame.payload[index] = static_cast< char >(frame.payload[index] ^ mask[index % 4]);
  return static_cast< long >(offset + length);
}

WebSocketClient::~WebSocketClient() {
  close();
}

bool WebSocketClient::connect(const HttpUrl &url, const std::string &path, const HttpHeaders &headers, int timeout_ms,
                              std::string &error, int &status) {
  close();
  status = 0;
  addrinfo hints{};
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo         *addresses = nullptr;
  const std::string port      = std::to_string(url.port);
  if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &addresses) != 0 || !addresses) {
    error = "Hôte introuvable : " + url.host;
    return false;
  }
  timeval timeout{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  for (addrinfo *address = addresses; address && fd_ < 0; address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0)
      continue;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0)
      fd_ = fd;
    else
      ::close(fd);
  }
  freeaddrinfo(addresses);
  if (fd_ < 0) {
    error = "Connexion impossible à " + url.host + ":" + port;
    return false;
  }
  std::uint8_t nonce[16];
  random_bytes(nonce, sizeof(nonce));
  std::string request = "GET " + url.base_path + path + " HTTP/1.1\r\n";
  request += "Host: " + url.host + ":" + port + "\r\n";
  request += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
  request += "Sec-WebSocket-Key: " + base64_encode(std::string(reinterpret_cast< char * >(nonce), sizeof(nonce))) + "\r\n";
  for (const auto &[name, value] : headers)
    request += name + ": " + value + "\r\n";
  request += "\r\n";
  if (!send_all(fd_, request)) {
    error = "Envoi de la poignée de main impossible";
    close();
    return false;
  }
  // reponse HTTP jusqu'a la ligne vide ; le reste appartient deja au flux WebSocket
  std::string response;
  char        chunk[1024];
  std::size_t end = std::string::npos;
  while ((end = response.find("\r\n\r\n")) == std::string::npos) {
    const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0 || response.size() > 16384) {
      error = "Pas de réponse du serveur";
      close();
      return false;
    }
    response.append(chunk, static_cast< std::size_t >(received));
  }
  const std::size_t space = response.find(' ');
  status = space == std::string::npos ? 0 : std::atoi(response.c_str() + space + 1);
  if (status != 101 || lower(response).find("upgrade: websocket") == std::string::npos) {
    const std::string body = response.substr(end + 4);
    error = "Refusé par le serveur (" + std::to_string(status) + ")" + (body.empty() ? "" : " : " + body.substr(0, 200));
    close();
    return false;
  }
  buffer_ = response.substr(end + 4);
  fragments_.clear();
  return true;
}

bool WebSocketClient::send_frame(int opcode, const std::string &payload) {
  std::lock_guard< std::mutex > lock(send_mutex_);
  if (fd_ < 0)
    return false;
  std::uint8_t mask[4];
  random_bytes(mask, sizeof(mask));
  return send_all(fd_, ws_encode_frame(opcode, payload, mask));
}

bool WebSocketClient::send_text(const std::string &text) {
  return send_frame(kText, text);
}

int WebSocketClient::receive(std::string &message, int timeout_ms) {
  for (;;) {
    if (fd_ < 0)
      return -1;
    WebSocketFrame frame;
    const long     consumed = ws_decode_frame(buffer_, frame);
    if (consumed < 0) {
      close();
      return -1;
    }
    if (consumed > 0) {
      buffer_.erase(0, static_cast< std::size_t >(consumed));
      if (frame.opcode == kPing) {
        send_frame(kPong, frame.payload);
        continue;
      }
      if (frame.opcode == kPong)
        continue;
      if (frame.opcode == kClose) {
        send_frame(kClose, frame.payload.substr(0, 2));
        close();
        return -1;
      }
      fragments_ += frame.payload;
      if (fragments_.size() > kMaxMessage) {
        close();
        return -1;
      }
      if (!frame.final)
        continue;
      message.swap(fragments_);
      fragments_.clear();
      return 1;
    }
    pollfd descriptor{ fd_, POLLIN, 0 };
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready == 0)
      return 0;
    char          chunk[4096];
    const ssize_t received = ready > 0 ? ::recv(fd_, chunk, sizeof(chunk), 0) : -1;
    if (received < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (received <= 0) {
      close();
      return -1;
    }
    buffer_.append(chunk, static_cast< std::size_t >(received));
    timeout_ms = 0; // la suite de la trame est deja arrivee ou arrivera au prochain appel
  }
}

void WebSocketClient::close() {
  std::lock_guard< std::mutex > lock(send_mutex_);
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
  }
  fd_ = -1;
}

} // namespace qrprotec
