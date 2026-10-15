// Host event regression tests exercise the actual SDL adapter without a GL context.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#define main z3cloudplay_host_program
#include "../src/homebrew/host.cpp"
#undef main
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static void require(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    require(argc == 2, "fixture directory argument");

    {
        char program[] = "z3cloudplay-homebrew";
        char* live_argv[] = {program};
        const auto live = parse_options(1, live_argv);
        require(live && !live->simulation, "host defaults to live services");

        char simulation_flag[] = "--simulation";
        char* simulation_argv[] = {program, simulation_flag};
        const auto explicit_simulation = parse_options(2, simulation_argv);
        require(explicit_simulation && explicit_simulation->simulation,
                "explicit simulation mode is accepted");

        char snapshot_flag[] = "--snapshot";
        char snapshot_path[] = "-";
        char* live_snapshot_argv[] = {program, snapshot_flag, snapshot_path};
        require(!parse_options(3, live_snapshot_argv),
                "live mode rejects state snapshots");

        char scenario_flag[] = "--scenario";
        char scenario_name[] = "signed-in";
        char* scenario_argv[] = {program, scenario_flag, scenario_name};
        const auto scenario = parse_options(3, scenario_argv);
        require(scenario && scenario->simulation,
                "scenario selection explicitly chooses simulation");

        char data_flag[] = "--data-dir";
        char data_path[] = "/tmp/z3cloudplay-live-data";
        char* data_argv[] = {program, data_flag, data_path};
        const auto data = parse_options(3, data_argv);
        require(data && !data->simulation && data->data_directory == data_path,
                "live data directory is independent from fixture profiles");
        char relative_data_path[] = "relative-z3cloudplay-data";
        char* relative_data_argv[] = {program, data_flag, relative_data_path};
        const auto relative_data = parse_options(3, relative_data_argv);
        require(relative_data && relative_data->data_directory.is_absolute(),
                "relative live CLI directory is resolved before strict settings storage");
    }

    const auto profile = std::filesystem::temp_directory_path() / ("z3cloudplay-hui-host-input-test-" + std::to_string(getpid()) + ".json");
    std::error_code error;
    std::filesystem::remove(profile, error);
    z3cloudplay::homebrew::Simulation simulation(argv[1], profile);
    simulation.action("r1");
    bool suppress = false;
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_SLASH;
    text_key_event(simulation, event, suppress);
    require(simulation.state().editor.active, "slash opens search");
    event.type = SDL_TEXTINPUT;
    std::strcpy(event.text.text, "/");
    text_key_event(simulation, event, suppress);
    require(simulation.state().editor.value.empty(), "search-opening slash is consumed");
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_f;
    text_key_event(simulation, event, suppress);
    event.type = SDL_TEXTINPUT;
    std::strcpy(event.text.text, "f");
    text_key_event(simulation, event, suppress);
    require(simulation.state().editor.value == "f" && simulation.state().navigation.modals.empty(),
            "printable editing key inserts instead of opening Filters");
    simulation.action("circle");
    hui::InputFrame input;
    input.pressed = hui::action_bit(hui::Action::confirm) | hui::action_bit(hui::Action::menu);
    apply_input(simulation, input);
    require(simulation.state().navigation.route == z3cloudplay::homebrew::Route::Details &&
            simulation.state().navigation.modals.empty(), "simultaneous Cross/Options applies one edge");
    input.held = hui::action_bit(hui::Action::west) | hui::action_bit(hui::Action::page_next);
    input.stick_x = 0.75F; input.stick2_y = -0.5F; input.trigger_l = 0.4F;
    simulation.controller_sample(z3cloudplay::homebrew::diagnostic_sample(input));
    require(simulation.state().controller.square && simulation.state().controller.r1 &&
            !simulation.state().controller.cross && simulation.state().controller.left_x == 0.75F &&
            simulation.state().controller.right_y == -0.5F && simulation.state().controller.l2 == 0.4F,
            "diagnostics reflect actual kit held buttons, sticks and triggers");
    const auto replay = profile.string() + ".replay";
    { std::ofstream stream(replay); stream << "frame 3\n"; }
    unsigned rendered = 0;
    const auto initial_clock = simulation.state().clock_ticks;
    require(apply_replay(simulation, replay, [&](const std::string&) { ++rendered; return true; }),
            "presentation-only frame replay is accepted when recording");
    require(rendered == 3 && simulation.state().clock_ticks == initial_clock,
            "motion proof draws requested frames without advancing simulation");
    std::filesystem::remove(replay, error);
    std::filesystem::remove(profile, error);
    std::puts("PASS: SDL Search gesture, printable edit isolation, simultaneous controller activation");
}
