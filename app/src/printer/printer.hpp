#pragma once

#include "../core/template.hpp"
#include "../render/raster.hpp"
#include "raw_transport.hpp"
#include "serial.hpp"

#include <functional>
#include <string>

namespace qrprotec {

// Modele d'imprimante. La Niimbot B1 reste pour le debug et le secours ; TSPL et ZPL couvrent les
// imprimantes d'etiquettes de bureau (TSC, IDPRT, Munbyn... / Zebra).
enum class PrinterKind { NiimbotB1, Tspl, Zpl };

const char* printer_kind_id(PrinterKind kind);          // "b1", "tspl", "zpl" (fichier de reglages)
PrinterKind printer_kind_from_id(const std::string& id); // inconnu = Niimbot B1

// Reglages des imprimantes TSPL / ZPL
struct RawPrinterSettings {
    std::string address = "/dev/usb/lp0"; // fichier de peripherique ou adresse reseau (port 9100)
    int darkness = 8;                     // 0-15
    bool ribbon = true;                   // transfert thermique ; decoche = thermique direct
    double gap_mm = 2.0;                  // espace entre deux etiquettes
    double offset_x_mm = 0.0;             // decalage de l'image sur l'etiquette
    double offset_y_mm = 0.0;
    int timeout_ms = 5000;
};

struct PrintSettings {
    PrinterKind kind = PrinterKind::NiimbotB1;
    SerialSettings serial;
    RawPrinterSettings raw;
    int copies = 1;
    int density = 3;
    int label_type = 1;
};

// Largeur maximale de la tete (mm) : 48 pour la B1, 104 pour une imprimante 4 pouces
double printer_max_width_mm(PrinterKind kind);

struct PrintRequest {
    RasterImage image;
    MediaSettings media;
    PrintSettings settings;
};

class Printer {
public:
    virtual ~Printer() = default;
    virtual bool connect(const PrintSettings& settings, std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) = 0;
    virtual bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) = 0;
};

class NiimbotB1Printer final : public Printer {
public:
    bool connect(const PrintSettings& settings, std::string& error) override;
    void disconnect() override;
    bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) override;
    bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) override;

private:
    SerialPort serial_;
};

// Imprimante TSPL ou ZPL : l'etiquette part d'un bloc. Pas de retour d'etat : une erreur papier ou
// ruban s'affiche sur l'imprimante, qui garde l'etiquette et la sort une fois le probleme regle.
class RawLabelPrinter final : public Printer {
public:
    bool connect(const PrintSettings& settings, std::string& error) override;
    void disconnect() override;
    bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) override;
    bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) override;

private:
    RawTransport transport_;
};

// Imprimante choisie dans les reglages (PrintSettings::kind)
class AnyPrinter final : public Printer {
public:
    bool connect(const PrintSettings& settings, std::string& error) override;
    void disconnect() override;
    bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) override;
    bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) override;

private:
    Printer& select(PrinterKind kind);

    NiimbotB1Printer b1_;
    RawLabelPrinter raw_;
    Printer* active_ = nullptr;
    PrinterKind active_kind_ = PrinterKind::NiimbotB1;
    PrintSettings active_settings_;
};

} // namespace qrprotec