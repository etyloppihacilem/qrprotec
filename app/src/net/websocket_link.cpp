/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          websocket_link.cpp
        -\-    _|__
         |\___/  . \        Created on 30 Sep. 2026 at 14:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "websocket_link.hpp"
#include "../printer/logging.hpp"

#include <cctype>
#include <chrono>

namespace qrprotec {

namespace {

std::string url_component(const std::string &value) {
  static const char hex[] = "0123456789ABCDEF";
  std::string       output;
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      output += static_cast< char >(character);
    } else {
      output += '%';
      output += hex[character >> 4];
      output += hex[character & 15];
    }
  }
  return output;
}

} // namespace

std::string remote_scanner_path(const std::string &session_id) {
  return "/ws/scanner/front?s=" + url_component(session_id);
}

WebSocketLink::~WebSocketLink() {
  stop();
}

void WebSocketLink::start(const ApiEndpoint &endpoint, const std::string &path, const std::string &context) {
  stop();
  {
    std::lock_guard< std::mutex > lock(mutex_);
    inbox_.clear();
    outbox_.clear();
    error_.clear();
  }
  stop_         = false;
  session_gone_ = false;
  running_      = true;
  thread_       = std::thread(&WebSocketLink::run, this, endpoint, path, context);
}

void WebSocketLink::stop() {
  stop_ = true;
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
  running_   = false;
  connected_ = false;
}

void WebSocketLink::send(const std::string &text) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    outbox_.push_back(text);
  }
  wake_.notify_all();
}

std::vector< std::string > WebSocketLink::take_messages() {
  std::lock_guard< std::mutex > lock(mutex_);
  std::vector< std::string >    messages(inbox_.begin(), inbox_.end());
  inbox_.clear();
  return messages;
}

std::string WebSocketLink::last_error() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return error_;
}

void WebSocketLink::run(ApiEndpoint endpoint, std::string path, std::string context) {
  using clock = std::chrono::steady_clock;
  HttpUrl     url;
  std::string error;
  if (!endpoint.parse(url, error)) {
    std::lock_guard< std::mutex > lock(mutex_);
    error_   = error;
    running_ = false;
    return;
  }
  const HttpHeaders headers  = endpoint.headers();
  int               delay_ms = 1000;
  while (!stop_) {
    WebSocketClient socket;
    int             status = 0;
    if (!socket.connect(url, path, headers, 5000, error, status)) {
      {
        std::lock_guard< std::mutex > lock(mutex_);
        error_ = error;
      }
      error_log(error, context);
      if (status == 404) { // session fermee (expiree) cote serveur, ou ancien serveur : inutile d'insister
        session_gone_ = true;
        break;
      }
      std::unique_lock< std::mutex > lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this] { return stop_.load(); });
      delay_ms = std::min(delay_ms * 2, 10000);
      continue;
    }
    connected_ = true;
    delay_ms   = 1000;
    {
      std::lock_guard< std::mutex > lock(mutex_);
      error_.clear();
    }
    auto last_ping = clock::now();
    while (!stop_ && socket.is_open()) {
      std::deque< std::string > pending;
      {
        std::lock_guard< std::mutex > lock(mutex_);
        pending.swap(outbox_);
      }
      for (const std::string &text : pending)
        socket.send_text(text);
      if (clock::now() - last_ping > std::chrono::seconds(15)) {
        socket.send_text("{\"type\":\"ping\"}");
        last_ping = clock::now();
      }
      std::string message;
      const int   result = socket.receive(message, 100);
      if (result == 1) {
        std::lock_guard< std::mutex > lock(mutex_);
        inbox_.push_back(std::move(message));
      }
    }
    connected_ = false;
    if (stop_) {
      socket.close();
      break;
    }
    std::lock_guard< std::mutex > lock(mutex_);
    error_ = "Connexion au serveur perdue";
    error_log(error_, context);
  }
  connected_ = false;
  running_   = false;
}

} // namespace qrprotec
