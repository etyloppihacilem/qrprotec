/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          http.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace qrprotec {

using HttpHeaders = std::vector< std::pair< std::string, std::string > >;

struct HttpUrl {
    std::string host;
    int         port = 80;
    std::string base_path; // sans '/' final
    bool        tls = false; // https://
    std::string ca_file;     // HTTPS : certificat d'autorite en plus du magasin du systeme (PEM)
};

// Accepte "http://hote[:port][/chemin]" (API locale) et "https://hote[:port][/chemin]" (back distant).
bool parse_http_url(const std::string &url, HttpUrl &out, std::string &error);
// Valeur de l'en-tete Host (port omis s'il est celui par defaut du schema)
std::string host_header(const HttpUrl &url);

// Acces du front au back : URL, jeton de l'API locale (X-QRProtec-Token) et/ou cle de front distant
// (X-QRProtec-Key), certificat d'autorite pour HTTPS.
struct ApiEndpoint {
    std::string url;
    std::string token;
    std::string key;
    std::string ca_file;

    bool        parse(HttpUrl &out, std::string &error) const;
    HttpHeaders headers() const;
};

struct HttpResponse {
    int         status = 0; // 0 = erreur reseau
    std::string body;
    std::string error;
};

HttpResponse http_request(
  const HttpUrl     &url,
  const std::string &method,
  const std::string &path,
  const std::string &body,
  const HttpHeaders &headers,
  int                timeout_ms
);

// Exposes pour les tests
bool parse_http_response(const std::string &raw, HttpResponse &response);

} // namespace qrprotec
