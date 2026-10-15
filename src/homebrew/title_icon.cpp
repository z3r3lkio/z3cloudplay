// title_icon.cpp - A game's PS5 title icon from its store cover art.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "homebrew/title_icon.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// A private copy of the PNG writer: the app's renderer has its own.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace z3cloudplay::homebrew {

std::optional<std::string> MakeTitleIcon(const ArtworkImage& cover) {
  if (cover.width == 0 || cover.height == 0 ||
      cover.rgba.size() != static_cast<std::size_t>(cover.width) * cover.height * 4)
    return std::nullopt;
  const std::uint32_t side = std::min(cover.width, cover.height);
  const std::uint32_t left = (cover.width - side) / 2, top = (cover.height - side) / 2;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTitleIconSize) * kTitleIconSize * 3);
  for (std::uint32_t y = 0; y < kTitleIconSize; ++y) {
    const std::uint32_t y0 = top + y * side / kTitleIconSize;
    const std::uint32_t y1 = std::max(y0 + 1, top + (y + 1) * side / kTitleIconSize);
    for (std::uint32_t x = 0; x < kTitleIconSize; ++x) {
      const std::uint32_t x0 = left + x * side / kTitleIconSize;
      const std::uint32_t x1 = std::max(x0 + 1, left + (x + 1) * side / kTitleIconSize);
      std::uint64_t sum[3] = {0, 0, 0};
      for (std::uint32_t sy = y0; sy < y1; ++sy)
        for (std::uint32_t sx = x0; sx < x1; ++sx) {
          const auto* p = &cover.rgba[(static_cast<std::size_t>(sy) * cover.width + sx) * 4];
          // Transparent areas become black, as the console shows icons on dark tiles.
          for (int c = 0; c < 3; ++c) sum[c] += static_cast<std::uint64_t>(p[c]) * p[3] / 255;
        }
      const std::uint64_t count = static_cast<std::uint64_t>(y1 - y0) * (x1 - x0);
      auto* out = &pixels[(static_cast<std::size_t>(y) * kTitleIconSize + x) * 3];
      for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
    }
  }
  std::string png;
  const auto append = [](void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
  };
  if (!stbi_write_png_to_func(append, &png, kTitleIconSize, kTitleIconSize, 3, pixels.data(),
                              static_cast<int>(kTitleIconSize * 3)) || png.empty())
    return std::nullopt;
  return png;
}

}  // namespace z3cloudplay::homebrew
