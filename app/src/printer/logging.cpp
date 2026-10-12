#include "logging.hpp"

#include <iostream>
#include <map>
#include <mutex>

#ifndef _WIN32
#include <syslog.h>
#include <unistd.h>
#endif

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
    const std::string line = "[QRProtec] erreur" + (source.empty() ? std::string() : " (" + source + ")") + " : " + message;
#ifdef _WIN32
    std::cerr << line << std::endl;
#else
    // syslog arrive dans journalctl quel que soit le devenir de la sortie d'erreur (Cage, bureau...),
    // rattache a l'unite qui a lance le front (journalctl -u qrprotec-kiosk)
    static const bool opened = [] {
        openlog("qrprotec-front", LOG_PID, LOG_USER);
        return true;
    }();
    (void)opened;
    syslog(LOG_ERR, "%s", line.c_str());
    if (isatty(STDERR_FILENO))
        std::cerr << line << std::endl;
#endif
}

} // namespace qrprotec
