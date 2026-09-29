/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          json.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace qrprotec {

namespace {

const Json &null_value() {
  static const Json value;
  return value;
}

class Parser {
  public:
    explicit Parser(const std::string &text) : text_(text) {}

    bool parse(Json &out, std::string &error) {
      skip_spaces();
      if (!parse_value(out, 0)) {
        error = error_.empty() ? "JSON invalide" : error_;
        return false;
      }
      skip_spaces();
      if (position_ != text_.size()) {
        error = "Caracteres inattendus apres le JSON";
        return false;
      }
      return true;
    }

  private:
    void skip_spaces() {
      while (position_ < text_.size()
             && (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r'
                 || text_[position_] == '\t'))
        ++position_;
    }

    bool fail(const std::string &message) {
      error_ = message + " (position " + std::to_string(position_) + ")";
      return false;
    }

    bool literal(const char *word) {
      std::size_t length = 0;
      while (word[length])
        ++length;
      if (text_.compare(position_, length, word) != 0)
        return fail("Valeur inconnue");
      position_ += length;
      return true;
    }

    static void append_utf8(std::string &output, unsigned int code) {
      if (code < 0x80) {
        output += static_cast< char >(code);
      } else if (code < 0x800) {
        output += static_cast< char >(0xc0 | (code >> 6));
        output += static_cast< char >(0x80 | (code & 0x3f));
      } else if (code < 0x10000) {
        output += static_cast< char >(0xe0 | (code >> 12));
        output += static_cast< char >(0x80 | ((code >> 6) & 0x3f));
        output += static_cast< char >(0x80 | (code & 0x3f));
      } else {
        output += static_cast< char >(0xf0 | (code >> 18));
        output += static_cast< char >(0x80 | ((code >> 12) & 0x3f));
        output += static_cast< char >(0x80 | ((code >> 6) & 0x3f));
        output += static_cast< char >(0x80 | (code & 0x3f));
      }
    }

    bool hex4(unsigned int &value) {
      if (position_ + 4 > text_.size())
        return fail("Echappement unicode tronque");
      value = 0;
      for (int index = 0; index < 4; ++index) {
        const char   character = text_[position_++];
        unsigned int digit     = 0;
        if (character >= '0' && character <= '9')
          digit = static_cast< unsigned int >(character - '0');
        else if (character >= 'a' && character <= 'f')
          digit = static_cast< unsigned int >(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
          digit = static_cast< unsigned int >(character - 'A' + 10);
        else
          return fail("Echappement unicode invalide");
        value = value * 16 + digit;
      }
      return true;
    }

    bool parse_string(std::string &output) {
      ++position_; // guillemet ouvrant
      output.clear();
      while (position_ < text_.size()) {
        const char character = text_[position_++];
        if (character == '"')
          return true;
        if (character != '\\') {
          output += character;
          continue;
        }
        if (position_ >= text_.size())
          break;
        const char escaped = text_[position_++];
        switch (escaped) {
          case '"': output += '"'; break;
          case '\\': output += '\\'; break;
          case '/': output += '/'; break;
          case 'b': output += '\b'; break;
          case 'f': output += '\f'; break;
          case 'n': output += '\n'; break;
          case 'r': output += '\r'; break;
          case 't': output += '\t'; break;
          case 'u': {
            unsigned int code = 0;
            if (!hex4(code))
              return false;
            if (code >= 0xd800 && code <= 0xdbff && text_.compare(position_, 2, "\\u") == 0) {
              position_ += 2;
              unsigned int low = 0;
              if (!hex4(low))
                return false;
              code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
            append_utf8(output, code);
            break;
          }
          default: return fail("Echappement invalide");
        }
      }
      return fail("Chaine non terminee");
    }

    bool parse_value(Json &out, int depth) {
      if (depth > 64)
        return fail("JSON trop profond");
      if (position_ >= text_.size())
        return fail("Fin inattendue");
      const char character = text_[position_];
      if (character == '{') {
        ++position_;
        out = Json::object();
        skip_spaces();
        if (position_ < text_.size() && text_[position_] == '}') {
          ++position_;
          return true;
        }
        for (;;) {
          skip_spaces();
          if (position_ >= text_.size() || text_[position_] != '"')
            return fail("Cle attendue");
          std::string key;
          if (!parse_string(key))
            return false;
          skip_spaces();
          if (position_ >= text_.size() || text_[position_] != ':')
            return fail("':' attendu");
          ++position_;
          skip_spaces();
          Json value;
          if (!parse_value(value, depth + 1))
            return false;
          out[key] = std::move(value);
          skip_spaces();
          if (position_ < text_.size() && text_[position_] == ',') {
            ++position_;
            continue;
          }
          if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
          }
          return fail("',' ou '}' attendu");
        }
      }
      if (character == '[') {
        ++position_;
        out = Json::array();
        skip_spaces();
        if (position_ < text_.size() && text_[position_] == ']') {
          ++position_;
          return true;
        }
        for (;;) {
          skip_spaces();
          Json value;
          if (!parse_value(value, depth + 1))
            return false;
          out.push_back(std::move(value));
          skip_spaces();
          if (position_ < text_.size() && text_[position_] == ',') {
            ++position_;
            continue;
          }
          if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
          }
          return fail("',' ou ']' attendu");
        }
      }
      if (character == '"') {
        std::string value;
        if (!parse_string(value))
          return false;
        out = Json(std::move(value));
        return true;
      }
      if (character == 't') {
        out = Json(true);
        return literal("true");
      }
      if (character == 'f') {
        out = Json(false);
        return literal("false");
      }
      if (character == 'n') {
        out = Json();
        return literal("null");
      }
      const char *start = text_.c_str() + position_;
      char       *end   = nullptr;
      const double value = std::strtod(start, &end);
      if (end == start)
        return fail("Valeur invalide");
      position_ += static_cast< std::size_t >(end - start);
      out = Json(value);
      return true;
    }

    const std::string &text_;
    std::size_t        position_ = 0;
    std::string        error_;
};

} // namespace

Json Json::array() {
  Json value;
  value.type_ = Type::Array;
  return value;
}

Json Json::object() {
  Json value;
  value.type_ = Type::Object;
  return value;
}

Json Json::parse(const std::string &text, std::string *error) {
  Json        result;
  std::string message;
  Parser      parser(text);
  if (!parser.parse(result, message)) {
    if (error)
      *error = message;
    return Json();
  }
  if (error)
    error->clear();
  return result;
}

std::string Json::str(const std::string &fallback) const {
  if (type_ == Type::String)
    return string_;
  if (type_ == Type::Number) {
    if (std::floor(number_) == number_ && std::fabs(number_) < 1e15)
      return std::to_string(static_cast< long long >(number_));
    return std::to_string(number_);
  }
  if (type_ == Type::Bool)
    return bool_ ? "true" : "false";
  return fallback;
}

double Json::num(double fallback) const {
  if (type_ == Type::Number)
    return number_;
  if (type_ == Type::String) {
    char        *end   = nullptr;
    const double value = std::strtod(string_.c_str(), &end);
    return end != string_.c_str() ? value : fallback;
  }
  if (type_ == Type::Bool)
    return bool_ ? 1.0 : 0.0;
  return fallback;
}

int Json::integer(int fallback) const {
  return static_cast< int >(std::lround(num(static_cast< double >(fallback))));
}

bool Json::boolean(bool fallback) const {
  if (type_ == Type::Bool)
    return bool_;
  if (type_ == Type::Number)
    return number_ != 0.0;
  return fallback;
}

bool Json::contains(const std::string &key) const {
  for (const Member &member : object_)
    if (member.first == key)
      return true;
  return false;
}

const Json &Json::operator[](const std::string &key) const {
  if (type_ != Type::Object)
    return null_value();
  for (const Member &member : object_)
    if (member.first == key)
      return member.second;
  return null_value();
}

Json &Json::operator[](const std::string &key) {
  if (type_ != Type::Object) {
    *this = object();
  }
  for (Member &member : object_)
    if (member.first == key)
      return member.second;
  object_.emplace_back(key, Json());
  return object_.back().second;
}

std::size_t Json::size() const {
  if (type_ == Type::Array)
    return array_.size();
  if (type_ == Type::Object)
    return object_.size();
  return 0;
}

const Json &Json::operator[](std::size_t index) const {
  if (type_ != Type::Array || index >= array_.size())
    return null_value();
  return array_[index];
}

void Json::push_back(Json value) {
  if (type_ != Type::Array)
    *this = array();
  array_.push_back(std::move(value));
}

std::string json_escape(const std::string &value) {
  std::string output;
  output.reserve(value.size() + 2);
  for (const char character : value) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (static_cast< unsigned char >(character) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast< unsigned char >(character));
          output += buffer;
        } else {
          output += character;
        }
    }
  }
  return output;
}

void Json::dump_to(std::string &output) const {
  switch (type_) {
    case Type::Null: output += "null"; break;
    case Type::Bool: output += bool_ ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(number_)) {
        output += "null";
      } else if (std::floor(number_) == number_ && std::fabs(number_) < 1e15) {
        output += std::to_string(static_cast< long long >(number_));
      } else {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
        output += buffer;
      }
      break;
    }
    case Type::String:
      output += '"';
      output += json_escape(string_);
      output += '"';
      break;
    case Type::Array: {
      output += '[';
      for (std::size_t index = 0; index < array_.size(); ++index) {
        if (index)
          output += ',';
        array_[index].dump_to(output);
      }
      output += ']';
      break;
    }
    case Type::Object: {
      output += '{';
      for (std::size_t index = 0; index < object_.size(); ++index) {
        if (index)
          output += ',';
        output += '"';
        output += json_escape(object_[index].first);
        output += "\":";
        object_[index].second.dump_to(output);
      }
      output += '}';
      break;
    }
  }
}

std::string Json::dump() const {
  std::string output;
  dump_to(output);
  return output;
}

} // namespace qrprotec
