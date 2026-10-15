// Z3CloudPlay - Direct ps5-homebrew-ui renderer boundary.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace z3cloudplay::homebrew
{

class Simulation;
class ArtworkService;

struct RenderStats
{
    std::uint32_t grid_focused_game_id = 0;
    std::string focused_control_key;
    std::size_t grid_drawn_cell_count = 0;
    std::size_t artwork_texture_count = 0;
    std::size_t live_artwork_texture_count = 0;
    std::size_t live_artwork_gpu_bytes = 0;
    int native_width = 0;
    int native_height = 0;
};

class Render final
{
  public:
    explicit Render(std::filesystem::path assets_directory, ArtworkService* artwork = nullptr);
    ~Render();

    Render(Render &&) noexcept;
    Render &operator=(Render &&) noexcept;
    Render(const Render &) = delete;
    Render &operator=(const Render &) = delete;

    void draw(Simulation &simulation, int width, int height, float dt = 1.0F / 60.0F);
    bool capture_png(const std::filesystem::path &path, int width, int height) const;
    [[nodiscard]] RenderStats diagnostics() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace z3cloudplay::homebrew
