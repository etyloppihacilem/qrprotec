/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          connection.hpp
        -\-    _|__
         |\___/  . \        Created on 1 Oct. 2026 at 23:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "http.hpp"

#include <cstddef>
#include <string>

typedef struct ssl_st ssl_st;

namespace qrprotec {

// Connexion TCP au back, chiffree (TLS, OpenSSL) si l'URL est en https://. Le certificat du serveur est
// verifie (magasin du systeme, plus url.ca_file s'il est renseigne) ainsi que le nom d'hote.
class Connection {
  public:
    Connection() = default;
    ~Connection();
    Connection(const Connection &)            = delete;
    Connection &operator=(const Connection &) = delete;

    bool open(const HttpUrl &url, int timeout_ms, std::string &error);
    bool send_all(const std::string &data);
    // > 0 : octets lus ; 0 : connexion fermee ; < 0 : erreur ou delai (SO_RCVTIMEO) depasse
    long receive(char *buffer, std::size_t size);
    // 1 : donnees a lire ; 0 : rien dans le delai ; -1 : erreur
    int  wait_readable(int timeout_ms);
    void close();
    bool is_open() const { return fd_ >= 0; }

  private:
    int     fd_  = -1;
    ssl_st *ssl_ = nullptr;
};

} // namespace qrprotec
