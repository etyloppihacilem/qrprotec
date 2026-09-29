/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          http.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace qrprotec {

namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast< char >(std::tolower(character));
  });
  return value;
}

class Socket {
  public:
    ~Socket() {
      if (fd_ >= 0)
        ::close(fd_);
    }
    int fd_ = -1;
};

bool decode_chunked(const std::string &body, std::string &output) {
  std::size_t position = 0;
  output.clear();
  for (;;) {
    const std::size_t line_end = body.find("\r\n", position);
    if (line_end == std::string::npos)
      return false;
    std::size_t size = 0;
    try {
      size = std::stoul(body.substr(position, line_end - position), nullptr, 16);
    } catch (const std::exception &) {
      return false;
    }
    position = line_end + 2;
    if (size == 0)
      return true;
    if (position + size > body.size())
      return false;
    output.append(body, position, size);
    position += size + 2;
  }
}

} // namespace

bool parse_http_url(const std::string &url, HttpUrl &out, std::string &error) {
  const std::string prefix = "http://";
  if (lower(url.substr(0, prefix.size())) != prefix) {
    error = "L'URL de l'API doit commencer par http://";
    return false;
  }
  std::string       rest  = url.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  std::string       authority = rest.substr(0, slash);
  out.base_path               = slash == std::string::npos ? "" : rest.substr(slash);
  while (!out.base_path.empty() && out.base_path.back() == '/')
    out.base_path.pop_back();
  out.port = 80;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      error = "Adresse IPv6 invalide";
      return false;
    }
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':')
      authority = authority.substr(close + 2);
    else
      authority.clear();
  } else {
    const std::size_t colon = authority.rfind(':');
    out.host                = authority.substr(0, colon);
    authority               = colon == std::string::npos ? "" : authority.substr(colon + 1);
  }
  if (!authority.empty()) {
    try {
      out.port = std::stoi(authority);
    } catch (const std::exception &) {
      error = "Port invalide";
      return false;
    }
  }
  if (out.host.empty() || out.port <= 0 || out.port > 65535) {
    error = "URL de l'API invalide";
    return false;
  }
  return true;
}

bool parse_http_response(const std::string &raw, HttpResponse &response) {
  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
    response.error = "Reponse HTTP invalide";
    return false;
  }
  const std::size_t space = raw.find(' ');
  try {
    response.status = std::stoi(raw.substr(space + 1, 3));
  } catch (const std::exception &) {
    response.error = "Statut HTTP invalide";
    return false;
  }
  const std::string headers = lower(raw.substr(0, header_end));
  std::string       body    = raw.substr(header_end + 4);
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    std::string decoded;
    if (!decode_chunked(body, decoded)) {
      response.error = "Reponse chunked invalide";
      return false;
    }
    body = decoded;
  } else {
    const std::size_t length_position = headers.find("content-length:");
    if (length_position != std::string::npos) {
      const std::size_t length = std::strtoul(headers.c_str() + length_position + 15, nullptr, 10);
      if (body.size() > length)
        body.resize(length);
    }
  }
  response.body = std::move(body);
  return true;
}

HttpResponse http_request(
  const HttpUrl     &url,
  const std::string &method,
  const std::string &path,
  const std::string &body,
  const HttpHeaders &headers,
  int                timeout_ms
) {
  HttpResponse response;
  addrinfo     hints{};
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo         *addresses = nullptr;
  const std::string port      = std::to_string(url.port);
  if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &addresses) != 0 || !addresses) {
    response.error = "Hote introuvable : " + url.host;
    return response;
  }
  Socket  socket_holder;
  timeval timeout{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  for (addrinfo *address = addresses; address; address = address->ai_next) {
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0)
      continue;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      socket_holder.fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(addresses);
  if (socket_holder.fd_ < 0) {
    response.error = "Connexion impossible a " + url.host + ":" + port;
    return response;
  }

  std::string request = method + " " + url.base_path + path + " HTTP/1.1\r\n";
  request += "Host: " + url.host + ":" + port + "\r\n";
  request += "Connection: close\r\nAccept: application/json\r\n";
  for (const auto &[name, value] : headers)
    request += name + ": " + value + "\r\n";
  if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += body;

  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t written = ::send(socket_holder.fd_, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (written <= 0) {
      response.error = "Envoi de la requete impossible";
      return response;
    }
    sent += static_cast< std::size_t >(written);
  }
  std::string raw;
  char        buffer[8192];
  for (;;) {
    const ssize_t received = ::recv(socket_holder.fd_, buffer, sizeof(buffer), 0);
    if (received == 0)
      break;
    if (received < 0) {
      if (errno == EINTR)
        continue;
      response.error = errno == EAGAIN || errno == EWOULDBLOCK ? "Delai de reponse depasse" : "Lecture impossible";
      return response;
    }
    raw.append(buffer, static_cast< std::size_t >(received));
    if (raw.size() > 64 * 1024 * 1024) {
      response.error = "Reponse trop volumineuse";
      return response;
    }
  }
  if (!parse_http_response(raw, response))
    response.status = 0;
  return response;
}

} // namespace qrprotec
