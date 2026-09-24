#include "logging.hpp"

#include <iostream>
#include <mutex>

namespace qrprotec {
namespace {
bool verbose = false;
std::mutex log_mutex;
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

} // namespace qrprotec
