#pragma once

#include "../render/raster.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qrprotec::niimbot {

struct Packet {
    std::uint8_t command = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_packet(std::uint8_t command, const std::vector<std::uint8_t>& payload);
bool decode_packet(const std::vector<std::uint8_t>& bytes, Packet& packet, std::string& error);
std::vector<std::uint8_t> u16(std::uint16_t value);
std::vector<std::uint8_t> encode_bitmap_row(const RasterImage& image, int row);

} // namespace qrprotec::niimbot
