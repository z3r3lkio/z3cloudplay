#include "homebrew/title_icon.hpp"

#include <cstdio>
#include <cstdlib>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using z3cloudplay::homebrew::ArtworkImage;
using z3cloudplay::homebrew::MakeTitleIcon;

int main() {
  // A 600x900 cover in three bands: red, green, blue. The icon is the centred
  // 600x600 square (rows 150..749), scaled to 512x512.
  ArtworkImage cover;
  cover.width = 600;
  cover.height = 900;
  cover.rgba.resize(600 * 900 * 4);
  for (std::uint32_t y = 0; y < 900; ++y)
    for (std::uint32_t x = 0; x < 600; ++x) {
      auto* p = &cover.rgba[(y * 600 + x) * 4];
      p[0] = y < 300 ? 255 : 0; p[1] = y >= 300 && y < 600 ? 255 : 0; p[2] = y >= 600 ? 255 : 0; p[3] = 255;
    }
  const auto png = MakeTitleIcon(cover);
  CHECK(png && png->size() > 8 && png->compare(1, 3, "PNG") == 0);
  int width = 0, height = 0, components = 0;
  auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png->data()), static_cast<int>(png->size()),
                                       &width, &height, &components, 3);
  CHECK(pixels && width == 512 && height == 512);
  const auto at = [&](int x, int y) { return pixels + (y * 512 + x) * 3; };
  CHECK(at(256, 0)[0] == 255 && at(256, 0)[1] == 0);      // top of the square: red band
  CHECK(at(256, 256)[1] == 255 && at(256, 256)[0] == 0);  // centre: green band
  CHECK(at(256, 511)[2] == 255 && at(256, 511)[1] == 0);  // bottom: blue band
  stbi_image_free(pixels);
  // Transparent pixels become black.
  ArtworkImage clear{4, 4, std::vector<std::uint8_t>(4 * 4 * 4, 0)};
  for (std::size_t i = 0; i < clear.rgba.size(); i += 4) clear.rgba[i] = 255;
  const auto dark = MakeTitleIcon(clear);
  pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(dark->data()), static_cast<int>(dark->size()),
                                 &width, &height, &components, 3);
  CHECK(pixels && pixels[0] == 0 && width == 512);
  stbi_image_free(pixels);
  // Malformed images make no icon.
  CHECK(!MakeTitleIcon(ArtworkImage{}));
  CHECK(!MakeTitleIcon(ArtworkImage{10, 10, std::vector<std::uint8_t>(3)}));
  std::puts("title icon tests passed");
}
