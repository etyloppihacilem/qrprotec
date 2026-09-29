#include "../src/inateck/hid_classifier.hpp"

#include <cassert>
#include <chrono>

using qrprotec::HidScanClassifier;
using qrprotec::HidScanConfig;

int main() {
    const auto start = HidScanClassifier::TimePoint{};
    HidScanClassifier classifier({std::chrono::milliseconds(30), 3});
    classifier.set_enabled(true);

    classifier.feed_character('A', start);
    classifier.feed_character('B', start + std::chrono::milliseconds(10));
    classifier.feed_character('1', start + std::chrono::milliseconds(20));
    const auto scan = classifier.finish();
    assert(scan.has_value());
    assert(*scan == "AB1");

    classifier.feed_character('A', start);
    classifier.feed_character('B', start + std::chrono::milliseconds(40));
    assert(!classifier.finish().has_value());
    classifier.reset();

    classifier.feed_character('X', start + std::chrono::milliseconds(50));
    classifier.feed_character('Y', start + std::chrono::milliseconds(60));
    assert(!classifier.finish().has_value());

    classifier.feed_character('\n', start + std::chrono::milliseconds(80));
    assert(!classifier.finish().has_value());

    classifier.set_enabled(false);
    classifier.feed_character('Q', start + std::chrono::milliseconds(90));
    assert(!classifier.finish().has_value());
    return 0;
}
