#pragma once

#include "../core/template.hpp"
#include "../render/raster.hpp"
#include "serial.hpp"

#include <functional>
#include <string>

namespace qrprotec {

struct PrintSettings {
    SerialSettings serial;
    int copies = 1;
    int density = 3;
    int label_type = 1;
};

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

} // namespace qrprotec