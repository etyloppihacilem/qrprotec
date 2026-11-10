/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          update_check.cpp
        -\-    _|__
         |\___/  . \        Created on 10 Nov. 2026 at 20:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "update_check.hpp"

#include "json.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace qrprotec {

namespace {

std::vector< long long > version_fields(std::string version) {
  if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
    version.erase(0, 1);
  std::vector< long long > fields;
  std::size_t              start = 0;
  for (;;) {
    const std::size_t dot   = version.find('.', start);
    const std::string field = version.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    long long         value = 0;
    for (const char character : field) {
      if (!std::isdigit(static_cast< unsigned char >(character)))
        break;
      value = value * 10 + (character - '0');
      if (value > 1000000000LL)
        break;
    }
    fields.push_back(value);
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  return fields;
}

bool is_hex64(const std::string &value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isdigit(c) || (c >= 'a' && c <= 'f');
         });
}

} // namespace

int compare_versions(const std::string &left, const std::string &right) {
  const std::vector< long long > a = version_fields(left);
  const std::vector< long long > b = version_fields(right);
  for (std::size_t index = 0; index < std::max(a.size(), b.size()); ++index) {
    const long long x = index < a.size() ? a[index] : 0;
    const long long y = index < b.size() ? b[index] : 0;
    if (x != y)
      return x < y ? -1 : 1;
  }
  return 0;
}

bool is_release_version(const std::string &version) {
  if (version.empty() || version.front() == '.' || version.back() == '.'
      || version.find("..") != std::string::npos)
    return false;
  if (!std::all_of(version.begin(), version.end(), [](unsigned char c) { return std::isdigit(c) || c == '.'; }))
    return false;
  return compare_versions(version, "0") > 0;
}

bool parse_latest_release(
  const std::string &json, const std::string &asset_name, ReleaseAsset &asset, std::string &error
) {
  std::string parse_error;
  const Json  release = Json::parse(json, &parse_error);
  if (!release.is_object()) {
    error = "Réponse de GitHub illisible";
    return false;
  }
  if (release["draft"].boolean() || release["prerelease"].boolean()) {
    error = "Dernière version pas encore publiée";
    return false;
  }
  std::string version = release["tag_name"].str();
  if (!version.empty() && version.front() == 'v')
    version.erase(0, 1);
  if (!is_release_version(version)) {
    error = "Tag de la dernière version invalide : " + release["tag_name"].str();
    return false;
  }
  for (const Json &item : release["assets"].items()) {
    if (item["name"].str() != asset_name)
      continue;
    const std::string digest = item["digest"].str();
    const std::string url    = item["browser_download_url"].str();
    if (digest.rfind("sha256:", 0) != 0 || !is_hex64(digest.substr(7))) {
      error = "Empreinte SHA-256 absente pour " + asset_name;
      return false;
    }
    if (url.rfind("https://", 0) != 0) {
      error = "Adresse de téléchargement invalide";
      return false;
    }
    asset.version = version;
    asset.url     = url;
    asset.sha256  = digest.substr(7);
    asset.size    = static_cast< long long >(item["size"].num());
    return true;
  }
  error = "Pas de " + asset_name + " dans la version " + version;
  return false;
}

std::string sha256_hex(const std::string &data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast< const unsigned char * >(data.data()), data.size(), digest);
  std::string hex;
  char        buffer[3];
  for (const unsigned char byte : digest) {
    std::snprintf(buffer, sizeof(buffer), "%02x", byte);
    hex += buffer;
  }
  return hex;
}

} // namespace qrprotec
