/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          websocket_link.hpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "websocket.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qrprotec {

// WebSocket du front vers le serveur (API locale ou distante) : session de telephone-douchette
// (/ws/scanner/front?s=ID, voir remote_scanner_path) ou impressions en attente en direct (/ws/label-prints).
// Un thread maintient la connexion (reconnexion automatique, ping toutes les 15 s, connexion muette depuis 45 s
// refaite) ; les messages recus sont lus depuis le thread de l'interface avec take_messages().
class WebSocketLink {
  public:
    WebSocketLink() = default;
    ~WebSocketLink();
    WebSocketLink(const WebSocketLink &)            = delete;
    WebSocketLink &operator=(const WebSocketLink &) = delete;

    // path : chemin du WebSocket ; context : nom dans le journal des erreurs (« téléphone-douchette »)
    void start(const ApiEndpoint &endpoint, const std::string &path, const std::string &context);
    void stop();
    void send(const std::string &text); // mis en file, envoye par le thread

    std::vector< std::string > take_messages();
    bool                       connected() const { return connected_; }
    bool                       running() const { return running_; }
    // la session n'existe plus cote serveur, ou serveur sans ce WebSocket (404 a la poignee de main)
    bool                       session_gone() const { return session_gone_; }
    std::string                last_error() const;

  private:
    void run(ApiEndpoint endpoint, std::string path, std::string context);

    std::thread                thread_;
    std::atomic< bool >        running_{ false };
    std::atomic< bool >        stop_{ false };
    std::atomic< bool >        connected_{ false };
    std::atomic< bool >        session_gone_{ false };
    mutable std::mutex         mutex_;
    std::condition_variable    wake_;
    std::deque< std::string >  inbox_;
    std::deque< std::string >  outbox_;
    std::string                error_;
};

// /ws/scanner/front?s=ID : session de telephone-douchette
std::string remote_scanner_path(const std::string &session_id);

} // namespace qrprotec
