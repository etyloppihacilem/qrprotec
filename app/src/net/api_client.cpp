/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          api_client.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "api_client.hpp"

#include "../printer/logging.hpp"
#include "http.hpp"

#include <cctype>
#include <cstdio>

namespace qrprotec {

std::string url_encode(const std::string &value) {
  std::string output;
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      output += static_cast< char >(character);
    } else {
      char buffer[4];
      std::snprintf(buffer, sizeof(buffer), "%%%02X", character);
      output += buffer;
    }
  }
  return output;
}

ApiClient::ApiClient() : thread_(&ApiClient::run, this) {}

ApiClient::~ApiClient() {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void ApiClient::configure(const std::string &base_url, const std::string &token) {
  std::lock_guard< std::mutex > lock(mutex_);
  base_url_ = base_url;
  token_    = token;
}

void ApiClient::enqueue(const std::string &method, const std::string &path, const Json *body, Callback callback) {
  {
    std::lock_guard< std::mutex > lock(mutex_);
    jobs_.push_back({ method, path, body ? body->dump() : std::string(), std::move(callback) });
  }
  condition_.notify_one();
}

void ApiClient::get(const std::string &path, Callback callback) {
  enqueue("GET", path, nullptr, std::move(callback));
}

void ApiClient::post(const std::string &path, const Json &body, Callback callback) {
  enqueue("POST", path, &body, std::move(callback));
}

void ApiClient::patch(const std::string &path, const Json &body, Callback callback) {
  enqueue("PATCH", path, &body, std::move(callback));
}

void ApiClient::remove(const std::string &path, Callback callback) {
  enqueue("DELETE", path, nullptr, std::move(callback));
}

void ApiClient::put(const std::string &path, const Json &body, Callback callback) {
  enqueue("PUT", path, &body, std::move(callback));
}

void ApiClient::poll() {
  std::deque< Done > done;
  {
    std::lock_guard< std::mutex > lock(mutex_);
    done.swap(done_);
  }
  for (Done &entry : done)
    if (entry.callback)
      entry.callback(entry.result);
}

bool ApiClient::online() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return online_;
}

bool ApiClient::busy() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return working_ || !jobs_.empty();
}

std::string ApiClient::last_error() const {
  std::lock_guard< std::mutex > lock(mutex_);
  return last_error_;
}

void ApiClient::run() {
  for (;;) {
    Job         job;
    std::string base_url;
    std::string token;
    {
      std::unique_lock< std::mutex > lock(mutex_);
      condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_)
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      base_url = base_url_;
      token    = token_;
      working_ = true;
    }
    ApiResult   result;
    HttpUrl     url;
    std::string error;
    if (!parse_http_url(base_url, url, error)) {
      result.error = error;
    } else {
      HttpHeaders headers;
      if (!token.empty())
        headers.emplace_back("X-QRProtec-Token", token);
      debug_log("API " + job.method + " " + job.path + " " + job.body);
      const HttpResponse response = http_request(url, job.method, job.path, job.body, headers, 8000);
      result.status               = response.status;
      if (response.status == 0) {
        result.error = response.error;
      } else {
        std::string parse_error;
        result.data = response.body.empty() ? Json() : Json::parse(response.body, &parse_error);
        result.ok   = response.status >= 200 && response.status < 300;
        if (!result.ok) {
          if (result.data["error"].is_string())
            result.error = result.data["error"].str();
          else if (result.data["detail"].is_string())
            result.error = result.data["detail"].str();
          else
            result.error = "Erreur HTTP " + std::to_string(response.status);
        } else if (!parse_error.empty()) {
          result.ok    = false;
          result.error = "Réponse illisible : " + parse_error;
        }
      }
    }
    std::lock_guard< std::mutex > lock(mutex_);
    working_ = false;
    online_  = result.status != 0;
    if (result.status == 0)
      last_error_ = result.error;
    done_.push_back({ std::move(result), std::move(job.callback) });
  }
}

} // namespace qrprotec
