#include "printer.hpp"

#include "label_languages.hpp"
#include "logging.hpp"
#include "niimbot_protocol.hpp"

#include <algorithm>
#include <cmath>
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
    const auto describe = [](std::uint8_t code) {
        const char* digits = "0123456789abcdef";
        return std::string("0x") + digits[(code >> 4) & 0xf] + digits[code & 0xf];
    };
    // refused : l'imprimante a repondu a la commande mais l'a refusee (payload 0), souvent parce
    // qu'elle termine encore l'etiquette precedente
    bool refused = false;
    auto command = [&](std::uint8_t id, const std::vector<std::uint8_t>& payload, std::uint8_t expected) {
        refused = false;
        const std::vector<std::uint8_t> bytes = niimbot::encode_packet(id, payload);
        debug_log("TX " + hex(bytes));
        if (!serial_.write_bytes(bytes, error)) return false;
        std::uint8_t response = 0;
        std::vector<std::uint8_t> response_payload;
        // on ignore les trames de service (0xd3, 0xe0) et quelques reponses parasites en retard
        for (int stale = 0;; ) {
            if (!serial_.read_packet(response, response_payload, error)) return false;
            debug_log("RX command=0x" + hex({response}) + " payload=" + hex(response_payload));
            if (response == 0xd3 || response == 0xe0) continue;
            if (response == expected || response == 0xdb || ++stale > 5) break;
            debug_log("Reponse ignoree (attendu " + describe(expected) + ")");
        }
        if (response == 0xdb && !response_payload.empty()) {
            error = "Erreur imprimante: " + print_error_description(response_payload[0]) + " (" +
                    describe(response_payload[0]) + ").";
            return false;
        }
        if (response != expected) {
            error = "Réponse inattendue de l'imprimante (" + describe(response) + " au lieu de " + describe(expected) + ").";
            return false;
        }
        if (response_payload.empty() || response_payload[0] == 0) {
            refused = true;
            error = "La commande imprimante a été refusée (" + describe(response) + ").";
            return false;
        }
        return true;
    };
    // commandes de preparation : reessayees tant que l'imprimante est occupee
    auto setup = [&](std::uint8_t id, const std::vector<std::uint8_t>& payload, std::uint8_t expected) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (command(id, payload, expected)) return true;
            if (!refused) return false;
            debug_log("Imprimante occupee, nouvel essai de " + describe(id));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return false;
    };
    serial_.discard_input();
    if (progress) progress(0.0f);
    if (!setup(0x21, {static_cast<std::uint8_t>(request.settings.density)}, 0x31)) return false;
    if (!setup(0x23, {static_cast<std::uint8_t>(request.settings.label_type)}, 0x33)) return false;
    if (!setup(0x01, {0, 1, 0, 0, 0, 0, 0}, 0x02)) return false;
    if (!setup(0x03, {1}, 0x04)) return false;
    std::vector<std::uint8_t> dimensions = niimbot::u16(static_cast<std::uint16_t>(request.image.height));
    const std::vector<std::uint8_t> width = niimbot::u16(static_cast<std::uint16_t>(request.image.width));
    dimensions.insert(dimensions.end(), width.begin(), width.end());
    const std::vector<std::uint8_t> quantity = niimbot::u16(static_cast<std::uint16_t>(copies));
    dimensions.insert(dimensions.end(), quantity.begin(), quantity.end());
    if (!setup(0x13, dimensions, 0x14)) return false;
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
    // l'etiquette est deja imprimee : un refus de fin d'impression ne doit pas la faire reimprimer
    if (!setup(0xf3, {1}, 0xf4))
        debug_log("Fin d'impression non confirmee : " + error);
    error.clear();
    return true;
}

const char* printer_kind_id(PrinterKind kind)
{
    switch (kind) {
    case PrinterKind::Tspl: return "tspl";
    case PrinterKind::Zpl: return "zpl";
    case PrinterKind::NiimbotB1: break;
    }
    return "b1";
}

PrinterKind printer_kind_from_id(const std::string& id)
{
    if (id == "tspl") return PrinterKind::Tspl;
    if (id == "zpl") return PrinterKind::Zpl;
    return PrinterKind::NiimbotB1;
}

double printer_max_width_mm(PrinterKind kind)
{
    return kind == PrinterKind::NiimbotB1 ? 48.0 : 104.0;
}

PrinterStatus Printer::query_status(const PrintSettings&)
{
    PrinterStatus status;
    status.text = "pas de lecture d'état pour ce modèle";
    return status;
}

bool RawLabelPrinter::connect(const PrintSettings& settings, std::string& error)
{
    silent_ = false;
    return transport_.open(settings.raw.address, settings.raw.timeout_ms, error);
}

PrinterStatus RawLabelPrinter::read_status(PrinterKind kind, int timeout_ms)
{
    PrinterStatus status;
    const bool zpl = kind == PrinterKind::Zpl;
    std::string error;
    if (!transport_.readable()) {
        status.text = "état non lisible (liaison en écriture seule)";
        return status;
    }
    if (!transport_.write_bytes(labels::status_request(zpl), error)) {
        status.text = error;
        return status;
    }
    const labels::PrinterState state = labels::parse_status(zpl, transport_.read_reply(timeout_ms));
    status.known = state.known;
    status.blocking = state.blocking();
    status.text = state.description();
    return status;
}

PrinterStatus RawLabelPrinter::query_status(const PrintSettings& settings)
{
    std::string error;
    if (!connect(settings, error)) {
        PrinterStatus status;
        status.text = error;
        return status;
    }
    const PrinterStatus status = read_status(settings.kind, 2000);
    disconnect();
    return status;
}

void RawLabelPrinter::disconnect()
{
    transport_.close();
}

bool RawLabelPrinter::send_command(const PrintSettings& settings, const std::string& command, std::string& error)
{
    if (!connect(settings, error)) return false;
    const std::vector<std::uint8_t> bytes(command.begin(), command.end());
    const bool sent = transport_.write_bytes(bytes, error);
    disconnect();
    return sent;
}

bool RawLabelPrinter::print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error)
{
    if (request.image.width != request.media.width_pixels() || request.image.height != request.media.height_pixels()) {
        error = "La résolution du raster ne correspond pas au media.";
        return false;
    }
    if (!transport_.is_open()) {
        error = "Imprimante non connectée.";
        return false;
    }
    if (request.image.width <= 0 || request.image.height <= 0) {
        error = "Étiquette vide.";
        return false;
    }
    const RawPrinterSettings& raw = request.settings.raw;
    const double ppm = request.media.pixels_per_mm > 0 ? request.media.pixels_per_mm : 8.0;
    labels::RawJob job;
    job.width_dots = request.image.width;
    job.height_dots = request.image.height;
    job.dots_per_mm = ppm;
    job.copies = std::max(1, request.settings.copies);
    job.darkness = raw.darkness;
    job.ribbon = raw.ribbon;
    job.gap_mm = raw.gap_mm;
    job.offset_x_dots = static_cast<int>(std::lround(raw.offset_x_mm * ppm));
    job.offset_y_dots = static_cast<int>(std::lround(raw.offset_y_mm * ppm));
    const std::vector<std::uint8_t> bytes = request.settings.kind == PrinterKind::Zpl
                                                ? labels::encode_zpl(request.image, job)
                                                : labels::encode_tspl(request.image, job);
    job.tear_off_dots = static_cast<int>(std::lround(raw.tear_off_mm * ppm));
    if (!silent_) {
        const PrinterStatus status = read_status(request.settings.kind, 1000);
        if (status.blocking) {
            error = "Imprimante : " + status.text + ".";
            return false;
        }
        silent_ = !status.known;
    }
    if (progress) progress(0.0f);
    debug_log(std::string(printer_kind_id(request.settings.kind)) + " : " + std::to_string(bytes.size()) + " octets");
    if (!transport_.write_bytes(bytes, error)) return false;
    if (progress) progress(1.0f);
    return true;
}

Printer& AnyPrinter::select(PrinterKind kind)
{
    if (kind == PrinterKind::NiimbotB1) return b1_;
    return raw_;
}

bool AnyPrinter::connect(const PrintSettings& settings, std::string& error)
{
    disconnect();
    Printer& printer = select(settings.kind);
    if (!printer.connect(settings, error)) return false;
    active_ = &printer;
    active_kind_ = settings.kind;
    active_settings_ = settings;
    return true;
}

void AnyPrinter::disconnect()
{
    if (active_ != nullptr) active_->disconnect();
    active_ = nullptr;
}

bool AnyPrinter::print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error)
{
    // reglages changes pendant que la file etait connectee (autre modele ou autre adresse) : on se reconnecte
    const bool same_link = active_ != nullptr && active_kind_ == request.settings.kind &&
                           (request.settings.kind == PrinterKind::NiimbotB1
                                ? active_settings_.serial.device == request.settings.serial.device &&
                                      active_settings_.serial.baud_rate == request.settings.serial.baud_rate
                                : active_settings_.raw.address == request.settings.raw.address);
    if (!same_link && !connect(request.settings, error)) return false;
    return active_->print(request, progress, error);
}

bool AnyPrinter::send_command(const PrintSettings& settings, const std::string& command, std::string& error)
{
    disconnect();
    return select(settings.kind).send_command(settings, command, error);
}

PrinterStatus AnyPrinter::query_status(const PrintSettings& settings)
{
    disconnect();
    return select(settings.kind).query_status(settings);
}

} // namespace qrprotec
