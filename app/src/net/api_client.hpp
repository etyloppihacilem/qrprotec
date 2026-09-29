/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          api_client.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include "../core/json.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace qrprotec {

struct ApiResult {
    bool        ok     = false;
    int         status = 0; // 0 = serveur injoignable
    Json        data;
    std::string error; // message lisible (reseau ou champ "error"/"detail" de l'API)
};

// Client asynchrone de l'API locale : les requetes sont executees dans un thread dedie et les
// callbacks sont appeles depuis le thread de l'interface lors de poll().
class ApiClient {
  public:
    using Callback = std::function< void(const ApiResult &) >;

    ApiClient();
    ~ApiClient();
    ApiClient(const ApiClient &)            = delete;
    ApiClient &operator=(const ApiClient &) = delete;

    void configure(const std::string &base_url, const std::string &token);

    void get(const std::string &path, Callback callback);
    void post(const std::string &path, const Json &body, Callback callback);
    void patch(const std::string &path, const Json &body, Callback callback);
    void put(const std::string &path, const Json &body, Callback callback);
    void remove(const std::string &path, Callback callback); // DELETE

    // A appeler a chaque frame depuis le thread UI.
    void poll();

    bool        online() const;
    bool        busy() const;
    std::string last_error() const;

  private:
    struct Job {
        std::string method;
        std::string path;
        std::string body;
        Callback    callback;
    };
    struct Done {
        ApiResult result;
        Callback  callback;
    };

    void enqueue(const std::string &method, const std::string &path, const Json *body, Callback callback);
    void run();

    mutable std::mutex      mutex_;
    std::condition_variable condition_;
    std::deque< Job >       jobs_;
    std::deque< Done >      done_;
    std::string             base_url_;
    std::string             token_;
    bool                    online_  = false;
    bool                    working_ = false;
    std::string             last_error_;
    bool                    stopping_ = false;
    std::thread             thread_;
};

std::string url_encode(const std::string &value);

} // namespace qrprotec
