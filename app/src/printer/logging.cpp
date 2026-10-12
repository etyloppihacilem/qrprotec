#include "logging.hpp"

#include <iostream>
#include <map>
#include <mutex>

namespace qrprotec {
namespace {
bool verbose = false;
std::mutex log_mutex;
std::map<std::string, std::string> last_errors;
}

void set_verbose_logging(bool enabled)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    verbose = enabled;
}

bool verbose_logging_enabled()
{
    std::lock_guard<std::mutex> lock(log_mutex);
    return verbose;
}

void debug_log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    if (verbose) std::cout << "[QRProtec] " << message << std::endl;
}

void error_log(const std::string& message, const std::string& source)
{
    if (message.empty())
        return;
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!source.empty()) {
        std::string& last = last_errors[source];
        if (last == message)
            return;
        last = message;
    }
    std::cerr << "[QRProtec] erreur" << (source.empty() ? "" : " (" + source + ")") << " : " << message << std::endl;
}

} // namespace qrprotec
