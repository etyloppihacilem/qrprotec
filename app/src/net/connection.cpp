/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          connection.cpp
        -\-    _|__
         |\___/  . \        Created on 1 Oct. 2026 at 23:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "connection.hpp"

#include "socket_compat.hpp"

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace qrprotec {

namespace {

std::string ssl_error_text() {
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return "erreur TLS";
  char buffer[256];
  ERR_error_string_n(code, buffer, sizeof(buffer));
  return buffer;
}

// Un contexte par fichier d'autorite (charger le magasin du systeme a chaque requete serait couteux)
SSL_CTX *tls_context(const std::string &ca_file, std::string &error) {
  static std::mutex                        mutex;
  static std::map< std::string, SSL_CTX * > contexts;
  std::lock_guard< std::mutex >            lock(mutex);
  const auto                               found = contexts.find(ca_file);
  if (found != contexts.end())
    return found->second;
  SSL_CTX *context = SSL_CTX_new(TLS_client_method());
  if (!context) {
    error = "TLS indisponible : " + ssl_error_text();
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
  // un serveur qui ferme sans close_notify termine simplement la reponse (Connection: close)
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
  SSL_CTX_set_options(context, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
  SSL_CTX_set_default_verify_paths(context);
#ifdef _WIN32
  // Windows : OpenSSL n'a pas de magasin par defaut, celui du systeme (autorites publiques, Let's Encrypt...) est
  // lu par le fournisseur winstore (OpenSSL 3.2 et suivants)
  if (SSL_CTX_load_verify_store(context, "org.openssl.winstore://") != 1)
    ERR_clear_error();
#endif
  if (!ca_file.empty() && SSL_CTX_load_verify_locations(context, ca_file.c_str(), nullptr) != 1) {
    error = "Certificat d'autorité illisible : " + ca_file + " (" + ssl_error_text() + ")";
    ERR_clear_error();
    SSL_CTX_free(context);
    return nullptr;
  }
  contexts[ca_file] = context;
  return context;
}

bool is_ip_address(const std::string &host) {
  unsigned char buffer[16];
  return inet_pton(AF_INET, host.c_str(), buffer) == 1 || inet_pton(AF_INET6, host.c_str(), buffer) == 1;
}

} // namespace

Connection::~Connection() {
  close();
}

bool Connection::open(const HttpUrl &url, int timeout_ms, std::string &error) {
  close();
  if (!sockets_ready()) {
    error = "Réseau indisponible (Winsock)";
    return false;
  }
  addrinfo hints{};
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo         *addresses = nullptr;
  const std::string port      = std::to_string(url.port);
  if (const int resolved = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &addresses); resolved != 0 || !addresses) {
    error = "Hôte introuvable : " + url.host + (resolved != 0 ? std::string(" (") + gai_strerror(resolved) + ")" : std::string());
    return false;
  }
  int last_errno = 0;
  for (addrinfo *address = addresses; address && fd_ < 0; address = address->ai_next) {
    const socket_handle fd = static_cast< socket_handle >(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (fd < 0) {
      last_errno = errno;
      continue;
    }
    set_socket_timeouts(fd, timeout_ms);
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    setsockopt(native_socket(fd), SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    if (::connect(native_socket(fd), address->ai_addr, static_cast< int >(address->ai_addrlen)) == 0) {
      fd_ = fd;
    } else {
#ifdef _WIN32
      last_errno = last_socket_error() == WSAETIMEDOUT ? ETIMEDOUT : ECONNREFUSED;
#else
      last_errno = errno;
#endif
      close_socket(fd);
    }
  }
  freeaddrinfo(addresses);
  if (fd_ < 0) {
    error = "Connexion impossible à " + url.host + ":" + port
          + (last_errno != 0 ? std::string(" (") + std::strerror(last_errno) + ")" : std::string());
    return false;
  }
  if (!url.tls)
    return true;

  SSL_CTX *context = tls_context(url.ca_file, error);
  if (!context) {
    close();
    return false;
  }
  ssl_ = SSL_new(context);
  if (!ssl_) {
    error = "TLS indisponible : " + ssl_error_text();
    close();
    return false;
  }
  SSL_set_fd(ssl_, static_cast< int >(fd_));
  if (is_ip_address(url.host)) {
    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), url.host.c_str());
  } else {
    SSL_set_tlsext_host_name(ssl_, url.host.c_str());
    SSL_set1_host(ssl_, url.host.c_str());
  }
  if (SSL_connect(ssl_) != 1) {
    const long verify = SSL_get_verify_result(ssl_);
    const bool unknown_authority = verify == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT
                                || verify == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN
                                || verify == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY
                                || verify == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE;
    if (verify != X509_V_OK)
      error = std::string("Certificat HTTPS refusé (") + X509_verify_cert_error_string(verify) + ")"
            + (unknown_authority ? " : indiquez le certificat de l'autorité du serveur dans les réglages"
                                 : " pour " + url.host);
    else
      error = "Poignée de main TLS impossible avec " + url.host + " : " + ssl_error_text();
    ERR_clear_error();
    close();
    return false;
  }
  return true;
}

bool Connection::send_all(const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    long written;
    if (ssl_) {
      written = SSL_write(ssl_, data.data() + sent, static_cast< int >(data.size() - sent));
      if (written <= 0) {
        ERR_clear_error();
        return false;
      }
    } else {
#ifdef _WIN32
      written = ::send(native_socket(fd_), data.data() + sent, static_cast< int >(data.size() - sent), 0);
#else
      written = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        return false;
    }
    sent += static_cast< std::size_t >(written);
  }
  return true;
}

long Connection::receive(char *buffer, std::size_t size) {
#ifdef _WIN32
  // Winsock ne passe pas par errno : delai depasse (SO_RCVTIMEO) traduit en EAGAIN, comme sous POSIX
  if (!ssl_) {
    const int received = ::recv(native_socket(fd_), buffer, static_cast< int >(size), 0);
    if (received < 0)
      errno = socket_timed_out(last_socket_error()) ? EAGAIN : EIO;
    return received;
  }
#else
  if (!ssl_)
    return ::recv(fd_, buffer, size, 0);
#endif
  const int received = SSL_read(ssl_, buffer, static_cast< int >(size));
  if (received > 0)
    return received;
  const int code = SSL_get_error(ssl_, received);
  ERR_clear_error();
#ifdef _WIN32
  if (code == SSL_ERROR_SYSCALL && socket_timed_out(last_socket_error())) {
    errno = EAGAIN;
    return -1;
  }
  errno = 0;
#endif
  if (code == SSL_ERROR_ZERO_RETURN)
    return 0;
  if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
    if (errno != EINTR)
      errno = EAGAIN; // delai SO_RCVTIMEO depasse
    return -1;
  }
  if (code == SSL_ERROR_SYSCALL && errno == 0)
    return 0;
  if (errno == EAGAIN || errno == EINTR)
    errno = EIO; // erreur TLS : ne pas la confondre avec un delai
  return -1;
}

int Connection::wait_readable(int timeout_ms) {
  if (ssl_ && SSL_pending(ssl_) > 0)
    return 1; // deja dechiffre, le socket n'a peut-etre plus rien
  pollfd descriptor{};
  descriptor.fd     = native_socket(fd_);
  descriptor.events = POLLIN;
  const int ready   = socket_poll(&descriptor, 1, timeout_ms);
  return ready < 0 ? (errno == EINTR ? 0 : -1) : ready > 0 ? 1 : 0;
}

void Connection::close() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (fd_ >= 0)
    close_socket(fd_);
  fd_ = -1;
}

} // namespace qrprotec
