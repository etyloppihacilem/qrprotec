#include "sdk_json.hpp"

#include "inateck_worker.hpp"
#include "scanner_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace qrprotec {

namespace {

void append_utf8(std::string& out, unsigned int code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

std::string sdk_json_string(const char* json, const char* key) {
    if (!json || !key)
        return {};
    const std::string needle = std::string("\"") + key + "\"";
    const char* start = std::strstr(json, needle.c_str());
    if (!start)
        return {};
    start = std::strchr(start + needle.size(), ':');
    if (!start)
        return {};
    ++start;
    while (*start && std::isspace(static_cast<unsigned char>(*start)))
        ++start;
    if (*start != '"')
        return {};
    std::string value;
    for (const char* cursor = start + 1; *cursor && *cursor != '"'; ++cursor) {
        if (*cursor != '\\') {
            value += *cursor;
            continue;
        }
        ++cursor;
        switch (*cursor) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'u': {
            unsigned int code_point = 0;
            int digits = 0;
            for (; digits < 4 && std::isxdigit(static_cast<unsigned char>(cursor[1])); ++digits, ++cursor) {
                const char digit = static_cast<char>(std::tolower(static_cast<unsigned char>(cursor[1])));
                code_point = code_point * 16 + static_cast<unsigned int>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
            }
            if (digits == 4)
                append_utf8(value, code_point);
            break;
        }
        case '\0': return value;
        default: value += *cursor; break; // \" \\ \/
        }
    }
    return value;
}

bool sdk_json_success(const char* json) {
    if (!json)
        return false;
    const char* status = std::strstr(json, "\"status\"");
    if (!status)
        return false;
    status = std::strchr(status, ':');
    if (!status)
        return false;
    ++status;
    while (*status && std::isspace(static_cast<unsigned char>(*status)))
        ++status;
    return *status == '0';
}

std::vector<std::string> sdk_json_device_objects(const char* json) {
    std::vector<std::string> objects;
    if (!json)
        return objects;
    const char* array = std::strstr(json, "\"devices\"");
    if (!array)
        array = std::strstr(json, "\"device_list\"");
    if (!array)
        return objects;
    array = std::strchr(array, '[');
    if (!array)
        return objects;
    const char* object_start = nullptr;
    int depth = 0;
    for (const char* cursor = array + 1; *cursor; ++cursor) {
        if (*cursor == '{') {
            if (depth == 0)
                object_start = cursor;
            ++depth;
        } else if (*cursor == '}' && depth > 0) {
            --depth;
            if (depth == 0 && object_start)
                objects.emplace_back(object_start, cursor + 1);
        } else if (*cursor == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

namespace {

// Entier de "key" dans un objet JSON plat (false si absent ou non numerique).
bool json_int(const std::string& object, const char* key, int& value) {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t position = object.find(needle);
    if (position == std::string::npos)
        return false;
    position = object.find(':', position + needle.size());
    if (position == std::string::npos)
        return false;
    const char* start = object.c_str() + position + 1;
    char* end = nullptr;
    const long parsed = std::strtol(start, &end, 10);
    if (end == start)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

// Objets {...} de premier niveau du tableau qui suit `array` (pointeur sur '[').
std::vector<std::string> array_objects(const char* array) {
    std::vector<std::string> objects;
    const char* object_start = nullptr;
    int depth = 0;
    bool in_string = false;
    for (const char* cursor = array + 1; *cursor; ++cursor) {
        if (in_string) {
            if (*cursor == '\\' && cursor[1])
                ++cursor;
            else if (*cursor == '"')
                in_string = false;
            continue;
        }
        if (*cursor == '"') {
            in_string = true;
        } else if (*cursor == '{') {
            if (depth == 0)
                object_start = cursor;
            ++depth;
        } else if (*cursor == '}' && depth > 0) {
            --depth;
            if (depth == 0 && object_start)
                objects.emplace_back(object_start, cursor + 1);
        } else if (*cursor == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

} // namespace

std::vector<ScannerSettingValue> sdk_json_settings(const char* json) {
    std::vector<ScannerSettingValue> settings;
    if (!json)
        return settings;
    const char* data = std::strstr(json, "\"data\"");
    if (!data)
        return settings;
    data = std::strchr(data, '[');
    if (!data)
        return settings;
    for (const std::string& object : array_objects(data)) {
        ScannerSettingValue setting;
        if (!json_int(object, "flag", setting.flag) || !json_int(object, "value", setting.value))
            continue;
        setting.name = sdk_json_string(object.c_str(), "name");
        settings.push_back(std::move(setting));
    }
    return settings;
}

std::string sdk_json_data_text(const char* json) {
    if (!json)
        return {};
    const char* data = std::strstr(json, "\"data\"");
    if (!data)
        return {};
    data = std::strchr(data, ':');
    if (!data)
        return {};
    ++data;
    while (*data && std::isspace(static_cast<unsigned char>(*data)))
        ++data;
    if (*data == '"')
        return sdk_json_string(json, "data");
    // nombre, booleen, tableau ou objet : texte brut jusqu'a la fin de la valeur
    int depth = 0;
    const char* cursor = data;
    for (; *cursor; ++cursor) {
        if (*cursor == '[' || *cursor == '{')
            ++depth;
        else if (*cursor == ']' || *cursor == '}') {
            if (depth == 0)
                break;
            if (--depth == 0) {
                ++cursor;
                break;
            }
        } else if (*cursor == ',' && depth == 0)
            break;
    }
    std::string text(data, cursor);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::string clean_scan_code(std::string code) {
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
        code.pop_back();
    return code;
}

bool looks_like_scanner(const std::string& name) {
    const std::string lower = lowercase(name);
    return lower.find("inateck") != std::string::npos || lower.rfind("bcst", 0) == 0;
}

std::vector<InateckDevice> ordered_devices(const std::vector<InateckDevice>& devices,
                                           const std::string& preferred_id,
                                           const std::vector<std::string>& rejected_ids) {
    std::vector<InateckDevice> candidates;
    for (const InateckDevice& device : devices)
        if (std::find(rejected_ids.begin(), rejected_ids.end(), device.id) == rejected_ids.end())
            candidates.push_back(device);
    const auto rank = [&preferred_id](const InateckDevice& device) {
        if (!preferred_id.empty() && device.id == preferred_id)
            return 0;
        return looks_like_scanner(device.name) ? 1 : 2;
    };
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&rank](const InateckDevice& a, const InateckDevice& b) { return rank(a) < rank(b); });
    return candidates;
}

} // namespace qrprotec
