#pragma once

#include <string>

namespace qrprotec {

void set_verbose_logging(bool enabled);
bool verbose_logging_enabled();
void debug_log(const std::string& message);

} // namespace qrprotec
