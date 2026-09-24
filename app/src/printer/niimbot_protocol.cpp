#include "niimbot_protocol.hpp"

#include <algorithm>

namespace qrprotec::niimbot {

std::vector<std::uint8_t> encode_packet(std::uint8_t command, const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> result{0x55, 0x55, command, static_cast<std::uint8_t>(payload.size())};
    result.insert(result.end(), payload.begin(), payload.end());
    std::uint8_t checksum = command ^ static_cast<std::uint8_t>(payload.size());
    for (const std::uint8_t value : payload) checksum ^= value;
    result.push_back(checksum);
    result.insert(result.end(), {0xaa, 0xaa});
    return result;
}

bool decode_packet(const std::vector<std::uint8_t>& bytes, Packet& packet, std::string& error)
{
    if (bytes.size() < 7 || bytes[0] != 0x55 || bytes[1] != 0x55) {
        error = "Trame Niimbot invalide: en-tete.";
        return false;
    }
    const std::size_t length = bytes[3];
    if (bytes.size() != length + 7 || bytes[length + 5] != 0xaa || bytes[length + 6] != 0xaa) {
        error = "Trame Niimbot invalide: longueur ou fin.";
        return false;
    }
    std::uint8_t checksum = bytes[2] ^ static_cast<std::uint8_t>(length);
    for (std::size_t index = 0; index < length; ++index) checksum ^= bytes[4 + index];
    if (bytes[length + 4] != checksum) {
        error = "Checksum Niimbot invalide.";
        return false;
    }
    packet.command = bytes[2];
    packet.payload.assign(bytes.begin() + 4, bytes.begin() + 4 + length);
    return true;
}

std::vector<std::uint8_t> u16(std::uint16_t value)
{
    return {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value & 0xff)};
}

std::vector<std::uint8_t> encode_bitmap_row(const RasterImage& image, int row)
{
    const int bytes_per_row = image.width / 8;
    std::vector<std::uint8_t> payload{static_cast<std::uint8_t>(row >> 8), static_cast<std::uint8_t>(row & 0xff), 0, 0, 0, 1};
    payload.reserve(6 + static_cast<std::size_t>(bytes_per_row));
    for (int byte_index = 0; byte_index < bytes_per_row; ++byte_index) {
        std::uint8_t packed = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (image.at(byte_index * 8 + bit, row) < 128)
                packed |= static_cast<std::uint8_t>(1U << (7 - bit));
        }
        payload.push_back(packed);
    }
    return payload;
}

} // namespace qrprotec::niimbot
