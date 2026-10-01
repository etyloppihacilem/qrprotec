/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          websocket.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "connection.hpp"
#include "http.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace qrprotec {

// Client WebSocket minimal (RFC 6455) : trames texte, sans extension ; ws:// ou wss:// selon l'URL.
class WebSocketClient {
  public:
    WebSocketClient() = default;
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient &)            = delete;
    WebSocketClient &operator=(const WebSocketClient &) = delete;

    // Poignee de main HTTP Upgrade. status recoit le code HTTP (101 si accepte, 0 si erreur reseau).
    bool connect(const HttpUrl &url, const std::string &path, const HttpHeaders &headers, int timeout_ms,
                 std::string &error, int &status);
    bool send_text(const std::string &text);
    // 1 : message recu dans `message` ; 0 : rien dans le delai ; -1 : connexion fermee
    int  receive(std::string &message, int timeout_ms);
    void close();
    bool is_open() const { return connection_.is_open(); }

  private:
    bool send_frame(int opcode, const std::string &payload);

    Connection  connection_;
    std::string buffer_;
    std::string fragments_;
    std::mutex  send_mutex_;
};

// Exposes pour les tests
struct WebSocketFrame {
    bool        final  = true;
    int         opcode = 1;
    std::string payload;
};
std::string ws_encode_frame(int opcode, const std::string &payload, const std::uint8_t mask[4]);
// Decode une trame en tete de `buffer` ; retourne le nombre d'octets consommes (0 si incomplete, -1 si invalide).
long        ws_decode_frame(const std::string &buffer, WebSocketFrame &frame);
std::string base64_encode(const std::string &data);

} // namespace qrprotec
