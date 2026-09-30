/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          remote_scanner_link.hpp
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

// Connexion du front a une session de telephone-douchette (/ws/scanner/front?s=ID sur l'API locale).
// Un thread maintient la connexion (reconnexion automatique, ping toutes les 15 s) ; les messages
// recus sont lus depuis le thread de l'interface avec take_messages().
class RemoteScannerLink {
  public:
    RemoteScannerLink() = default;
    ~RemoteScannerLink();
    RemoteScannerLink(const RemoteScannerLink &)            = delete;
    RemoteScannerLink &operator=(const RemoteScannerLink &) = delete;

    void start(const std::string &api_url, const std::string &token, const std::string &session_id);
    void stop();
    void send(const std::string &text); // mis en file, envoye par le thread

    std::vector< std::string > take_messages();
    bool                       connected() const { return connected_; }
    bool                       running() const { return running_; }
    // la session n'existe plus cote serveur (404 a la poignee de main)
    bool                       session_gone() const { return session_gone_; }
    std::string                last_error() const;

  private:
    void run(std::string api_url, std::string token, std::string session_id);

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

} // namespace qrprotec
