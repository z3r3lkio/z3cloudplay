#include "homebrew/artwork_cache.hpp"
// Z3CloudPlay - Direct ps5-homebrew-ui comparison renderer.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/render.hpp"

#include "homebrew/browse.hpp"
#include "homebrew/focus_motion.hpp"
#include "homebrew/qr.hpp"
#include "homebrew/simulation.hpp"
#include <ctime>

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/gl.hpp"
#include "gfx/renderer.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/list.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/tabs.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace z3cloudplay::homebrew
{
namespace
{
namespace gfx = hui::gfx;
namespace ui = hui::ui;

constexpr float kWidth = 1920.0F;
constexpr float kHeight = 1080.0F;
constexpr float kSafeX = 96.0F;
constexpr float kFooterY = 986.0F;

const gfx::Color kBackground = gfx::Color::rgb(0x0E0D0C);
const gfx::Color kPanel = gfx::Color::rgb(0x16140F);
const gfx::Color kText = gfx::Color::rgb(0xF3EFE9);
const gfx::Color kSecondary = gfx::Color::rgb(0xA8A29E);
const gfx::Color kMuted = gfx::Color::rgb(0x6B655F);
const gfx::Color kAccent = gfx::Color::rgb(0xF0863A);
const gfx::Color kGreen = gfx::Color::rgb(0x7BBF6A);
const gfx::Color kAmber = gfx::Color::rgb(0xE2B53C);
const gfx::Color kRed = gfx::Color::rgb(0xE05A4F);
const gfx::Color kBlue = gfx::Color::rgb(0x7AA7D9);

ui::Theme z3cloudplay_theme()
{
    ui::Theme theme{};
    theme.id = "z3cloudplay";
    theme.name = "Z3CloudPlay";
    theme.family = "Warm console";
    theme.summary = "Warm near-black surfaces with restrained orange focus";
    theme.backdrop.mode = gfx::BackdropMode::gradient;
    theme.backdrop.colors[0] = kBackground;
    theme.backdrop.colors[1] = kBackground;
    theme.page = kBackground;
    theme.page_text = kText;
    theme.page_text_muted = kSecondary;
    theme.surface = kPanel;
    theme.surface_high = gfx::Color::rgb(0x2A2623);
    theme.text = kText;
    theme.text_muted = kSecondary;
    theme.primary = kAccent;
    theme.on_primary = gfx::Color::rgb(0x1A1210);
    theme.secondary = gfx::Color::rgb(0x24211E);
    theme.on_secondary = kText;
    theme.accent = kAccent;
    theme.outline = kText.with_alpha(0.20F);
    theme.focus = kAccent;
    theme.shadow = gfx::Color::rgb(0x000000, 0.55F);
    theme.light = kText.with_alpha(0.12F);
    theme.danger = kRed;
    theme.success = kGreen;
    theme.warning = kAmber;
    theme.style = ui::SurfaceStyle::flat;
    theme.corner = ui::Corner::round;
    theme.radius = 8.0F;
    theme.radius_card = 10.0F;
    theme.border = 2.0F;
    theme.button_border = 2.0F;
    theme.focus_width = 4.0F;
    theme.focus_gap = 5.0F;
    theme.heading = ui::FontRole::display;
    theme.label = ui::FontRole::semibold;
    theme.omega = 22.0F;
    theme.damping = 1.0F;
    theme.dark = true;
    return theme;
}

bool read_file(const std::filesystem::path &path, std::string *out)
{
    std::FILE *file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr)
        return false;

    const auto close_file = [&file]() { std::fclose(file); };
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        close_file();
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || static_cast<unsigned long>(length) > std::numeric_limits<std::size_t>::max())
    {
        close_file();
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0)
    {
        close_file();
        return false;
    }

    std::string bytes(static_cast<std::size_t>(length), '\0');
    const std::size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    const bool ok = read == bytes.size() && std::ferror(file) == 0;
    close_file();
    if (!ok)
        return false;
    *out = std::move(bytes);
    return true;
}

std::filesystem::path first_existing(const std::vector<std::filesystem::path> &paths)
{
    for (const auto &path : paths)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            return path;
    }
    return {};
}

std::string fit(const ui::FontRef &font, std::string_view text, float size, float width)
{
    return font.font->fit(text, size, width);
}

gfx::Color id_color(GameId id)
{
    static constexpr std::array<std::uint32_t, 8> colors = {
        0x634B3B, 0x354F52, 0x53405B, 0x5E4936, 0x334D3E, 0x4D435E, 0x675039, 0x3D4B63};
    return gfx::Color::rgb(colors[id % colors.size()]);
}

const Game *game_by_id(const State &state, GameId id)
{
    const auto found = std::find_if(state.games.begin(), state.games.end(),
                                    [id](const Game &game) { return game.id == id; });
    return found == state.games.end() ? nullptr : &*found;
}

std::string byte_count(std::optional<std::uint64_t> bytes)
{
    if (!bytes)
        return "Unknown";
    constexpr double mib = 1024.0 * 1024.0;
    constexpr double gib = 1024.0 * mib;
    std::array<char, 32> value{};
    // Below a gigabyte, megabytes: "0.2 GB" says little about a small game.
    if (*bytes < static_cast<std::uint64_t>(gib))
    {
        const int written = std::snprintf(value.data(), value.size(), "%.0f MB", static_cast<double>(*bytes) / mib);
        return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : "Unknown";
    }
    const int precision = *bytes >= static_cast<std::uint64_t>(10.0 * gib) ? 0 : 1;
    const int written = std::snprintf(value.data(), value.size(), "%.*f GB", precision,
                                      static_cast<double>(*bytes) / gib);
    return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : "Unknown";
}

// Sizes as macOS shows files (decimal units), matching the model's labels: megabytes below a gigabyte.
std::string short_size(std::uint64_t bytes)
{
    if (bytes == 0) return "0 MB";
    std::array<char, 32> value{};
    const double amount = static_cast<double>(bytes);
    const int written = amount >= 1e9 ? std::snprintf(value.data(), value.size(), "%.1f GB", amount / 1e9)
                                      : std::snprintf(value.data(), value.size(), "%.0f MB", std::max(1.0, amount / 1e6));
    return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : std::string("Unknown");
}

std::string join_words(const std::vector<std::string> &words)
{
    if (words.empty())
        return "Unknown";
    std::string result;
    for (const std::string &word : words)
    {
        if (!result.empty())
            result += " · ";
        result += word;
    }
    return result;
}

std::string playtime_label(std::optional<std::uint64_t> minutes)
{
    if (!minutes)
        return "Unknown";
    if (*minutes < 60)
        return std::to_string(*minutes) + " min";
    return std::to_string(*minutes / 60) + " h " + std::to_string(*minutes % 60) + " min";
}

std::string relative_time(std::optional<std::int64_t> timestamp, std::int64_t now)
{
    if (!timestamp)
        return "Unknown";
    const std::int64_t seconds = std::max<std::int64_t>(0, now - *timestamp);
    if (seconds < 3600)
        return "Less than an hour ago";
    if (seconds < 86400)
        return std::to_string(seconds / 3600) + " h ago";
    if (seconds < 30 * 86400)
        return std::to_string(seconds / 86400) + " d ago";
    // Further back a month and year reads better than a count of days ("Jan 2014").
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // Civil date from days since 1970 (Howard Hinnant's algorithm): the PS5 libc has no gmtime_r.
    const std::int64_t days = (*timestamp >= 0 ? *timestamp : *timestamp - 86399) / 86400 + 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;  // 0 is March
    const std::int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    return std::string(kMonths[month - 1]) + " " + std::to_string(year);
}

std::size_t utf8_boundary(std::string_view text, std::size_t offset)
{
    offset = std::min(offset, text.size());
    while (offset > 0 && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
        --offset;
    return offset;
}

const char *rating_label(Compatibility rating)
{
    switch (rating)
    {
    case Compatibility::Untested:
        return "Untested";
    case Compatibility::Works:
        return "Works · user rating";
    case Compatibility::Playable:
        return "Playable · user rating";
    case Compatibility::Broken:
        return "Broken · user rating";
    }
    return "Untested";
}

const char* job_stage_label(JobStage stage)
{
    switch (stage)
    {
    case JobStage::Download:
        return "Download";
    case JobStage::Verify:
        return "Verification";
    case JobStage::Install:
        return "Install";
    case JobStage::Uninstall:
        return "Removal";
    }
    return "Transfer";
}

const char* job_state_label(JobState state)
{
    switch (state)
    {
    case JobState::Queued:
        return "Queued";
    case JobState::Downloading:
        return "Downloading";
    case JobState::Pausing:
        return "Pausing";
    case JobState::Paused:
        return "Paused";
    case JobState::Verifying:
        return "Verifying";
    case JobState::Downloaded:
        return "Complete";
    case JobState::Failed:
        return "Failed";
    case JobState::Cancelled:
        return "Cancelled";
    case JobState::Interrupted:
        return "Interrupted";
    }
    return "Transfer";
}

ui::Button legend_button(std::string_view legend, std::size_t index)
{
    if (legend.rfind("Cross", 0) == 0)
        return ui::Button::cross;
    if (legend.rfind("Circle", 0) == 0)
        return ui::Button::circle;
    if (legend.rfind("Square", 0) == 0)
        return ui::Button::square;
    if (legend.rfind("Triangle", 0) == 0)
        return ui::Button::triangle;
    if (legend.rfind("Options", 0) == 0)
        return ui::Button::options;
    if (legend.rfind("Touchpad", 0) == 0)
        return ui::Button::touchpad;
    if (legend.find("Favorite") != std::string_view::npos)
        return ui::Button::square;
    if (legend.find("Backspace") != std::string_view::npos)
        return ui::Button::square;
    if (legend.find("Sort") != std::string_view::npos || legend.find("Filter") != std::string_view::npos)
        return ui::Button::triangle;
    if (legend.find("More") != std::string_view::npos || legend.find("Action") != std::string_view::npos)
        return ui::Button::options;
    if (legend.find("Back") != std::string_view::npos || legend.find("Cancel") != std::string_view::npos)
        return ui::Button::circle;
    if (legend.find("Previous") != std::string_view::npos)
        return ui::Button::l1;
    if (legend.find("Next") != std::string_view::npos)
        return ui::Button::r1;
    return index == 0 ? ui::Button::cross : ui::Button::circle;
}

ui::Status progress_status(const ViewRow &row)
{
    if (row.dangerous || row.badge.find("Failed") != std::string::npos)
        return ui::Status::danger;
    if (row.badge.find("Paused") != std::string::npos || row.badge.find("Offline") != std::string::npos)
        return ui::Status::warning;
    if (row.progress >= 1.0F)
        return ui::Status::success;
    return ui::Status::accent;
}

// How the overlay layer (sheets, dialogs, the keyboard) moves between frames. The scrim and
// a sheet's panel animate in only when an overlay opens over the page; going from one overlay
// to another (a sheet to its picker, a dialog back to its sheet) keeps the scrim and the panel
// where they are and only fades the new content in, so the page behind never flashes.
class OverlayTransition
{
public:
    void update(const std::string &key, bool reduced_motion)
    {
        if (key != key_)
        {
            const bool swap = !key_.empty() && !key.empty();
            key_ = key;
            if (key.empty() || reduced_motion)
                enter_ = content_ = 1.0F;
            else if (swap)
            {
                content_ = 0.0F;
                from_width_ = panel_width_;
            }
            else
                enter_ = content_ = 0.0F;
            return;
        }
        enter_ = reduced_motion ? 1.0F : std::min(1.0F, enter_ + kStep / 0.22F);
        content_ = reduced_motion ? 1.0F : std::min(1.0F, content_ + kStep / 0.16F);
    }
    // Scrim and panel entry, eased: 0 closed, 1 fully in.
    float enter() const { return 1.0F - std::pow(1.0F - enter_, 3.0F); }
    // The current overlay's content: fades in after a swap.
    float content() const { return 1.0F - std::pow(1.0F - content_, 3.0F); }
    // A sheet panel's width, easing from the previous sheet's width after a swap.
    float panel_width(float target)
    {
        if (from_width_ <= 0.0F) from_width_ = target;
        panel_width_ = from_width_ + (target - from_width_) * content();
        if (content_ >= 1.0F) from_width_ = target;
        return panel_width_;
    }

private:
    static constexpr float kStep = 1.0F / 60.0F;
    std::string key_;
    float enter_ = 1.0F;
    float content_ = 1.0F;
    float from_width_ = 0.0F;
    float panel_width_ = 0.0F;
};

} // namespace

class Render::Impl
{
  public:
    explicit Impl(std::filesystem::path assets_directory, ArtworkService* artwork)
        : assets_(std::move(assets_directory)), theme_(z3cloudplay_theme()), texture_backend_(renderer_)
    {
        initialise(artwork == nullptr);
        if (artwork) live_artwork_ = std::make_unique<LiveArtworkCache>(*artwork, texture_backend_);
    }

    ~Impl()
    {
        live_artwork_.reset();
        for (const std::uint32_t texture : art_)
        {
            if (texture != 0)
                glDeleteTextures(1, &texture);
        }
        if (hero_ != 0)
            glDeleteTextures(1, &hero_);
        if (capture_color_ != 0)
            glDeleteRenderbuffers(1, &capture_color_);
        if (capture_framebuffer_ != 0)
            glDeleteFramebuffers(1, &capture_framebuffer_);
    }

    void draw(Simulation &simulation, int width, int height, float dt)
    {
        if (!ready_ || width <= 0 || height <= 0)
            return;

        scene_.clear();
        overlay_.clear();
        stats_.grid_focused_game_id = 0;
        stats_.grid_drawn_cell_count = 0;
        stats_.focused_control_key.clear();
        stats_.native_width = width;
        stats_.native_height = height;
        const State &state = simulation.state();
        if (live_artwork_) live_artwork_->BeginFrame(state.simulation_mode ? 0 : state.account_generation);
        update_qr_cache(state);
        focus_motion_.begin_frame(dt, state.reduced_motion);
        frame_dt_ = std::isfinite(dt) ? std::clamp(dt, 0.0F, 0.1F) : 0.0F;
        const ViewData &view = simulation.view();
        const ViewData &route_view = simulation.route_view();
        const std::string overlay_key = state.editor.active ? "keyboard" :
            state.navigation.modals.empty() ? std::string{} : state.navigation.modals.back().id;
        overlay_transition_.update(overlay_key, state.reduced_motion);
        route_focus_ = !state.editor.active && state.navigation.modals.empty() && !state.navigation.tabs_focused;
        current_state_ = &state;
        if (state.simulation_mode) clock_ = static_cast<float>(state.clock_ticks) / 60.0F;
        else {
            live_animation_clock_ = std::fmod(live_animation_clock_ +
                std::clamp(std::isfinite(dt) ? dt : 0.0F, 0.0F, 0.25F), 3600.0F);
            clock_ = live_animation_clock_;
        }
        ui::Canvas scene{scene_, fonts_, renderer_.glass_texture(), clock_};

        draw_background();
        const bool root = state.navigation.route == Route::Home || state.navigation.route == Route::Library ||
                          state.navigation.route == Route::Downloads || state.navigation.route == Route::Saves ||
                          state.navigation.route == Route::Settings;
        if (root)
            draw_top_navigation(scene, state.navigation.route, state.reduced_motion);

        switch (state.navigation.route)
        {
        case Route::Startup:
            draw_startup(scene, route_view, state);
            break;
        case Route::SignIn:
            draw_sign_in(scene, route_view, state);
            break;
        case Route::Home:
            draw_home(scene, simulation);
            break;
        case Route::Library:
            draw_library(scene, simulation);
            break;
        case Route::Downloads:
            draw_downloads(scene, route_view, state);
            break;
        case Route::Settings:
            draw_settings(scene, route_view, state);
            break;
        case Route::Saves:
            draw_saves(route_view, state);
            break;
        case Route::SaveDetails:
            draw_save_details(route_view, state);
            break;
        case Route::Details:
            draw_details(scene, route_view, state);
            break;
        case Route::Session:
            draw_session(scene, route_view, state);
            break;
        }

        draw_persistent_status(scene, state);
        draw_footer(scene, view);
        draw_overlays(state, view);
        focus_motion_.end_frame();
        if (live_artwork_) {
            live_artwork_->EndFrame();
            const auto artwork_stats = live_artwork_->Stats();
            stats_.live_artwork_texture_count = artwork_stats.texture_count;
            stats_.live_artwork_gpu_bytes = artwork_stats.gpu_bytes;
        }

        renderer_.begin();
        gfx::BackdropSpec backdrop = theme_.backdrop;
        backdrop.time = clock_;
        renderer_.backdrop(backdrop);
        renderer_.draw(scene_);
        if (!overlay_.empty())
        {
            renderer_.glass();
            renderer_.draw(overlay_);
        }
        renderer_.present(0, width, height);
        last_width_ = width;
        last_height_ = height;
        has_frame_ = true;
    }

    [[nodiscard]] RenderStats diagnostics() const noexcept
    {
        return stats_;
    }

    bool capture_png(const std::filesystem::path &path, int width, int height) const
    {
        if (!ready_ || !has_frame_ || width <= 0 || height <= 0 || !ensure_capture(width, height))
            return false;

        glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer_);
        glViewport(0, 0, width, height);
        glClearColor(kBackground.r, kBackground.g, kBackground.b, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer_.present(capture_framebuffer_, width, height);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t index = 3; index < pixels.size(); index += 4)
            pixels[index] = 255;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) != 0;
    }

  private:
    bool initialise(bool fixture_artwork)
    {
        if (!renderer_.init())
            return false;
        if (!load_fonts())
            return false;
        if (fixture_artwork) load_art();
        stats_.artwork_texture_count = static_cast<std::size_t>(std::count_if(
            art_.begin(), art_.end(), [](std::uint32_t texture) { return texture != 0; })) +
            (hero_ == 0 ? 0U : 1U);
        configure_components();
        ready_ = true;
        return true;
    }

    std::filesystem::path asset(std::string_view relative) const
    {
        const std::filesystem::path direct = assets_ / relative;
        const std::filesystem::path nested = assets_ / "assets" / relative;
        const std::filesystem::path comparison = assets_ / "ui-comparison" / "assets" / relative;
        const std::filesystem::path fixture = assets_ / "fixtures" / "ui-comparison" / "assets" / relative;
        return first_existing({direct, nested, comparison, fixture});
    }

    bool load_face(gfx::Font *face, ui::FontRef *ref,
                   const std::vector<std::string> &names)
    {
        std::vector<std::filesystem::path> candidates;
        for (const std::string &name : names)
        {
            candidates.push_back(asset("fonts/" + name));
            candidates.push_back(assets_ / "fonts" / name);
        }
        const auto path = first_existing(candidates);
        std::string bytes;
        if (path.empty() || !read_file(path, &bytes) || !face->load(bytes))
            return false;
        ref->font = face;
        ref->texture = renderer_.batch().create_font_texture(*face);
        return ref->texture != 0;
    }

    bool load_fonts()
    {
        const bool regular = load_face(&regular_, &fonts_.regular,
                                       {"barlow-regular.huifont", "inter-regular.huifont"});
        const bool semibold = load_face(&semibold_, &fonts_.semibold,
                                        {"barlow-semibold.huifont", "barlow-medium.huifont",
                                         "inter-semibold.huifont"});
        const bool display = load_face(&display_, &fonts_.display,
                                       {"barlow-condensed-semibold.huifont",
                                        "barlow-condensed-medium.huifont", "montserrat-medium.huifont"});
        if (!regular || !semibold || !display)
            return false;
        fonts_.mono = fonts_.regular;
        fonts_.pixel = fonts_.semibold;
        fonts_.hand = fonts_.regular;
        if (!load_face(&greek_face_, &greek_, {"noto-greek-fallback.huifont"}))
            return false;
        load_face(&cjk_face_, &cjk_, {"noto-cjk-fallback.huifont"});
        load_face(&arabic_face_, &arabic_, {"noto-arabic-fallback.huifont"});
        return true;
    }

    const ui::FontRef &font_for(std::string_view value, const ui::FontRef &preferred) const
    {
        std::size_t index = 0;
        while (index < value.size())
        {
            const std::uint32_t codepoint = gfx::next_codepoint(value, &index);
            const bool greek = (codepoint >= 0x0370 && codepoint <= 0x03FF) ||
                               (codepoint >= 0x1F00 && codepoint <= 0x1FFF);
            if (greek && greek_.font != nullptr && greek_.font->has_glyph(codepoint))
                return greek_;
            const bool arabic = codepoint >= 0x0600 && codepoint <= 0x06FF;
            if (arabic && arabic_.font != nullptr)
                return arabic_;
            if (!preferred.font->has_glyph(codepoint) && cjk_.font != nullptr &&
                cjk_.font->has_glyph(codepoint))
                return cjk_;
        }
        return preferred;
    }

    void dynamic_text(std::string_view value, float x, float y, float size, gfx::Color color,
                      float max_width, bool display = true,
                      gfx::Align align = gfx::Align::left)
    {
        const ui::FontRef &preferred = display ? fonts_.display : fonts_.regular;
        const ui::FontRef &face = font_for(value, preferred);
        ui::text(scene_, face, face.font->fit(value, size, max_width), x, y, size, color, align);
    }

    void dynamic_paragraph(std::string_view value, float x, float y, float size, float width,
                           float line_height, gfx::Color color, int lines,
                           gfx::Align align = gfx::Align::left)
    {
        const ui::FontRef &face = font_for(value, fonts_.regular);
        ui::paragraph(scene_, face, value, x, y, size, width, line_height, color, lines, align);
    }

    std::uint32_t load_texture(const std::filesystem::path &path)
    {
        if (path.empty())
            return 0;
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr)
            return 0;
        const std::uint32_t texture = renderer_.batch().create_texture(width, height, pixels);
        stbi_image_free(pixels);
        return texture;
    }

    void load_art()
    {
        for (std::size_t index = 0; index < art_.size(); ++index)
            art_[index] = load_texture(asset("art/poster" + std::to_string(index + 1) + ".png"));
        hero_ = load_texture(asset("art/hero.png"));
    }

    void configure_components()
    {
        tabs_.style.theme = theme_;
        tabs_.style.theme.primary = kAccent;
        tabs_.style.theme.on_primary = theme_.on_primary;
        tabs_.style.theme.label = ui::FontRole::display;
        tabs_.style.kind = ui::TabKind::boxed;
        tabs_.style.height = 56.0F;
        tabs_.style.gap = 14.0F;
        tabs_.style.padding = 20.0F;
        tabs_.style.text_size = 29.0F;
        tabs_.style.track = false;
        tabs_.style.focus_ring = false;
        tabs_.style.on_page = true;
        tabs_.set_tabs({{"Home"}, {"Library"}, {"Downloads", 1}, {"Settings"}});
        tabs_.set_bounds({96.0F, 54.0F, 760.0F, 56.0F});

        grid_.style.theme = theme_;
        grid_.style.columns = 6;
        grid_.style.cell_width = 210.0F;
        grid_.style.cell_height = 315.0F;
        grid_.style.gap_x = 24.0F;
        grid_.style.gap_y = 24.0F;
        grid_.style.padding = 27.0F;
        grid_.style.scroll_thumb = false;
        grid_.style.edge_fade = 0.0F;
        grid_.style.entrance_step = 0.0F;
        grid_.style.card.text = ui::CardText::none;
        // Custom poster content owns the per-key spring so fast retargets retain
        // momentum instead of snapping the grid highlight on each model draw.
        grid_.style.card.focus_scale = 1.0F;
        grid_.style.card.ring = false;
        grid_.style.card.glow = false;
        grid_.style.card.lift = 0.0F;
        grid_.style.card.radius = 8.0F;
        // The third row continues beneath the y=986 legend, matching the pinned
        // reference rather than reserving a separate blank footer strip.
        grid_.set_bounds({417.0F, 123.0F, 1434.0F, 903.0F});

        list_.style.theme = theme_;
        list_.style.panel = false;
        list_.style.cards = true;
        list_.style.row_height = 90.0F;
        list_.style.header_height = 48.0F;
        list_.style.gap = 10.0F;
        list_.style.title_size = 28.0F;
        list_.style.subtitle_size = 20.0F;
        list_.style.value_size = 23.0F;
        list_.style.entrance_step = 0.0F;
        list_.style.highlight.kind = ui::HighlightKind::ring;

        keyboard_.style.theme = theme_;
        keyboard_.style.reduced_motion = false;
        keyboard_.style.key_width = 76.0F;
        keyboard_.style.key_height = 66.0F;
        keyboard_.style.gap = 10.0F;
        keyboard_.style.label_size = 28.0F;
        keyboard_.style.panel = true;
        keyboard_.set_bounds({500.0F, 466.0F, 920.0F, 440.0F});
    }

    void draw_background()
    {
        scene_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, kBackground);
    }

    // Tab icons drawn from primitives, after the SF Symbols the macOS tabs use.
    void tab_icon(int tab, float x, float cy, gfx::Color ink)
    {
        constexpr float w = 1.8F;
        switch (tab)
        {
        case 0: // house
            scene_.line(x + 1.0F, cy - 1.0F, x + 13.0F, cy - 11.0F, w, ink);
            scene_.line(x + 13.0F, cy - 11.0F, x + 25.0F, cy - 1.0F, w, ink);
            scene_.line(x + 5.0F, cy - 4.0F, x + 5.0F, cy + 11.0F, w, ink);
            scene_.line(x + 21.0F, cy - 4.0F, x + 21.0F, cy + 11.0F, w, ink);
            scene_.line(x + 5.0F, cy + 11.0F, x + 21.0F, cy + 11.0F, w, ink);
            scene_.line(x + 10.5F, cy + 11.0F, x + 10.5F, cy + 4.0F, w, ink);
            scene_.line(x + 15.5F, cy + 11.0F, x + 15.5F, cy + 4.0F, w, ink);
            scene_.line(x + 10.5F, cy + 4.0F, x + 15.5F, cy + 4.0F, w, ink);
            break;
        case 1: // grid
            for (int index = 0; index < 4; ++index)
                scene_.bordered_rect({x + 2.0F + (index % 2) * 13.0F, cy - 11.0F + (index / 2) * 13.0F, 10.0F, 10.0F},
                                     2.5F, ink.with_alpha(0.0F), w, ink);
            break;
        case 2: // arrow down to a line
            scene_.line(x + 13.0F, cy - 11.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 6.0F, cy - 2.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 20.0F, cy - 2.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 3.0F, cy + 11.0F, x + 23.0F, cy + 11.0F, w, ink);
            break;
        case 3: // cloud
            scene_.line(x + 4.0F, cy + 9.0F, x + 22.0F, cy + 9.0F, w, ink);
            scene_.ring(x + 6.5F, cy + 3.0F, 6.0F, w, ink);
            scene_.ring(x + 13.0F, cy - 3.0F, 8.0F, w, ink);
            scene_.ring(x + 20.0F, cy + 3.0F, 6.0F, w, ink);
            scene_.rounded_rect({x + 3.0F, cy + 3.5F, 20.0F, 4.5F}, 0.0F, kBackground);
            scene_.line(x + 4.0F, cy + 9.0F, x + 22.0F, cy + 9.0F, w, ink);
            break;
        default: // gear
            for (int tooth = 0; tooth < 8; ++tooth)
            {
                const float angle = static_cast<float>(tooth) * 0.785398F;
                scene_.line(x + 13.0F + std::sin(angle) * 8.0F, cy - std::cos(angle) * 8.0F,
                            x + 13.0F + std::sin(angle) * 12.0F, cy - std::cos(angle) * 12.0F, 3.4F, ink);
            }
            scene_.ring(x + 13.0F, cy, 8.5F, w, ink);
            scene_.ring(x + 13.0F, cy, 3.5F, w, ink);
            break;
        }
    }

    // The macOS top bar: icon and label per tab; the current one sits in a quiet box.
    void draw_top_navigation(ui::Canvas &, Route route, bool)
    {
        int selected = 0;
        if (route == Route::Library)
            selected = 1;
        else if (route == Route::Downloads)
            selected = 2;
        else if (route == Route::Saves)
            selected = 3;
        else if (route == Route::Settings)
            selected = 4;
        std::size_t pending = 0;
        if (current_state_->simulation_mode)
            pending = 1;
        else
            pending = static_cast<std::size_t>(std::count_if(current_state_->jobs.begin(), current_state_->jobs.end(),
                [](const Job &job) {
                    return job.state != JobState::Cancelled && job.state != JobState::Downloaded && job.stage != JobStage::Uninstall;
                }));
        // Saves: the games that need the player, or a spinner while a check or Sync all runs.
        std::size_t needs_you = 0;
        for (const auto &[id, cloud] : current_state_->cloud)
            if (!cloud.queued && (cloud.status == CloudStatus::Conflict || cloud.status == CloudStatus::NeedsAccount ||
                                  cloud.status == CloudStatus::Failed))
                ++needs_you;
        const bool saves_busy = current_state_->saves_job.kind != SavesJobKind::None;
        static constexpr std::array<const char *, 5> names = {"Home", "Library", "Downloads", "Saves", "Settings"};
        float x = kSafeX;
        constexpr float cy = 82.0F;
        for (int tab = 0; tab < 5; ++tab)
        {
            const bool active = tab == selected;
            const float label = fonts_.display.measure(names[static_cast<std::size_t>(tab)], 30.0F);
            const std::string count = tab == 2 && pending > 0 ? std::to_string(pending)
                                    : tab == 3 && !saves_busy && needs_you > 0 ? std::to_string(needs_you) : std::string{};
            const float badge = tab == 3 && saves_busy ? 34.0F
                              : count.empty() ? 0.0F : fonts_.semibold.measure(count, 16.0F) + 26.0F;
            const float width = 22.0F + 26.0F + 10.0F + label + badge + 22.0F;
            const gfx::Rect box{x, cy - 26.0F, width, 52.0F};
            // With the bar focused (Up from the top of a page) the focused tab carries the focus ring.
            const bool tab_focused = current_state_->navigation.tabs_focused &&
                                     current_state_->navigation.modals.empty() && current_state_->navigation.tab_focus == tab;
            const float amount = control_focus("tab-" + std::to_string(tab), tab_focused);
            focus_glow(scene_, box, amount);
            if (active)
                scene_.bordered_rect(box, 8.0F, kText.with_alpha(0.12F), 2.0F, kText.with_alpha(0.35F));
            focus_ring(scene_, box, amount);
            if (tab_focused)
                stats_.focused_control_key = "tab-" + std::to_string(tab);
            const gfx::Color ink = active ? kText : kSecondary;
            tab_icon(tab, x + 22.0F, cy, ink);
            ui::text(scene_, fonts_.display, names[static_cast<std::size_t>(tab)], x + 58.0F, cy + 11.0F, 30.0F, ink);
            if (tab == 3 && saves_busy)
                spinner(x + 58.0F + label + 22.0F, cy, 10.0F, kBlue);
            if (!count.empty())
            {
                const float badge_x = x + 58.0F + label + 10.0F;
                scene_.rounded_rect({badge_x, cy - 14.0F, badge - 10.0F, 28.0F}, 14.0F, kAccent);
                ui::text(scene_, fonts_.semibold, count, badge_x + (badge - 10.0F) * 0.5F, cy + 6.0F, 16.0F,
                         kBackground, gfx::Align::center);
            }
            x += width + 8.0F;
        }

        std::string clock_label = "21:42";
        if (!current_state_->simulation_mode) {
            const std::time_t wall_time = static_cast<std::time_t>(current_state_->clock_ticks);
            char formatted[6]{};
            const auto* local = std::localtime(&wall_time);
            clock_label = local != nullptr && std::strftime(formatted, sizeof(formatted), "%H:%M", local)
                ? formatted : "--:--";
        }
        ui::text(scene_, fonts_.regular, clock_label, 1824.0F, 91.0F, 22.0F, kSecondary,
                 gfx::Align::right);
    }

    void section_label(std::string_view value, float x, float y)
    {
        ui::text(scene_, fonts_.display, ui::upper(value), x, y, 22.0F, kSecondary,
                 gfx::Align::left, 2.6F);
    }

    void draw_title(const ViewData &view, float x = kSafeX, float y = 212.0F, float width = 760.0F)
    {
        ui::text(scene_, fonts_.display, fit(fonts_.display, view.title, 68.0F, width), x, y, 68.0F,
                 kText);
        if (!view.subtitle.empty())
            ui::paragraph(scene_, fonts_.regular, view.subtitle, x, y + 58.0F, 27.0F, width, 37.0F,
                          kSecondary, 4);
    }

    // The macOS focus treatment: an accent glow spilling out of the control (drawn before it,
    // so the control covers the glow's middle) and a ring just outside it (drawn after).
    void focus_glow(gfx::DrawList &list, const gfx::Rect &rect, float amount, bool compact = false)
    {
        if (amount <= 0.0F)
            return;
        list.glow(rect.inset(compact ? -5.0F : -7.0F), compact ? 12.0F : 13.0F, 22.0F,
                  kAccent.with_alpha(0.5F * amount));
    }

    void focus_ring(gfx::DrawList &list, const gfx::Rect &rect, float amount, bool compact = false)
    {
        if (amount <= 0.0F)
            return;
        list.bordered_rect(rect.inset(compact ? -6.5F : -9.0F), compact ? 13.5F : 15.0F,
                           kBackground.with_alpha(0.0F), compact ? 3.0F : 4.0F, kAccent.with_alpha(amount));
    }

    // Focus for a pill (a filter chip): the glow and ring follow its round ends.
    void pill_focus_glow(const gfx::Rect &rect, float amount)
    {
        if (amount > 0.0F)
            scene_.glow(rect.inset(-5.0F), rect.h * 0.5F + 5.0F, 22.0F, kAccent.with_alpha(0.5F * amount));
    }
    void pill_focus_ring(const gfx::Rect &rect, float amount)
    {
        if (amount > 0.0F)
            scene_.bordered_rect(rect.inset(-6.5F), rect.h * 0.5F + 6.5F, kBackground.with_alpha(0.0F), 3.0F,
                                 kAccent.with_alpha(amount));
    }

    // Two arrows chasing round a circle: sync.
    void sync_icon(float cx, float cy, float radius, float width, gfx::Color ink)
    {
        constexpr int kSteps = 10;
        for (const float start : {0.35F, 3.49F})  // two arcs of 140°, leaving gaps at the ends
        {
            const float sweep = 2.44F;
            float px = cx + std::cos(start) * radius, py = cy + std::sin(start) * radius;
            for (int step = 1; step <= kSteps; ++step)
            {
                const float angle = start + sweep * static_cast<float>(step) / kSteps;
                const float x = cx + std::cos(angle) * radius, y = cy + std::sin(angle) * radius;
                scene_.line(px, py, x, y, width, ink);
                px = x;
                py = y;
            }
            // The arrowhead at the arc's end, pointing along it.
            const float end = start + sweep;
            const float tx = -std::sin(end), ty = std::cos(end);  // direction of travel
            const float nx = std::cos(end), ny = std::sin(end);   // outward
            const float head = radius * 0.55F;
            scene_.line(px, py, px - tx * head + nx * head * 0.7F, py - ty * head + ny * head * 0.7F, width, ink);
            scene_.line(px, py, px - tx * head - nx * head * 0.7F, py - ty * head - ny * head * 0.7F, width, ink);
        }
    }

    // The macOS `ActionButton`: primary is filled with the accent, the others are outlined;
    // focus adds the ring and glow and lifts the button slightly.
    void mac_button(gfx::DrawList &list, const gfx::Rect &rect, std::string_view label, bool primary,
                    float amount, bool disabled = false, std::string_view detail = {}, bool large = false,
                    gfx::Color backdrop = kBackground)
    {
        list.push_transform(1.0F + 0.04F * amount, rect.cx(), rect.cy(), 0.0F, 0.0F);
        if (disabled)
            list.push_opacity(0.45F);
        focus_glow(list, rect, amount);
        if (primary)
            list.rounded_rect(rect, 8.0F, kAccent);
        else
            list.bordered_rect(rect, 8.0F, amount > 0.0F ? backdrop : backdrop.with_alpha(0.0F), 2.0F,
                               kText.with_alpha(0.2F));
        const float size = large ? 38.0F : 26.0F;
        const gfx::Color ink = primary ? theme_.on_primary : kText;
        const float detail_width = detail.empty() ? 0.0F : fonts_.semibold.measure(detail, 22.0F) + 18.0F;
        const float label_width = fonts_.display.measure(label, size);
        const float content = std::min(rect.w - 32.0F, label_width + detail_width);
        const float x = rect.cx() - content * 0.5F;
        const float baseline = rect.cy() + size * 0.36F;
        ui::text(list, fonts_.display, fit(fonts_.display, label, size, rect.w - 32.0F - detail_width), x,
                 baseline, size, ink);
        if (!detail.empty())
            ui::text(list, fonts_.semibold, detail, x + content, rect.cy() + 8.0F, 22.0F, ink.with_alpha(0.8F),
                     gfx::Align::right);
        focus_ring(list, rect, amount);
        if (disabled)
            list.pop_opacity();
        list.pop_transform();
    }

    float control_focus(std::string_view key, bool focused)
    {
        return focus_motion_.amount(std::string("control-") + std::string(key), focused);
    }

    void draw_action(const ViewRow &row, const gfx::Rect &rect, bool focused, bool primary = false)
    {
        if (focused && route_focus_)
            stats_.focused_control_key = row.key;
        const float amount = control_focus(row.key, focused && route_focus_);
        const auto title = row.kind == RowKind::Toggle ? row.title + (row.selected ? " · On" : " · Off") : row.title;
        mac_button(scene_, rect, title, primary && row.enabled, amount, !row.enabled, row.value, primary && rect.h >= 80.0F);
    }

    // Returns the y just below the last action drawn.
    float draw_actions(const ViewData &view, float x, float y, float width, bool large_first = true)
    {
        std::vector<const ViewRow *> actions;
        if (!view.actions.empty())
        {
            for (const ViewRow &row : view.actions)
                actions.push_back(&row);
        }
        else
        {
            for (const ViewRow &row : view.rows)
            {
                if (row.kind == RowKind::Action || row.kind == RowKind::Toggle)
                    actions.push_back(&row);
            }
        }
        const int focused = view.actions.empty() ? view.focused_row : view.focused_action;
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const bool details = current_state_->navigation.route == Route::Details;
            const float height = large_first && index == 0 ? (details ? 72.0F : 84.0F) : (details ? 54.0F : 64.0F);
            const bool is_focused = view.actions.empty() ?
                (focused >= 0 && static_cast<std::size_t>(focused) < view.rows.size() &&
                 view.rows[static_cast<std::size_t>(focused)].key == actions[index]->key) :
                static_cast<int>(index) == focused;
            draw_action(*actions[index], {x, y, width, height},
                        is_focused, index == 0);
            y += height + (details ? 12.0F : 16.0F);
        }
        return y;
    }

    void draw_startup(ui::Canvas &, const ViewData &view, const State &state)
    {
        section_label("Z3CloudPlay", 96.0F, 78.0F);
        draw_title(view, 96.0F, 292.0F, 720.0F);
        if (state.startup_loading)
        {
            const gfx::Rect loading{96.0F, 520.0F, 560.0F, 112.0F};
            scene_.rounded_rect(loading, 10.0F, kPanel);
            scene_.circle(130.0F, 558.0F, 7.0F, kAccent);
            ui::text(scene_, fonts_.semibold,
                     state.simulation_mode ? "Loading saved fixture state…" :
                                             "Restoring your Steam session…",
                     154.0F, 566.0F,
                     24.0F, kText);
            scene_.rounded_rect({128.0F, 594.0F, 496.0F, 6.0F}, 3.0F,
                                kText.with_alpha(0.14F));
            const float pulse = 0.18F + 0.22F * (0.5F + 0.5F * std::sin(clock_ * 5.0F));
            scene_.rounded_rect({128.0F, 594.0F, 496.0F * pulse, 6.0F}, 3.0F, kAccent);
        }
        else
            draw_actions(view, 96.0F, 520.0F, 560.0F);

        scene_.rounded_rect({988.0F, 194.0F, 724.0F, 680.0F}, 14.0F, kPanel);
        scene_.glow({1050.0F, 268.0F, 600.0F, 360.0F}, 12.0F, 42.0F, kAccent.with_alpha(0.08F));
        if (hero_ != 0)
            scene_.image_gradient(hero_, {1050.0F, 268.0F, 600.0F, 360.0F}, gfx::kFullUv,
                                  kText.with_alpha(0.75F), kBackground.with_alpha(0.90F), 12.0F);
        else
            scene_.gradient_rect({1050.0F, 268.0F, 600.0F, 360.0F}, 12.0F,
                                 kAccent.with_alpha(0.25F), kPanel);
        ui::text(scene_, fonts_.display, "YOUR GAMES", 1100.0F, 704.0F, 48.0F, kText);
        ui::text(scene_, fonts_.regular,
                 state.startup_loading ?
                     (state.simulation_mode ? "Restoring local fixture…" : "Connecting securely…") :
                 state.persistence_available ? "Ready when you are." : "Browsing can continue without saving.",
                 1100.0F, 752.0F, 25.0F,
                 state.persistence_available || state.startup_loading ? kSecondary : kAmber);
    }

    void clear_qr_cache()
    {
        std::fill(cached_qr_url_.begin(), cached_qr_url_.end(), '\0');
        std::string{}.swap(cached_qr_url_);
        cached_qr_ = {};
        cached_qr_error_.clear();
    }

    void update_qr_cache(const State &state)
    {
        const bool usable = state.account.authentication == AuthState::Waiting &&
                            !state.account.challenge_url.empty();
        if (!usable) {
            clear_qr_cache();
            return;
        }
        if (cached_qr_url_ == state.account.challenge_url)
            return;
        clear_qr_cache();
        cached_qr_url_ = state.account.challenge_url;
        cached_qr_ = encode_qr(cached_qr_url_, &cached_qr_error_);
    }

    void draw_simulated_qr(float x, float y, float side)
    {
        scene_.rounded_rect({x, y, side, side}, 16.0F, kText);
        constexpr int cells = 21;
        const float cell = (side - 48.0F) / static_cast<float>(cells);
        for (int row = 0; row < cells; ++row)
        {
            for (int column = 0; column < cells; ++column)
            {
                const bool finder = ((row < 7 || row >= cells - 7) && column < 7) ||
                                    (row < 7 && column >= cells - 7);
                const bool bit = finder ? (row % 6 != 1 && column % 6 != 1) :
                                          ((row * 17 + column * 31 + row * column) % 7 < 3);
                if (bit)
                    scene_.rounded_rect({x + 24.0F + column * cell, y + 24.0F + row * cell,
                                         std::ceil(cell), std::ceil(cell)},
                                        0.0F, kBackground);
            }
        }
    }

    bool draw_encoded_qr(float x, float y, int available_side)
    {
        const QrLayout layout = qr_layout(cached_qr_, available_side);
        if (layout.module_pixels <= 0)
            return false;
        const float x0 = x + static_cast<float>(layout.offset_pixels);
        const float y0 = y + static_cast<float>(layout.offset_pixels);
        scene_.rounded_rect({x0, y0, static_cast<float>(layout.side_pixels),
                             static_cast<float>(layout.side_pixels)},
                            0.0F, kText);
        for (int row = 0; row < cached_qr_.size(); ++row) {
            for (int column = 0; column < cached_qr_.size();) {
                if (!cached_qr_.at(column, row)) {
                    ++column;
                    continue;
                }
                int end = column + 1;
                while (end < cached_qr_.size() && cached_qr_.at(end, row))
                    ++end;
                scene_.rounded_rect(
                    {x0 + static_cast<float>((kQrQuietZoneModules + column) * layout.module_pixels),
                     y0 + static_cast<float>((kQrQuietZoneModules + row) * layout.module_pixels),
                     static_cast<float>((end - column) * layout.module_pixels),
                     static_cast<float>(layout.module_pixels)},
                    0.0F, kBackground);
                column = end;
            }
        }
        return true;
    }

    void draw_qr_placeholder(float x, float y, float side, std::string_view title,
                             std::string_view detail)
    {
        scene_.rounded_rect({x, y, side, side}, 0.0F, kText);
        ui::text(scene_, fonts_.display, title, x + side * 0.5F, y + side * 0.48F,
                 30.0F, kBackground, gfx::Align::center);
        ui::text(scene_, fonts_.regular, detail, x + side * 0.5F, y + side * 0.57F,
                 20.0F, kBackground.with_alpha(0.78F), gfx::Align::center);
    }

    void draw_sign_in(ui::Canvas &, const ViewData &view, const State &state)
    {
        section_label("Connect Steam", 96.0F, 78.0F);
        draw_title(view, 96.0F, 272.0F, 720.0F);
        draw_actions(view, 96.0F, 594.0F, 540.0F);
        constexpr float qr_x = 1116.0F;
        constexpr float qr_y = 224.0F;
        constexpr int qr_side = 430;
        if (state.simulation_mode && state.account.challenge_url.empty()) {
            draw_simulated_qr(qr_x, qr_y, static_cast<float>(qr_side));
        } else if (state.account.authentication == AuthState::Waiting &&
                   state.account.challenge_url.empty()) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Preparing QR code", "Waiting for a secure challenge…");
        } else if (state.account.authentication == AuthState::Syncing) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Sign-in approved", "Loading your library…");
        } else if (state.account.authentication == AuthState::Expired) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "QR code expired", "Refresh to continue");
        } else if (state.account.authentication == AuthState::Failed) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Connection failed", "Retry the connection");
        } else if (!cached_qr_error_.empty() || !draw_encoded_qr(qr_x, qr_y, qr_side)) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "QR code unavailable", "Retry the connection");
        }
        if (state.simulation_mode) {
            scene_.rounded_rect({1185.0F, 678.0F, 292.0F, 42.0F}, 21.0F,
                                kAmber.with_alpha(0.18F));
            ui::text(scene_, fonts_.semibold, "SIMULATED QR — NOT LIVE", 1331.0F, 706.0F,
                     18.0F, kAmber, gfx::Align::center);
        }
        const char *status = !state.simulation_mode && state.account.challenge_url.empty() ?
            "Connecting securely" : "Waiting for approval";
        gfx::Color color = kAccent;
        if (!cached_qr_error_.empty())
        {
            status = "QR code unavailable · Retry connection";
            color = kRed;
        }
        else if (state.account.authentication == AuthState::Expired)
        {
            status = "Challenge expired";
            color = kAmber;
        }
        else if (state.account.authentication == AuthState::Failed)
        {
            status = state.account.error.empty() ? "Connection failed" : state.account.error.c_str();
            color = kRed;
        }
        else if (state.account.authentication == AuthState::Syncing)
        {
            status = state.simulation_mode ? "Approved · Syncing library" : "Approved · Loading library";
            color = kGreen;
        }
        else if (state.account.authentication == AuthState::Restoring)
        {
            status = state.simulation_mode ? "Restoring fixture state" : "Restoring your Steam session";
        }
        else if (state.account.authentication == AuthState::SigningOut)
        {
            status = "Signing out securely";
        }
        scene_.circle(1198.0F, 772.0F, 7.0F, color);
        ui::text(scene_, fonts_.regular, status, 1220.0F, 780.0F, 25.0F, kText);
    }

    void draw_home(ui::Canvas &, Simulation &simulation)
    {
        const ViewData &view = simulation.route_view();
        const State &state = simulation.state();
        if (view.shelves.empty())
        {
            section_label(view.title.empty() ? "Home" : view.title, 96.0F, 168.0F);
            ViewData copy = view;
            draw_title(copy, 96.0F, 360.0F, 820.0F);
            draw_actions(copy, 96.0F, 560.0F, 520.0F);
            return;
        }

        // As on macOS (`FocusViewport.reveal`): Home scrolls only when the focused tile or shelf
        // would leave the screen, and eases there, so the highlight is what moves; each shelf
        // keeps its own column and scroll position.
        // macOS spacing: room under each shelf for the focused game's title and status.
        constexpr float shelf_pitch = 456.0F;
        constexpr float tile_pitch = 233.0F;
        constexpr float tile_width = 213.0F;
        const int active_shelf = std::clamp(state.navigation.home_shelf, 0,
                                            static_cast<int>(view.shelves.size()) - 1);
        const float shelves_height = static_cast<float>(view.shelves.size()) * shelf_pitch;
        // Vertically too, only as far as needed to show the focused shelf and its title.
        constexpr float shelf_extent = 430.0F;
        home_scroll_.target = reveal(home_scroll_.target, active_shelf * shelf_pitch,
                                     active_shelf * shelf_pitch + shelf_extent, kFooterY - 205.0F,
                                     shelves_height - shelf_pitch + shelf_extent);
        const float vertical = home_scroll_.step(frame_dt_, state.reduced_motion);
        scene_.push_clip({0.0F, 180.0F, kWidth, kFooterY - 180.0F});
        for (int shelf_index = 0; shelf_index < static_cast<int>(view.shelves.size()); ++shelf_index)
        {
            const ViewData::Shelf &shelf = view.shelves[static_cast<std::size_t>(shelf_index)];
            const float top = 205.0F + shelf_index * shelf_pitch - vertical;
            const auto remembered = state.navigation.home_shelf_focus.find(shelf.key);
            const int count = static_cast<int>(shelf.items.size());
            const int focused_item = std::clamp(remembered == state.navigation.home_shelf_focus.end() ?
                                                    shelf.focused_item : remembered->second, 0, std::max(0, count - 1));
            Scroll &row = home_rows_[shelf.key];
            row.target = reveal(row.target, focused_item * tile_pitch, focused_item * tile_pitch + tile_width,
                                kWidth - 2.0F * kSafeX, count * tile_pitch - (tile_pitch - tile_width));
            const float horizontal = row.step(frame_dt_, state.reduced_motion);
            if (top + shelf_pitch < 180.0F || top > kFooterY)
                continue;
            section_label(shelf.title, 96.0F, top);
            const int first = std::max(0, static_cast<int>(horizontal / tile_pitch) - 1);
            const int last = std::min(count, static_cast<int>((horizontal + kWidth) / tile_pitch) + 1);
            for (int item_index = first; item_index < last; ++item_index)
            {
                const ViewRow &cell = shelf.items[static_cast<std::size_t>(item_index)];
                const gfx::Rect art{kSafeX + item_index * tile_pitch - horizontal, top + 28.0F, tile_width, 320.0F};
                const bool focused = route_focus_ && shelf_index == active_shelf &&
                                     item_index == focused_item;
                if (cell.game_id == 0 && cell.action == "shelf-more")
                {
                    // The rest of the shelf lives in the Library: a poster-sized card with the count.
                    if (focused)
                        stats_.focused_control_key = cell.key;
                    const float amount = control_focus(cell.key, focused);
                    focus_glow(scene_, art, amount);
                    scene_.rounded_rect(art, 8.0F, kBackground);
                    scene_.rounded_rect(art, 8.0F, kText.with_alpha(0.06F));
                    scene_.bordered_rect(art, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kText.with_alpha(0.2F));
                    ui::text(scene_, fonts_.display, cell.value, art.cx(), art.cy() + 4.0F, 56.0F, kText, gfx::Align::center);
                    ui::text(scene_, fonts_.display, "Library", art.cx(), art.cy() + 52.0F, 26.0F, kSecondary,
                             gfx::Align::center);
                    focus_ring(scene_, art, amount);
                    if (amount > 0.0F)
                    {
                        scene_.push_opacity(amount);
                        dynamic_text(cell.title, art.x, art.y + art.h + 46.0F, 24.0F, kText, 420.0F);
                        scene_.pop_opacity();
                    }
                    continue;
                }
                const Game *game = game_by_id(state, cell.game_id);
                if (game == nullptr)
                    continue;
                if (focused)
                    stats_.focused_control_key = shelf.items[static_cast<std::size_t>(item_index)].key;
                const std::string key = "home-" + shelf.key + "-" + std::to_string(game->id);
                draw_poster(*game, art, focused, job_progress(game->id), false, key);
                const float amount = focus_motion_.amount(key, focused);
                if (amount > 0.0F)
                {
                    scene_.push_opacity(amount);
                    dynamic_text(game->title, art.x, art.y + art.h + 46.0F, 24.0F, kText, 420.0F);
                    ui::text(scene_, fonts_.regular, fit(fonts_.regular, tile_subtitle(*game, tile_status(game->id)), 18.0F, 420.0F),
                             art.x, art.y + art.h + 74.0F, 18.0F, kSecondary);
                    scene_.pop_opacity();
                }
            }
        }
        scene_.pop_clip();
    }

    // The smallest scroll that keeps [item_min, item_max] inside the viewport (macOS FocusViewport).
    static float reveal(float offset, float item_min, float item_max, float viewport, float content)
    {
        const float maximum = std::max(0.0F, content - viewport);
        float result = std::clamp(offset, 0.0F, maximum);
        if (item_min < result)
            result = item_min;
        else if (item_max > result + viewport)
            result = item_max - viewport;
        return std::clamp(result, 0.0F, maximum);
    }

    // A scroll position easing to its target, close to the macOS 0.18 s ease-out.
    struct Scroll
    {
        float target = 0.0F;
        float value = 0.0F;
        bool started = false;
        float step(float dt, bool reduced_motion)
        {
            if (!started || reduced_motion)
            {
                started = true;
                value = target;
                return value;
            }
            value += (target - value) * (1.0F - std::exp(-dt / 0.05F));
            if (std::fabs(target - value) < 0.5F)
                value = target;
            return value;
        }
    };

    // A vertical list's scroll, eased like Home's: keeps [item_min, item_max] (content coordinates)
    // in a viewport of the given height, one position per list key.
    float list_scroll(const std::string &key, float item_min, float item_max, float viewport, float content,
                      bool reduced_motion)
    {
        Scroll &scroll = list_scrolls_[key];
        scroll.target = reveal(scroll.target, item_min, item_max, viewport, content);
        return scroll.step(frame_dt_, reduced_motion);
    }

    void draw_library(ui::Canvas &canvas, Simulation &simulation)
    {
        const State &state = simulation.state();
        const ViewData &view = simulation.route_view();
        const auto games = simulation.visible_games();
        draw_library_rail(view, state);

        if (games.empty())
        {
            ui::text(scene_, fonts_.display, view.title.empty() ? "No games here" : view.title,
                     1134.0F, 434.0F, 56.0F, kText, gfx::Align::center);
            ui::paragraph(scene_, fonts_.regular,
                          view.subtitle.empty() ? "Clear the search or choose another library view."
                                                : view.subtitle,
                          784.0F, 486.0F, 26.0F, 700.0F, 36.0F, kSecondary, 3,
                          gfx::Align::center);
            draw_actions(view, 914.0F, 620.0F, 440.0F);
            return;
        }

        visible_grid_games_ = games;
        grid_.style.reduced_motion = state.reduced_motion;
        const bool grid_active = route_focus_ && !state.navigation.library_rail_focused;
        grid_.style.theme.focus = grid_active ? kAccent : kAccent.with_alpha(0.0F);
        grid_.set_count(static_cast<int>(games.size()));
        int grid_focus = std::clamp(state.navigation.selected_index, 0,
                                    static_cast<int>(games.size()) - 1);
        const auto selected = std::find_if(games.begin(), games.end(), [&state](const Game *game) {
            return game->id == state.navigation.selected_game_id;
        });
        if (selected != games.end())
            grid_focus = static_cast<int>(std::distance(games.begin(), selected));
        stats_.grid_focused_game_id = games[static_cast<std::size_t>(grid_focus)]->id;
        if (grid_active)
            stats_.focused_control_key = "game-" + std::to_string(stats_.grid_focused_game_id);
        grid_.set_focus(grid_focus, true);
        grid_.set_active(grid_active);
        grid_.content = [this](ui::Canvas &, const gfx::Rect &cell, const ui::CardItem &, int index,
                              float /*focus*/) {
            ++stats_.grid_drawn_cell_count;
            const Game &game = *visible_grid_games_[static_cast<std::size_t>(index)];
            const bool focused = route_focus_ && !current_state_->navigation.library_rail_focused &&
                                 game.id == stats_.grid_focused_game_id;
            draw_poster(game, cell, focused, job_progress(game.id), true,
                        "library-" + std::to_string(game.id));
        };
        grid_.accent = [this](int index) {
            return id_color(visible_grid_games_[static_cast<std::size_t>(index)]->id);
        };
        grid_.update(1.0F / 60.0F);
        grid_.draw(canvas);
        if (state.library_syncing || state.library_loading)
        {
            scene_.rounded_rect({1398.0F, 86.0F, 426.0F, 48.0F}, 8.0F, kPanel);
            scene_.circle(1424.0F, 110.0F, 6.0F, kAccent);
            ui::text(scene_, fonts_.regular, "Refreshing library · focus preserved", 1444.0F,
                     119.0F, 20.0F, kSecondary);
        }
    }

    void draw_library_rail(const ViewData &view, const State &state)
    {
        struct RailEntry
        {
            std::string key;
            std::string title;
            std::string value;
            bool selected = false;
        };
        const std::size_t installed = std::count_if(
            state.installations.begin(), state.installations.end(),
            [&state](const Installation &installation) {
                return installation.installed_bytes > 0 || !state.simulation_mode;
            });
        std::vector<RailEntry> entries = {
            {"rail-installed", "Installed", std::to_string(installed), state.library.scope == "installed"},
            {"rail-all", "All", std::to_string(state.games.size()), state.library.scope == "all"},
            {"rail-favorites", "Favorites", std::to_string(state.library.favorites.size()),
             state.library.scope == "favorites"},
            {"rail-hidden", "Hidden", std::to_string(state.library.hidden.size()), state.library.scope == "hidden"}};
        for (const Collection &collection : state.library.collections)
            entries.push_back({"rail-" + collection.id, collection.name,
                               std::to_string(collection.members.size()),
                               state.library.scope == collection.id});
        entries.push_back({"rail-new-collection", "New collection", "+", false});
        const auto selected_entry = std::find_if(entries.begin(), entries.end(),
                                                 [](const RailEntry &entry) { return entry.selected; });
        const int selected = selected_entry == entries.end() ? -1 :
                                 static_cast<int>(std::distance(entries.begin(), selected_entry));
        const int focus_index = state.navigation.library_rail_focused ? state.navigation.selected_index :
                                                                         std::max(0, selected);
        // An applied search gets its own row above the rail, so nothing is drawn over a focus ring.
        const bool searching = !state.library.query.empty();
        const float rail_top = searching ? 200.0F : 150.0F;
        if (searching)
        {
            scene_.rounded_rect({96.0F, 150.0F, 300.0F, 38.0F}, 6.0F, kPanel);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, state.library.query, 22.0F, 260.0F),
                     118.0F, 177.0F, 22.0F, kText);
        }
        const ShelfWindow window = shelf_window(static_cast<int>(entries.size()), focus_index, searching ? 11 : 12);
        for (int visible = 0; visible < window.count; ++visible)
        {
            const int index = window.first + visible;
            const float y = rail_top + visible * 66.0F;
            const bool focused = route_focus_ && state.navigation.library_rail_focused &&
                                 index == state.navigation.selected_index;
            const gfx::Rect entry{96.0F, y, 300.0F, 60.0F};
            const float amount = control_focus(entries[static_cast<std::size_t>(index)].key, focused);
            focus_glow(scene_, entry, amount, true);
            scene_.rounded_rect(entry, 8.0F, kBackground);
            if (entries[static_cast<std::size_t>(index)].selected || index == selected)
                scene_.rounded_rect(entry, 8.0F, kText.with_alpha(0.10F));
            focus_ring(scene_, entry, amount, true);
            if (focused)
                stats_.focused_control_key = entries[static_cast<std::size_t>(index)].key;
            ui::text(scene_, fonts_.display,
                     fit(fonts_.display, entries[static_cast<std::size_t>(index)].title, 28.0F, 225.0F),
                     118.0F, y + 40.0F, 28.0F,
                     entries[static_cast<std::size_t>(index)].selected || index == selected ? kText
                                                                                             : kSecondary);
            ui::text(scene_, fonts_.regular, entries[static_cast<std::size_t>(index)].value,
                     374.0F, y + 38.0F, 20.0F, kMuted,
                     gfx::Align::right);
        }
        if (!view.subtitle.empty() && entries.size() <= (searching ? 7U : 8U))
            ui::paragraph(scene_, fonts_.regular, view.subtitle, 96.0F, 722.0F, 22.0F, 300.0F,
                          30.0F, kSecondary, 4);
    }

    float job_progress(GameId id) const
    {
        if (current_state_ == nullptr)
            return -1.0F;
        const auto found = std::find_if(current_state_->jobs.begin(), current_state_->jobs.end(),
                                        [id](const Job &job) { return job.game_id == id; });
        if (found == current_state_->jobs.end() || found->total_bytes == 0)
            return -1.0F;
        return static_cast<float>(found->completed_bytes) / static_cast<float>(found->total_bytes);
    }

    // What a tile says about a game's install, as the macOS tiles do: a badge for a job or a
    // missing drive, the size once installed, a download mark while it is not.
    struct TileStatus
    {
        const Job *job = nullptr;
        const Installation *installation = nullptr;
        bool installed = false;
        bool disconnected = false;
    };

    TileStatus tile_status(GameId id) const
    {
        TileStatus status;
        if (current_state_ == nullptr)
            return status;
        const State &state = *current_state_;
        const auto job = std::find_if(state.jobs.begin(), state.jobs.end(), [id](const Job &item) {
            // A finished download listed in Downloads is the installation now, not a transfer.
            return item.game_id == id && item.state != JobState::Cancelled && item.state != JobState::Downloaded &&
                   item.stage != JobStage::Uninstall;
        });
        if (job != state.jobs.end())
            status.job = &*job;
        const auto installation = std::find_if(state.installations.begin(), state.installations.end(),
                                               [id, &state](const Installation &item) {
                                                   return item.game_id == id &&
                                                          (item.installed_bytes > 0 || !state.simulation_mode);
                                               });
        if (installation != state.installations.end())
        {
            status.installation = &*installation;
            const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                             [&installation](const Volume &item) {
                                                 return item.id == installation->volume_id;
                                             });
            status.disconnected = !installation->available || (volume != state.volumes.end() && !volume->available);
            status.installed = !status.disconnected;
        }
        return status;
    }

    std::string tile_subtitle(const Game &game, const TileStatus &status) const
    {
        if (status.job != nullptr)
        {
            if (status.job->state == JobState::Downloading && status.job->total_bytes > 0)
                return std::to_string(static_cast<int>(100.0 * status.job->completed_bytes / status.job->total_bytes)) +
                       "% · " + job_state_label(status.job->state);
            return job_state_label(status.job->state);
        }
        if (status.disconnected)
            return "Reconnect its drive";
        if (status.installed)
        {
            const std::string size = status.installation->installed_bytes > 0 ?
                short_size(status.installation->installed_bytes) + " · " : std::string{};
            return size + (game.playtime_minutes && *game.playtime_minutes > 0 ?
                               playtime_label(game.playtime_minutes) + " played" : std::string("Installed"));
        }
        return game.download_bytes ? "Not installed · " + short_size(*game.download_bytes) : "Not installed";
    }

    void tile_badge(const gfx::Rect &rect, std::string_view text, gfx::Color dot)
    {
        constexpr float size = 16.0F;
        const float width = std::min(rect.w - 20.0F, fonts_.semibold.measure(text, size) + 44.0F);
        scene_.rounded_rect({rect.x + 10.0F, rect.y + 10.0F, width, 32.0F}, 6.0F, kBackground.with_alpha(0.85F));
        scene_.circle(rect.x + 26.0F, rect.y + 26.0F, 4.0F, dot);
        ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, text, size, width - 44.0F), rect.x + 38.0F,
                 rect.y + 32.0F, size, kText);
    }

    void download_mark(float cx, float cy, float opacity)
    {
        scene_.circle(cx, cy, 15.0F, kBackground.with_alpha(0.8F * opacity));
        scene_.ring(cx, cy, 15.0F, 1.5F, kText.with_alpha(0.5F * opacity));
        const gfx::Color ink = kText.with_alpha(opacity);
        scene_.line(cx, cy - 7.0F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx - 4.5F, cy - 1.5F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx + 4.5F, cy - 1.5F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx - 6.0F, cy + 7.0F, cx + 6.0F, cy + 7.0F, 1.8F, ink);
    }

    void draw_poster(const Game &game, const gfx::Rect &rect, bool focused, float progress,
                     bool title_overlay = true, std::string_view motion_key = {})
    {
        // The focus amount eases in and out, so a focus change cross-fades the two cells.
        const float amount = motion_key.empty() ? (focused ? 1.0F : 0.0F) : focus_motion_.amount(motion_key, focused);
        const float scale = motion_key.empty() ? 1.0F : 1.0F + (FocusMotion::focused_scale - 1.0F) * amount;
        scene_.push_transform(scale, rect.cx(), rect.cy(), 0.0F, 0.0F);
        const std::size_t texture_index = game.id >= 900001 ? (game.id - 900001) % art_.size() :
                                                              game.id % art_.size();
        const bool simulated = current_state_ != nullptr && current_state_->simulation_mode;
        const std::uint32_t texture = simulated
            ? (game.art_failed || game.art_pending ? 0 : art_[texture_index])
            : (live_artwork_ && current_state_ && !current_state_->account.id.empty()
                ? live_artwork_->TextureFor(game.id,
                    "https://cdn.cloudflare.steamstatic.com/steam/apps/" +
                    std::to_string(game.id) + "/library_600x900.jpg") : 0);
        const TileStatus status = tile_status(game.id);
        // Glow first: it is a filled soft shape, and the art must cover its middle so only the
        // light outside the tile shows, as on macOS. The art itself is never tinted.
        scene_.shadow(rect, 8.0F, 12.0F + 14.0F * amount, gfx::Color::rgb(0x000000, 0.35F + 0.3F * amount));
        if (amount > 0.0F)
            scene_.glow(rect.inset(-7.0F), 13.0F, 24.0F, kAccent.with_alpha(0.5F * amount));
        if (texture != 0)
            scene_.image(texture, rect, gfx::kFullUv, kText, 8.0F);
        else
        {
            scene_.gradient_rect(rect, 8.0F, id_color(game.id), gfx::Color::rgb(0x24201D));
            if (game.art_pending)
            {
                ui::text(scene_, fonts_.display, "Artwork loading", rect.cx(), rect.y + rect.h * 0.52F,
                         25.0F, kText, gfx::Align::center);
            }
            else
            {
                // The focus overlay names a focused tile, so the placeholder need not repeat it.
                const float placeholder = title_overlay ? 1.0F - amount : 1.0F;
                const ui::FontRef &face = font_for(game.title, fonts_.display);
                ui::paragraph(scene_, face, game.title, rect.cx(), rect.y + rect.h * 0.42F,
                              28.0F, rect.w - 36.0F, 31.0F, kText.with_alpha(placeholder), 4, gfx::Align::center);
            }
        }
        const bool downloading = status.job != nullptr &&
            (status.job->state == JobState::Downloading || status.job->state == JobState::Verifying ||
             status.job->state == JobState::Pausing || status.job->state == JobState::Paused);
        const bool show_download_mark = status.job == nullptr && status.installation == nullptr;
        if (title_overlay && amount > 0.0F && !game.art_pending && !downloading)
        {
            scene_.push_opacity(amount);
            scene_.gradient_rect({rect.x, rect.y + rect.h - 105.0F, rect.w, 105.0F}, 8.0F,
                                 kBackground.with_alpha(0.0F), kBackground.with_alpha(0.92F));
            const float text_width = rect.w - 28.0F - (show_download_mark ? 36.0F : 0.0F);
            dynamic_text(game.title, rect.x + 14.0F, rect.y + rect.h - 40.0F, 24.0F, kText, text_width);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, tile_subtitle(game, status), 15.0F, text_width),
                     rect.x + 14.0F, rect.y + rect.h - 17.0F, 15.0F, kSecondary);
            scene_.pop_opacity();
        }
        if (amount > 0.0F)
            scene_.bordered_rect(rect.inset(-9.0F), 15.0F, kBackground.with_alpha(0.0F), 4.0F,
                                 kAccent.with_alpha(amount));
        // A thumbnail (the Downloads card) is too small for tile badges; its card says it all.
        if (rect.w < 180.0F)
        {
            scene_.pop_transform();
            return;
        }
        if (status.job != nullptr && !downloading)
            tile_badge(rect, job_state_label(status.job->state),
                       status.job->state == JobState::Failed ? kAmber : kSecondary);
        else if (status.disconnected)
            tile_badge(rect, "Drive disconnected", kAmber);
        if (show_download_mark)
            download_mark(rect.x + rect.w - 25.0F, rect.y + rect.h - 25.0F, 1.0F);
        else if (status.installed && status.installation->installed_bytes > 0 && amount < 1.0F && title_overlay)
        {
            const std::string size = short_size(status.installation->installed_bytes);
            const float width = fonts_.semibold.measure(size, 16.0F) + 20.0F;
            scene_.push_opacity(1.0F - amount);
            scene_.rounded_rect({rect.x + rect.w - 10.0F - width, rect.y + rect.h - 42.0F, width, 32.0F}, 6.0F,
                                kBackground.with_alpha(0.85F));
            ui::text(scene_, fonts_.semibold, size, rect.x + rect.w - 20.0F, rect.y + rect.h - 20.0F, 16.0F,
                     kText, gfx::Align::right);
            scene_.pop_opacity();
        }
        if (downloading && progress >= 0.0F)
        {
            const float value = std::clamp(progress, 0.0F, 1.0F);
            scene_.gradient_rect({rect.x, rect.y + rect.h - 64.0F, rect.w, 64.0F}, 8.0F,
                                 kBackground.with_alpha(0.0F), kBackground.with_alpha(0.9F));
            ui::text(scene_, fonts_.semibold, job_state_label(status.job->state), rect.x + 12.0F,
                     rect.y + rect.h - 30.0F, 16.0F, kText);
            if (status.job->state == JobState::Downloading)
                ui::text(scene_, fonts_.semibold, std::to_string(static_cast<int>(value * 100.0F)) + "%",
                         rect.x + rect.w - 12.0F, rect.y + rect.h - 30.0F, 16.0F, kText, gfx::Align::right);
            scene_.rounded_rect({rect.x + 12.0F, rect.y + rect.h - 18.0F, rect.w - 24.0F, 6.0F},
                                3.0F, kText.with_alpha(0.20F));
            scene_.rounded_rect({rect.x + 12.0F, rect.y + rect.h - 18.0F, (rect.w - 24.0F) * value, 6.0F},
                                3.0F, status.job->state == JobState::Paused ? kAmber : kAccent);
        }
        scene_.pop_transform();
    }

    std::vector<ui::ListItem> list_items(const ViewData &view) const
    {
        std::vector<ui::ListItem> items;
        items.reserve(view.rows.size());
        for (const ViewRow &row : view.rows)
        {
            ui::ListItem item;
            item.title = row.title;
            item.subtitle = row.subtitle;
            item.value = row.value;
            item.badge = row.badge;
            item.disabled = !row.enabled;
            item.header = row.kind == RowKind::Heading || row.kind == RowKind::Separator;
            item.chevron = row.kind == RowKind::Choice || row.kind == RowKind::Profile ||
                           !row.action.empty();
            items.push_back(std::move(item));
        }
        return items;
    }

    void draw_view_list(ui::Canvas &canvas, const ViewData &view, const gfx::Rect &bounds,
                        float row_height = 90.0F)
    {
        list_.style.row_height = row_height;
        list_.style.theme.focus = route_focus_ ? kAccent : kAccent.with_alpha(0.0F);
        list_.set_items(list_items(view));
        list_.set_bounds(bounds);
        list_.set_focus(std::clamp(view.focused_row, 0, std::max(0, static_cast<int>(view.rows.size()) - 1)),
                        true);
        list_.set_active(route_focus_);
        list_.update(1.0F / 60.0F);
        list_.draw(canvas);

        for (std::size_t index = 0; index < view.rows.size(); ++index)
        {
            const ViewRow &row = view.rows[index];
            if (row.progress <= 0.0F && row.kind != RowKind::Job && row.kind != RowKind::Volume)
                continue;
            const gfx::Rect row_rect = list_.row_rect(static_cast<int>(index));
            if (row_rect.y + row_rect.h < bounds.y || row_rect.y > bounds.y + bounds.h)
                continue;
            ui::ProgressBar progress;
            progress.style.theme = theme_;
            progress.style.height = 7.0F;
            progress.style.placement = ui::LabelPlacement::none;
            progress.style.status = progress_status(row);
            progress.style.finish_flash = false;
            progress.set_bounds({row_rect.x + 24.0F, row_rect.y + row_rect.h - 16.0F,
                                 row_rect.w - 48.0F, 7.0F});
            progress.set_value(std::clamp(row.progress, 0.0F, 1.0F), true);
            progress.draw(canvas);
        }
    }

    void draw_downloads(ui::Canvas &canvas, const ViewData &view, const State &state)
    {
        (void)view;
        std::vector<const Job*> jobs;
        for (const Job& job : state.jobs)
            if (job.state != JobState::Cancelled)
                jobs.push_back(&job);
        std::stable_sort(jobs.begin(), jobs.end(), [](const Job* left, const Job* right) {
            return left->queue_order < right->queue_order;
        });
        // The card shows the running transfer, or else the first one in queue order (a failed or paused
        // download does not jump ahead of the queue).
        const auto active = std::find_if(jobs.begin(), jobs.end(), [](const Job* job) {
            return job->state == JobState::Downloading || job->state == JobState::Verifying ||
                   job->state == JobState::Pausing;
        });
        // The card shows a transfer; finished downloads only follow the queue in the list below.
        const Job* primary = active != jobs.end() ? *active :
            (jobs.empty() || jobs.front()->state == JobState::Downloaded ? nullptr : jobs.front());

        section_label("Transfers", 96.0F, 146.0F);
        const gfx::Rect active_card{96.0F, 180.0F, 1000.0F, 230.0F};
        scene_.shadow(active_card, 12.0F, 24.0F, gfx::Color::rgb(0x000000, 0.35F));
        scene_.rounded_rect(active_card, 12.0F, kPanel);
        if (primary == nullptr)
        {
            ui::text(scene_, fonts_.display, "No transfers", 136.0F, 272.0F, 56.0F, kText);
            ui::text(scene_, fonts_.regular,
                     state.simulation_mode ? "Install a game to start a simulated transfer."
                                           : "Install a game from its details to download it here.",
                     136.0F, 322.0F, 26.0F, kSecondary);
        }
        else
        {
            const Game* game = game_by_id(state, primary->game_id);
            if (game != nullptr)
                draw_poster(*game, {116.0F, 196.0F, 132.0F, 198.0F}, false, -1.0F, false);
            else
                scene_.rounded_rect({116.0F, 196.0F, 132.0F, 198.0F}, 8.0F, kMuted.with_alpha(0.25F));
            const std::string title = game == nullptr ? "Unknown game" : game->title;
            dynamic_text(title, 282.0F, 238.0F, 36.0F, kText, 750.0F);
            ui::text(scene_, fonts_.semibold, job_stage_label(primary->stage), 282.0F, 278.0F,
                     24.0F, primary->state == JobState::Failed ? kRed :
                            primary->state == JobState::Paused ? kAmber : kAccent);
            ui::text(scene_, fonts_.regular, job_state_label(primary->state), 1038.0F, 278.0F,
                     22.0F, kSecondary, gfx::Align::right);
            const float progress = primary->total_bytes == 0 ? 0.0F :
                static_cast<float>(primary->completed_bytes) / static_cast<float>(primary->total_bytes);
            ui::ProgressBar bar;
            bar.style.theme = theme_;
            bar.style.height = 12.0F;
            bar.style.placement = ui::LabelPlacement::none;
            bar.style.status = primary->state == JobState::Failed ? ui::Status::danger :
                               primary->state == JobState::Paused ? ui::Status::warning : ui::Status::accent;
            bar.style.finish_flash = false;
            bar.set_bounds({282.0F, 302.0F, 756.0F, 12.0F});
            bar.set_value(std::clamp(progress, 0.0F, 1.0F), true);
            bar.draw(canvas);
            const std::string bytes = byte_count(primary->completed_bytes) + " / " +
                                      byte_count(primary->total_bytes);
            ui::text(scene_, fonts_.regular, bytes, 282.0F, 352.0F, 22.0F, kText);
            ui::text(scene_, fonts_.regular, FormatDownloadRate(primary->state == JobState::Downloading ? primary->bytes_per_second : std::nullopt,
                                         primary->state == JobState::Downloading ? primary->eta_seconds : std::nullopt), 1038.0F,
                     352.0F, 22.0F, kSecondary, gfx::Align::right);
            const bool card_focused = route_focus_ && state.navigation.focus_key == "job-" + primary->id;
            focus_ring(scene_, active_card, control_focus("job-" + primary->id, card_focused));
            if (card_focused)
                stats_.focused_control_key = "job-" + primary->id;
        }

        std::vector<const Job*> secondary;
        secondary.reserve(jobs.size());
        for (const Job *job : jobs)
            if (job != primary)
                secondary.push_back(job);
        // While a job's sheet is open the list keeps that job's row in view instead of jumping to the top.
        const std::string &list_focus = state.navigation.modals.empty() ? state.navigation.focus_key
                                                                         : state.navigation.modals.front().opener_focus_key;
        const auto focused_job = std::find_if(secondary.begin(), secondary.end(), [&list_focus](const Job *job) {
            return list_focus == "job-" + job->id;
        });
        const int secondary_focus = focused_job == secondary.end() ? 0 :
            static_cast<int>(std::distance(secondary.begin(), focused_job));
        // Five rows fit above the button guide; longer queues scroll smoothly to the focused row. The
        // window is exactly five rows tall, so scrolling moves whole rows and none shows in part.
        constexpr float job_pitch = 104.0F, job_top = 438.0F, job_viewport = 5 * job_pitch - 12.0F;
        const float job_offset = list_scroll("downloads", secondary_focus * job_pitch, secondary_focus * job_pitch + 92.0F,
                                             job_viewport, secondary.empty() ? 0.0F : secondary.size() * job_pitch - 12.0F,
                                             state.reduced_motion);
        // The clip leaves room for the focus ring around the first and the last row.
        scene_.push_clip({80.0F, job_top - 10.0F, 1032.0F, job_viewport + 20.0F});
        for (std::size_t index = 0; index < secondary.size(); ++index)
        {
            const float row_y = job_top + index * job_pitch - job_offset;
            if (row_y + job_pitch < job_top - 8.0F || row_y > job_top + job_viewport)
                continue;
            const Job *job = secondary[index];
            const Game* game = game_by_id(state, job->game_id);
            const gfx::Rect row{96.0F, row_y, 1000.0F, 92.0F};
            scene_.rounded_rect(row, 8.0F, kPanel);
            dynamic_text(game == nullptr ? std::string_view("Unknown game") : std::string_view(game->title),
                         122.0F, row_y + 42.0F, 30.0F, kText, 560.0F);
            const bool finished = job->state == JobState::Downloaded;
            std::string state_text = job_state_label(job->state);
            if (finished && job->finished_at > 0 && !state.simulation_mode)
            {
                // Short enough to stay clear of the size on the right.
                const std::int64_t now = static_cast<std::int64_t>(state.clock_ticks);
                const std::int64_t seconds = std::max<std::int64_t>(0, now - job->finished_at);
                state_text += " · " + (seconds < 60     ? std::string("just now")
                                       : seconds < 3600 ? std::to_string(seconds / 60) + " min ago"
                                                        : relative_time(job->finished_at, now));
            }
            ui::text(scene_, fonts_.regular, state_text, 712.0F, row_y + 42.0F,
                     22.0F, job->state == JobState::Failed ? kRed : kSecondary);
            const std::string bytes = finished ? byte_count(job->total_bytes)
                                               : byte_count(job->completed_bytes) + " / " + byte_count(job->total_bytes);
            ui::text(scene_, fonts_.regular, bytes, 1068.0F, row_y + 42.0F, 22.0F, kSecondary,
                     gfx::Align::right);
            const float progress = job->total_bytes == 0 ? 0.0F :
                static_cast<float>(job->completed_bytes) / static_cast<float>(job->total_bytes);
            scene_.rounded_rect({122.0F, row_y + 68.0F, 946.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
            scene_.rounded_rect({122.0F, row_y + 68.0F, 946.0F * std::clamp(progress, 0.0F, 1.0F), 5.0F},
                                2.5F, job->state == JobState::Failed ? kRed : kAccent);
            const bool row_focused = route_focus_ && state.navigation.focus_key == "job-" + job->id;
            focus_ring(scene_, row, control_focus("job-" + job->id, row_focused), true);
            if (row_focused)
                stats_.focused_control_key = "job-" + job->id;
        }
        scene_.pop_clip();

        scene_.rounded_rect({1160.0F, 160.0F, 664.0F, 780.0F}, 12.0F, kPanel);
        ui::text(scene_, fonts_.display, "Storage", 1200.0F, 218.0F, 38.0F, kText);
        // Every drive fits in the panel: rows shrink when the console lists many drives.
        const bool any_unreported = std::any_of(state.volumes.begin(), state.volumes.end(),
                                                [](const Volume &volume) { return !volume.capacity_known; });
        const float list_bottom = any_unreported ? 880.0F : 920.0F;
        float wanted = 0.0F;
        for (const Volume &volume : state.volumes)
            wanted += volume.capacity_known ? 192.0F : 100.0F;
        // Shrink the spacing to fit; rows keep their minimum height, and drives past the end are counted.
        const float scale = wanted <= list_bottom - 276.0F ? 1.0F :
            std::max(0.72F, (list_bottom - 276.0F) / wanted);
        float y = 276.0F;
        std::size_t drawn = 0;
        for (const Volume &volume : state.volumes)
        {
            const float step = (volume.capacity_known ? 192.0F : 100.0F) * scale;
            if (y + step - 40.0F > list_bottom)
            {
                ui::text(scene_, fonts_.regular, "+" + std::to_string(state.volumes.size() - drawn) + " more drives",
                         1200.0F, y, 22.0F, kMuted);
                break;
            }
            ++drawn;
            const std::uint64_t unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
            const std::uint64_t free = unused - std::min(volume.reserved_bytes, unused);
            const float fraction = volume.capacity_bytes == 0 ? 0.0F :
                static_cast<float>(volume.used_bytes) / static_cast<float>(volume.capacity_bytes);
            const float reserved = volume.capacity_bytes == 0 ? 0.0F :
                static_cast<float>(volume.reserved_bytes) / static_cast<float>(volume.capacity_bytes);
            ui::text(scene_, fonts_.display, volume.label, 1200.0F, y, 30.0F, kText);
            const bool is_default = !state.simulation_mode && volume.id == state.default_volume_id;
            ui::text(scene_, fonts_.regular,
                     volume.available ? (volume.writable ? (is_default ? "Default · Available" : "Available") : "Read only")
                                      : "Disconnected",
                     1784.0F, y, 22.0F, volume.available && volume.writable ? kSecondary : kAmber,
                     gfx::Align::right);
            if (!volume.capacity_known)
            {
                // A failed space query leaves capacity unknown: show only what Z3CloudPlay knows.
                ui::text(scene_, fonts_.regular, "Used by games  " + short_size(volume.games_bytes),
                         1200.0F, y + 42.0F, 22.0F, kSecondary);
                // Space held for queued downloads, only when there is some.
                if (volume.reserved_bytes > 0)
                    ui::text(scene_, fonts_.regular, "Reserved by queue  " + short_size(volume.reserved_bytes),
                             1784.0F, y + 42.0F, 22.0F, kSecondary, gfx::Align::right);
                y += step;
                continue;
            }
            const float bar_x = 1200.0F;
            const float bar_y = y + 28.0F;
            const float bar_w = 584.0F;
            scene_.rounded_rect({bar_x, bar_y, bar_w, 12.0F}, 6.0F, kText.with_alpha(0.14F));
            scene_.rounded_rect({bar_x, bar_y, bar_w * std::clamp(fraction, 0.0F, 1.0F), 12.0F},
                                5.0F, volume.available ? kAccent : kMuted);
            scene_.rounded_rect({bar_x + bar_w * std::clamp(fraction, 0.0F, 1.0F), bar_y,
                                 bar_w * std::clamp(reserved, 0.0F, 1.0F - std::clamp(fraction, 0.0F, 1.0F)), 12.0F},
                                5.0F, volume.available ? kAmber : kMuted);
            ui::text(scene_, fonts_.regular, "Capacity  " + short_size(volume.capacity_bytes),
                     1200.0F, y + 76.0F, 22.0F, kSecondary);
            ui::text(scene_, fonts_.regular, "Used  " + short_size(volume.used_bytes),
                     1200.0F, y + 106.0F, 22.0F, kSecondary);
            ui::text(scene_, fonts_.regular, "Reserved  " + short_size(volume.reserved_bytes),
                     1784.0F, y + 76.0F, 22.0F, kSecondary, gfx::Align::right);
            ui::text(scene_, fonts_.semibold, "Free  " + short_size(free),
                     1784.0F, y + 106.0F, 22.0F, volume.available ? kText : kMuted,
                     gfx::Align::right);
            y += step;
        }
        if (any_unreported)
            ui::text(scene_, fonts_.regular, "Free space unavailable for some drives",
                     1200.0F, 912.0F, 22.0F, kMuted);
    }

    // The macOS Settings screen: a rail of sections, then the focused section's rows, each
    // with its title and summary and a toggle or a button on the right.
    void draw_settings(ui::Canvas &, const ViewData &view, const State &state)
    {
        if (view.rows.empty())
            return;
        const int focused = std::clamp(view.focused_row, 0, static_cast<int>(view.rows.size()) - 1);
        const std::string &current = view.rows[static_cast<std::size_t>(focused)].section;
        std::vector<std::string> sections;
        for (const ViewRow &row : view.rows)
            if (sections.empty() || sections.back() != row.section)
                sections.push_back(row.section);
        const bool rail = state.navigation.settings_rail_focused;
        for (std::size_t index = 0; index < sections.size(); ++index)
        {
            const gfx::Rect entry{kSafeX, 150.0F + index * 66.0F, 300.0F, 60.0F};
            const bool active = sections[index] == current;
            const float amount = control_focus("section-" + sections[index], route_focus_ && rail && active);
            focus_glow(scene_, entry, amount, true);
            scene_.rounded_rect(entry, 8.0F, kBackground);
            if (active)
                scene_.rounded_rect(entry, 8.0F, kText.with_alpha(0.10F));
            focus_ring(scene_, entry, amount, true);
            ui::text(scene_, fonts_.display, sections[index], entry.x + 22.0F, entry.y + 40.0F, 28.0F,
                     active ? kText : kSecondary);
            if (route_focus_ && rail && active)
                stats_.focused_control_key = "section-" + sections[index];
        }

        constexpr float left = 444.0F;
        constexpr float width = 1380.0F;
        section_label(current, left, 172.0F);
        if (!view.subtitle.empty())
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, view.subtitle, 22.0F, width), left + 300.0F, 172.0F,
                     22.0F, kAmber);
        // Rows of the section; long sections scroll to keep the focused row in view.
        std::vector<int> rows;
        for (int index = 0; index < static_cast<int>(view.rows.size()); ++index)
            if (view.rows[static_cast<std::size_t>(index)].section == current &&
                !view.rows[static_cast<std::size_t>(index)].rail_only)
                rows.push_back(index);
        const auto height = [&](const ViewRow &row) { return row.subtitle.empty() ? 92.0F : 118.0F; };
        float offset = 0.0F;
        {
            // Eased to the focused row like Home; with the focus on the rail the list stays where it was.
            float top = 0.0F, focus_min = -1.0F, focus_max = 0.0F;
            for (const int index : rows)
            {
                const float h = height(view.rows[static_cast<std::size_t>(index)]);
                if (index == focused)
                {
                    focus_min = top;
                    focus_max = top + h;
                }
                top += h + 24.0F;
            }
            Scroll &scroll = list_scrolls_["settings-" + current];
            const float content = std::max(0.0F, top - 24.0F);
            scroll.target = focus_min >= 0.0F ? reveal(scroll.target, focus_min, focus_max, 740.0F, content)
                                              : std::clamp(scroll.target, 0.0F, std::max(0.0F, content - 740.0F));
            offset = scroll.step(frame_dt_, state.reduced_motion);
        }
        scene_.push_clip({left - 24.0F, 190.0F, width + 48.0F, 790.0F});
        float y = 206.0F - offset;
        for (const int index : rows)
        {
            const ViewRow &row = view.rows[static_cast<std::size_t>(index)];
            const float h = height(row);
            const gfx::Rect card{left, y, width, h};
            const bool is_focused = route_focus_ && !rail && index == focused;
            if (is_focused)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, is_focused);
            focus_glow(scene_, card, amount);
            scene_.rounded_rect(card, 8.0F, kBackground);
            scene_.rounded_rect(card, 8.0F, kText.with_alpha(row.dangerous ? 0.0F : 0.04F));
            if (row.dangerous)
                scene_.bordered_rect(card, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kRed.with_alpha(0.35F));
            const gfx::Color title_ink = !row.enabled ? kMuted : row.dangerous ? kRed : kText;
            const float title_y = row.subtitle.empty() ? card.cy() + 11.0F : card.y + 50.0F;
            ui::text(scene_, fonts_.display, fit(fonts_.display, row.title, 30.0F, 760.0F), card.x + 30.0F, title_y,
                     30.0F, title_ink);
            if (!row.subtitle.empty())
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.subtitle, 22.0F, 900.0F), card.x + 30.0F,
                         card.y + 88.0F, 22.0F, kSecondary);
            const float right = card.x + card.w - 30.0F;
            if (row.kind == RowKind::Toggle)
            {
                const gfx::Rect track{right - 84.0F, card.cy() - 22.0F, 84.0F, 44.0F};
                scene_.rounded_rect(track, 22.0F, row.selected ? kAccent : kText.with_alpha(0.15F));
                scene_.circle(row.selected ? track.x + track.w - 22.0F : track.x + 22.0F, track.cy(), 18.0F,
                              row.selected ? kBackground : kSecondary);
            }
            else if (!row.enabled || row.kind == RowKind::Status)
            {
                if (!row.value.empty())
                    ui::text(scene_, fonts_.display, row.value, right, card.cy() + 9.0F, 26.0F, kMuted,
                             gfx::Align::right);
            }
            else
            {
                // "Manage ›" on macOS; the chevron is drawn, as the console fonts lack it.
                const std::string label = row.value.empty() ? std::string(row.dangerous ? "Review" : "Open") : row.value;
                const float pill = fonts_.display.measure(label, 26.0F) + 64.0F;
                const gfx::Rect button{right - pill, card.cy() - 28.0F, pill, 56.0F};
                scene_.bordered_rect(button, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kText.with_alpha(0.2F));
                ui::text(scene_, fonts_.display, label, button.x + 22.0F, button.cy() + 9.0F, 26.0F, kText);
                const float chevron = button.x + button.w - 26.0F;
                scene_.line(chevron - 3.0F, button.cy() - 6.0F, chevron + 2.5F, button.cy(), 2.2F, kText);
                scene_.line(chevron + 2.5F, button.cy(), chevron - 3.0F, button.cy() + 6.0F, 2.2F, kText);
            }
            focus_ring(scene_, card, amount);
            y += h + 24.0F;
        }
        scene_.pop_clip();
    }

    std::string steam_art(GameId id, std::string_view name) const
    {
        return "https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_string(id) + "/" + std::string(name);
    }

    // A label over its value, as the macOS details metadata.
    void detail_cell(std::string_view label, std::string_view value, float x, float y, float width)
    {
        ui::text(scene_, fonts_.display, ui::upper(label), x, y + 14.0F, 17.0F, kSecondary, gfx::Align::left, 1.7F);
        const ui::FontRef &face = font_for(value, fonts_.semibold);
        ui::text(scene_, face, face.font->fit(value, 26.0F, width), x, y + 50.0F, 26.0F, kText);
    }

    // The macOS game page: the game's hero across the top with its logo, a row of actions,
    // then the description on the left and the facts on the right.
    // A ring of dots that turns (still with reduced motion).
    void spinner(float cx, float cy, float radius, gfx::Color color)
    {
        const bool still = current_state_ != nullptr && current_state_->reduced_motion;
        const float turn = still ? 0.0F : std::fmod(clock_ * 1.2F, 1.0F);
        for (int dot = 0; dot < 8; ++dot)
        {
            const float angle = static_cast<float>(dot) * 0.785398F;
            const float age = std::fmod(static_cast<float>(dot) / 8.0F - turn + 1.0F, 1.0F);
            scene_.circle(cx + std::sin(angle) * radius, cy - std::cos(angle) * radius, 2.2F,
                          color.with_alpha(0.25F + 0.75F * age));
        }
    }

    gfx::Color save_tone(std::string_view tone) const
    {
        return tone == "good" ? kGreen : tone == "busy" ? kBlue : tone == "attention" ? kAmber
             : tone == "problem" ? kRed : kMuted.with_alpha(0.9F);
    }

    // A game's picture for a Saves row or page: its cover when it is loaded, else its colour.
    void save_art(const State &state, GameId id, const gfx::Rect &rect)
    {
        // The cover when it is loaded; a small tile would wrap the poster's title letter by letter.
        if (const Game *game = game_by_id(state, id); game != nullptr && state.simulation_mode)
        {
            const std::size_t index = id >= 900001 ? (id - 900001) % art_.size() : id % art_.size();
            if (!game->art_failed && !game->art_pending && art_[index] != 0)
            {
                scene_.image(art_[index], rect, gfx::kFullUv, kText, 6.0F);
                return;
            }
        }
        if (live_artwork_ && !state.account.id.empty())
            if (const std::uint32_t cover = live_artwork_->TextureFor(
                    id, "https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_string(id) + "/library_600x900.jpg"))
            {
                scene_.image(cover, rect, gfx::kFullUv, kText, 6.0F);
                return;
            }
        // No cover: the game's colour alone.
        scene_.gradient_rect(rect, 6.0F, id_color(id), gfx::Color::rgb(0x24201D));
    }

    // The Saves tab: filter chips and Sync all over one list of games with saves on this console.
    void draw_saves(const ViewData &view, const State &state)
    {
        section_label("Game saves on this console", kSafeX, 160.0F);
        const bool busy = state.saves_job.kind != SavesJobKind::None;
        float summary_x = 1824.0F;
        if (busy)
        {
            const float width = fonts_.semibold.measure(view.subtitle, 22.0F);
            spinner(summary_x - width - 22.0F, 153.0F, 9.0F, kBlue);
        }
        ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, view.subtitle, 22.0F, 1000.0F), summary_x, 161.0F, 22.0F,
                 kSecondary, gfx::Align::right);

        const auto focused_key = [&]() -> std::string {
            if (view.focused_row < 0 || static_cast<std::size_t>(view.focused_row) >= view.rows.size()) return {};
            return view.rows[static_cast<std::size_t>(view.focused_row)].key;
        }();
        const auto focus_of = [&](const ViewRow &row) {
            const bool focused = route_focus_ && focused_key == row.key;
            if (focused)
                stats_.focused_control_key = row.key;
            return control_focus(row.key, focused);
        };
        // Chips, then Sync all at the right.
        float x = kSafeX;
        std::vector<const ViewRow *> games;
        for (const ViewRow &row : view.rows)
        {
            if (row.key.rfind("saves-filter-", 0) == 0)
            {
                const float label = fonts_.display.measure(row.title, 26.0F);
                const float count = fonts_.semibold.measure(row.value, 20.0F);
                const gfx::Rect chip{x, 186.0F, label + count + 54.0F, 48.0F};
                const float amount = focus_of(row);
                pill_focus_glow(chip, amount);
                if (row.selected)
                    scene_.rounded_rect(chip, 24.0F, kText);
                else
                    scene_.bordered_rect(chip, 24.0F, kText.with_alpha(0.0F), 1.5F, kText.with_alpha(0.25F));
                const gfx::Color ink = row.selected ? kBackground : kText;
                ui::text(scene_, fonts_.display, row.title, chip.x + 20.0F, chip.cy() + 9.0F, 26.0F, ink);
                ui::text(scene_, fonts_.semibold, row.value, chip.x + 20.0F + label + 12.0F, chip.cy() + 7.0F, 20.0F,
                         row.selected ? kBackground.with_alpha(0.65F) : kSecondary);
                pill_focus_ring(chip, amount);
                x += chip.w + 12.0F;
            }
            else if (row.key == "saves-sync-all")
            {
                const float label = fonts_.display.measure(row.title, 26.0F);
                const gfx::Rect button{1824.0F - label - 74.0F, 186.0F, label + 74.0F, 48.0F};
                const float amount = focus_of(row);
                focus_glow(scene_, button, amount, true);
                if (!row.enabled)
                    scene_.push_opacity(0.45F);
                scene_.bordered_rect(button, 8.0F, kText.with_alpha(0.06F), 1.5F, (row.selected ? kBlue : kText).with_alpha(0.5F));
                if (row.selected)
                    spinner(button.x + 26.0F, button.cy(), 8.0F, kBlue);
                else
                    sync_icon(button.x + 26.0F, button.cy(), 9.0F, 2.0F, kText);
                ui::text(scene_, fonts_.display, row.title, button.x + 48.0F, button.cy() + 9.0F, 26.0F,
                         row.selected ? kBlue : kText);
                if (!row.enabled)
                    scene_.pop_opacity();
                focus_ring(scene_, button, amount, true);
                if (busy)
                {
                    scene_.rounded_rect({kSafeX, 250.0F, 1824.0F - kSafeX, 4.0F}, 2.0F, kText.with_alpha(0.1F));
                    scene_.rounded_rect({kSafeX, 250.0F, (1824.0F - kSafeX) * std::clamp(row.progress, 0.0F, 1.0F), 4.0F},
                                        2.0F, kBlue);
                }
            }
            else
                games.push_back(&row);
        }

        // The list: seven rows, scrolled to keep the focus in view.
        int focus_index = 0;
        for (std::size_t index = 0; index < games.size(); ++index)
            if (games[index]->key == focused_key)
                focus_index = static_cast<int>(index);
        constexpr float save_pitch = 100.0F, save_top = 272.0F, save_viewport = 7 * save_pitch;
        const float save_offset = list_scroll("saves", focus_index * save_pitch, focus_index * save_pitch + 90.0F, save_viewport,
                                              games.empty() ? 0.0F : games.size() * save_pitch - 10.0F, state.reduced_motion);
        scene_.push_clip({0.0F, save_top - 8.0F, kWidth, save_viewport + 4.0F});
        for (std::size_t index = 0; index < games.size(); ++index)
        {
            const float y = save_top + index * save_pitch - save_offset;
            if (y + save_pitch < save_top - 8.0F || y > save_top + save_viewport)
                continue;
            const ViewRow &row = *games[index];
            const gfx::Rect box{kSafeX, y, 1824.0F - kSafeX, 90.0F};
            if (row.kind == RowKind::Status)
            {
                ui::text(scene_, fonts_.regular, row.title, kSafeX + 8.0F, y + 50.0F, 26.0F, kSecondary);
                break;
            }
            const float amount = focus_of(row);
            focus_glow(scene_, box, amount, true);
            scene_.rounded_rect(box, 10.0F, kPanel);
            save_art(state, row.game_id, {box.x + 14.0F, y + 9.0F, 48.0F, 72.0F});
            const bool two_lines = !row.subtitle.empty();
            dynamic_text(row.title, box.x + 84.0F, y + (two_lines ? 40.0F : 56.0F), 32.0F, kText, 820.0F);
            if (two_lines)
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.subtitle, 21.0F, 820.0F), box.x + 84.0F, y + 72.0F,
                         21.0F, kSecondary);
            const float status_x = 1040.0F;
            const gfx::Color tone = save_tone(row.tone);
            const bool moving = row.progress > 0.0F;
            if (row.tone == "busy" && row.value != "Will sync after play")
                spinner(status_x + 8.0F, y + (moving ? 38.0F : 45.0F), 8.0F, kBlue);
            else
                scene_.circle(status_x + 8.0F, y + (moving ? 38.0F : 45.0F), 7.0F, tone);
            ui::text(scene_, fonts_.semibold, row.value, status_x + 28.0F, y + (moving ? 46.0F : 53.0F), 24.0F, kText);
            if (moving)
            {
                scene_.rounded_rect({status_x + 28.0F, y + 60.0F, 200.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
                scene_.rounded_rect({status_x + 28.0F, y + 60.0F, 200.0F * std::clamp(row.progress, 0.0F, 1.0F), 5.0F}, 2.5F, kBlue);
            }
            ui::text(scene_, fonts_.semibold, row.aside, 1490.0F, y + 53.0F, 24.0F, kText.with_alpha(0.85F), gfx::Align::right);
            ui::text(scene_, fonts_.regular, row.badge, box.x + box.w - 24.0F, y + 53.0F, 21.0F, kSecondary, gfx::Align::right);
            focus_ring(scene_, box, amount, true);
        }
        scene_.pop_clip();
    }

    // One game's saves: Steam Cloud and this console side by side, and what can be done.
    void draw_save_details(const ViewData &view, const State &state)
    {
        const GameId id = state.navigation.save_id;
        const auto found = state.cloud.find(id);
        const LiveCloudGame *cloud = found == state.cloud.end() ? nullptr : &found->second;
        const LiveKeptBottle *kept = nullptr;
        for (const auto &item : state.kept_bottles)
            if (item.game_id == id)
                kept = &item;
        const bool installed = std::any_of(state.installations.begin(), state.installations.end(),
                                           [id](const Installation &item) { return item.game_id == id; });
        scene_.gradient_rect({0.0F, 0.0F, kWidth, 380.0F}, 0.0F, id_color(id), kBackground);
        ui::draw_button(scene_, fonts_, ui::GlyphStyle::mono(kText, kBackground), ui::Button::circle, kSafeX, 74.0F, 36.0F);
        ui::text(scene_, fonts_.semibold, "Saves", kSafeX + 48.0F, 82.0F, 22.0F, kText);
        save_art(state, id, {kSafeX, 170.0F, 120.0F, 180.0F});
        dynamic_text(view.title, kSafeX + 150.0F, 262.0F, 64.0F, kText, 1500.0F);
        // The status, from the same row the Saves list shows.
        scene_.circle(kSafeX + 158.0F, 306.0F, 7.0F, save_tone(view.tone));
        ui::text(scene_, fonts_.semibold, view.subtitle, kSafeX + 176.0F, 314.0F, 26.0F, kText);

        // The comparison.
        const gfx::Color label = gfx::Color::rgb(0xD9D3CB);
        const gfx::Color newer = gfx::Color::rgb(0xF2C94C);
        const gfx::Color absent = gfx::Color::rgb(0x9A938B);
        const float left = kSafeX, cloud_x = kSafeX + 300.0F, here_x = kSafeX + 680.0F, right = kSafeX + 1060.0F;
        float y = 420.0F;
        ui::text(scene_, fonts_.display, ui::upper("Steam Cloud"), cloud_x, y, 22.0F, label, gfx::Align::left, 2.6F);
        ui::text(scene_, fonts_.display, ui::upper("This console"), here_x, y, 22.0F, label, gfx::Align::left, 2.6F);
        const bool has_cloud = cloud != nullptr && cloud->status != CloudStatus::Unsupported && cloud->status != CloudStatus::None;
        const bool conflict = cloud != nullptr && cloud->status == CloudStatus::Conflict;
        const auto when = [&](std::int64_t time) -> std::string {
            if (time <= 0) return "—";
            const std::time_t value = static_cast<std::time_t>(time);
            char text[32]{};
            const auto *found = std::localtime(&value);
            if (found == nullptr) return "—";
            const std::tm local = *found;  // localtime reuses one buffer
            const std::time_t now = std::time(nullptr);
            const auto *today = std::localtime(&now);
            // Another year shows the year instead of the time: "31 Aug 2024" next to "770 days ago" (#50).
            const char *format = today != nullptr && today->tm_year != local.tm_year ? "%d %b %Y" : "%d %b %H:%M";
            return std::strftime(text, sizeof(text), format, &local) ? text : "—";
        };
        const auto size = [](std::uint64_t bytes) {
            char text[32];
            if (bytes >= 1000ull * 1000) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
            else std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1e3);
            return std::string(text);
        };
        const auto line = [&](const char *name, const std::string &remote, const std::string &local, gfx::Color remote_ink,
                              gfx::Color local_ink) {
            y += 58.0F;
            scene_.rounded_rect({left, y - 40.0F, right - left, 1.5F}, 0.0F, kText.with_alpha(0.14F));
            ui::text(scene_, fonts_.regular, name, left, y, 28.0F, label);
            ui::text(scene_, fonts_.semibold, remote, cloud_x, y, 28.0F, remote_ink);
            ui::text(scene_, fonts_.semibold, local, here_x, y, 28.0F, local_ink);
        };
        const std::uint64_t local_bytes = state.save_sizes.count(id) ? state.save_sizes.at(id) : 0;
        if (has_cloud)
        {
            line("Files", std::to_string(cloud->remote_files), std::to_string(cloud->local_files), kText, kText);
            line("Size", size(cloud->remote_bytes), size(cloud->local_bytes), kText, kText);
            const bool cloud_newer = cloud->remote_latest > cloud->local_latest;
            line("Newest change", when(static_cast<std::int64_t>(cloud->remote_latest)), when(static_cast<std::int64_t>(cloud->local_latest)),
                 conflict && cloud_newer ? newer : kText, conflict && !cloud_newer ? newer : kText);
            // Synced before Z3CloudPlay kept the time: unknown, not never.
            line("Last sync", cloud->last_sync > 0 ? when(cloud->last_sync) : cloud->first_sync ? "Never" : "—", "",
                 cloud->last_sync > 0 ? kText : absent, kText);
        }
        else
        {
            line("Size", "No Steam Cloud", local_bytes > 0 ? size(local_bytes) : "—", absent, kText);
        }
        if (!installed && kept != nullptr)
        {
            line("Other data", "—", short_size(kept->bytes) + " (Windows folder, caches)", absent, kText);
        }

        // What is going on, at the right.
        std::string note;
        const auto status = cloud == nullptr ? CloudStatus::None : cloud->status;
        if (cloud != nullptr && (status == CloudStatus::Checking || status == CloudStatus::Syncing || cloud->queued))
            note = "Z3CloudPlay is checking this game with Steam Cloud. Its actions come back when that is done; you can leave this page meanwhile.";
        else if (cloud != nullptr && cloud->after_play)
            note = "The game may be running. Z3CloudPlay syncs its saves once it stops.";
        else if (status == CloudStatus::Conflict)
            note = cloud->first_sync ? "Never synced on this PS5, and both sides already have saves. Pick which to keep; the other side is backed up first."
                   : cloud->first_overwrite
                       ? "Saves here would replace the ones in Steam Cloud, and this PS5 hasn't uploaded any for this game before. If the game didn't show your progress, keep the Cloud saves. Either side is backed up first."
                       : "Both changed since the last sync. Pick which saves to keep; saves replaced here are backed up first.";
        else if (status == CloudStatus::NeedsAccount)
            note = "Saves here were never synced with this Steam account. Confirm to upload them.";
        else if (status == CloudStatus::Failed)
            note = cloud->message + " Z3CloudPlay tries again on the next check.";
        else if (status == CloudStatus::Synced || status == CloudStatus::UpToDate)
            note = "Matches Steam Cloud.";
        else if (status == CloudStatus::Unsupported)
            note = "This game doesn't use Steam Cloud. Its saves exist only on this PS5.";
        else if (status == CloudStatus::Waiting)
            note = installed ? cloud->message + ". Steam Cloud has saves this console doesn't; Sync now or Play brings them down."
                             : "Steam Cloud has newer saves. They come down when you reinstall the game or press Sync now.";
        else
            note = "Not checked yet. Opening Saves or Sync now checks it.";
        if (!installed)
            note = "The game was uninstalled; its saves and settings were kept. " + note;
        ui::text(scene_, fonts_.display, ui::upper("What's going on"), 1240.0F, 420.0F, 22.0F, label, gfx::Align::left, 2.6F);
        ui::paragraph(scene_, fonts_.regular, note, 1240.0F, 470.0F, 25.0F, 584.0F, 36.0F, kText.with_alpha(0.9F), 6);

        // Actions in a row.
        float x = kSafeX;
        for (const ViewRow &row : view.rows)
        {
            if (row.kind != RowKind::Action)
                continue;
            const float height = 60.0F;
            const float width = fonts_.display.measure(row.title, 26.0F) +
                                (row.value.empty() ? 0.0F : fonts_.semibold.measure(row.value, 22.0F) + 18.0F) + 56.0F;
            const bool focused = view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size() &&
                                 view.rows[static_cast<std::size_t>(view.focused_row)].key == row.key;
            if (focused && route_focus_)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, focused && route_focus_);
            const gfx::Rect button{x, 860.0F - height * 0.5F, width, height};
            const bool primary = &row == &view.rows.front() && !row.dangerous;
            if (row.dangerous)
            {
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                focus_glow(scene_, button, amount);
                if (!row.enabled)
                    scene_.push_opacity(0.45F);
                scene_.bordered_rect(button, 8.0F, kRed.with_alpha(0.08F), 2.0F, kRed.with_alpha(0.55F));
                const float content = fonts_.display.measure(row.title, 26.0F) +
                                      (row.value.empty() ? 0.0F : fonts_.semibold.measure(row.value, 22.0F) + 18.0F);
                const float text_x = button.cx() - content * 0.5F;
                ui::text(scene_, fonts_.display, row.title, text_x, button.cy() + 9.0F, 26.0F, gfx::Color::rgb(0xF1B0AA));
                if (!row.value.empty())
                    ui::text(scene_, fonts_.semibold, row.value, text_x + content, button.cy() + 8.0F, 22.0F, kSecondary,
                             gfx::Align::right);
                if (!row.enabled)
                    scene_.pop_opacity();
                focus_ring(scene_, button, amount);
                scene_.pop_transform();
            }
            else
                mac_button(scene_, button, row.title, primary, amount, !row.enabled, row.value);
            x += width + 26.0F;
        }
    }

    void draw_details(ui::Canvas &, const ViewData &view, const State &state)
    {
        const GameId id = state.navigation.detail_id.value_or(state.navigation.selected_game_id);
        const Game *game = game_by_id(state, id);
        const gfx::Rect hero{0.0F, 0.0F, kWidth, 620.0F};
        const bool live_art = !state.simulation_mode && live_artwork_ && !state.account.id.empty() && id != 0;
        const std::uint32_t hero_texture = state.simulation_mode ? hero_ :
            live_art ? live_artwork_->TextureFor(id, steam_art(id, "library_hero.jpg")) : 0;
        if (hero_texture != 0)
            scene_.image(hero_texture, hero, gfx::kFullUv, kText);
        else
            scene_.gradient_rect(hero, 0.0F, id_color(id), kBackground);
        scene_.gradient_rect({0.0F, 0.0F, kWidth, 186.0F}, 0.0F, kBackground.with_alpha(0.4F), kBackground.with_alpha(0.0F));
        scene_.gradient_rect({0.0F, 465.0F, kWidth, 156.0F}, 0.0F, kBackground.with_alpha(0.0F), kBackground);

        // Where Circle returns, over the hero.
        const Route origin = state.navigation.detail_origin;
        const char *back = origin == Route::Home ? "Home" : origin == Route::Downloads ? "Downloads" :
                           origin == Route::Settings ? "Settings" : "Library";
        ui::draw_button(scene_, fonts_, ui::GlyphStyle::mono(kText, kBackground), ui::Button::circle, kSafeX, 74.0F, 36.0F);
        ui::text(scene_, fonts_.semibold, back, kSafeX + 48.0F, 82.0F, 22.0F, kText);

        const std::string title = game == nullptr ? view.title : game->title;
        std::uint32_t logo_width = 0;
        std::uint32_t logo_height = 0;
        const std::uint32_t logo = live_art ?
            live_artwork_->TextureFor(id, steam_art(id, "logo.png"), &logo_width, &logo_height) : 0;
        if (logo != 0 && logo_width > 0 && logo_height > 0)
        {
            const float fit_scale = std::min(460.0F / static_cast<float>(logo_width),
                                             160.0F / static_cast<float>(logo_height));
            const float w = static_cast<float>(logo_width) * fit_scale;
            const float h = static_cast<float>(logo_height) * fit_scale;
            const gfx::Rect box{kSafeX, 430.0F + (160.0F - h) * 0.5F, w, h};
            scene_.shadow(box.inset(10.0F), 20.0F, 30.0F, gfx::Color::rgb(0x000000, 0.35F));
            scene_.image(logo, box, gfx::kFullUv, kText);
        }
        else
        {
            // Without a logo the title stands in, as the macOS artwork fallback does.
            const ui::FontRef &title_face = font_for(title, fonts_.display);
            const bool long_title = title_face.font->measure(title, 72.0F) > 900.0F;
            ui::paragraph(scene_, title_face, title, kSafeX, long_title ? 520.0F : 586.0F, 72.0F, 900.0F, 70.0F,
                          kText, 2);
        }

        // Actions in a row, all one size; Install, Play or a running download is the primary
        // one, filled with the accent whatever its state.
        float x = kSafeX;
        for (const ViewRow &row : view.rows)
        {
            if (row.kind != RowKind::Action && row.kind != RowKind::Toggle)
                continue;
            const bool primary = row.key == "details-install" || row.key == "details-play" ||
                                 row.key == "details-download";
            const bool favorite = row.key == "details-favorite";
            const std::string label = favorite ? (row.selected ? "Favorited" : "Favorite") : row.title;
            // A download shows its percentage from the start ("0%" before the first bytes), and Play
            // while it prepares a known amount; other job states read as words ("Queued", "Paused").
            const bool percent = (row.key == "details-download" && (row.progress > 0.0F || row.value == "downloading")) ||
                                 (row.key == "details-play" && row.progress > 0.0F);
            std::string detail = percent ? std::to_string(static_cast<int>(row.progress * 100.0F)) + "%" : row.value;
            if (row.key == "details-download" && !percent && !detail.empty())
                detail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(detail[0])));
            const float height = 60.0F;
            // Favorite is a square icon button: a star, filled while the game is a favorite.
            // A percentage is sized for "100%" so the button keeps its width as it counts up, and
            // Install for the largest size it shows, so it keeps its width once Steam gives the size.
            const auto wider = [this](const std::string &a, const std::string &b) {
                return fonts_.semibold.measure(a, 22.0F) >= fonts_.semibold.measure(b, 22.0F) ? a : b;
            };
            const std::string widest = percent ? std::string("100%")
                                     : row.key == "details-install" ? wider(detail, "999.9 GB") : detail;
            const float width = favorite ? height :
                fonts_.display.measure(label, 26.0F) +
                    (widest.empty() ? 0.0F : fonts_.semibold.measure(widest, 22.0F) + 18.0F) + 56.0F;
            const bool focused = view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size() &&
                                 view.rows[static_cast<std::size_t>(view.focused_row)].key == row.key;
            if (focused && route_focus_)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, focused && route_focus_);
            const gfx::Rect button{x, 682.0F - height * 0.5F, width, height};
            if ((row.key == "details-play" || row.key == "details-download") && row.progress > 0.0F)
            {
                // Downloading, preparing or removing with a known fraction: the button is not
                // dimmed; it is outlined and the accent fills it from the left as the work advances.
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                focus_glow(scene_, button, amount);
                scene_.rounded_rect(button, 8.0F, kAccent.with_alpha(0.14F));
                scene_.rounded_rect({button.x, button.y, button.w * std::clamp(row.progress, 0.0F, 1.0F), button.h}, 8.0F,
                                    kAccent.with_alpha(0.55F));
                scene_.bordered_rect(button, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kAccent.with_alpha(0.7F));
                const float detail_width = widest.empty() ? 0.0F : fonts_.semibold.measure(widest, 22.0F) + 18.0F;
                const float content = std::min(button.w - 32.0F, fonts_.display.measure(label, 26.0F) + detail_width);
                const float text_x = button.cx() - content * 0.5F;
                ui::text(scene_, fonts_.display, label, text_x, button.cy() + 26.0F * 0.36F, 26.0F, kText);
                if (!detail.empty())
                    ui::text(scene_, fonts_.semibold, detail, text_x + content, button.cy() + 8.0F, 22.0F,
                             kText.with_alpha(0.8F), gfx::Align::right);
                focus_ring(scene_, button, amount);
                scene_.pop_transform();
            }
            else if (favorite)
            {
                mac_button(scene_, button, "", false, amount, !row.enabled);
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                if (row.selected)
                    scene_.star(button.cx(), button.cy() + 1.0F, 16.0F, kAccent);
                else
                    scene_.star(button.cx(), button.cy() + 1.0F, 16.0F, kText, 2.5F);
                scene_.pop_transform();
            }
            else
                mac_button(scene_, button, label, primary, amount, !row.enabled, detail);
            // Wide enough that a focused button's ring (9 px out, 4 px stroke, 4% larger) clears its neighbours.
            x += width + 26.0F;
        }

        // Left column: install status, the description and genres.
        float y = 774.0F;
        if (!state.simulation_mode)
        {
            std::string note;
            gfx::Color tone = kSecondary;
            for (const ViewRow &row : view.rows)
            {
                if (row.key.rfind("details-", 0) != 0 || row.key == "details-kept")
                    continue;
                if (row.kind == RowKind::Status)
                {
                    note = row.title;
                    if (!row.value.empty())
                        note += " · " + row.value;
                    if (!row.subtitle.empty())
                        note += " · " + row.subtitle;
                    tone = row.title == "Storage disconnected" ? kAmber : kSecondary;
                    break;
                }
                if (row.key == "details-install" && !row.subtitle.empty())
                {
                    note = row.subtitle.rfind("To ", 0) == 0 ? "Installs to " + row.subtitle.substr(3) : row.subtitle;
                    break;
                }
                // The Download button already shows the state, or the percentage while it downloads;
                // only a state hidden behind a percentage (paused, verifying) is said here.
                if (row.key == "details-download")
                {
                    if (row.progress > 0.0F && row.value != "downloading")
                        note = "Download " + row.value;
                    break;
                }
            }
            if (!note.empty())
            {
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, note, 22.0F, 1128.0F), kSafeX, y + 4.0F, 22.0F,
                         tone);
                y += 42.0F;
            }
            // Saves kept from a previous install, under the install note.
            for (const ViewRow &row : view.rows)
            {
                if (row.key != "details-kept")
                    continue;
                std::string kept_note = row.title;
                if (!row.value.empty())
                    kept_note += " · " + row.value;
                if (!row.subtitle.empty())
                    kept_note += " · " + row.subtitle;
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, kept_note, 22.0F, 1128.0F), kSafeX, y + 4.0F,
                         22.0F, kSecondary);
                y += 42.0F;
            }
            // The game's PS5 title state, under the install note.
            for (const ViewRow &row : view.rows)
            {
                if (row.key != "details-runtime" || row.subtitle.empty())
                    continue;
                // Its percentage is on the Play button.
                const std::string runtime_note = "PS5 title · " + row.value + " · " + row.subtitle;
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, runtime_note, 22.0F, 1128.0F), kSafeX, y + 4.0F,
                         22.0F, kSecondary);
                y += 42.0F;
            }
            // What Play will do first, or why it stopped ("Could not start: …"), under the PS5 title note.
            std::string runtime_reason;
            for (const ViewRow &row : view.rows)
                if (row.key == "details-runtime")
                    runtime_reason = row.subtitle;
            for (const ViewRow &row : view.rows)
            {
                if (row.key != "details-play" || row.subtitle.empty() || row.subtitle == runtime_reason)
                    continue;
                const bool stopped = row.subtitle.rfind("Could not start", 0) == 0 ||
                                     row.subtitle.rfind("The game could not", 0) == 0;
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, row.subtitle, 22.0F, 1128.0F), kSafeX, y + 4.0F,
                         22.0F, stopped ? kAmber : kSecondary);
                y += 42.0F;
            }
        }
        const std::string description = game == nullptr || game->description.empty() ?
                                            "Description unavailable." : game->description;
        const auto detail_scroll = state.navigation.scroll_offsets.find("details-content");
        // The left column ends above the footer hints. Live pages carry status lines above the
        // description, so it shows two lines a page (the model pages by the same size).
        constexpr float column_bottom = 975.0F;
        const int description_lines = state.simulation_mode ? 3 : 2;
        const std::size_t page_size = DetailsDescriptionPage(state.simulation_mode);
        const int requested_page = detail_scroll == state.navigation.scroll_offsets.end() ? 0 :
                                       std::max(0, detail_scroll->second);
        const int max_page = description.empty() ? 0 :
                                 static_cast<int>((description.size() - 1) / page_size);
        const int page = std::min(requested_page, max_page);
        const std::size_t offset = utf8_boundary(description, static_cast<std::size_t>(page) * page_size);
        const std::size_t end = utf8_boundary(description, std::min(description.size(), offset + page_size));
        std::string page_text = description.substr(offset, end - offset);
        if (offset > 0)
            page_text.insert(0, "…");
        if (offset + page_size < description.size())
            page_text += "…";
        const ui::FontRef &body = font_for(page_text, fonts_.regular);
        const float text_bottom = ui::paragraph(scene_, body, page_text, kSafeX, y + 20.0F, 26.0F, 1128.0F, 37.0F,
                                                gfx::Color::rgb(0xD6D0C8), description_lines);
        y = std::max(y + 20.0F + 37.0F, text_bottom) + 18.0F;
        if (description.size() > page_size)
        {
            ui::text(scene_, fonts_.regular, "L2/R2 Read more · page " + std::to_string(page + 1) + " of " +
                         std::to_string(max_page + 1), kSafeX, y + 6.0F, 18.0F, kMuted);
            y += 30.0F;
        }
        if (game != nullptr && y + 42.0F <= column_bottom)
        {
            float tag_x = kSafeX;
            for (std::size_t index = 0; index < game->genres.size() && index < 4; ++index)
            {
                const std::string &genre = game->genres[index];
                const float width = fonts_.semibold.measure(genre, 20.0F) + 32.0F;
                if (tag_x + width > kSafeX + 1128.0F)
                    break;
                scene_.rounded_rect({tag_x, y, width, 42.0F}, 6.0F, kText.with_alpha(0.1F));
                ui::text(scene_, fonts_.semibold, genre, tag_x + 16.0F, y + 28.0F, 20.0F, kText);
                tag_x += width + 12.0F;
            }
        }

        // Right column: the facts.
        if (game == nullptr)
            return;
        const Installation *installation = nullptr;
        const auto found_installation = std::find_if(
            state.installations.begin(), state.installations.end(), [&game, &state](const Installation &item) {
                return item.game_id == game->id && (item.installed_bytes > 0 || !state.simulation_mode);
            });
        if (found_installation != state.installations.end())
            installation = &*found_installation;
        std::string location = "Not installed";
        if (installation != nullptr)
        {
            const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                             [installation](const Volume &item) {
                                                 return item.id == installation->volume_id;
                                             });
            location = volume == state.volumes.end() ? installation->volume_id : volume->label;
            if (!installation->available || (volume != state.volumes.end() && !volume->available))
                location += " · Disconnected";
        }
        const auto rating = state.library.ratings.find(game->id);
        const Compatibility compatibility = rating == state.library.ratings.end() ?
                                                Compatibility::Untested : rating->second;
        const bool installed = installation != nullptr && installation->installed_bytes > 0;
        // One size for a game: what it takes on the drive (installed, or the install estimate).
        const std::string size = installed ? short_size(installation->installed_bytes) :
            game->install_bytes ? short_size(*game->install_bytes) : std::string("Unknown");
        const std::string playtime = game->playtime_minutes && *game->playtime_minutes > 0 ?
            playtime_label(game->playtime_minutes) : std::string("Never played");
        std::string compat = rating_label(compatibility);
        if (const auto dot = compat.find(" · "); dot != std::string::npos)
            compat.resize(dot);
        std::string controller = game->controller_support.empty() ? "Unknown" : game->controller_support;
        controller.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(controller.front())));
        constexpr float left = 1304.0F;
        constexpr float right = 1584.0F;
        detail_cell("Playtime", playtime, left, 754.0F, 240.0F);
        detail_cell("Size", size, right, 754.0F, 240.0F);
        detail_cell("Store", game->source.empty() ? "Steam" : game->source, left, 830.0F, 240.0F);
        detail_cell("Location", location, right, 830.0F, 240.0F);
        detail_cell("Compatibility", compat, left, 906.0F, 240.0F);
        if (game->last_played)
            detail_cell("Last played", relative_time(game->last_played, state.clock_ticks), right, 906.0F, 240.0F);
        else
            detail_cell("Controller", controller, right, 906.0F, 240.0F);
    }

    void draw_session(ui::Canvas &, const ViewData &view, const State &state)
    {
        if (hero_ != 0)
            scene_.image(hero_, {0.0F, 0.0F, 1920.0F, 1080.0F}, gfx::kFullUv,
                         kText.with_alpha(0.42F));
        scene_.rounded_rect({0.0F, 0.0F, 1920.0F, 1080.0F}, 0.0F, kBackground.with_alpha(0.68F));
        const Game *game = game_by_id(state, state.session_game_id);
        section_label("Simulated game session", 96.0F, 82.0F);
        dynamic_text(game == nullptr ? std::string_view(view.title) : std::string_view(game->title),
                     96.0F, 256.0F, 64.0F, kText, 1000.0F);
        ui::text(scene_, fonts_.display, view.subtitle, 96.0F, 320.0F, 34.0F,
                 state.session == SessionState::Failed || state.session == SessionState::Crashed ? kRed : kSecondary);
        draw_actions(view, 96.0F, 428.0F, 540.0F);

        scene_.rounded_rect({1210.0F, 226.0F, 614.0F, 540.0F}, 16.0F, kPanel.with_alpha(0.94F));
        ui::text(scene_, fonts_.display, "Session controls", 1254.0F, 294.0F, 44.0F, kText);
        ui::paragraph(scene_, fonts_.regular,
                      state.session_error.empty() ? "This is a deterministic fixture. No game process is launched."
                                                  : state.session_error,
                      1254.0F, 352.0F, 25.0F, 522.0F, 36.0F,
                      state.session_error.empty() ? kSecondary : kAmber, 5);
    }

    void draw_persistent_status(ui::Canvas &, const State &state)
    {
        float y = 126.0F;
        for (const Notice &notice : state.notices)
        {
            if (!notice.persistent)
                continue;
            const float width = 560.0F;
            scene_.rounded_rect({kWidth - kSafeX - width, y, width, 54.0F}, 8.0F,
                                kAmber.with_alpha(0.16F));
            scene_.circle(kWidth - kSafeX - width + 25.0F, y + 27.0F, 6.0F, kAmber);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, notice.message, 21.0F, width - 64.0F),
                     kWidth - kSafeX - width + 44.0F, y + 35.0F, 21.0F, kText);
            y += 64.0F;
        }
    }

    void draw_footer(ui::Canvas &, const ViewData &view)
    {
        std::vector<std::string> fallback;
        const std::vector<std::string> &labels = view.legends.empty() ? fallback : view.legends;
        std::vector<ui::Hint> hints;
        hints.reserve(std::max<std::size_t>(labels.size(), 2));
        // L1/R1 switch sections unless the view gives them another job ("L1/R1 Jump").
        std::string shoulder = "Sections";
        if (labels.empty())
        {
            hints.push_back({ui::Button::cross, "Select"});
            hints.push_back({ui::Button::circle, "Back"});
        }
        else
        {
            for (std::size_t index = 0; index < labels.size() && hints.size() < 5; ++index)
            {
                if (labels[index].find("Sections") != std::string::npos)
                    continue;
                if (labels[index].rfind("L1/R1 ", 0) == 0)
                {
                    shoulder = labels[index].substr(6);
                    continue;
                }
                hints.push_back({legend_button(labels[index], index), labels[index].c_str()});
            }
            // Circle is already spoken for by "Circle …" legends ("Circle Clear search") and Back/Cancel ones.
            const bool has_back = std::any_of(labels.begin(), labels.end(), [](const std::string &label) {
                return label.rfind("Circle", 0) == 0 || label.find("Back") != std::string::npos ||
                       label.find("Cancel") != std::string::npos;
            });
            if (!has_back)
                hints.push_back({ui::Button::circle, "Back"});
        }
        hints.push_back({ui::Button::l1, shoulder.c_str(), ui::Button::r1});
        ui::HintLayout layout;
        layout.size = 38.0F;
        layout.text_size = 22.0F;
        layout.cy = kFooterY + 20.0F;
        layout.item_gap = 28.0F;
        layout.font = &fonts_.semibold;
        ui::GlyphStyle glyphs = ui::GlyphStyle::mono(kText, kBackground);
        ui::draw_hints(scene_, fonts_, glyphs, hints.data(), static_cast<int>(hints.size()), 96.0F,
                       false, layout);
        if (view.show_simulation_label)
        {
            scene_.rounded_rect({1652.0F, 989.0F, 172.0F, 34.0F}, 17.0F, kText.with_alpha(0.08F));
            ui::text(scene_, fonts_.semibold, "UI SIMULATION", 1738.0F, 1012.0F, 16.0F, kSecondary,
                     gfx::Align::center, 1.2F);
        }
    }

    void draw_overlays(const State &state, const ViewData &view)
    {
        ui::Canvas overlay{overlay_, fonts_, renderer_.glass_texture(), clock_};
        if (state.editor.active)
        {
            draw_editor_overlay(overlay, state);
            return;
        }
        if (!state.navigation.modals.empty())
        {
            const ModalEntry &modal = state.navigation.modals.back();
            if (modal.destructive || modal.id == "confirmation")
                draw_modal_overlay(modal, view);
            else
                draw_sheet_overlay(view);
            return;
        }
        if (view.surface != "home" && view.surface != "library" && view.surface != "downloads" &&
            view.surface != "settings" && view.surface != "details" && view.surface != "qr" &&
            view.surface != "startup" && view.surface != "session" && view.surface != "saves" &&
            view.surface != "save-details")
            draw_sheet_overlay(view);

        // Live notifications wait behind dialogs (they also stop counting down there).
        const bool hide_transient = !state.simulation_mode &&
            (!state.navigation.modals.empty() || state.editor.active);
        float y = 118.0F;
        for (const Notice &notice : state.notices)
        {
            if (notice.persistent || hide_transient)
                continue;
            overlay_.shadow({1288.0F, y, 536.0F, 84.0F}, 10.0F, 28.0F, gfx::Color::rgb(0x000000, 0.6F));
            overlay_.rounded_rect({1288.0F, y, 536.0F, 84.0F}, 10.0F, kPanel);
            overlay_.circle(1320.0F, y + 42.0F, 7.0F, notice.failure ? kRed : kAccent);
            ui::text(overlay_, fonts_.semibold, fit(fonts_.semibold, notice.message, 22.0F, 440.0F),
                     1342.0F, y + 36.0F, 22.0F, kText);
            const std::string &second = notice.detail.empty() ? notice.action : notice.detail;
            if (!second.empty())
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, second, 18.0F, 460.0F), 1342.0F, y + 62.0F,
                         18.0F, notice.detail.empty() ? kAccent : kSecondary);
            y += 98.0F;
        }
    }

    void draw_editor_overlay(ui::Canvas &canvas, const State &state)
    {
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, gfx::Color::rgb(0x000000, 0.68F));
        overlay_.rounded_rect({420.0F, 116.0F, 1080.0F, 820.0F}, 16.0F, kPanel);
        ui::text(overlay_, fonts_.display,
                 state.editor.purpose.empty() ? "Enter text" : state.editor.purpose,
                 476.0F, 188.0F, 46.0F, kText);
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        const gfx::Rect field{476.0F, 232.0F, 968.0F, 76.0F};
        const bool secret_entry = state.editor.purpose == "Secure Session Passphrase";
        // Render only a fixed mask; never put secret text into the GPU draw list.
        const std::string display_value = secret_entry
            ? std::string(std::min<std::size_t>(state.editor.value.size(), 64), '*')
            : state.editor.value;
        painter.field(field, display_value, false,
                      {0.0F, 0.0F, false});
        const std::size_t caret = std::min(state.editor.caret, state.editor.value.size());
        const std::string caret_text = secret_entry
            ? std::string(std::min<std::size_t>(caret, 64), '*')
            : state.editor.value.substr(0, caret);
        const float caret_x = field.x + 22.0F +
                              fonts_.regular.measure(caret_text, 24.0F);
        overlay_.rounded_rect({std::min(caret_x, field.x + field.w - 24.0F), field.y + 18.0F,
                               2.0F, field.h - 36.0F},
                              1.0F, kAccent);
        keyboard_.style.reduced_motion = state.reduced_motion;
        keyboard_.set_length(static_cast<int>(state.editor.value.size()));
        keyboard_.set_shift(state.editor.shifted ? ui::KeyboardShift::lock : ui::KeyboardShift::off);
        keyboard_.set_layout(state.editor.symbols ? 1 : 0);
        keyboard_.set_focus(state.editor.focus_row, state.editor.focus_key, true);
        keyboard_.set_active(true);
        keyboard_.update(1.0F / 60.0F);
        keyboard_.draw(canvas);
    }

    void draw_modal_overlay(const ModalEntry &modal, const ViewData &view)
    {
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, gfx::Color::rgb(0x000000, 0.72F));
        // Three buttons need the wide panel or their labels are cut.
        const float width = modal.destructive && view.rows.size() < 3 ? 820.0F : 960.0F;
        const gfx::Rect panel{(kWidth - width) * 0.5F, 254.0F, width, 500.0F};
        overlay_.shadow(panel, 16.0F, 40.0F, gfx::Color::rgb(0x000000, 0.65F));
        overlay_.rounded_rect(panel, 16.0F, kPanel);
        const float heading_bottom = ui::paragraph(
            overlay_, fonts_.display, modal.title, panel.x + 48.0F, panel.y + 76.0F,
            48.0F, panel.w - 96.0F, 54.0F, modal.destructive ? kAmber : kText, 3);
        const std::string body = !modal.body.empty() ? modal.body : view.subtitle;
        ui::paragraph(overlay_, fonts_.regular, body, panel.x + 48.0F, heading_bottom + 24.0F, 25.0F,
                      panel.w - 96.0F, 35.0F, kSecondary,
                      std::max(1, static_cast<int>((panel.y + 352.0F - heading_bottom - 24.0F) / 35.0F) + 1));
        const std::size_t count = std::max<std::size_t>(1, view.rows.size());
        const float button_width = (panel.w - 112.0F) / count;
        for (std::size_t index = 0; index < view.rows.size(); ++index)
        {
            std::string label = overlay_label(view.rows[index]);
            if (view.rows[index].action == "confirm-destructive")
            {
                if (modal.title.find("Uninstall") != std::string::npos)
                    label = "Uninstall";
                else if (modal.title.find("download") != std::string::npos)
                    label = "Cancel download";
                else
                    label = "Confirm";
            }
            const gfx::Rect button{panel.x + 48.0F + index * (button_width + 16.0F), panel.y + 388.0F,
                                   button_width, 68.0F};
            // A choice in the dialog (Delete saves too) says whether it is on.
            const bool toggle = view.rows[index].kind == RowKind::Toggle;
            mac_button(overlay_, button, label, view.rows[index].dangerous,
                       control_focus("modal-" + view.rows[index].key, static_cast<int>(index) == view.focused_row),
                       !view.rows[index].enabled, toggle ? (view.rows[index].selected ? "On" : "Off") : "", false,
                       kPanel);
        }
    }

    // The small label above a sheet's title names where the sheet belongs, never its surface id.
    static std::string sheet_eyebrow(const std::string &surface)
    {
        static const std::map<std::string, std::string> names = {
            {"choice-picker", "Game settings"}, {"preset-picker", "Game settings"}, {"button-mapping", "Game settings"},
            {"game-settings", "Z3CloudPlay"}, {"game-maintenance", "Z3CloudPlay"}, {"kept-data", "Settings"},
            {"job-actions", "Downloads"}, {"context", "Game"}, {"compatibility", "Game"}, {"collections", "Game"}, {"collections-manage", "Library"},
            {"filters", "Library"}, {"logs-controls", "Logs"}, {"setting-value", "Settings"}, {"diagnostics", "Settings"},
            {"notices", "Notifications"}, {"qr-controls", "Sign in"}, {"session-controls", "Simulation"},
            {"simulation-controls", "Simulation"}, {"scenarios", "Simulation"}};
        if (const auto found = names.find(surface); found != names.end())
            return found->second;
        std::string label = surface;
        std::replace(label.begin(), label.end(), '-', ' ');
        return label;
    }

    void draw_sheet_overlay(const ViewData &view)
    {
        const float ease = overlay_transition_.enter();
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F,
                              gfx::Color::rgb(0x000000, 0.58F * ease));
        const bool wide = view.surface.find("settings") != std::string::npos ||
                          view.surface.find("profile") != std::string::npos ||
                          view.surface == "choice-picker" || view.surface == "preset-picker" ||
                          view.surface == "button-mapping" || view.surface == "game-maintenance" ||
                          view.surface == "controller" || view.surface == "logs" ||
                          view.surface == "storage" || view.surface == "install" || view.surface == "kept-data";
        const float width = overlay_transition_.panel_width(wide ? 960.0F : 640.0F);
        const gfx::Rect panel{kWidth - width + (1.0F - ease) * width, 0.0F, width, kHeight};
        overlay_.shadow({panel.x - 18.0F, 0.0F, panel.w + 18.0F, panel.h}, 0.0F, 36.0F,
                        gfx::Color::rgb(0x000000, 0.75F));
        overlay_.rounded_rect(panel, 0.0F, kPanel);
        overlay_.push_opacity(overlay_transition_.content());
        section_overlay(sheet_eyebrow(view.surface), panel.x + 48.0F, 72.0F);
        ui::text(overlay_, fonts_.display, view.title, panel.x + 48.0F, 142.0F, 48.0F, kText);
        if (!view.subtitle.empty())
            ui::paragraph(overlay_, fonts_.regular, view.subtitle, panel.x + 48.0F, 190.0F, 23.0F,
                          panel.w - 96.0F, 32.0F, kSecondary, view.surface == "install" ? 6 : 3);
        if (view.surface == "storage")
            draw_storage_overlay_rows(view, {panel.x + 40.0F, 246.0F, panel.w - 80.0F, 710.0F});
        else if (view.surface == "logs")
            draw_log_overlay(view, {panel.x + 40.0F, 246.0F, panel.w - 80.0F, 710.0F});
        else if (view.surface == "controller")
            draw_controller_overlay(view, {panel.x + 40.0F, 236.0F, panel.w - 80.0F, 720.0F});
        else
            draw_overlay_rows(view, {panel.x + 40.0F, view.surface == "install" ? 414.0F : 292.0F,
                                     panel.w - 80.0F, view.surface == "install" ? 528.0F : 650.0F});
        if (view.comparison)
            draw_preset_comparison(*view.comparison, {72.0F, 150.0F, panel.x - 144.0F, 760.0F}, ease);
        overlay_.pop_opacity();
    }

    // Preset chooser: what the focused preset changes, beside the sheet (Current → This preset).
    void draw_preset_comparison(const ViewData::Comparison &comparison, const gfx::Rect &bounds, float ease)
    {
        if (bounds.w < 360.0F)
            return;
        const gfx::Rect card{bounds.x, bounds.y, bounds.w, bounds.h};
        overlay_.shadow(card, 14.0F, 32.0F, gfx::Color::rgb(0x000000, 0.55F * ease));
        overlay_.rounded_rect(card, 14.0F, kPanel.with_alpha(0.96F * ease));
        float y = card.y + 64.0F;
        ui::text(overlay_, fonts_.display, fit(fonts_.display, comparison.title, 40.0F, card.w - 96.0F), card.x + 48.0F, y,
                 40.0F, kText);
        y += 18.0F;
        if (!comparison.hint.empty())
            y = ui::paragraph(overlay_, fonts_.regular, comparison.hint, card.x + 48.0F, y + 24.0F, 21.0F,
                              card.w - 96.0F, 29.0F, kSecondary, 3);
        y += 36.0F;
        const float name_w = card.w * 0.34F;
        const float column_w = (card.w - 96.0F - name_w - 40.0F) * 0.5F;
        const float current_x = card.x + 48.0F + name_w;
        const float arrow_x = current_x + column_w + 20.0F;
        const float preset_x = arrow_x + 20.0F;
        ui::text(overlay_, fonts_.semibold, "SETTING", card.x + 48.0F, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        ui::text(overlay_, fonts_.semibold, "CURRENT", current_x, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        ui::text(overlay_, fonts_.semibold, "THIS PRESET", preset_x, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        y += 16.0F;
        for (const auto &row : comparison.rows)
        {
            if (y + 52.0F > card.y + card.h - 80.0F)
                break;
            overlay_.rounded_rect({card.x + 40.0F, y + 6.0F, card.w - 80.0F, 1.0F}, 0.0F, kText.with_alpha(0.10F));
            const bool changes = row[1] != row[2];
            ui::text(overlay_, fonts_.semibold, fit(fonts_.semibold, row[0], 21.0F, name_w - 16.0F), card.x + 48.0F,
                     y + 40.0F, 21.0F, kText);
            ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row[1], 20.0F, column_w), current_x, y + 40.0F, 20.0F,
                     kSecondary);
            if (changes)
            {
                // The UI fonts have no arrow glyph: draw one.
                const float cy = y + 33.0F;
                overlay_.line(arrow_x - 8.0F, cy, arrow_x + 7.0F, cy, 2.0F, kAccent);
                overlay_.line(arrow_x + 2.0F, cy - 5.0F, arrow_x + 8.0F, cy, 2.0F, kAccent);
                overlay_.line(arrow_x + 2.0F, cy + 5.0F, arrow_x + 8.0F, cy, 2.0F, kAccent);
            }
            ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row[2], 20.0F, column_w), preset_x, y + 40.0F, 20.0F,
                     changes ? kAccent : kSecondary);
            y += 56.0F;
        }
        ui::paragraph(overlay_, fonts_.regular, comparison.note, card.x + 48.0F, card.y + card.h - 56.0F, 19.0F,
                      card.w - 96.0F, 26.0F, kSecondary, 2);
    }

    void draw_storage_overlay_rows(const ViewData &view, const gfx::Rect &bounds)
    {
        if (current_state_ == nullptr)
            return;
        float y = bounds.y;
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        for (const ViewRow &row : view.rows)
        {
            if (row.action.rfind("volume-", 0) != 0)
                continue;
            const std::string id = row.action.substr(7);
            const auto found = std::find_if(current_state_->volumes.begin(), current_state_->volumes.end(),
                                            [&id](const Volume &volume) { return volume.id == id; });
            if (found == current_state_->volumes.end())
                continue;
            const Volume &volume = *found;
            const std::uint64_t unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
            const std::uint64_t free = unused - std::min(volume.reserved_bytes, unused);
            const int row_index = static_cast<int>(&row - view.rows.data());
            const gfx::Rect card{bounds.x, y, bounds.w, 156.0F};
            const float amount = control_focus("volume-" + row.key, row_index == view.focused_row);
            focus_glow(overlay_, card, amount);
            overlay_.rounded_rect(card, 8.0F, kPanel);
            overlay_.rounded_rect(card, 8.0F, kText.with_alpha(row.selected ? 0.10F : 0.04F));
            focus_ring(overlay_, card, amount);
            ui::text(overlay_, fonts_.display, volume.label, card.x + 22.0F, card.y + 38.0F,
                     27.0F, row.enabled ? kText : kMuted);
            const bool is_default = !current_state_->simulation_mode && volume.id == current_state_->default_volume_id;
            ui::text(overlay_, fonts_.regular, is_default && row.enabled ? "Default · " + row.subtitle : row.subtitle,
                     card.x + card.w - 22.0F, card.y + 38.0F, 18.0F, row.enabled ? kSecondary : kAmber,
                     gfx::Align::right);
            if (!volume.capacity_known)
            {
                ui::text(overlay_, fonts_.regular, "Used by games  " + short_size(volume.games_bytes),
                         card.x + 22.0F, card.y + 98.0F, 17.0F, kSecondary);
                if (volume.reserved_bytes > 0)
                    ui::text(overlay_, fonts_.regular, "Reserved by queue  " + short_size(volume.reserved_bytes),
                             card.x + card.w - 22.0F, card.y + 98.0F, 17.0F, kSecondary, gfx::Align::right);
                ui::text(overlay_, fonts_.regular, "Free space unavailable",
                         card.x + 22.0F, card.y + 128.0F, 17.0F, kMuted);
                y += 174.0F;
                continue;
            }
            const float fraction = volume.capacity_bytes == 0 ? 0.0F :
                static_cast<float>(volume.capacity_bytes - free) / static_cast<float>(volume.capacity_bytes);
            overlay_.rounded_rect({card.x + 22.0F, card.y + 58.0F, card.w - 44.0F, 8.0F}, 4.0F,
                                  kText.with_alpha(0.14F));
            overlay_.rounded_rect({card.x + 22.0F, card.y + 58.0F,
                                   (card.w - 44.0F) * std::clamp(fraction, 0.0F, 1.0F), 8.0F},
                                  4.0F, volume.available ? kAccent : kMuted);
            ui::text(overlay_, fonts_.regular, "Capacity  " + short_size(volume.capacity_bytes),
                     card.x + 22.0F, card.y + 98.0F, 17.0F, kSecondary);
            ui::text(overlay_, fonts_.regular, "Used  " + short_size(volume.used_bytes),
                     card.x + 22.0F, card.y + 128.0F, 17.0F, kSecondary);
            ui::text(overlay_, fonts_.regular, "Reserved  " + short_size(volume.reserved_bytes),
                     card.x + card.w - 22.0F, card.y + 98.0F, 17.0F, kSecondary,
                     gfx::Align::right);
            ui::text(overlay_, fonts_.semibold, "Free  " + short_size(free),
                     card.x + card.w - 22.0F, card.y + 128.0F, 17.0F, kText,
                     gfx::Align::right);
            y += 174.0F;
        }
        const auto cancel = std::find_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
            return row.action == "cancel";
        });
        if (cancel != view.rows.end() && y + 70.0F < bounds.y + bounds.h)
        {
            const int index = static_cast<int>(std::distance(view.rows.begin(), cancel));
            painter.row({bounds.x, y, bounds.w, 66.0F}, "Cancel", "", 0.0F,
                        {index == view.focused_row ? 1.0F : 0.0F, 0.0F, false});
        }
    }

    void draw_log_overlay(const ViewData &view, const gfx::Rect &bounds)
    {
        overlay_.rounded_rect(bounds, 8.0F, kBackground.with_alpha(0.72F));
        if (current_state_ == nullptr)
            return;
        const State &state = *current_state_;
        const gfx::Rect viewport{bounds.x + 20.0F, bounds.y + 20.0F,
                                 bounds.w - 40.0F, bounds.h - 132.0F};
        overlay_.rounded_rect(viewport, 6.0F, kPanel.with_alpha(0.48F));

        if (state.log_state == LogState::Loading)
        {
            overlay_.circle(viewport.x + 34.0F, viewport.y + 44.0F, 7.0F, kAccent);
            ui::text(overlay_, fonts_.semibold, "Loading fixture logs…", viewport.x + 58.0F,
                     viewport.y + 52.0F, 22.0F, kText);
            overlay_.rounded_rect({viewport.x + 24.0F, viewport.y + 78.0F,
                                   viewport.w - 48.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
            const float pulse = 0.18F + 0.22F * (0.5F + 0.5F * std::sin(clock_ * 5.0F));
            overlay_.rounded_rect({viewport.x + 24.0F, viewport.y + 78.0F,
                                   (viewport.w - 48.0F) * pulse, 5.0F}, 2.5F, kAccent);
        }
        else if (state.log_state == LogState::Failed)
        {
            ui::text(overlay_, fonts_.display, "Logs unavailable", viewport.x + 24.0F,
                     viewport.y + 54.0F, 30.0F, kRed);
            const auto error = std::find_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
                return row.key == "logs-error";
            });
            const std::string_view message = error == view.rows.end() || error->subtitle.empty() ?
                                                 (state.simulation_mode ? std::string_view("The simulated log source failed.")
                                                                        : std::string_view(state.log_context)) :
                                                 std::string_view(error->subtitle);
            dynamic_overlay_text(message, viewport.x + 24.0F, viewport.y + 96.0F, 20.0F,
                                 kSecondary, viewport.w - 48.0F);
        }
        else if (state.log_lines.empty())
        {
            ui::text(overlay_, fonts_.display, "No log entries", viewport.cx(),
                     viewport.y + viewport.h * 0.48F, 34.0F, kSecondary, gfx::Align::center);
            ui::text(overlay_, fonts_.regular,
                     state.simulation_mode ? "This fixture has no lines for the current context."
                                           : "Nothing has been logged here yet.",
                     viewport.cx(), viewport.y + viewport.h * 0.48F + 38.0F, 19.0F, kMuted,
                     gfx::Align::center);
        }
        else
        {
            constexpr int visible_lines = 8;
            const int begin = std::clamp(state.log_offset, 0,
                                         std::max(0, static_cast<int>(state.log_lines.size()) - visible_lines));
            const int end = std::min(static_cast<int>(state.log_lines.size()), begin + visible_lines);
            float y = viewport.y + 38.0F;
            for (int index = begin; index < end; ++index)
            {
                std::array<char, 12> number{};
                std::snprintf(number.data(), number.size(), "%03d", index + 1);
                ui::text(overlay_, fonts_.regular, number.data(), viewport.x + 22.0F, y,
                         16.0F, kMuted);
                dynamic_overlay_text(state.log_lines[static_cast<std::size_t>(index)],
                                     viewport.x + 72.0F, y, 18.0F, kText, viewport.w - 106.0F);
                y += 52.0F;
            }
            const std::string range = std::to_string(begin + 1) + "–" + std::to_string(end) +
                                      " of " + std::to_string(state.log_lines.size());
            ui::text(overlay_, fonts_.regular, range, viewport.x + viewport.w - 24.0F,
                     viewport.y + viewport.h - 18.0F, 16.0F, kMuted, gfx::Align::right);
            if (state.log_lines.size() > visible_lines)
            {
                const float track_h = viewport.h - 52.0F;
                const float thumb_h = std::max(36.0F, track_h * visible_lines /
                                                        static_cast<float>(state.log_lines.size()));
                const float travel = track_h - thumb_h;
                const float denominator = static_cast<float>(state.log_lines.size() - visible_lines);
                const float thumb_y = viewport.y + 18.0F + travel * begin / denominator;
                overlay_.rounded_rect({viewport.x + viewport.w - 7.0F, viewport.y + 18.0F,
                                       3.0F, track_h}, 1.5F, kText.with_alpha(0.10F));
                overlay_.rounded_rect({viewport.x + viewport.w - 7.0F, thumb_y, 3.0F, thumb_h},
                                      1.5F, kSecondary);
            }
        }

        std::vector<const ViewRow*> actions;
        for (const ViewRow &row : view.rows)
            if (!row.action.empty())
                actions.push_back(&row);
        if (!actions.empty())
        {
            const float width = std::min(260.0F, (bounds.w - 48.0F) / actions.size() - 10.0F);
            for (std::size_t index = 0; index < actions.size(); ++index)
            {
                const bool focused = state.navigation.focus_key == actions[index]->key;
                mac_button(overlay_, {bounds.x + 24.0F + index * (width + 16.0F), bounds.y + bounds.h - 82.0F, width, 58.0F},
                           overlay_label(*actions[index]), false, control_focus("log-" + actions[index]->key, focused),
                           !actions[index]->enabled, {}, false, kBackground);
            }
        }
    }

    void draw_controller_overlay(const ViewData &view, const gfx::Rect &bounds)
    {
        const bool connected = current_state_ != nullptr && current_state_->controller_connected;
        const ControllerState input = current_state_ == nullptr ? ControllerState{} :
                                                                  current_state_->controller;
        overlay_.rounded_rect(bounds, 12.0F, kBackground.with_alpha(0.52F));
        ui::text(overlay_, fonts_.display, connected ? "Controller connected" : "Controller disconnected",
                 bounds.x + bounds.w * 0.5F, bounds.y + 54.0F, 31.0F,
                 connected ? kGreen : kAmber, gfx::Align::center);
        const gfx::Color shell = connected ? kText.with_alpha(0.12F) : kMuted.with_alpha(0.10F);
        const float cx = bounds.x + bounds.w * 0.5F;
        const float cy = bounds.y + 292.0F;
        overlay_.rounded_rect({cx - 286.0F, cy - 136.0F, 572.0F, 250.0F}, 88.0F, shell);
        overlay_.circle(cx - 224.0F, cy + 96.0F, 92.0F, shell);
        overlay_.circle(cx + 224.0F, cy + 96.0F, 92.0F, shell);
        overlay_.rounded_rect({cx - 242.0F, cy - 164.0F, 158.0F, 24.0F}, 8.0F,
                              input.l2 > 0.5F ? kAccent : kText.with_alpha(0.16F));
        overlay_.rounded_rect({cx + 84.0F, cy - 164.0F, 158.0F, 24.0F}, 8.0F,
                              input.r2 > 0.5F ? kAccent : kText.with_alpha(0.16F));
        ui::text(overlay_, fonts_.semibold, "L2", cx - 163.0F, cy - 146.0F, 16.0F, kText,
                 gfx::Align::center);
        ui::text(overlay_, fonts_.semibold, "R2", cx + 163.0F, cy - 146.0F, 16.0F, kText,
                 gfx::Align::center);

        const auto stick = [this](float x, float y, float axis_x, float axis_y) {
            overlay_.circle(x, y, 48.0F, kBackground.with_alpha(0.72F));
            overlay_.circle(x + axis_x * 24.0F, y + axis_y * 24.0F, 22.0F,
                            (axis_x != 0.0F || axis_y != 0.0F) ? kAccent : kSecondary);
        };
        stick(cx - 112.0F, cy + 48.0F, input.left_x, input.left_y);
        stick(cx + 72.0F, cy + 48.0F, input.right_x, input.right_y);

        const auto face = [this](float x, float y, bool active, ui::Button button) {
            overlay_.circle(x, y, 23.0F, active ? kAccent : kBackground.with_alpha(0.78F));
            ui::draw_button(overlay_, fonts_,
                            ui::GlyphStyle::mono(active ? kBackground : kText, kBackground),
                            button, x - ui::button_width(button, 30.0F) * 0.5F, y, 30.0F);
        };
        face(cx + 200.0F, cy - 28.0F, input.triangle, ui::Button::triangle);
        face(cx + 248.0F, cy + 20.0F, input.circle, ui::Button::circle);
        face(cx + 200.0F, cy + 68.0F, input.cross, ui::Button::cross);
        face(cx + 152.0F, cy + 20.0F, input.square, ui::Button::square);

        const gfx::Color dpad = (input.left_x != 0.0F || input.left_y != 0.0F) ? kAccent : kSecondary;
        overlay_.rounded_rect({cx - 232.0F, cy - 4.0F, 92.0F, 28.0F}, 5.0F, dpad);
        overlay_.rounded_rect({cx - 200.0F, cy - 36.0F, 28.0F, 92.0F}, 5.0F, dpad);
        overlay_.rounded_rect({cx - 34.0F, cy - 18.0F, 68.0F, 30.0F}, 8.0F,
                              input.options ? kAccent : kBackground.with_alpha(0.72F));
        ui::text(overlay_, fonts_.semibold, "OPTIONS", cx, cy + 3.0F, 12.0F, kText,
                 gfx::Align::center, 0.8F);
        ui::text(overlay_, fonts_.regular, "Left stick", cx - 112.0F, cy + 122.0F, 16.0F,
                 kSecondary, gfx::Align::center);
        ui::text(overlay_, fonts_.regular, "Right stick", cx + 72.0F, cy + 122.0F, 16.0F,
                 kSecondary, gfx::Align::center);
        draw_overlay_rows(view, {bounds.x + 20.0F, bounds.y + bounds.h - 104.0F,
                                 bounds.w - 40.0F, 84.0F});
    }

    void dynamic_overlay_text(std::string_view value, float x, float y, float size,
                              gfx::Color color, float max_width)
    {
        const ui::FontRef &face = font_for(value, fonts_.regular);
        ui::text(overlay_, face, face.font->fit(value, size, max_width), x, y, size, color);
    }

    void section_overlay(std::string_view value, float x, float y)
    {
        ui::text(overlay_, fonts_.display, ui::upper(value), x, y, 20.0F, kSecondary,
                 gfx::Align::left, 2.4F);
    }

    std::string overlay_label(const ViewRow &row) const
    {
        if (row.action == "cancel")
            return "Cancel";
        // Titles come labeled from the model; only a bare action key needs its dashes softened.
        std::string label = row.title.empty() ? row.action : row.title;
        if (row.title.empty())
            std::replace(label.begin(), label.end(), '-', ' ');
        if (!label.empty() && label.front() >= 'a' && label.front() <= 'z')
            label.front() = static_cast<char>(label.front() - 'a' + 'A');
        return label;
    }

    std::string overlay_value(const ViewRow &row) const
    {
        if (row.key == "account-auth")
        {
            std::string status = row.value;
            if (!status.empty() && status.front() >= 'a' && status.front() <= 'z')
                status.front() = static_cast<char>(status.front() - 'a' + 'A');
            if (!row.subtitle.empty())
                status += (status.empty() ? "" : " · ") + row.subtitle;
            return status;
        }
        if (row.key == "account-last-sync" && current_state_ != nullptr && row.value != "Never")
        {
            std::int64_t timestamp = 0;
            const auto parsed = std::from_chars(row.value.data(), row.value.data() + row.value.size(), timestamp);
            if (parsed.ec == std::errc{})
                return relative_time(timestamp, current_state_->clock_ticks);
        }
        if (!row.value.empty())
            return row.value;
        if (!row.badge.empty())
            return row.badge;
        if (!row.subtitle.empty())
            return row.subtitle;
        return {};
    }

    void draw_overlay_rows(const ViewData &view, const gfx::Rect &bounds)
    {
        const bool settings_sheet = view.surface == "game-settings" || view.surface == "choice-picker" ||
                                    view.surface == "preset-picker" || view.surface == "button-mapping" ||
                                    view.surface == "game-maintenance";
        // Every row's place in the list; the list scrolls only when it is taller than the sheet,
        // just enough to keep the focused row and a little of its neighbours in view, and eases there.
        const auto pitch = [&](const ViewRow &row) {
            if (settings_sheet && row.kind == RowKind::Heading)
                return 54.0F + 12.0F;
            if (row.kind == RowKind::Heading || row.kind == RowKind::Separator)
                return 54.0F;
            const bool checkbox = (view.surface == "kept-data" || (view.surface == "collections" && row.action.rfind("collection-toggle-", 0) == 0)) &&
                                  row.kind == RowKind::Toggle;
            const bool described = (settings_sheet || checkbox) && !row.subtitle.empty();
            return (described ? 96.0F : 72.0F) + 12.0F;
        };
        std::vector<float> tops;
        float total = 0.0F;
        for (const ViewRow &row : view.rows)
        {
            tops.push_back(total);
            total += pitch(row);
        }
        const std::string scroll_key = view.surface + "\n" + view.title;
        if (scroll_key != sheet_scroll_key_)
        {
            sheet_scroll_key_ = scroll_key;
            sheet_scroll_ = sheet_scroll_target_ = 0.0F;
        }
        const float limit = std::max(0.0F, total - 12.0F - bounds.h);
        if (view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size())
        {
            const auto focused = static_cast<std::size_t>(view.focused_row);
            const float context = 48.0F;  // part of the next row stays visible
            const float top = tops[focused] - context;
            const float bottom = tops[focused] + pitch(view.rows[focused]) - 12.0F + context;
            if (top < sheet_scroll_target_)
                sheet_scroll_target_ = top;
            else if (bottom > sheet_scroll_target_ + bounds.h)
                sheet_scroll_target_ = bottom - bounds.h;
        }
        sheet_scroll_target_ = std::clamp(sheet_scroll_target_, 0.0F, limit);
        sheet_scroll_ = current_state_ && current_state_->reduced_motion ? sheet_scroll_target_
            : sheet_scroll_ + (sheet_scroll_target_ - sheet_scroll_) * std::min(1.0F, frame_dt_ * 14.0F);
        if (std::fabs(sheet_scroll_target_ - sheet_scroll_) < 0.5F)
            sheet_scroll_ = sheet_scroll_target_;
        // The clip reaches the panel's edges (the rows sit 40 px in) so a focused row's glow and
        // zoom are never cut; above and below it leaves room for the glow too.
        overlay_.push_clip({bounds.x - 40.0F, bounds.y - 32.0F, bounds.w + 80.0F, bounds.h + 64.0F});
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        for (int index = 0; index < static_cast<int>(view.rows.size()); ++index)
        {
            const ViewRow &row = view.rows[static_cast<std::size_t>(index)];
            float y = bounds.y + tops[static_cast<std::size_t>(index)] - sheet_scroll_;
            if (y + pitch(row) < bounds.y - 32.0F || y > bounds.y + bounds.h + 32.0F)
                continue;
            if (settings_sheet && row.kind == RowKind::Heading)
            {
                // Collapsible section header: focusable, shows "Show n" or "Hide".
                const gfx::Rect rect{bounds.x, y, bounds.w, 54.0F};
                const float amount = control_focus("sheet-" + row.key, index == view.focused_row);
                focus_glow(overlay_, rect, amount);
                ui::text(overlay_, fonts_.display, ui::upper(row.title), rect.x + 24.0F, rect.y + 35.0F,
                         20.0F, kSecondary, gfx::Align::left, 2.4F);
                ui::text(overlay_, fonts_.regular, row.value, rect.x + rect.w - 24.0F, rect.y + 35.0F, 18.0F,
                         kMuted, gfx::Align::right);
                focus_ring(overlay_, rect, amount);
                y += rect.h + 12.0F;
                continue;
            }
            if (row.kind == RowKind::Heading || row.kind == RowKind::Separator)
            {
                ui::text(overlay_, fonts_.display, ui::upper(row.title), bounds.x + 12.0F, y + 28.0F,
                         18.0F, kMuted, gfx::Align::left, 1.8F);
                y += 54.0F;
                continue;
            }
            // Game settings rows carry an effect line under the name (or the reason a row is
            // disabled), an accent dot when the player changed the value, and a tag.
            // Kept game data: each game is a checkbox row with its kept date under the name.
            const bool checkbox = (view.surface == "kept-data" || (view.surface == "collections" && row.action.rfind("collection-toggle-", 0) == 0)) &&
                                  row.kind == RowKind::Toggle;
            const bool described = (settings_sheet || checkbox) && !row.subtitle.empty() && row.kind != RowKind::Heading;
            const gfx::Rect rect{bounds.x, y, bounds.w, described ? 96.0F : 72.0F};
            const float amount = control_focus("sheet-" + row.key, index == view.focused_row);
            focus_glow(overlay_, rect, amount);
            overlay_.rounded_rect(rect, 8.0F, kPanel);
            overlay_.rounded_rect(rect, 8.0F, kText.with_alpha(row.selected ? 0.10F : 0.04F));
            const gfx::Color ink = !row.enabled ? kMuted : row.dangerous ? kRed : kText;
            const std::string value = settings_sheet && row.kind != RowKind::Heading ? row.value : overlay_value(row);
            const float value_width = value.empty() ? 0.0F : std::min(rect.w * 0.45F, fonts_.regular.measure(value, 22.0F));
            const float name_y = described ? rect.y + 40.0F : rect.cy() + 10.0F;
            float name_x = rect.x + 24.0F;
            if (checkbox)
            {
                const gfx::Rect box{name_x, name_y - 24.0F, 28.0F, 28.0F};
                if (row.selected)
                {
                    overlay_.rounded_rect(box, 6.0F, kAccent);
                    // A check mark drawn as two strokes.
                    overlay_.line(box.x + 7.0F, box.y + 14.0F, box.x + 12.0F, box.y + 20.0F, 3.0F, theme_.on_primary);
                    overlay_.line(box.x + 12.0F, box.y + 20.0F, box.x + 22.0F, box.y + 8.0F, 3.0F, theme_.on_primary);
                }
                else
                    overlay_.bordered_rect(box, 6.0F, kPanel.with_alpha(0.0F), 2.0F, kText.with_alpha(0.45F));
                name_x += 44.0F;
            }
            if (row.modified)
            {
                overlay_.circle(name_x + 5.0F, name_y - 9.0F, 5.0F, kAccent);
                name_x += 20.0F;
            }
            const std::string label = fit(fonts_.display, overlay_label(row), 28.0F, rect.w - 64.0F - value_width);
            ui::text(overlay_, fonts_.display, label, name_x, name_y, 28.0F, ink);
            if (settings_sheet && !row.badge.empty() && row.kind != RowKind::Heading)
            {
                const float tag_x = name_x + fonts_.display.measure(label, 28.0F) + 14.0F;
                const float tag_w = fonts_.semibold.measure(ui::upper(row.badge), 13.0F) + 16.0F;
                if (tag_x + tag_w < rect.x + rect.w - 40.0F - value_width)
                {
                    overlay_.rounded_rect({tag_x, name_y - 21.0F, tag_w, 24.0F}, 4.0F, kText.with_alpha(0.08F));
                    ui::text(overlay_, fonts_.semibold, ui::upper(row.badge), tag_x + 8.0F, name_y - 4.0F, 13.0F, kSecondary);
                }
            }
            if (described)
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row.subtitle, 19.0F, rect.w - 72.0F - value_width),
                         checkbox ? name_x : rect.x + 24.0F, rect.y + 74.0F, 19.0F, row.enabled ? kSecondary : kMuted);
            if (!value.empty())
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, value, 22.0F, value_width), rect.x + rect.w - 24.0F,
                         described ? rect.y + 52.0F : rect.cy() + 8.0F, 22.0F,
                         row.modified || row.selected ? kAccent : row.enabled ? kSecondary : kMuted, gfx::Align::right);
            if (row.progress > 0.0F)
                painter.progress({rect.x + 22.0F, rect.y + 60.0F, rect.w - 44.0F, 6.0F}, row.progress);
            focus_ring(overlay_, rect, amount);
            y += rect.h + 12.0F;
        }
        overlay_.pop_clip();
    }

    bool ensure_capture(int width, int height) const
    {
        if (capture_framebuffer_ != 0 && capture_width_ == width && capture_height_ == height)
            return true;
        if (capture_color_ != 0)
            glDeleteRenderbuffers(1, &capture_color_);
        if (capture_framebuffer_ != 0)
            glDeleteFramebuffers(1, &capture_framebuffer_);
        capture_color_ = 0;
        capture_framebuffer_ = 0;
        glGenFramebuffers(1, &capture_framebuffer_);
        glGenRenderbuffers(1, &capture_color_);
        glBindRenderbuffer(GL_RENDERBUFFER, capture_color_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  capture_color_);
        const bool okay = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!okay)
            return false;
        capture_width_ = width;
        capture_height_ = height;
        return true;
    }

    class GlTextureBackend final : public TextureBackend {
      public:
        explicit GlTextureBackend(gfx::Renderer& renderer) : renderer_(renderer) {}
        std::uint32_t Upload(const ArtworkImage& image) override {
            while (glGetError() != GL_NO_ERROR) {}
            std::uint32_t texture = renderer_.batch().create_texture(static_cast<int>(image.width),
                static_cast<int>(image.height), image.rgba.data());
            // A failed allocation still yields a name; report it as a failure so the
            // placeholder stays and the blank texture is not charged to the budget.
            if (texture != 0 && glGetError() != GL_NO_ERROR) {
                glDeleteTextures(1, &texture);
                return 0;
            }
            return texture;
        }
        void Release(std::uint32_t texture) override { glDeleteTextures(1, &texture); }
      private:
        gfx::Renderer& renderer_;
    };

    std::filesystem::path assets_;
    ui::Theme theme_;
    mutable gfx::Renderer renderer_;
    GlTextureBackend texture_backend_;
    std::unique_ptr<LiveArtworkCache> live_artwork_;
    gfx::Font regular_;
    gfx::Font semibold_;
    gfx::Font display_;
    gfx::Font greek_face_;
    gfx::Font cjk_face_;
    gfx::Font arabic_face_;
    ui::Fonts fonts_;
    ui::FontRef greek_;
    ui::FontRef cjk_;
    ui::FontRef arabic_;
    gfx::DrawList scene_;
    gfx::DrawList overlay_;
    FocusMotion focus_motion_;
    Scroll home_scroll_;
    std::map<std::string, Scroll> list_scrolls_;  // Downloads, Saves and Settings lists
    std::map<std::string, Scroll> home_rows_;
    float frame_dt_ = 1.0F / 60.0F;
    ui::TabBar tabs_;
    ui::GridView grid_;
    ui::ListView list_;
    ui::Keyboard keyboard_;
    std::array<std::uint32_t, 8> art_{};
    std::uint32_t hero_ = 0;
    std::string cached_qr_url_;
    QrMatrix cached_qr_;
    std::string cached_qr_error_;
    std::vector<const Game *> visible_grid_games_;
    const State *current_state_ = nullptr;
    bool simulation_tabs_ = true;
    float clock_ = 0.0F;
    float live_animation_clock_ = 0.0F;
    bool ready_ = false;
    bool has_frame_ = false;
    bool route_focus_ = true;
    OverlayTransition overlay_transition_;
    std::string sheet_scroll_key_;  // the sheet the scroll belongs to
    float sheet_scroll_ = 0.0F;
    float sheet_scroll_target_ = 0.0F;
    int last_width_ = 0;
    int last_height_ = 0;
    mutable GLuint capture_framebuffer_ = 0;
    mutable GLuint capture_color_ = 0;
    mutable int capture_width_ = 0;
    mutable int capture_height_ = 0;
    RenderStats stats_{};
};

Render::Render(std::filesystem::path assets_directory, ArtworkService* artwork)
    : impl_(std::make_unique<Impl>(std::move(assets_directory), artwork))
{
}

Render::~Render() = default;
Render::Render(Render &&) noexcept = default;
Render &Render::operator=(Render &&) noexcept = default;

void Render::draw(Simulation &simulation, int width, int height, float dt)
{
    impl_->draw(simulation, width, height, dt);
}

bool Render::capture_png(const std::filesystem::path &path, int width, int height) const
{
    return impl_->capture_png(path, width, height);
}

RenderStats Render::diagnostics() const noexcept
{
    return impl_->diagnostics();
}

} // namespace z3cloudplay::homebrew
