#include "template_io.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace qrprotec {

namespace {
std::string escape_json(const std::string& value)
{
    std::string result;
    for (const char character : value) {
        if (character == '\\') result += "\\\\";
        else if (character == '"') result += "\\\"";
        else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\t') result += "\\t";
        else result += character;
    }
    return result;
}

bool find_value(const std::string& json, const std::string& key, std::string& value)
{
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_position = json.find(marker);
    if (key_position == std::string::npos) return false;
    const std::size_t colon = json.find(':', key_position + marker.size());
    if (colon == std::string::npos) return false;
    std::size_t start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    if (start >= json.size() || json[start] != '"') return false;
    ++start;
    value.clear();
    bool escaped = false;
    for (std::size_t position = start; position < json.size(); ++position) {
        const char character = json[position];
        if (escaped) {
            value += character == 'n' ? '\n' : character == 'r' ? '\r' : character == 't' ? '\t' : character;
            escaped = false;
        } else if (character == '\\') escaped = true;
        else if (character == '"') return true;
        else value += character;
    }
    return false;
}

bool find_number(const std::string& json, const std::string& key, double& value)
{
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_position = json.find(marker);
    if (key_position == std::string::npos) return false;
    const std::size_t colon = json.find(':', key_position + marker.size());
    if (colon == std::string::npos) return false;
    std::size_t start = colon + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    char* end = nullptr;
    value = std::strtod(json.c_str() + start, &end);
    return end != json.c_str() + start;
}

std::string object_for_key(const std::string& json, const std::string& key)
{
    const std::size_t key_position = json.find("\"" + key + "\"");
    if (key_position == std::string::npos) return {};
    const std::size_t start = json.find('{', key_position);
    if (start == std::string::npos) return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t position = start; position < json.size(); ++position) {
        const char character = json[position];
        if (escaped) { escaped = false; continue; }
        if (character == '\\' && quoted) { escaped = true; continue; }
        if (character == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (character == '{') ++depth;
        if (character == '}' && --depth == 0) return json.substr(start, position - start + 1);
    }
    return {};
}

std::vector<std::string> array_objects(const std::string& array)
{
    std::vector<std::string> objects;
    std::size_t start = std::string::npos;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t position = 0; position < array.size(); ++position) {
        const char character = array[position];
        if (escaped) { escaped = false; continue; }
        if (character == '\\' && quoted) { escaped = true; continue; }
        if (character == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (character == '{') { if (depth++ == 0) start = position; }
        else if (character == '}' && --depth == 0 && start != std::string::npos)
            objects.push_back(array.substr(start, position - start + 1));
    }
    return objects;
}
}

bool save_template(const TemplateDocument& document, const std::string& path, std::string& error)
{
    std::ofstream output(path);
    if (!output) { error = "Impossible d'ouvrir le fichier template."; return false; }
    output << "{\n  \"version\": 1,\n  \"name\": \"" << escape_json(document.name) << "\",\n"
            << "  \"media\": {\"width_mm\": " << document.media.width_mm
            << ", \"height_mm\": " << document.media.height_mm
            << ", \"pixels_per_mm\": " << document.media.pixels_per_mm
            << ", \"orientation\": \"" << (document.media.orientation == Orientation::Portrait ? "portrait" : "landscape")
            << "},\n  \"parameters\": {";
    bool first = true;
    for (const auto& [key, value] : document.parameters) {
        if (!first) output << ", ";
        first = false;
        output << "\"" << escape_json(key) << "\": \"" << escape_json(value) << "\"";
    }
    output << "},\n  \"elements\": [\n";
    for (std::size_t index = 0; index < document.elements.size(); ++index) {
        const TemplateElement& element = document.elements[index];
        output << "    {\"id\": \"" << escape_json(element.id) << "\", \"kind\": \""
                << (element.kind == ElementKind::Text ? "text" : "qr") << "\", ";
        if (element.kind == ElementKind::Text) {
            const TextElement& text = std::get<TextElement>(element.content);
            output << "\"text\": \"" << escape_json(text.text) << "\", \"x_mm\": " << text.x_mm
                    << ", \"y_mm\": " << text.y_mm << ", \"width_mm\": " << text.width_mm
                    << ", \"height_mm\": " << text.height_mm << ", \"font_size_mm\": " << text.font_size_mm;
        } else {
            const QrElement& qr = std::get<QrElement>(element.content);
            output << "\"payload\": \"" << escape_json(qr.payload) << "\", \"x_mm\": " << qr.x_mm
                    << ", \"y_mm\": " << qr.y_mm << ", \"size_mm\": " << qr.size_mm;
        }
        output << "}" << (index + 1 == document.elements.size() ? "" : ",") << "\n";
    }
    output << "  ]\n}\n";
    if (!output) { error = "Erreur d'ecriture du template."; return false; }
    return true;
}

bool load_template(TemplateDocument& document, const std::string& path, std::string& error)
{
    std::ifstream input(path);
    if (!input) { error = "Impossible d'ouvrir le template."; return false; }
    const std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    TemplateDocument loaded;
    if (!find_value(json, "name", loaded.name)) { error = "Template invalide : nom absent."; return false; }
    double number = 0.0;
    if (!find_number(json, "width_mm", number)) { error = "Template invalide : largeur absente."; return false; }
    loaded.media.width_mm = number;
    if (!find_number(json, "height_mm", number)) { error = "Template invalide : hauteur absente."; return false; }
    loaded.media.height_mm = number;
    if (!find_number(json, "pixels_per_mm", number)) { error = "Template invalide : resolution absente."; return false; }
    loaded.media.pixels_per_mm = number;
    std::string orientation;
    if (find_value(json, "orientation", orientation))
        loaded.media.orientation = orientation == "portrait" ? Orientation::Portrait : Orientation::Landscape;
    const std::string parameters = object_for_key(json, "parameters");
    std::size_t position = 0;
    while ((position = parameters.find('"', position)) != std::string::npos) {
        const std::size_t key_end = parameters.find('"', position + 1);
        if (key_end == std::string::npos) break;
        const std::string key = parameters.substr(position + 1, key_end - position - 1);
        std::string value;
        if (!find_value(parameters.substr(position), key, value)) break;
        loaded.parameters[key] = value;
        position = key_end + 1;
    }
    const std::size_t elements_start = json.find("\"elements\"");
    if (elements_start != std::string::npos) {
        const std::size_t array_start = json.find('[', elements_start);
        const std::size_t array_end = json.find(']', array_start);
        for (const std::string& object : array_objects(json.substr(array_start, array_end - array_start + 1))) {
            std::string id;
            std::string kind;
            if (!find_value(object, "id", id) || !find_value(object, "kind", kind)) continue;
            if (kind == "text") {
                TextElement text;
                double x = 0.0;
                find_value(object, "text", text.text);
                find_number(object, "x_mm", x); text.x_mm = static_cast<float>(x);
                find_number(object, "y_mm", x); text.y_mm = static_cast<float>(x);
                find_number(object, "width_mm", x); text.width_mm = static_cast<float>(x);
                find_number(object, "height_mm", x); text.height_mm = static_cast<float>(x);
                find_number(object, "font_size_mm", x); text.font_size_mm = static_cast<float>(x);
                loaded.elements.push_back({id, ElementKind::Text, text});
            } else if (kind == "qr") {
                QrElement qr;
                double x = 0.0;
                find_value(object, "payload", qr.payload);
                find_number(object, "x_mm", x); qr.x_mm = static_cast<float>(x);
                find_number(object, "y_mm", x); qr.y_mm = static_cast<float>(x);
                find_number(object, "size_mm", x); qr.size_mm = static_cast<float>(x);
                loaded.elements.push_back({id, ElementKind::QrCode, qr});
            }
        }
    }
    document = std::move(loaded);
    return true;
}

} // namespace qrprotec