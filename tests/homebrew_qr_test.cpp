// homebrew_qr_test.cpp - Bounded QR matrix encoding tests.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT

#include "homebrew/qr.hpp"

#include <cstdio>
#include <string>

#if defined(Z3CLOUDPLAY_QR_GRAPHICAL_TEST)
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"

#include "gfx/gl_program.hpp"

#include <SDL.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool finder(const z3cloudplay::homebrew::QrMatrix& matrix, int left, int top)
{
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            const bool expected = x == 0 || x == 6 || y == 0 || y == 6 ||
                                  (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            if (matrix.at(left + x, top + y) != expected)
                return false;
        }
    }
    return true;
}

} // namespace

#if defined(Z3CLOUDPLAY_QR_GRAPHICAL_TEST)
bool capture_synthetic_qr(const char* output, int width, int height)
{
    if (width <= 0 || height <= 0 || SDL_Init(SDL_INIT_VIDEO) != 0)
        return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_Window* window = SDL_CreateWindow("Z3CloudPlay QR verification", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, width, height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window == nullptr ? nullptr : SDL_GL_CreateContext(window);
    bool captured = false;
    if (window != nullptr && context != nullptr) {
        hui::gfx::set_glsl_prefix("#version 410 core\n");
        z3cloudplay::homebrew::Simulation model({}, {}, z3cloudplay::homebrew::ModelMode::Live);
        z3cloudplay::homebrew::AccountUpdate update;
        update.authentication = z3cloudplay::homebrew::AuthState::Waiting;
        update.challenge_url = "https://synthetic.invalid/qr/rendered-roundtrip";
        model.UpdateAccount(std::move(update));
        {
            auto renderer = std::make_unique<z3cloudplay::homebrew::Render>(
                std::filesystem::path(Z3CLOUDPLAY_HOMEBREW_ASSETS_DIR));
            for (int settle = 0; settle < 16; ++settle)
                renderer->draw(model, width, height);
            captured = renderer->capture_png(output, width, height);
        }
    }
    if (context != nullptr)
        SDL_GL_DeleteContext(context);
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
    return captured;
}
#endif

int main(int argc, char** argv)
{
    using z3cloudplay::homebrew::encode_qr;

    std::string error;
    const auto challenge = encode_qr("https://synthetic.invalid/qr/challenge-a", &error);
    check(error.empty() && !challenge.empty(), "synthetic challenge encodes");
    check(challenge.size() >= 21 && challenge.size() <= 177,
          "matrix size stays within QR version bounds");
    check(challenge.modules().size() == static_cast<std::size_t>(challenge.size() * challenge.size()),
          "matrix storage exactly matches its dimensions");
    check(finder(challenge, 0, 0) && finder(challenge, challenge.size() - 7, 0) &&
              finder(challenge, 0, challenge.size() - 7),
          "matrix contains three standard finder patterns");
    check(!challenge.at(-1, 0) && !challenge.at(challenge.size(), 0),
          "matrix lookup is bounded");
    const auto layout = z3cloudplay::homebrew::qr_layout(challenge, 430);
    check(layout.module_pixels > 0 && layout.side_pixels % layout.module_pixels == 0 &&
              layout.side_pixels ==
                  layout.module_pixels * (challenge.size() + 2 * z3cloudplay::homebrew::kQrQuietZoneModules) &&
              layout.offset_pixels * 2 + layout.side_pixels <= 430,
          "layout uses integer modules and a four-module quiet zone");
    check(z3cloudplay::homebrew::qr_layout(challenge, 1).module_pixels == 0 &&
              z3cloudplay::homebrew::qr_layout(challenge, 430, -1).module_pixels == 0,
          "layout rejects a surface too small for one module and an invalid quiet zone");

    const auto replacement = encode_qr("https://synthetic.invalid/qr/challenge-b", &error);
    check(!replacement.empty() && replacement.modules() != challenge.modules(),
          "replacement challenge changes the module matrix");

    const auto empty = encode_qr("", &error);
    check(error.empty() && !empty.empty(), "empty generic payload remains encodable");

    const auto embedded_nul = encode_qr(std::string("https://example.invalid/a\0b", 27), &error);
    check(embedded_nul.empty() && !error.empty(), "embedded NUL is rejected");

    const std::string malformed("\xF0\x28\x8C\x28", 4);
    const auto invalid_utf8 = encode_qr(malformed, &error);
    check(invalid_utf8.empty() && !error.empty(), "malformed UTF-8 is rejected");

    const auto over_input_limit = encode_qr(std::string(4097, 'x'), &error);
    check(over_input_limit.empty() && !error.empty(), "input over the auth challenge bound is rejected");

    const auto over_qr_capacity = encode_qr(std::string(4096, 'x'), &error);
    check(over_qr_capacity.empty() && !error.empty(), "payload over QR capacity is rejected");

    #if defined(Z3CLOUDPLAY_QR_GRAPHICAL_TEST)
    if (failures == 0 && argc == 4) {
        const int width = std::atoi(argv[2]);
        const int height = std::atoi(argv[3]);
        check(capture_synthetic_qr(argv[1], width, height),
              "synthetic QR framebuffer capture succeeds");
    } else if (failures == 0 && argc != 1) {
        check(false, "graphical usage is CAPTURE.png WIDTH HEIGHT");
    }
    #else
    (void)argc;
    (void)argv;
    #endif

    if (failures == 0)
        std::puts("homebrew QR tests passed");
    return failures == 0 ? 0 : 1;
}
