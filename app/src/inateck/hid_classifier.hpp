#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace qrprotec {

struct HidScanConfig {
    std::chrono::milliseconds max_intercharacter{30};
    std::size_t minimum_length = 3;
};

class HidScanClassifier {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit HidScanClassifier(HidScanConfig config = {});

    void set_enabled(bool enabled);
    void set_config(HidScanConfig config);
    void reset();

    std::optional<std::string> feed_character(char32_t character, TimePoint now);
    std::optional<std::string> finish();

private:
    HidScanConfig config_;
    std::string buffer_;
    TimePoint last_character_{};
    bool enabled_ = false;
    bool has_last_character_ = false;
};

} // namespace qrprotec
