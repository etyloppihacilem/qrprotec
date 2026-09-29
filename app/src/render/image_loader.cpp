/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          image_loader.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 15:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "image_loader.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#include <filesystem>
#include <map>
#include <mutex>

namespace qrprotec {

namespace {
struct CacheEntry {
    std::filesystem::file_time_type    modified;
    std::shared_ptr< const GrayImage > image;
};
std::mutex                          cache_mutex;
std::map< std::string, CacheEntry > cache;
} // namespace

std::shared_ptr< const GrayImage > load_gray_image(const std::string &path, std::string &error) {
  std::error_code                       code;
  const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, code);
  if (code) {
    error = "Image introuvable : " + path;
    return nullptr;
  }
  std::lock_guard< std::mutex > lock(cache_mutex);
  const auto                    found = cache.find(path);
  if (found != cache.end() && found->second.modified == modified)
    return found->second.image;

  int            width = 0, height = 0, channels = 0;
  unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, 4);
  if (!data) {
    error = std::string("Image illisible : ") + stbi_failure_reason();
    return nullptr;
  }
  auto image    = std::make_shared< GrayImage >();
  image->width  = width;
  image->height = height;
  image->pixels.resize(static_cast< std::size_t >(width) * static_cast< std::size_t >(height));
  for (std::size_t index = 0; index < image->pixels.size(); ++index) {
    const unsigned char *pixel = data + index * 4;
    const float          luma  = 0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2];
    const float          alpha = pixel[3] / 255.0f;
    image->pixels[index]       = static_cast< std::uint8_t >(luma * alpha + 255.0f * (1.0f - alpha));
  }
  stbi_image_free(data);
  cache[path] = { modified, image };
  return image;
}

} // namespace qrprotec
