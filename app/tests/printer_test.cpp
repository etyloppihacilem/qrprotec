#include "printer/label_languages.hpp"
#include "printer/niimbot_protocol.hpp"
#include "printer/raw_transport.hpp"

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

    // TSPL / ZPL : image 10 x 2 (largeur non multiple de 8), noir en x=0 et x=9 sur la 1re ligne
    RasterImage label{10, 2, std::vector<std::uint8_t>(20, 255)};
    label.at(0, 0) = 0;
    label.at(9, 0) = 0;
    label.at(1, 1) = 0;
    assert((qrprotec::labels::pack_rows(label, true) == std::vector<std::uint8_t>{0x80, 0x40, 0x40, 0x00}));
    assert((qrprotec::labels::pack_rows(label, false) == std::vector<std::uint8_t>{0x7f, 0xbf, 0xbf, 0xff}));

    qrprotec::labels::RawJob job;
    job.width_dots = 320;
    job.height_dots = 240;
    job.copies = 2;
    job.darkness = 10;
    job.gap_mm = 2.5;
    const auto as_text = [](const std::vector<std::uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); };
    const std::string tspl = as_text(qrprotec::labels::encode_tspl(label, job));
    assert(tspl.rfind("SIZE 40 mm,30 mm\r\nGAP 2.5 mm,0 mm\r\n", 0) == 0);
    assert(tspl.find("DENSITY 10\r\n") != std::string::npos);
    assert(tspl.find("SET RIBBON ON\r\n") != std::string::npos);
    const std::string bitmap = "BITMAP 0,0,2,2,0,";
    const std::size_t at = tspl.find(bitmap);
    assert(at != std::string::npos);
    assert(tspl.substr(at + bitmap.size(), 6) == std::string("\x7f\xbf\xbf\xff\r\n"));
    assert(tspl.size() >= 13 && tspl.compare(tspl.size() - 13, 13, "\r\nPRINT 1,2\r\n") == 0);

    job.ribbon = false;
    const std::string zpl = as_text(qrprotec::labels::encode_zpl(label, job));
    // format de l'etiquette : largeur d'impression = largeur de l'etiquette, positions explicites
    assert(zpl.rfind("~SD20\n~TA000\n^XA\n^MTD\n^MNY\n^PW320\n^LL240\n^LH0,0\n^LS0\n^LT0\n", 0) == 0);
    assert(zpl.find("^FO0,0^GFA,4,4,2,8040\n4000^FS\n") != std::string::npos);
    assert(zpl.find("^PQ2\n^XZ\n") != std::string::npos);
    assert(tspl.find("SHIFT 0,0\r\nOFFSET 0 mm\r\nCLS\r\n") != std::string::npos);

    // decalages : ^LS positif = vers la gauche (donc oppose), ^LT positif = vers le bas, ~TA ; borne ±120 points
    job.offset_x_dots = 16;
    job.offset_y_dots = -16;
    job.tear_off_dots = -8;
    const std::string shifted = as_text(qrprotec::labels::encode_zpl(label, job));
    assert(shifted.find("~TA-08\n^XA\n") != std::string::npos);
    assert(shifted.find("^PW320\n^LL240\n^LH0,0\n^LS-16\n^LT-16\n^FO0,0^GFA,4,4,2,") != std::string::npos);
    assert(as_text(qrprotec::labels::encode_tspl(label, job)).find("SHIFT 16,-16\r\nOFFSET -1 mm\r\nCLS\r\nBITMAP 0,0,") !=
           std::string::npos);
    job.offset_x_dots = -40;
    job.offset_y_dots = 200;
    job.tear_off_dots = 400;
    const std::string bounded = as_text(qrprotec::labels::encode_zpl(label, job));
    assert(bounded.find("~TA120\n") != std::string::npos && bounded.find("^LS40\n^LT120\n") != std::string::npos);
    assert(as_text(qrprotec::labels::encode_tspl(label, job)).find("SHIFT -40,200\r\nOFFSET 25.4 mm\r\n") != std::string::npos);

    // maintenance : calibrage sur l'etiquette chargee (40 x 30 mm, thermique direct)
    using qrprotec::labels::Maintenance;
    qrprotec::labels::RawJob media;
    media.width_dots = 320;
    media.height_dots = 240;
    media.ribbon = false;
    media.gap_mm = 3.0;
    media.offset_x_dots = 8;
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Calibrate, media) ==
           "~TA000\n^XA\n^MTD\n^MNY\n^PW320\n^LL240\n^LH0,0\n^LS-8\n^LT0\n^XZ\n~JC\n^XA^JUS^XZ\n");
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Config, media) == "~WC\n");
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Feed, media) == "~PH\n");
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Factory, media) == "^XA^JUF^XZ\n^XA^JUS^XZ\n");
    assert(qrprotec::labels::maintenance_command(false, Maintenance::Calibrate, media) ==
           "SIZE 40 mm,30 mm\r\nGAP 3 mm,0 mm\r\nSET RIBBON OFF\r\nGAPDETECT\r\n");
    assert(qrprotec::labels::maintenance_command(false, Maintenance::Feed, media) == "FORMFEED\r\n");
    assert(qrprotec::labels::maintenance_command(false, Maintenance::Factory, media) == "INITIALPRINTER\r\n");

    // etat de l'imprimante : ~HS (ZPL), <ESC>!? (TSPL)
    using qrprotec::labels::parse_status;
    const auto host_status = [](const std::string& first, const std::string& second) {
        const std::string text = "\x02" + first + "\x03\r\n\x02" + second + "\x03\r\n\x02" "1234,0\x03\r\n";
        return std::vector<std::uint8_t>(text.begin(), text.end());
    };
    const auto ready = parse_status(true, host_status("030,0,0,0240,000,0,0,0,000,0,0,0", "001,0,0,0,1,2,6,0,00000000,1,000"));
    assert(ready.known && !ready.blocking() && ready.description() == "prête");
    const auto out = parse_status(true, host_status("030,1,0,0240,000,0,0,0,000,0,0,0", "001,0,1,0,1,2,6,0,00000000,1,000"));
    assert(out.known && out.paper_out && out.head_open && !out.ribbon_out && out.blocking());
    const auto paused = parse_status(true, host_status("030,0,1,0240,001,0,0,0,000,0,0,0", "001,0,0,1,1,2,6,0,00000002,1,000"));
    assert(paused.paused && paused.ribbon_out && paused.formats_waiting == 1 && paused.labels_remaining == 2);
    assert(!parse_status(true, {}).known && !parse_status(true, {}).blocking());
    assert(parse_status(false, {0x00}).known && !parse_status(false, {0x00}).blocking());
    const auto tspl_state = parse_status(false, {0x05});
    assert(tspl_state.head_open && tspl_state.paper_out && !tspl_state.ribbon_out && tspl_state.blocking());
    assert(parse_status(false, {0x20}).printing && !parse_status(false, {0x20}).blocking());
    assert((qrprotec::labels::status_request(true) == std::vector<std::uint8_t>{'~', 'H', 'S', '\n'}));
    assert((qrprotec::labels::status_request(false) == std::vector<std::uint8_t>{0x1b, '!', '?'}));

    // adresse de l'imprimante : fichier ou reseau
    using qrprotec::RawTransport;
    assert(!RawTransport::is_network_address("/dev/usb/lp0"));
    assert(RawTransport::is_network_address("192.168.1.50"));
    std::string host;
    std::string port;
    assert(RawTransport::split_host_port("192.168.1.50", host, port) && host == "192.168.1.50" && port == "9100");
    assert(RawTransport::split_host_port("tcp://zebra.local:6101/", host, port) && host == "zebra.local" && port == "6101");
    assert(RawTransport::split_host_port("[fe80::1]:9100", host, port) && host == "fe80::1" && port == "9100");
    assert(!RawTransport::split_host_port("imprimante:abc", host, port));
    return 0;
}
