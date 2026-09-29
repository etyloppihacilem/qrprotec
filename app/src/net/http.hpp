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

struct HttpUrl {
    std::string host;
    int         port = 80;
    std::string base_path; // sans '/' final
};

// Accepte "http://hote:port[/chemin]" (HTTPS non gere : l'API locale est sur le reseau local).
bool parse_http_url(const std::string &url, HttpUrl &out, std::string &error);

struct HttpResponse {
    int         status = 0; // 0 = erreur reseau
    std::string body;
    std::string error;
};

using HttpHeaders = std::vector< std::pair< std::string, std::string > >;

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
