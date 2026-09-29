/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          json.hpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qrprotec {

// Valeur JSON minimale pour dialoguer avec l'API (lecture tolerante : un acces a une cle absente
// renvoie une valeur nulle plutot que de lever une exception).
class Json {
  public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair< std::string, Json >;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(long value) : type_(Type::Number), number_(static_cast< double >(value)) {}
    Json(long long value) : type_(Type::Number), number_(static_cast< double >(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char *value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}

    static Json array();
    static Json object();
    static Json parse(const std::string &text, std::string *error = nullptr);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Conversions tolerantes
    std::string str(const std::string &fallback = {}) const;
    double      num(double fallback = 0.0) const;
    int         integer(int fallback = 0) const;
    bool        boolean(bool fallback = false) const;

    // Objets
    bool        contains(const std::string &key) const;
    const Json &operator[](const std::string &key) const;
    Json       &operator[](const std::string &key);
    const Json &operator[](const char *key) const { return (*this)[std::string(key)]; }
    Json       &operator[](const char *key) { return (*this)[std::string(key)]; }
    const std::vector< Member > &members() const { return object_; }

    // Tableaux
    std::size_t                size() const;
    const Json                &operator[](std::size_t index) const;
    const Json                &operator[](int index) const { return (*this)[static_cast< std::size_t >(index)]; }
    void                       push_back(Json value);
    const std::vector< Json > &items() const { return array_; }
    std::vector< Json >       &items() { return array_; }

    std::string dump() const;

  private:
    void dump_to(std::string &output) const;

    Type                  type_   = Type::Null;
    bool                  bool_   = false;
    double                number_ = 0.0;
    std::string           string_;
    std::vector< Json >   array_;
    std::vector< Member > object_;
};

std::string json_escape(const std::string &value);

} // namespace qrprotec
