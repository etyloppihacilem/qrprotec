#include "printer.hpp"

#include "logging.hpp"
#include "niimbot_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace {
std::string hex(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t value : bytes) output << std::setw(2) << static_cast<int>(value);
    return output.str();
}

std::string print_error_description(std::uint8_t code)
{
    switch (code) {
    case 0x01: return "couvercle ouvert";
    case 0x02: return "papier absent";
    case 0x09: return "imprimante occupée";
    case 0x14: return "écriture RFID impossible";
    case 0x18: return "paramètres de page invalides";
    case 0x34: return "délai de réception des données dépassé";
    default: return "erreur matérielle ou de protocole";
    }
}
}

namespace qrprotec {

bool NiimbotB1Printer::connect(const PrintSettings& settings, std::string& error)
{
    return serial_.open(settings.serial, error);
}

void NiimbotB1Printer::disconnect()
{
    serial_.close();
}

bool NiimbotB1Printer::send_command(const PrintSettings& settings, const std::string& command, std::string& error)
{
    if (!connect(settings, error)) return false;
    const std::vector<std::uint8_t> bytes(command.begin(), command.end());
    const bool sent = serial_.write_bytes(bytes, error);
    disconnect();
    return sent;
}

bool NiimbotB1Printer::print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error)
{
    if (request.image.width != request.media.width_pixels() || request.image.height != request.media.height_pixels()) {
        error = "La résolution du raster ne correspond pas au media.";
        return false;
    }
    if (!serial_.is_open()) {
        error = "Imprimante non connectée.";
        return false;
    }
    if (request.image.width % 8 != 0 || request.image.width > 384 || request.image.width <= 0 || request.image.height <= 0 ||
        request.image.height > 0xffff) {
        error = "La résolution B1 doit avoir une largeur multiple de 8 et au plus 384 pixels.";
        return false;
    }
    if (request.settings.density < 1 || request.settings.density > 5 || request.settings.label_type < 1 || request.settings.label_type > 3) {
        error = "Densité ou type de label B1 invalide.";
        return false;
    }
    const int copies = std::max(1, std::min(request.settings.copies, 0xffff));
    auto command = [&](std::uint8_t id, const std::vector<std::uint8_t>& payload, std::uint8_t expected) {
        const std::vector<std::uint8_t> bytes = niimbot::encode_packet(id, payload);
        debug_log("TX " + hex(bytes));
        if (!serial_.write_bytes(bytes, error)) return false;
        std::uint8_t response = 0;
        std::vector<std::uint8_t> response_payload;
        for (;;) {
            if (!serial_.read_packet(response, response_payload, error)) return false;
            debug_log("RX command=0x" + hex({response}) + " payload=" + hex(response_payload));
            if (response != 0xd3 && response != 0xe0) break;
        }
        if (response != expected || response_payload.empty() || response_payload[0] == 0) {
            if (response == 0xdb && !response_payload.empty()) {
                error = "Erreur imprimante: " + print_error_description(response_payload[0]) + " (0x";
                const char* digits = "0123456789abcdef";
                error += digits[(response_payload[0] >> 4) & 0xf];
                error += digits[response_payload[0] & 0xf];
                error += ").";
                return false;
            }
            error = "La commande imprimante a été refusée (0x";
            const char* digits = "0123456789abcdef";
            error += digits[(response >> 4) & 0xf];
            error += digits[response & 0xf];
            error += ").";
            return false;
        }
        return true;
    };
    if (progress) progress(0.0f);
    if (!command(0x21, {static_cast<std::uint8_t>(request.settings.density)}, 0x31)) return false;
    if (!command(0x23, {static_cast<std::uint8_t>(request.settings.label_type)}, 0x33)) return false;
    if (!command(0x01, {0, 1, 0, 0, 0, 0, 0}, 0x02)) return false;
    if (!command(0x03, {1}, 0x04)) return false;
    std::vector<std::uint8_t> dimensions = niimbot::u16(static_cast<std::uint16_t>(request.image.height));
    const std::vector<std::uint8_t> width = niimbot::u16(static_cast<std::uint16_t>(request.image.width));
    dimensions.insert(dimensions.end(), width.begin(), width.end());
    const std::vector<std::uint8_t> quantity = niimbot::u16(static_cast<std::uint16_t>(copies));
    dimensions.insert(dimensions.end(), quantity.begin(), quantity.end());
    if (!command(0x13, dimensions, 0x14)) return false;
    for (int row = 0; row < request.image.height; ++row) {
        const std::vector<std::uint8_t> line = niimbot::encode_bitmap_row(request.image, row);
        const std::vector<std::uint8_t> bytes = niimbot::encode_packet(0x85, line);
        debug_log("TX row " + std::to_string(row) + " " + hex(bytes));
        if (!serial_.write_bytes(bytes, error)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (progress) progress(static_cast<float>(row + 1) / static_cast<float>(request.image.height));
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (command(0xe3, {1}, 0xe4)) break;
        if (attempt == 19) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::uint8_t response = 0;
        std::vector<std::uint8_t> response_payload;
        if (!serial_.write_bytes(niimbot::encode_packet(0xa3, {1}), error)) return false;
        do {
            if (!serial_.read_packet(response, response_payload, error)) return false;
            debug_log("RX status command=0x" + hex({response}) + " payload=" + hex(response_payload));
        } while (response == 0xd3 || response == 0xe0);
        if (response != 0xb3 || response_payload.size() < 4) {
            error = "Statut d'impression B1 invalide.";
            return false;
        }
        const int printed_pages = (static_cast<int>(response_payload[0]) << 8) | response_payload[1];
        if (printed_pages >= copies) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (attempt == 99) {
            error = "Timeout de fin d'impression B1.";
            return false;
        }
    }
    if (!command(0xf3, {1}, 0xf4)) return false;
    return true;
}

} // namespace qrprotec