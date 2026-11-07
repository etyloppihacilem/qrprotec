/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          front_config.cpp
        -\-    _|__
         |\___/  . \        Created on 7 Nov. 2026 at 13:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "front_config.hpp"

#include "../core/json.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace qrprotec {

namespace {

// Memes valeurs que database/inventory/front_config.py
constexpr const char *kMagic         = "front-config";
constexpr int         kVersion       = 1;
constexpr const char *kKdf           = "PBKDF2-HMAC-SHA256";
constexpr const char *kCipher        = "AES-256-GCM";
constexpr int         kMinIterations = 100000;
constexpr int         kMaxIterations = 10000000;
constexpr const char *kAad           = "qrprotec-front-config-v1";
constexpr std::size_t kTagSize       = 16;
constexpr std::size_t kNonceSize     = 12;
constexpr std::uintmax_t kMaxFileSize = 1 << 20;

bool base64_decode(const std::string &text, std::string &output) {
  if (text.empty() || text.size() % 4 != 0)
    return false;
  for (const char character : text)
    if (!std::isalnum(static_cast< unsigned char >(character)) && character != '+' && character != '/'
        && character != '=')
      return false;
  std::string decoded(text.size() / 4 * 3, '\0');
  const int   length = EVP_DecodeBlock(reinterpret_cast< unsigned char * >(decoded.data()),
                                       reinterpret_cast< const unsigned char * >(text.data()),
                                       static_cast< int >(text.size()));
  if (length < 0)
    return false;
  std::size_t padding = 0;
  if (text[text.size() - 1] == '=')
    ++padding;
  if (text[text.size() - 2] == '=')
    ++padding;
  decoded.resize(static_cast< std::size_t >(length) - padding);
  output = std::move(decoded);
  return true;
}

struct CipherContextDeleter {
    void operator()(EVP_CIPHER_CTX *context) const { EVP_CIPHER_CTX_free(context); }
};

// AES-256-GCM : `data` = texte chiffre suivi de l'etiquette (convention de cryptography.AESGCM)
bool aes_gcm_decrypt(const unsigned char *key, const std::string &nonce, const std::string &data, std::string &plain) {
  if (data.size() < kTagSize || nonce.size() != kNonceSize)
    return false;
  std::unique_ptr< EVP_CIPHER_CTX, CipherContextDeleter > context(EVP_CIPHER_CTX_new());
  if (!context)
    return false;
  const std::size_t ciphertext_size = data.size() - kTagSize;
  std::string       tag             = data.substr(ciphertext_size);
  std::string       output(ciphertext_size + 16, '\0');
  int               length = 0;
  int               total  = 0;
  const auto *aad = reinterpret_cast< const unsigned char * >(kAad);
  if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
      || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast< int >(nonce.size()), nullptr) != 1
      || EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key,
                            reinterpret_cast< const unsigned char * >(nonce.data())) != 1
      || EVP_DecryptUpdate(context.get(), nullptr, &length, aad, static_cast< int >(std::char_traits< char >::length(kAad))) != 1
      || EVP_DecryptUpdate(context.get(), reinterpret_cast< unsigned char * >(output.data()), &length,
                           reinterpret_cast< const unsigned char * >(data.data()),
                           static_cast< int >(ciphertext_size)) != 1)
    return false;
  total = length;
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast< int >(kTagSize), tag.data()) != 1
      || EVP_DecryptFinal_ex(context.get(), reinterpret_cast< unsigned char * >(output.data()) + total, &length) != 1) {
    OPENSSL_cleanse(output.data(), output.size());
    return false;
  }
  total += length;
  output.resize(static_cast< std::size_t >(total));
  plain = std::move(output);
  return true;
}

} // namespace

bool decrypt_front_config(const std::string &text, const std::string &password, FrontConfig &config,
                          std::string &error) {
  std::string parse_error;
  const Json  document = Json::parse(text, &parse_error);
  if (!parse_error.empty() || !document.is_object() || document["qrprotec"].str() != kMagic) {
    error = "Ce n'est pas un fichier de configuration QRProtec.";
    return false;
  }
  if (document["version"].integer(-1) != kVersion || document["kdf"].str() != kKdf
      || document["cipher"].str() != kCipher) {
    error = "Version du fichier de configuration non prise en charge (mettre QRProtec à jour).";
    return false;
  }
  const double iterations = document["iterations"].num(0.0);
  std::string  salt;
  std::string  nonce;
  std::string  data;
  if (!document["iterations"].is_number() || iterations < kMinIterations || iterations > kMaxIterations
      || !base64_decode(document["salt"].str(), salt) || !base64_decode(document["nonce"].str(), nonce)
      || !base64_decode(document["data"].str(), data) || salt.size() < 8 || nonce.size() != kNonceSize
      || data.size() < kTagSize) {
    error = "Fichier de configuration invalide.";
    return false;
  }
  if (password.empty()) {
    error = "Mot de passe du fichier requis.";
    return false;
  }
  unsigned char key[32];
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast< int >(password.size()),
                        reinterpret_cast< const unsigned char * >(salt.data()), static_cast< int >(salt.size()),
                        static_cast< int >(iterations), EVP_sha256(), sizeof(key), key) != 1) {
    error = "Dérivation de la clé impossible (OpenSSL).";
    return false;
  }
  std::string plain;
  const bool  decrypted = aes_gcm_decrypt(key, nonce, data, plain);
  OPENSSL_cleanse(key, sizeof(key));
  if (!decrypted) {
    error = "Mot de passe incorrect, ou fichier modifié.";
    return false;
  }
  const Json payload = Json::parse(plain, &parse_error);
  OPENSSL_cleanse(plain.data(), plain.size());
  if (!parse_error.empty() || !payload.is_object()) {
    error = "Contenu du fichier de configuration invalide.";
    return false;
  }
  FrontConfig result;
  result.name       = payload["name"].str();
  result.api_url    = payload["api_url"].str();
  result.api_key    = payload["api_key"].str();
  result.api_ca_pem = payload["api_ca_pem"].str();
  result.created    = payload["created"].str();
  if (result.api_url.rfind("https://", 0) != 0 && result.api_url.rfind("http://", 0) != 0) {
    error = "URL du serveur absente du fichier.";
    return false;
  }
  if (!result.api_ca_pem.empty() && result.api_ca_pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
    error = "Certificat de l'autorité invalide dans le fichier.";
    return false;
  }
  config = std::move(result);
  return true;
}

bool read_front_config(const std::filesystem::path &path, const std::string &password, FrontConfig &config,
                       std::string &error) {
  std::error_code     code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  if (code) {
    error = "Fichier introuvable : " + path.string();
    return false;
  }
  if (size > kMaxFileSize) {
    error = "Ce n'est pas un fichier de configuration QRProtec (trop gros).";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Impossible de lire " + path.string();
    return false;
  }
  std::ostringstream text;
  text << input.rdbuf();
  return decrypt_front_config(text.str(), password, config, error);
}

bool write_server_ca(const std::filesystem::path &path, const std::string &pem, std::string &error) {
  std::error_code code;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), code);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << pem;
  if (!output) {
    error = "Impossible d'écrire " + path.string();
    return false;
  }
  return true;
}

} // namespace qrprotec
