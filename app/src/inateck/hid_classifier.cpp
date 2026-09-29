#include "hid_classifier.hpp"

#include <algorithm>
#include <limits>

namespace qrprotec {

HidScanClassifier::HidScanClassifier(HidScanConfig config) : config_(config) {}

void HidScanClassifier::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_)
        reset();
}

void HidScanClassifier::set_config(HidScanConfig config) {
    config_ = config;
    reset();
}

void HidScanClassifier::reset() {
    buffer_.clear();
    has_last_character_ = false;
}

std::optional<std::string> HidScanClassifier::feed_character(char32_t character, TimePoint now) {
    if (!enabled_ || character < 0x20 || character > 0x7e)
        return std::nullopt;

    if (has_last_character_ && now - last_character_ > config_.max_intercharacter)
        reset();

    buffer_.push_back(static_cast<char>(character));
    last_character_ = now;
    has_last_character_ = true;
    return std::nullopt;
}

std::optional<std::string> HidScanClassifier::finish() {
    if (!enabled_ || buffer_.size() < config_.minimum_length)
        return std::nullopt;

    std::string scan = std::move(buffer_);
    reset();
    return scan;
}

} // namespace qrprotec
