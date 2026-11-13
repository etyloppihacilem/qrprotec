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
    double offset_x_mm = 0.0;             // decalage horizontal : positif = vers la droite
    double offset_y_mm = 0.0;             // decalage vertical : positif = vers le bas
    double tear_off_mm = 0.0;             // position d'arret (decoupe) : positif = l'etiquette sort plus loin
    int timeout_ms = 5000;
};

struct PrintSettings {
    PrinterKind kind = PrinterKind::NiimbotB1;
    SerialSettings serial;
    RawPrinterSettings raw;
    // Type d'etiquette charge dans l'imprimante (nom) : un modele d'un autre type met la file en pause
    std::string loaded_label_type;
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

// Etat lu aupres de l'imprimante (TSPL / ZPL) ; la Niimbot B1 signale ses erreurs pendant l'impression
struct PrinterStatus {
    bool known = false;    // l'imprimante a repondu
    bool blocking = false; // tete ouverte, plus d'etiquettes, plus de ruban, pause...
    std::string text;      // en clair, pour l'interface
};

class Printer {
public:
    virtual ~Printer() = default;
    virtual bool connect(const PrintSettings& settings, std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) = 0;
    virtual bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) = 0;
    virtual PrinterStatus query_status(const PrintSettings& settings);
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

// Imprimante TSPL ou ZPL : l'etiquette part d'un bloc. Avant chaque etiquette, l'etat est demande
// (~HS en ZPL, <ESC>!? en TSPL) : tete ouverte, plus d'etiquettes ou de ruban, pause arretent la file
// avec la raison. Une imprimante qui ne repond pas (liaison a sens unique) imprime sans verification.
class RawLabelPrinter final : public Printer {
public:
    bool connect(const PrintSettings& settings, std::string& error) override;
    void disconnect() override;
    bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) override;
    bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) override;
    PrinterStatus query_status(const PrintSettings& settings) override;

private:
    PrinterStatus read_status(PrinterKind kind, int timeout_ms);

    RawTransport transport_;
    bool silent_ = false; // pas de reponse a la premiere demande d'etat sur cette connexion : on ne redemande pas
};

// Imprimante choisie dans les reglages (PrintSettings::kind)
class AnyPrinter final : public Printer {
public:
    bool connect(const PrintSettings& settings, std::string& error) override;
    void disconnect() override;
    bool print(const PrintRequest& request, const std::function<void(float)>& progress, std::string& error) override;
    bool send_command(const PrintSettings& settings, const std::string& command, std::string& error) override;
    PrinterStatus query_status(const PrintSettings& settings) override;

private:
    Printer& select(PrinterKind kind);

    NiimbotB1Printer b1_;
    RawLabelPrinter raw_;
    Printer* active_ = nullptr;
    PrinterKind active_kind_ = PrinterKind::NiimbotB1;
    PrintSettings active_settings_;
};

} // namespace qrprotec