// Z3CloudPlay - Real library titles outside the baked catalog render with an explicit '?' fallback.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/artwork.hpp"
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include <SDL.h>
#include <cassert>
#include <filesystem>

using namespace z3cloudplay::homebrew;
namespace {
// Every cover is unavailable, so each cell shows its placeholder title.
struct NoArtwork final : ArtworkBackend {
    std::optional<ArtworkImage> Load(const ArtworkKey&, const std::atomic<bool>&) override { return std::nullopt; }
};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    auto* window = SDL_CreateWindow("Glyph fallback", 0, 0, 1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    assert(window);
    auto context = SDL_GL_CreateContext(window);
    assert(context);
    SDL_GL_SetSwapInterval(0);
    hui::gfx::set_glsl_prefix("#version 410 core\n");
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    {
        NoArtwork backend;
        ArtworkService service(backend);
        Render renderer(Z3CLOUDPLAY_HOMEBREW_ASSETS_DIR, &service);
        Simulation model({}, {}, ModelMode::Live);
        model.SetWallClock(1700000000);
        model.UpdateAccount({AuthState::SignedIn, "123", "Synthetic account", {}, {}, true});
        // None of these non-Latin characters except some Arabic letters occur in the
        // catalog the CJK/Arabic atlases are baked from (checked when choosing them).
        assert(model.UpdateLibrary({{
            {701, "Persona 5 ザ・ロイヤル", {}, {}, {}},
            {702, "東方紅魔郷", {}, {}, {}},
            {703, "FIFA 23 كرة القدم", {}, {}, {}},
            {704, "เกมส์ไทย", {}, {}, {}},
            {705, "Party Night 🎮", {}, {}, {}},
            {706, "Ωmega Ρrotocol", {}, {}, {}},
        }, false, {}, false, true, 1700000000}));
        assert(model.action("r1"));
        for (int i = 0; i < 16; ++i) renderer.draw(model, 1920, 1080);
        assert(renderer.capture_png(output / "library.png", 1920, 1080));
        // Name order puts the Latin/Arabic title first; open its details.
        assert(model.action("cross"));
        for (int i = 0; i < 16; ++i) renderer.draw(model, 1920, 1080);
        assert(renderer.capture_png(output / "details.png", 1920, 1080));
        service.Shutdown();
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
