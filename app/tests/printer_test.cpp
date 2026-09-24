#include "printer/niimbot_protocol.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

int main()
{
    using qrprotec::RasterImage;
    using qrprotec::niimbot::Packet;

    const std::vector<std::uint8_t> encoded = qrprotec::niimbot::encode_packet(0x21, {3});
    assert((encoded == std::vector<std::uint8_t>{0x55, 0x55, 0x21, 0x01, 0x03, 0x23, 0xaa, 0xaa}));

    Packet packet;
    std::string error;
    assert(qrprotec::niimbot::decode_packet(encoded, packet, error));
    assert(packet.command == 0x21);
    assert(packet.payload == std::vector<std::uint8_t>{3});

    std::vector<std::uint8_t> invalid = encoded;
    invalid[5] ^= 0x01;
    assert(!qrprotec::niimbot::decode_packet(invalid, packet, error));

    RasterImage image{8, 1, {0, 255, 0, 255, 255, 255, 255, 0}};
    const std::vector<std::uint8_t> row = qrprotec::niimbot::encode_bitmap_row(image, 0);
    assert((row == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 1, 0xa1}));

    const std::vector<std::uint8_t> dimensions = qrprotec::niimbot::u16(384);
    assert((dimensions == std::vector<std::uint8_t>{0x01, 0x80}));
    return 0;
}
