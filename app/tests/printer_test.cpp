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
    job.offset_x_dots = 16;
    const std::string zpl = as_text(qrprotec::labels::encode_zpl(label, job));
    assert(zpl.rfind("~SD20\n^XA\n^MTD\n", 0) == 0);
    assert(zpl.find("^PW448\n^LL240\n") != std::string::npos);
    assert(zpl.find("^FO80,0^GFA,4,4,2,8040\n4000^FS\n") != std::string::npos);
    assert(zpl.find("^PQ2\n^XZ\n") != std::string::npos);

    // decalage negatif : zone ^PW elargie et ^LT en ZPL, SHIFT en TSPL
    job.offset_x_dots = -8;
    job.offset_y_dots = -16;
    const std::string shifted = as_text(qrprotec::labels::encode_zpl(label, job));
    assert(shifted.find("^LT-16\n^FO56,0^GFA") != std::string::npos);
    assert(as_text(qrprotec::labels::encode_tspl(label, job)).find("SHIFT -8,-16\r\nCLS\r\nBITMAP 0,0,") != std::string::npos);
    // vers le bas : toute l'etiquette descend (^LT / SHIFT), l'image n'est pas coupee par ^LL
    job.offset_x_dots = 0;
    job.offset_y_dots = 16;
    assert(as_text(qrprotec::labels::encode_zpl(label, job)).find("^LT16\n^FO64,0^GFA") != std::string::npos);
    assert(as_text(qrprotec::labels::encode_tspl(label, job)).find("SHIFT 0,16\r\nCLS\r\nBITMAP 0,0,") != std::string::npos);

    // maintenance : calibrage sur l'etiquette chargee (40 x 30 mm, thermique direct)
    using qrprotec::labels::Maintenance;
    qrprotec::labels::RawJob media;
    media.width_dots = 320;
    media.height_dots = 240;
    media.ribbon = false;
    media.gap_mm = 3.0;
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Calibrate, media) ==
           "^XA^MNY^MTD^PW448^LL240^JUS^XZ\n~JC\n^XA^JUS^XZ\n");
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Config, media) == "~WC\n");
    assert(qrprotec::labels::maintenance_command(true, Maintenance::Feed, media) == "^XA^MNY^LL240^XZ\n");
    assert(qrprotec::labels::maintenance_command(false, Maintenance::Calibrate, media) ==
           "SIZE 40 mm,30 mm\r\nGAP 3 mm,0 mm\r\nSET RIBBON OFF\r\nGAPDETECT\r\n");
    assert(qrprotec::labels::maintenance_command(false, Maintenance::Feed, media) == "FORMFEED\r\n");

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
