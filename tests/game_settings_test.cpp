#include "runtime/game_settings.h"
#include "runtime/game_probe.h"
#include "runtime/install_script.h"
#include "runtime/settings_publication.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

namespace {

using namespace z3cloudplay::runtime;

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path);
  CHECK(file.good());
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

const std::string& Value(const EffectiveSettings& settings, const char* id) { return settings.values.at(id).value; }

// A minimal PE header: "MZ", e_lfanew, "PE\0\0", machine and optional-header magic.
void WritePe(const std::filesystem::path& path, bool pe64) {
  std::filesystem::create_directories(path.parent_path());
  std::string bytes(0x80 + 26, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; bytes[0x3c] = 0x40;
  bytes[0x40] = 'P'; bytes[0x41] = 'E';
  bytes[0x44] = pe64 ? 0x64 : 0x4c; bytes[0x45] = pe64 ? static_cast<char>(0x86) : 0x01;
  bytes[0x40 + 24] = 0x0b; bytes[0x40 + 25] = pe64 ? 0x02 : 0x01;
  std::ofstream(path, std::ios::binary) << bytes;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path source = argc > 1 ? argv[1] : ".";
  std::string reason;

  GameRecipe hl2;
  CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-220-hl2-v1.json"), &hl2, &reason));
  std::vector<SettingsPreset> presets;
  CHECK(ParseSettingsPresets(ReadText(source / "runtime/presets.json"), &presets, &reason));
  CHECK(presets.size() == 4 && presets.front().id == "z3cloudplay-default");
  const SettingsPreset* mouse = nullptr;
  for (const auto& preset : presets)
    if (preset.id == "mouse-driven") mouse = &preset;
  CHECK(mouse);

  // Every catalogue choice default is one of its own choices.
  for (const auto& definition : SettingsCatalogue()) {
    if (definition.kind != SettingKind::kChoice || definition.choices.empty()) continue;
    CHECK(ValidateSettingValue(hl2, definition.id, definition.z3cloudplay_default, &reason));
  }

  // DefaultSkipsPlayerProgram: a recipe generated for the player's program lists it first; without
  // that choice the default is Steam's entry, as the next preparation makes it, and the choice still finds it.
  {
    GameRecipe chosen = hl2;
    chosen.launch_entries = {{kProgramEntryId, "FalloutNV.exe", "FalloutNV.exe", "", ""},
                             {"launch-1", "Launcher", "FalloutNVLauncher.exe", "", ""}};
    EffectiveSettings values;
    SettingsError problem;
    CHECK(BuildEffectiveSettings(chosen, nullptr, {}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == "launch-1");
    CHECK(BuildEffectiveSettings(chosen, nullptr, {{"program", "FalloutNV.exe"}}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == kProgramEntryId && values.launch->executable == "FalloutNV.exe");
    chosen.launch_entries.resize(1);
    CHECK(BuildEffectiveSettings(chosen, nullptr, {}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == kProgramEntryId);
  }

  // RecipeArgumentsPreserved: with no overrides the HL2 recipe gives the owner-verified profile.
  const ProfileTarget target{"z3cloudplay-steam-220", "/data/homebrew/FAKE77031/games/220"};
  EffectiveSettings settings;
  SettingsError error;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments == "-game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1");
  CHECK(settings.dll_overrides == "d3d9=n");
  CHECK(settings.values.at("resolution").source == ValueSource::kRecipe);
  CHECK(settings.values.at("performance_overlay").source == ValueSource::kZ3CloudPlay);
  CHECK(!settings.values.at("scaling").available);          // DXVK output ignores scaling today
  CHECK(!settings.values.at("refresh").available);          // OpenGL only
  CHECK(!settings.values.at("frame_limit").available);      // needs the dxvk.conf probe
  CHECK(!settings.values.at("windows_version").available);  // needs registry overlays
  CHECK(!settings.values.at("pointer_speed").available);    // no pointer stick
  ProsperoProfile profile;
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text ==
        "; Generated by Z3CloudPlay from recipe steam-220-hl2-v1 v1. Edit settings in Z3CloudPlay.\n"
        "[application]\n"
        "id = z3cloudplay-steam-220\n"
        "name = Half-Life 2\n"
        "executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\hl2.exe\n"
        "working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\n"
        "arguments = -game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1\n"
        "dll_overrides = d3d9=n\n"
        "prefix = game\n"
        "runtime = wine-wow64\n"
        "architecture = pe32\n"
        "graphics = dxvk\n"
        "\n[display]\n"
        "desktop = 1920x1080\n"
        "view = window\n"
        "show_fps = false\n"
        "\n[input]\n"
        "mode = xinput\n"
        "mouse = none\n"
        "options = escape\n");
  CHECK(RenderDxvkConfig(settings).empty());

  // Overrides: resolution fills the recipe's -w/-h; extra arguments and libraries append.
  std::map<std::string, std::string> overrides = {{"resolution", "2560x1440"},
                                                  {"extra_arguments", "-console \"+map d1_trainstation_01\""},
                                                  {"library_overrides", "winmm=n,b"},
                                                  {"performance_overlay", "on"},
                                                  {"logging", "verbose"},
                                                  {"button.touchpad", "f5"},
                                                  {"graphics", "gdi"}};  // locked: ignored
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments ==
        "-game hl2 -steam -w 2560 -h 1440 -novid +exec 360controller.cfg +joystick 1 -console \"+map d1_trainstation_01\"");
  CHECK(settings.dll_overrides == "d3d9=n;winmm=n,b");
  CHECK(settings.graphics == "dxvk" && settings.values.at("graphics").locked);
  CHECK(Value(settings, "resolution") == "2560x1440" && settings.values.at("resolution").source == ValueSource::kUser);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("desktop = 2560x1440\n") != std::string::npos);
  CHECK(profile.text.find("show_fps = true\n") != std::string::npos);
  CHECK(profile.text.find("touchpad = f5\n") != std::string::npos);
  CHECK(profile.text.find("[debug]\nwinedebug = ") != std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  // Input logging traces Raw Input, DirectInput and winmm joysticks, in a channel list the profile accepts.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"logging", "input"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[debug]\nwinedebug = err+all,warn+all,+loaddll,+process,+rawinput,+dinput,+winmm,+ntoskrnl,+plugplay,+hid,+hidclass\n") !=
        std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", "-name \"John  Doe\""}}, KitCapabilities{},
                               &settings, &error));
  std::vector<std::string> spaced_arguments;
  CHECK(SplitProsperoArguments(settings.arguments, &spaced_arguments));
  CHECK(spaced_arguments.back() == "John  Doe");
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", R"(-path C:\"Program  Files\")"}},
                               KitCapabilities{}, &settings, &error));
  CHECK(SplitProsperoArguments(settings.arguments, &spaced_arguments));
  CHECK(spaced_arguments.back() == "C:\\Program  Files\\");

  // An explicit "off" is a value, not an absence: it beats a preset that turns it on.
  SettingsPreset overlay{"overlay", "Overlay", "", "", {{"performance_overlay", "on"}}};
  CHECK(BuildEffectiveSettings(hl2, &overlay, {}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "performance_overlay") == "on" && settings.values.at("performance_overlay").source == ValueSource::kPreset);
  CHECK(BuildEffectiveSettings(hl2, &overlay, {{"performance_overlay", "off"}}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "performance_overlay") == "off" && settings.values.at("performance_overlay").source == ValueSource::kUser);

  // Presets beat recipe defaults; the mouse preset turns on the pointer and its speed.
  CHECK(BuildEffectiveSettings(hl2, mouse, {}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "controller") == "keyboard" && settings.values.at("controller").source == ValueSource::kPreset);
  CHECK(settings.values.at("pointer_speed").available && Value(settings, "pointer_speed") == "900");
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("preset = mouse\nmode = keyboard\nmouse = left_stick\nmouse_speed = 900\n") != std::string::npos);
  // Choosing only the Controller preset row leaves the input file's mode, stick and speed in charge:
  // Z3CloudPlay's defaults (Xbox controller, no pointer stick) must not override them.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[input]\npreset = mouse\n") != std::string::npos);
  CHECK(profile.text.find("\nmode = ") == std::string::npos && profile.text.find("\nmouse = ") == std::string::npos &&
        profile.text.find("\nmouse_speed = ") == std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  // A row the player set still wins over the preset.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}, {"pointer_stick", "right_stick"}},
                               KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nmouse = right_stick\n") != std::string::npos && profile.text.find("\nmode = ") == std::string::npos);
  // The sheet shows the preset's own values, and Pointer speed can be tuned; a slower speed reaches the profile.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "controller") == "keyboard" && Value(settings, "pointer_stick") == "left_stick" &&
        Value(settings, "pointer_speed") == "900" && settings.values.at("pointer_speed").available &&
        settings.values.at("pointer_stick").source == ValueSource::kPreset);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}, {"pointer_speed", "600"}}, KitCapabilities{},
                               &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nmouse_speed = 600\n") != std::string::npos && profile.text.find("\nmouse = ") == std::string::npos);
  // Z3CloudPlay's table of preset rows matches the shipped input files.
  for (const auto& [name, rows] : ControllerPresetInputs()) {
    const auto file = ReadText(source / ("runtime/input/" + name + ".input"));
    const std::map<std::string, std::string> keys = {{"controller", "mode"}, {"pointer_stick", "mouse"}, {"pointer_speed", "mouse_speed"}};
    for (const auto& [id, key] : keys) {
      const auto line = "\n" + key + " = ";
      const auto at = file.find(line);
      const std::string in_file = at == std::string::npos ? "" : file.substr(at + line.size(), file.find('\n', at + 1) - at - line.size());
      const auto row = rows.find(id);
      const std::string in_table = row == rows.end() ? "" : row->second;
      if (in_file != in_table && !(in_file.empty() && in_table == "none"))
        std::fprintf(stderr, "%s.input %s: file '%s' table '%s'\n", name.c_str(), key.c_str(), in_file.c_str(), in_table.c_str());
      CHECK(in_file == in_table || (in_file.empty() && in_table == "none"));
    }
  }

  // dxvk.conf settings apply only when the kit reads dxvk.conf.
  KitCapabilities dxvk_kit;
  dxvk_kit.dxvk_config = true;
  overrides = {{"frame_limit", "30"}, {"vsync", "off"}, {"frame_latency", "1"}, {"reported_vram", "4096"}};
  // VSync off needs a display that honors DXVK's present interval; the rest apply today.
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, dxvk_kit, &settings, &error));
  CHECK(settings.values.at("frame_limit").available && !settings.values.at("vsync").available &&
        settings.values.at("frame_latency").available && settings.values.at("reported_vram").available);
  CHECK(RenderDxvkConfig(settings).find("presentInterval") == std::string::npos);
  dxvk_kit.dxvk_present = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, KitCapabilities{}, &settings, &error));
  CHECK(RenderDxvkConfig(settings).empty());
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, dxvk_kit, &settings, &error));
  const auto config = RenderDxvkConfig(settings);
  CHECK(config.find("dxgi.maxFrameRate = 30\nd3d9.maxFrameRate = 30\n") != std::string::npos);
  CHECK(config.find("dxgi.syncInterval = 0\nd3d9.presentInterval = 0\n") != std::string::npos);
  CHECK(config.find("d3d9.maxFrameLatency = 1\n") != std::string::npos);
  CHECK(config.find("dxgi.maxDeviceMemory = 4096\nd3d9.maxAvailableMemory = 4096\n") != std::string::npos);

  // With dxvk.conf, a DXVK overlay is written as dxvk.hud and show_fps stays false.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}, {"overlay_detail", "frametimes"}}, dxvk_kit,
                               &settings, &error));
  CHECK(RenderDxvkConfig(settings).find("dxvk.hud = fps,frametimes\n") != std::string::npos);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("show_fps = false\n") != std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"overlay_detail", "full"}}, dxvk_kit, &settings, &error));
  CHECK(!settings.values.at("overlay_detail").available);   // overlay off
  CHECK(RenderDxvkConfig(settings).find("dxvk.hud") == std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("show_fps = true\n") != std::string::npos);  // no dxvk.conf: the title's HUD

  // OpenGL games get refresh and the worker thread; GDI cannot show the overlay.
  auto opengl = hl2;
  opengl.graphics = "opengl";
  CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"scaling", "integer"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("scaling = integer\n") != std::string::npos);
  CHECK(profile.text.find("refresh = 120\nopengl_thread = false\n") != std::string::npos);
  // A Zink kit ignores both, so the rows say so and the profile keeps their defaults.
  KitCapabilities zink;
  zink.opengl_zink = true;
  CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}}, zink, &settings, &error));
  CHECK(!settings.values.at("refresh").available && !settings.values.at("opengl_thread").available);
  CHECK(settings.values.at("refresh").reason.find("Zink") != std::string::npos);
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("refresh = 60\n") != std::string::npos);
  // A Zink kit's runtime reads Environment variables as [debug] env lines; names it does not take are left out.
  CHECK(!settings.values.at("environment").reason.size());
  CHECK(BuildEffectiveSettings(opengl, nullptr,
                               {{"environment", "MESA_GL_VERSION_OVERRIDE=3.3; SDL_HINT_X=1; ZINK_DEBUG=compact"},
                                {"logging", "verbose"}},
                               zink, &settings, &error));
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("\n[debug]\nwinedebug = ") != std::string::npos);
  CHECK(profile.text.find("\nenv = MESA_GL_VERSION_OVERRIDE=3.3\nenv = ZINK_DEBUG=compact\n") != std::string::npos);
  CHECK(profile.text.find("SDL_HINT_X") == std::string::npos && profile.text.find("[environment]") == std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));  // without the feature the checker refuses env lines
  // A Godot game asks for opengl3; on a Zink kit it draws with Vulkan instead, elsewhere it keeps OpenGL.
  auto godot = opengl;
  godot.detected = "godot,d3d12,dxgi,opengl,vulkan graphics=opengl";
  godot.arguments = "--rendering-driver opengl3";
  CHECK(BuildEffectiveSettings(godot, nullptr, {}, zink, &settings, &error));
  CHECK(settings.arguments == "--rendering-driver vulkan");
  CHECK(BuildEffectiveSettings(godot, nullptr, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments == "--rendering-driver opengl3");
  auto gdi = hl2;
  gdi.graphics = "gdi";
  CHECK(BuildEffectiveSettings(gdi, nullptr, {{"performance_overlay", "on"}}, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("performance_overlay").available && Value(settings, "performance_overlay") == "off");

  // Environment variables need a runtime that reads [environment]; released ones refuse it.
  const std::map<std::string, std::string> environment = {{"environment", "WINE_CPU_TOPOLOGY=4:0,1,2,3; DXVK_FRAME_RATE = 45"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, environment, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("environment").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[environment]") == std::string::npos);
  KitCapabilities environment_kit;
  environment_kit.environment = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, environment, environment_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nDXVK_FRAME_RATE = 45\n") != std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses it
  CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));
  // A Zink kit that also reads [environment] (kit H r5): CPU cores and the names [environment] takes go there, the
  // diagnostics names only [debug] env takes stay env lines, and no name is written twice.
  {
    auto both = zink;
    both.environment = true;
    CHECK(BuildEffectiveSettings(opengl, nullptr,
                                 {{"environment", "DXVK_FRAME_RATE=60; PW_NATIVE_PROFILE=1; MESA_GL_VERSION_OVERRIDE=3.3"},
                                  {"cpu_topology", "4"}},
                                 both, &settings, &error));
    CHECK(settings.values.at("cpu_topology").available);
    CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[debug]\nenv = PW_NATIVE_PROFILE=1\n") != std::string::npos);
    CHECK(profile.text.find(
              "\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nDXVK_FRAME_RATE = 60\nMESA_GL_VERSION_OVERRIDE = 3.3\n") !=
          std::string::npos);
    CHECK(profile.text.find("env = DXVK_") == std::string::npos && profile.text.find("env = MESA_") == std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true, .debug_env = true}));
  }
  // Stick dead zone and rumble need a runtime with controller tuning and the Xbox controller;
  // only values that differ from the runtime's defaults are written.
  const std::map<std::string, std::string> tuning = {{"stick_deadzone", "15"}, {"rumble", "50"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, tuning, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("stick_deadzone").available && !settings.values.at("rumble").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("stick_deadzone") == std::string::npos && profile.text.find("rumble") == std::string::npos);
  KitCapabilities tuning_kit;
  tuning_kit.input_tuning = true;
  // Every setting is listed whatever the kit; what the kit lacks is disabled with its reason.
  CHECK(SettingShown(*FindSetting("rumble"), KitCapabilities{}) && SettingShown(*FindSetting("environment"), dxvk_kit));
  CHECK(!SettingShown(*FindSetting("button.cross"), KitCapabilities{}));  // under Button mapping
  // Rows nothing can apply yet are listed but never take effect, even with every capability.
  {
    const auto every = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true);
    const std::map<std::string, std::string> unbuilt = {{"invert_stick", "right_y"},
        {"locale", "de-DE"}, {"registry_dll_overrides", "dinput8=n,b"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, unbuilt, every, &settings, &error));
    for (const auto& [id, value] : unbuilt) {
      CHECK(SettingShown(*FindSetting(id), every));
      CHECK(!settings.values.at(id).available && !settings.values.at(id).reason.empty());
    }
    // The reported GPU is a dxvk.conf option and CPU cores a Wine variable in [environment]: no runtime change.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"reported_gpu", "nvidia"}, {"cpu_topology", "4"}}, every, &settings, &error));
    CHECK(settings.values.at("reported_gpu").available && settings.values.at("cpu_topology").available);
    CHECK(RenderDxvkConfig(settings).find("\ndxgi.customVendorId = 10de\nd3d9.customVendorId = 10de\n") != std::string::npos);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"cpu_topology", "4"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("cpu_topology").available && !settings.values.at("reported_gpu").available);
    CHECK(!ValidateSettingValue(hl2, "registry_dll_overrides", "not a list", &reason));
    // The title publication carries at most the two registry values it owns; a longer plan cannot be applied.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"},
        {"registry_dll_overrides", "winmm=d"}}, every, &settings, &error));
    CHECK(RenderRegistryValues(settings).size() <= 2);
    CHECK(FindSetting("launch_option")->apply == ApplyMode::kNextLaunch);
    CHECK(FindSetting("locale")->apply == ApplyMode::kGameClosed);
  }
  CHECK(BuildEffectiveSettings(hl2, nullptr, tuning, tuning_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nstick_deadzone = 15\nrumble = 50\n") != std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses it
  CHECK(CheckProsperoProfile(profile.text, &reason, {.input_tuning = true}));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, tuning_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("stick_deadzone") == std::string::npos && profile.text.find("rumble") == std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller", "keyboard"}, {"rumble", "0"}}, tuning_kit, &settings, &error));
  CHECK(!settings.values.at("rumble").available && settings.values.at("rumble").reason == "Only with the Xbox controller.");
  CHECK(!ValidateSettingValue(hl2, "stick_deadzone", "60", &reason) && ValidateSettingValue(hl2, "rumble", "25", &reason));

  // Trigger dead zone rides with controller tuning; the wheel and volume have their own gates.
  const std::map<std::string, std::string> more_tuning = {
      {"trigger_deadzone", "10"}, {"wheel_stick", "right_stick"}, {"volume", "50"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, more_tuning, tuning_kit, &settings, &error));
  CHECK(settings.values.at("trigger_deadzone").available && !settings.values.at("wheel_stick").available &&
        !settings.values.at("volume").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\ntrigger_deadzone = 10\n") != std::string::npos &&
        profile.text.find("wheel") == std::string::npos && profile.text.find("[audio]") == std::string::npos);
  const auto all_kit = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), false);
  CHECK(BuildEffectiveSettings(hl2, nullptr, more_tuning, all_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nwheel = right_stick\n") != std::string::npos);
  CHECK(profile.text.find("\n[audio]\nvolume = 50\n") != std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses both
  CHECK(CheckProsperoProfile(profile.text, &reason,
                             {.input_tuning = true, .input_wheel = true, .audio_section = true}));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"volume", "100"}, {"wheel_stick", "none"}}, all_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[audio]") == std::string::npos && profile.text.find("wheel") == std::string::npos);
  CHECK(std::is_sorted(SettingsKitCapabilityNames().begin(), SettingsKitCapabilityNames().end()));
  CHECK(!ValidateSettingValue(hl2, "environment", "WINEPREFIX=C:\\x", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_HUD=full", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_A=1;DXVK_A=2", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_A", &reason));

  // Registry settings: Windows version per executable and text scale, edited in user.reg.
  KitCapabilities registry_kit;
  registry_kit.registry = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"}}, KitCapabilities{},
                               &settings, &error));
  CHECK(RenderRegistryValues(settings).empty());   // no bottle access: nothing to write
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"}}, registry_kit,
                               &settings, &error));
  auto registry = RenderRegistryValues(settings);
  CHECK(registry.size() == 2 && registry[0].key == "Software\\Wine\\AppDefaults\\hl2.exe" && registry[0].data == "\"win7\"");
  CHECK(registry[1].key == "Control Panel\\Desktop" && registry[1].data == "dword:00000078");
  std::string user_reg =
      "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n"
      "[Control Panel\\\\Desktop] 1791211318\n#time=1dd54d7acc7d208\n\"CaretWidth\"=dword:00000001\n\"Other\"=\"x\"\n\n"
      "[Software\\\\Wine\\\\AppDefaults] 1791210546\n#time=1dd54d5e1bfad0c\n\n"
      "[Software\\\\Wine\\\\Fonts] 1791229249\n#time=1dd54d7acc7d208\n\"LogPixels\"=dword:00000060\n\n"
      "[Software\\\\Wine\\\\Fonts\\\\External Fonts] 1791211318\n\"A\"=\"b\"\n";
  const auto before_registry = user_reg;
  CHECK(ApplyRegistryValues(&user_reg, registry, 1791300000, &reason));
  CHECK(user_reg.find("\"CaretWidth\"=dword:00000001\n\"Other\"=\"x\"\n\"LogPixels\"=dword:00000078\n\n[Software") != std::string::npos);
  // Wine's font cache marker is left alone.
  CHECK(user_reg.find("[Software\\\\Wine\\\\Fonts] 1791229249\n#time=1dd54d7acc7d208\n\"LogPixels\"=dword:00000060\n") != std::string::npos);
  CHECK(user_reg.find("\n[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe] 1791300000\n\"Version\"=\"win7\"\n") != std::string::npos);
  CHECK(user_reg.find("[Software\\\\Wine\\\\Fonts\\\\External Fonts] 1791211318\n\"A\"=\"b\"\n") != std::string::npos);
  // Applying again changes nothing; Windows 10 and 100% remove or reset the values.
  auto again = user_reg;
  CHECK(ApplyRegistryValues(&again, registry, 1791400000, &reason) && again == user_reg);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, registry_kit, &settings, &error));
  CHECK(ApplyRegistryValues(&user_reg, RenderRegistryValues(settings), 1791400000, &reason));
  CHECK(user_reg.find("\"Version\"") == std::string::npos && user_reg.find("dword:00000078") == std::string::npos);
  CHECK(user_reg.find("\"LogPixels\"=dword:00000060\n") != std::string::npos);
  std::string not_registry = "hello";
  CHECK(!ApplyRegistryValues(&not_registry, registry, 1, &reason));
  // Wine section and value names are case-insensitive. Sections need no timestamp, and
  // CRLF plus an unterminated final assignment are both valid input forms.
  std::string crlf_registry =
      "WINE REGISTRY Version 2\r\n\r\n[control panel\\\\desktop]\r\n\"logpixels\"=dword:00000060";
  CHECK(ApplyRegistryValues(&crlf_registry, {registry[1]}, 1, &reason));
  CHECK(crlf_registry.find("\"logpixels\"=dword:00000078") != std::string::npos);
  CHECK(crlf_registry.find("[Control Panel\\\\Desktop]") == std::string::npos);
  std::string duplicate_sections =
      "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop]\n\"LogPixels\"=dword:00000060\n\n"
      "[control panel\\\\desktop] 2\n\"Other\"=\"x\"\n";
  const auto duplicate_sections_before = duplicate_sections;
  CHECK(!ApplyRegistryValues(&duplicate_sections, {registry[1]}, 1, &reason));
  CHECK(duplicate_sections == duplicate_sections_before);
  std::string duplicate_assignments =
      "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop]\n"
      "\"LogPixels\"=dword:00000060\n\"logpixels\"=dword:00000070\n";
  const auto duplicate_assignments_before = duplicate_assignments;
  CHECK(!ApplyRegistryValues(&duplicate_assignments, {registry[1]}, 1, &reason));
  CHECK(duplicate_assignments == duplicate_assignments_before);
  (void)before_registry;

  // An Unreal Engine 3 (UDK) game's EULA is marked accepted in the bottle's system.reg.
  {
    std::string guid;
    CHECK(ParseUe3InstallGuid("<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>"
                              "dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>\r\n</InstallInfo>", &guid));
    CHECK(guid == "dcf140f3-829e-4033-950d-b3be8d76b933");
    CHECK(ParseUe3InstallGuid("<InstallGuidString> DCF140F3-829E-4033-950D-B3BE8D76B933\n</InstallGuidString>", &guid));
    CHECK(guid == "DCF140F3-829E-4033-950D-B3BE8D76B933");
    for (const char* bad : {"<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b93</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b93g</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d]b3be8d76b933</InstallGuidString>",
                            "<InstallGuidString>dcf140f3\\829e-4033-950d-b3be8d76b933</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933",
                            "<InstallGuidString></InstallGuidString>", "no guid here",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>"
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>"})
      CHECK(!ParseUe3InstallGuid(bad, &guid));

    const auto eula_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                           ("z3cloudplay-ue3-eula-" + std::to_string(::getpid()));
    std::filesystem::remove_all(eula_root);
    const auto game = eula_root / "game", bottle = eula_root / "bottle";
    std::filesystem::create_directories(game / "Binaries/Win32");
    std::filesystem::create_directories(bottle);
    std::filesystem::permissions(eula_root, std::filesystem::perms::owner_all);
    std::filesystem::permissions(bottle, std::filesystem::perms::owner_all);
    const std::string system_reg =
        "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall] 1791227731\n\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall] 1791227735\n";
    std::ofstream(bottle / "system.reg") << system_reg;
    ::chmod((bottle / "system.reg").c_str(), 0600);
    const std::vector<std::string> programs{"Binaries/Win32/UDK.exe"};
    // Not a UDK game: no InstallInfo.xml (and then no UnSetup.exe) means nothing is touched.
    auto eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791500000);
    CHECK(!eula.found && eula.ok && !eula.written);
    std::ofstream(game / "Binaries/InstallInfo.xml")
        << "<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933"
           "</InstallGuidString>\r\n</InstallInfo>";
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791500000);
    CHECK(!eula.found && !eula.written);
    std::ofstream(game / "Binaries/UnSetup.exe") << "MZ";
    // A program outside Binaries/Win32 or Win64 is not the UDK layout.
    eula = AcceptUe3Eula(game.string(), {"Game.exe"}, bottle.string(), 1791500000);
    CHECK(!eula.found && !eula.written);
    CHECK(ReadText(bottle / "system.reg") == system_reg && !std::filesystem::exists(bottle / "system.reg.z3cloudplay-backup"));
    eula = AcceptUe3Eula(game.string(), {"Binaries\\Win32\\UDK.exe"}, bottle.string(), 1791500000);
    CHECK(eula.found && eula.ok && eula.written && eula.guid == "dcf140f3-829e-4033-950d-b3be8d76b933");
    const auto accepted = ReadText(bottle / "system.reg");
    CHECK(accepted.find("\n[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\"
                        "UDK-dcf140f3-829e-4033-950d-b3be8d76b933] 1791500000\n\"EULAAccepted\"=dword:00000001\n") !=
          std::string::npos);
    CHECK(accepted.find("\n[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\"
                        "UDK-dcf140f3-829e-4033-950d-b3be8d76b933] 1791500000\n\"EULAAccepted\"=dword:00000001\n") !=
          std::string::npos);
    CHECK(accepted.compare(0, system_reg.size(), system_reg) == 0);
    CHECK(ReadText(bottle / "system.reg.z3cloudplay-backup") == system_reg);
    // Again: already accepted, so neither the hive nor its backup is rewritten.
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791600000);
    CHECK(eula.found && eula.ok && !eula.written);
    CHECK(ReadText(bottle / "system.reg") == accepted && ReadText(bottle / "system.reg.z3cloudplay-backup") == system_reg);
    // A bad InstallInfo.xml is reported, not guessed at.
    std::ofstream(game / "Binaries/InstallInfo.xml") << "<InstallGuidString>not-a-guid</InstallGuidString>";
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791600000);
    CHECK(eula.found && !eula.ok && !eula.written && ReadText(bottle / "system.reg") == accepted);
    std::filesystem::remove_all(eula_root);
  }

  // A Steam install script's Registry section goes into the bottle (Fallout: New Vegas's launcher
  // offers only "Install" without it); its Run Process entries are not run.
  {
    const std::string fnv =
        "\"InstallScript\"\n{\n    \"Registry\"\n    {\n"
        "        \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Bethesda Softworks\\\\FalloutNV\"\n        {\n"
        "            \"string\"\n            {\n                \"Installed Path\"      \"%INSTALLDIR%\\\\\"\n"
        "            }\n        }\n    }\n    \"Run Process\"\n    {\n        \"DirectX\"\n        {\n"
        "            \"process 1\"     \"%INSTALLDIR%\\\\Redists\\\\directx\\\\DXSETUP.exe\"\n        }\n    }\n}\n";
    InstallScriptValues values;
    CHECK(ParseInstallScript(fnv, "Z:\\data\\games\\22380", &values, &reason));
    CHECK(values.user.empty() && values.machine.size() == 2);
    CHECK(values.machine[0].key == "SOFTWARE\\Bethesda Softworks\\FalloutNV" && values.machine[0].name == "Installed Path" &&
          values.machine[0].data == "\"Z:\\\\data\\\\games\\\\22380\\\\\"");
    CHECK(values.machine[1].key == "Software\\Wow6432Node\\Bethesda Softworks\\FalloutNV" &&
          values.machine[1].data == values.machine[0].data);
    // UTF-16 with its mark, HKCU, dwords; another %TOKEN%, other roots and other types are skipped.
    const std::string mixed =
        "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\Studio\\\\Game\" { "
        "\"dword\" { \"Installed\" \"1\" \"Bad\" \"x\" } \"string\" { \"Lang\" \"%LANGUAGE%\" \"Ok\" \"yes\" } "
        "\"binary\" { \"Blob\" \"00ff\" } } \"HKEY_CLASSES_ROOT\\\\.sav\" { \"string\" { \"\" \"Game.Save\" } } } }";
    std::string utf16 = "\xff\xfe";
    for (const char value : mixed) utf16 += std::string{value, '\0'};
    values = {};
    CHECK(ParseInstallScript(utf16, "Z:\\g", &values, &reason));
    CHECK(values.machine.empty() && values.user.size() == 2);
    CHECK(values.user[0].key == "Software\\Studio\\Game" && values.user[0].name == "Installed" &&
          values.user[0].data == "dword:00000001");
    CHECK(values.user[1].name == "Ok" && values.user[1].data == "\"yes\"");
    values = {};
    CHECK(!ParseInstallScript("\"InstallScript\" {", "Z:\\g", &values, &reason));

    const auto script_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                             ("z3cloudplay-install-script-" + std::to_string(::getpid()));
    std::filesystem::remove_all(script_root);
    const auto game = script_root / "game", bottle = script_root / "bottle";
    std::filesystem::create_directories(game);
    std::filesystem::create_directories(bottle);
    std::filesystem::permissions(script_root, std::filesystem::perms::owner_all);
    std::filesystem::permissions(bottle, std::filesystem::perms::owner_all);
    const std::string system_reg = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n";
    std::ofstream(bottle / "system.reg") << system_reg;
    ::chmod((bottle / "system.reg").c_str(), 0600);
    // No script: nothing found, nothing written.
    auto applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found.empty() && !std::filesystem::exists(bottle / "system.reg.z3cloudplay-backup"));
    std::ofstream(game / "InstallScript.vdf") << fnv;
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found == std::vector<std::string>{"InstallScript.vdf"} && applied.values == 2);
    const auto written = ReadText(bottle / "system.reg");
    CHECK(written.find("\n[SOFTWARE\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n"
                       "\"Installed Path\"=\"Z:\\\\data\\\\games\\\\22380\\\\\"\n") != std::string::npos);
    CHECK(written.find("\n[Software\\\\Wow6432Node\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n") != std::string::npos);
    CHECK(ReadText(bottle / "system.reg.z3cloudplay-backup") == system_reg);
    // Again: already there, so the hive is left as it is.
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791600000);
    CHECK(applied.ok && ReadText(bottle / "system.reg") == written);
    std::filesystem::remove_all(script_root);
  }

  // A prospero-win-profiles sidecar becomes a community preset for its game.
  CommunityPreset community;
  CHECK(ParseProfileSidecar(R"({"schema_version":1,"profile":"half-life-2",
      "game":{"name":"Half-Life 2","steam_appid":220,"executable":"hl2.exe"},
      "requires":{"prospero_win":["#289","#296"]},
      "checks":[{"date":"2026-09-30","firmware":"12.02","rating":"works"},
                {"date":"2026-10-02","firmware":"12.02","rating":"playable","notes":"59.7 fps in the train station."}],
      "settings":{"resolution":"1920x1080","controller":"keyboard","graphics":"gdi","future_setting":"x","vsync":"maybe"}})",
                            &community, &reason));
  CHECK(community.steam_appid == 220 && community.preset.id == "community-half-life-2");
  CHECK(community.preset.name == "Community · Half-Life 2" && community.rating == "playable");
  CHECK(community.preset.short_hint == "playable on 12.02, 2026-10-02");
  CHECK(community.preset.settings.size() == 2 && community.preset.settings.at("controller") == "keyboard");
  CHECK(community.requires_prospero_win.size() == 2);
  CHECK(CommunityPresetSupported(community, KitCapabilities{}));       // pull request numbers are not features
  auto needs_unknown = community;
  needs_unknown.requires_prospero_win.push_back("future-runtime-feature");
  CHECK(!CommunityPresetSupported(needs_unknown,
      KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true)));
  for (const auto& requirements_json : {"{\"prospero_win\":[17]}", "{\"prospero_win\":[\"\"]}",
                               "{\"prospero_win\":false}", "[]"}) {
    const std::string malformed = std::string(R"({"schema_version":1,"profile":"half-life-2",
      "game":{"name":"Half-Life 2"},"checks":[{}],"requires":)") + requirements_json + "}";
    CHECK(!ParseProfileSidecar(malformed, &needs_unknown, &reason));
  }
  auto needs_volume = community;
  needs_volume.requires_prospero_win.push_back("audio-volume");
  CHECK(!CommunityPresetSupported(needs_volume, KitCapabilities{}));
  CHECK(CommunityPresetSupported(needs_volume, KitCapabilitiesFromManifest({kKitAudioVolume}, false)));
  needs_volume.requires_prospero_win.push_back("profile-environment");
  CHECK(!CommunityPresetSupported(needs_volume, KitCapabilitiesFromManifest({kKitAudioVolume}, false)));
  CHECK(BuildEffectiveSettings(hl2, &community.preset, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.values.at("controller").value == "keyboard");
  CHECK(!ParseProfileSidecar(R"({"schema_version":2,"profile":"x","game":{"name":"X"},"checks":[{}]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X"},"checks":[]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X","steam_appid":1.5},"checks":[{}]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X","steam_appid":4294967296},"checks":[{}]})", &community, &reason));

  // Bad player values are rejected before saving; stale ones never block a launch.
  CHECK(!ValidateSettingValue(hl2, "resolution", "3840x2160", &reason));
  CHECK(!ValidateSettingValue(hl2, "extra_arguments", "-a \"b", &reason));
  CHECK(!ValidateSettingValue(hl2, "extra_arguments", "{resolution}", &reason));
  CHECK(!ValidateSettingValue(hl2, "library_overrides", "d3d9", &reason));
  CHECK(!ValidateSettingValue(hl2, "button.cross", "f99", &reason));
  CHECK(!ValidateSettingValue(hl2, "launch_option", "missing", &reason));
  CHECK(!ValidateSettingValue(hl2, "unknown", "x", &reason));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"launch_option", "missing"}, {"resolution", "640x480"}}, KitCapabilities{}, &settings, &error));
  CHECK(settings.launch->id == "hl2" && Value(settings, "resolution") == "1920x1080");
  std::string long_arguments;
  for (int index = 0; index < 30; ++index) long_arguments += "-x ";
  CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", long_arguments}}, KitCapabilities{}, &settings, &error));
  CHECK(error.setting_id == "extra_arguments");

  // Recipes and presets with bad data are refused.
  CHECK(!ParseGameRecipe("{}", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"graphics":"vulkan"})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"defaults":{"resolution":"1x1"}})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1.5,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":-1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":4294967296,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"graphics":"gdi"}}]})", &presets, &reason));
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"extra_arguments":"-a"}}]})", &presets, &reason));

  // The per-game record round-trips and drops settings this build does not know.
  GameSettingsRecord record{"mouse-driven", {{"resolution", "1280x720"}, {"extra_arguments", "-a \"b c\"\\"}}, 7};
  GameSettingsRecord loaded;
  CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &loaded, &reason));
  CHECK(loaded.preset_id == record.preset_id && loaded.overrides == record.overrides && loaded.revision == 7);
  for (const auto revision : {UINT64_C(9007199254740993), std::numeric_limits<std::uint64_t>::max()}) {
    record.revision = revision;
    const auto serialized = SerializeGameSettingsRecord(record);
    CHECK(serialized.find(std::to_string(revision)) != std::string::npos);
    CHECK(ParseGameSettingsRecord(serialized, &loaded, &reason) && loaded.revision == revision);
  }
  CHECK(!ParseGameSettingsRecord(R"({"preset":"","revision":-1,"overrides":{}})", &loaded, &reason));
  CHECK(!ParseGameSettingsRecord(R"({"preset":"","revision":1.5,"overrides":{}})", &loaded, &reason));
  CHECK(!ParseGameSettingsRecord(
      R"({"preset":"","revision":18446744073709551616,"overrides":{}})", &loaded, &reason));
  CHECK(ParseGameSettingsRecord(R"({"preset":"","overrides":{"future_setting":"x","vsync":"on"}})", &loaded, &reason));
  CHECK(loaded.overrides.size() == 1 && loaded.overrides.count("vsync"));
  CHECK(!ParseGameSettingsRecord("[]", &loaded, &reason));

  // The bundled data loads through the recipe list, as the console title reads it.
  std::vector<SettingsPreset> loaded_presets;
  std::vector<GameRecipe> loaded_recipes;
  CHECK(LoadSettingsData((source / "runtime").string(), &loaded_presets, &loaded_recipes, &reason));
  CHECK(loaded_presets.size() == 4 && loaded_recipes.size() == 1 && loaded_recipes.front().app_id == 220);
  CHECK(!LoadSettingsData((source / "missing").string(), &loaded_presets, &loaded_recipes, &reason));

  // ApplyGameSettings writes the profile and a Z3CloudPlay dxvk.conf, removes it when nothing is
  // set, and refuses to touch a dxvk.conf that came with the game.
  const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    ("z3cloudplay-apply-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "game");
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  const ProfileTarget apply_target{"z3cloudplay-steam-220", (root / "game").string()};
  const auto prospero = (root / "prospero").string();
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(hl2, settings, apply_target, prospero, &reason));
  CHECK(ReadText(root / "game" / "dxvk.conf") == RenderDxvkConfig(settings));
  CHECK(ReadText(root / "prospero" / "profiles" / "z3cloudplay-steam-220.profile").find("[display]") != std::string::npos);
  CHECK(ReadText(root / "prospero" / "profiles" / "profiles.lst") == "z3cloudplay-steam-220.profile\n");
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(hl2, settings, apply_target, prospero, &reason));
  CHECK(!std::filesystem::exists(root / "game" / "dxvk.conf"));
  std::ofstream(root / "game" / "dxvk.conf") << "d3d9.samplerAnisotropy = 16\n";
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "60"}}, dxvk_kit, &settings, &error));
  const auto profile_before = ReadText(root / "prospero" / "profiles" / "z3cloudplay-steam-220.profile");
  CHECK(!ApplyGameSettings(hl2, settings, apply_target, prospero, &reason) && reason.find("own dxvk.conf") != std::string::npos);
  CHECK(ReadText(root / "game" / "dxvk.conf") == "d3d9.samplerAnisotropy = 16\n");
  CHECK(ReadText(root / "prospero" / "profiles" / "z3cloudplay-steam-220.profile") == profile_before);
  // DXVK reads dxvk.conf from the launch working directory.
  auto nested = hl2;
  nested.launch_entries.front().executable = "bin/hl2.exe";
  nested.launch_entries.front().working_directory = "bin";
  std::filesystem::create_directory(root / "game" / "bin");
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  CHECK(std::filesystem::exists(root / "game" / "bin" / "dxvk.conf"));
  CHECK(ReadText(root / "game" / "dxvk.conf") == "d3d9.samplerAnisotropy = 16\n");
  std::filesystem::create_directory(root / "game" / "fifo-cwd");
  CHECK(::mkfifo((root / "game" / "fifo-cwd" / "dxvk.conf").c_str(), 0600) == 0);
  nested.launch_entries.front().working_directory = "fifo-cwd";
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(!ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  // Clearing the shader cache removes only <exe>.dxvk-cache.
  std::ofstream(root / "game" / "hl2.dxvk-cache") << "cache";
  std::ofstream(root / "game" / "hl2.exe") << "game";
  CHECK(ClearShaderCache(hl2.launch_entries.front(), (root / "game").string(), &reason));
  CHECK(!std::filesystem::exists(root / "game" / "hl2.dxvk-cache") && std::filesystem::exists(root / "game" / "hl2.exe"));
  CHECK(ClearShaderCache(hl2.launch_entries.front(), (root / "game").string(), &reason));  // nothing to clear
  std::ofstream(root / "game" / "Mixed.dxvk-cache") << "cache";
  CHECK(ClearShaderCache({"mixed", "Mixed", "Mixed.Exe", ""}, (root / "game").string(), &reason));
  CHECK(!std::filesystem::exists(root / "game" / "Mixed.dxvk-cache"));
  CHECK(!ClearShaderCache({"bad", "Bad", "../escape.exe", ""}, (root / "game").string(), &reason));
  const auto outside = root / "outside";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "escape.dxvk-cache") << "foreign";
  std::filesystem::create_directory_symlink(outside, root / "game" / "linked");
  CHECK(!ClearShaderCache({"escape", "Escape", "escape.exe", "linked"}, (root / "game").string(), &reason));
  CHECK(ReadText(outside / "escape.dxvk-cache") == "foreign");
  nested.launch_entries.front().working_directory = "linked";
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(!ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  CHECK(!std::filesystem::exists(outside / "dxvk.conf"));

  // An oversized registry file is rejected before backup or replacement.
  const auto prefix = root / "prefix";
  std::filesystem::create_directory(prefix);
  std::filesystem::permissions(prefix, std::filesystem::perms::owner_all);
  std::string oversized_registry = "WINE REGISTRY Version 2\n#";
  oversized_registry.resize(32u * 1024 * 1024 + 1, 'x');
  std::ofstream(prefix / "user.reg", std::ios::binary) << oversized_registry;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"text_scale", "120"}}, registry_kit, &settings, &error));
  CHECK(!ApplyRegistrySettings(settings, prefix.string(), 1, &reason));
  CHECK(ReadText(prefix / "user.reg") == oversized_registry);
  CHECK(!std::filesystem::exists(prefix / "user.reg.z3cloudplay-backup"));
  const auto linked_prefix = root / "linked-prefix";
  std::filesystem::create_directory(linked_prefix);
  std::ofstream(outside / "user.reg") << "WINE REGISTRY Version 2\n";
  std::filesystem::create_symlink(outside / "user.reg", linked_prefix / "user.reg");
  CHECK(!ApplyRegistrySettings(settings, linked_prefix.string(), 1, &reason));
  CHECK(ReadText(outside / "user.reg") == "WINE REGISTRY Version 2\n");

  // The per-game settings file round-trips reset records and tolerates junk entries without
  // truncating out-of-range app ids into valid keys.
  const auto file = (root / "game-settings.json").string();
  std::map<std::uint32_t, GameSettingsRecord> records = {
      {220, {"mouse-driven", {{"resolution", "1280x720"}}, UINT64_C(9007199254740993)}},
      {400, {"", {}, std::numeric_limits<std::uint64_t>::max()}}};
  std::map<std::uint32_t, GameSettingsRecord> loaded_records;
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason) && loaded_records.empty());  // missing file
  const auto stale_temporary = file + ".z3cloudplay-tmp-" + std::to_string(static_cast<long long>(::getpid()));
  std::ofstream(stale_temporary) << "stale";
  CHECK(SaveGameSettingsFile(file, records, &reason));
  CHECK(ReadText(stale_temporary) == "stale");
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason));
  CHECK(loaded_records == records);
  std::ofstream(outside / "settings.json") << "foreign";
  const auto linked_settings = root / "linked-settings.json";
  std::filesystem::create_symlink(outside / "settings.json", linked_settings);
  CHECK(!SaveGameSettingsFile(linked_settings.string(), records, &reason));
  CHECK(ReadText(outside / "settings.json") == "foreign");
  std::ofstream(file) << R"({"version":1,"games":{"220":{"preset":"","overrides":{"vsync":"on"}},"221":{"preset":"bad","revision":-1,"overrides":{}},"222":{"preset":"bad","revision":1.5,"overrides":{}},"223":{"preset":"bad","revision":18446744073709551616,"overrides":{}},"4294967295":{"preset":"","revision":1,"overrides":{}},"4294967296":{"preset":"bad","revision":2,"overrides":{}},"x":{},"0":{}}})";
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason) && loaded_records.size() == 2);
  CHECK(loaded_records.count(std::numeric_limits<std::uint32_t>::max()) == 1);
  CHECK(loaded_records.count(220) == 1);
  std::ofstream(file) << "{";
  CHECK(!LoadGameSettingsFile(file, &loaded_records, &reason));
  const auto unreadable = root / "settings-directory";
  std::filesystem::create_directory(unreadable);
  CHECK(!LoadGameSettingsFile(unreadable.string(), &loaded_records, &reason));
  CHECK(reason.find("cannot read") != std::string::npos);
  const auto fifo_settings = root / "settings-fifo";
  CHECK(::mkfifo(fifo_settings.c_str(), 0600) == 0);
  CHECK(!LoadGameSettingsFile(fifo_settings.string(), &loaded_records, &reason));
  CHECK(reason.find("cannot read") != std::string::npos);
  std::filesystem::remove_all(root);

  // Integration contract: kit capabilities come from the verified kit manifest; bottle access
  // is the adapter's own fact.
  const auto none_kit = KitCapabilitiesFromManifest({"wine-wow64"}, false);
  CHECK(!none_kit.dxvk_config && !none_kit.environment && !none_kit.input_tuning && !none_kit.registry);
  const auto full_kit = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true);
  CHECK(full_kit.dxvk_config && full_kit.dxvk_present && full_kit.environment && full_kit.input_tuning && full_kit.registry);
  // A kit that ships DXVK (kit H) takes dxvk.conf settings without declaring settings-dxvk-config.
  const auto dxvk_only = KitCapabilitiesFromManifest({"dxvk", "opengl-zink"}, true);
  CHECK(dxvk_only.dxvk_config && !dxvk_only.dxvk_present && !dxvk_only.environment);

  // BuildSettingsLaunchPlan: profile, referenced input presets, dxvk.conf, registry, revision.
  const auto plan_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                         ("z3cloudplay-plan-" + std::to_string(::getpid()));
  std::filesystem::remove_all(plan_root);
  std::filesystem::create_directories(plan_root / "game");
  std::filesystem::create_directories(plan_root / "prefix");
  std::filesystem::permissions(plan_root / "prefix", std::filesystem::perms::owner_all);
  { std::ofstream(plan_root / "game" / "hl2.exe") << "MZ"; }
  const std::string bottle_reg = "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop] 1\n\"CaretWidth\"=dword:00000001\n";
  { std::ofstream(plan_root / "prefix" / "user.reg") << bottle_reg; }
  SettingsLaunchInput launch_input;
  launch_input.recipe = &hl2;
  launch_input.record.overrides = {{"frame_limit", "30"}, {"controller_preset", "gamepad"}, {"windows_version", "win7"}};
  launch_input.record.revision = 42;
  launch_input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, false);
  launch_input.target = {"z3cloudplay-steam-220", (plan_root / "game").string()};
  launch_input.runtime_dir = (source / "runtime").string();
  SettingsLaunchPlan plan;
  launch_input.record.preset_id = "mouse-driven";
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // named preset missing
  launch_input.preset = &presets.front();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // wrong named preset
  launch_input.preset = mouse;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  launch_input.record.preset_id.clear();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // named preset cannot masquerade as default
  launch_input.preset = &presets.front();
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // canonical default object
  launch_input.preset = nullptr;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  CHECK(plan.app_id == 220 && plan.recipe_id == hl2.id && plan.recipe_version == std::to_string(hl2.version));
  CHECK(plan.profile_id == "z3cloudplay-steam-220" && plan.settings_revision == 42 && plan.launch_entry_id == "hl2");
  CHECK(plan.profile_bytes.find("\npreset = gamepad\n") != std::string::npos && CheckProsperoProfile(plan.profile_bytes, &reason));
  CHECK(plan.input_presets.size() == 1 && plan.input_presets[0].first == "gamepad.input" &&
        plan.input_presets[0].second == ReadText(source / "runtime/input/gamepad.input"));
  CHECK(plan.dxvk_config.find("d3d9.maxFrameRate = 30\n") != std::string::npos);
  CHECK(plan.registry.empty());                                   // no bottle access: nothing for the registry
  ProfilePublication publication;
  publication.recipe_id = "wrong-recipe";
  publication.recipe_version = "999";
  publication.required_input_presets = {"stale.input"};
  FillProfilePublication(plan, &publication);
  CHECK(publication.profile_id == plan.profile_id && publication.profile_bytes == plan.profile_bytes &&
        publication.settings_revision == 42);
  CHECK(publication.recipe_id == plan.recipe_id && publication.recipe_version == plan.recipe_version);
  CHECK(publication.required_input_presets == std::vector<std::string>{"gamepad.input"} &&
        publication.input_presets.size() == 1 && publication.input_presets[0].filename == "gamepad.input");
  // A preset the build does not ship blocks the plan; a stale override is ignored instead.
  launch_input.runtime_dir = (plan_root / "missing").string();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason) && reason.find("gamepad") != std::string::npos);
  launch_input.runtime_dir = (source / "runtime").string();
  auto bad_input = launch_input;
  bad_input.record.overrides["frame_limit"] = "999";
  CHECK(BuildSettingsLaunchPlan(bad_input, &plan, &reason) && plan.dxvk_config.empty());

  // ApplySettingsFiles writes dxvk.conf and, with bottle access, the registry with a backup.
  launch_input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, true);
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.registry.size() == 2);  // version, and 100% text scale (removes)
  CHECK(::chmod((plan_root / "prefix/user.reg").c_str(), 0644) == 0);  // legacy owned file
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300000, &reason));
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == plan.dxvk_config);
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe] 1791300000\n\"Version\"=\"win7\"\n") !=
        std::string::npos);
  CHECK(ReadText(plan_root / "prefix" / "user.reg.z3cloudplay-backup") == bottle_reg);
  struct stat registry_mode {}, registry_backup_mode {};
  CHECK(::stat((plan_root / "prefix/user.reg").c_str(), &registry_mode) == 0);
  CHECK((registry_mode.st_mode & 0777) == 0600);
  CHECK(::stat((plan_root / "prefix/user.reg.z3cloudplay-backup").c_str(), &registry_backup_mode) == 0);
  CHECK((registry_backup_mode.st_mode & 0777) == 0600);
  // The apply consumes a value snapshot, even after recipes reload/change.
  GameRecipe temporary_recipe = hl2;
  launch_input.recipe = &temporary_recipe;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  for (auto& launch : temporary_recipe.launch_entries) launch.working_directory = "missing-after-reload";
  plan.registry = {{"Software\\Wine\\AppDefaults\\hl2.exe", "Version", "\"winxp\""}};
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300000, &reason));
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("\"Version\"=\"winxp\"") != std::string::npos);
  launch_input.recipe = &hl2;

  // Back to defaults: Z3CloudPlay's dxvk.conf goes, the per-game Windows version is removed.
  launch_input.record.overrides.clear();
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.dxvk_config.empty() && plan.input_presets.empty());
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300001, &reason));
  CHECK(!std::filesystem::exists(plan_root / "game" / "dxvk.conf"));
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("\"Version\"") == std::string::npos);
  // A dxvk.conf that came with the game is never replaced, and then the bottle is untouched too.
  { std::ofstream(plan_root / "game" / "dxvk.conf") << "d3d9.maxFrameRate = 20\n"; }
  launch_input.record.overrides = {{"frame_limit", "30"}, {"windows_version", "winxp"}};
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  const auto reg_before = ReadText(plan_root / "prefix" / "user.reg");
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300002, &reason));
  CHECK(reason.find("own dxvk.conf") != std::string::npos);
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == "d3d9.maxFrameRate = 20\n" &&
        ReadText(plan_root / "prefix" / "user.reg") == reg_before);
  // A bottle whose user.reg cannot be edited refuses the whole apply: dxvk.conf is put back.
  std::filesystem::remove(plan_root / "game" / "dxvk.conf");
  { std::ofstream(plan_root / "prefix" / "user.reg") << "not a registry"; }
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300003, &reason));
  CHECK(!std::filesystem::exists(plan_root / "game" / "dxvk.conf"));
  CHECK(ReadText(plan_root / "prefix" / "user.reg") == "not a registry");
  { std::ofstream(plan_root / "game" / "dxvk.conf") << "# Generated by Z3CloudPlay from this game's settings. old\n"; }
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300004, &reason));
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == "# Generated by Z3CloudPlay from this game's settings. old\n");
  CHECK(::chmod((plan_root / "game" / "dxvk.conf").c_str(), 0600) == 0);
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300005, &reason));
  struct stat preserved_mode {};
  CHECK(::stat((plan_root / "game" / "dxvk.conf").c_str(), &preserved_mode) == 0);
  CHECK((preserved_mode.st_mode & 0777) == 0600);
  // Removing a default config then restoring it must also preserve its mode.
  launch_input.record.overrides = {{"windows_version", "winxp"}};
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.dxvk_config.empty());
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300006, &reason));
  CHECK(::stat((plan_root / "game" / "dxvk.conf").c_str(), &preserved_mode) == 0);
  CHECK((preserved_mode.st_mode & 0777) == 0600);
  std::filesystem::remove_all(plan_root);

  // Games without a shipped recipe: one is generated from Steam's launch options.
  {
    const auto game_root = std::filesystem::temp_directory_path() / ("z3cloudplay-generated-recipe-" + std::to_string(::getpid()));
    std::filesystem::remove_all(game_root);
    WritePe(game_root / "Bin" / "Game.exe", true);
    WritePe(game_root / "Launcher32.exe", false);
    WritePe(game_root / "Bin" / "Tool.exe", true);
    std::vector<LaunchEntry> launches = {
        {"", "", "setup.bat", "", ""},                                  // not an .exe
        {"", "Play", "bin\\game.exe", "Bin", "-windowed  -nosplash"},  // case differs from the disk
        {"", "", "Launcher32.exe", "", ""},                             // other architecture
        {"", "", "Missing.exe", "", ""},
        {"", "", "Bin/Tool.exe", "", ""},
    };
    GameRecipe generated;
    CHECK(GenerateGameRecipe(4242, "Example", launches, game_root.string(), &generated, &reason));
    CHECK(generated.id == GeneratedRecipeId(4242) && IsGeneratedRecipeId(generated.id) && !IsGeneratedRecipeId(hl2.id));
    CHECK(generated.app_id == 4242 && generated.version == 2 && generated.architecture == "pe64");
    CHECK(generated.graphics == "auto" && generated.dll_overrides.empty() && generated.locked.empty());
    CHECK(generated.launch_entries.size() == 2);
    CHECK(generated.launch_entries[0].executable == "Bin/Game.exe" && generated.launch_entries[0].label == "Play");
    CHECK(generated.launch_entries[0].working_directory == "Bin");
    CHECK(generated.launch_entries[0].arguments == "-windowed -nosplash");
    CHECK(generated.launch_entries[1].executable == "Bin/Tool.exe" && generated.launch_entries[1].label == "Tool.exe");
    // The recipe round-trips through its JSON form.
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(generated), &reread, &reason));
    CHECK(SerializeGameRecipe(reread) == SerializeGameRecipe(generated));
    // The launch entry's own arguments reach the profile, before the player's.
    EffectiveSettings generated_settings;
    SettingsError generated_error;
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"extra_arguments", "-dev"}}, KitCapabilities{},
                                 &generated_settings, &generated_error));
    CHECK(generated_settings.arguments == "-windowed -nosplash -dev");
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"launch_option", "launch-2"}}, KitCapabilities{},
                                 &generated_settings, &generated_error));
    CHECK(generated_settings.arguments.empty() && generated_settings.launch->executable == "Bin/Tool.exe");
    // A 32-bit game is pe32.
    CHECK(GenerateGameRecipe(4243, "Old", {{"", "", "Launcher32.exe", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(generated.architecture == "pe32");
    // Nothing startable: a missing program names itself; no .exe at all says so.
    CHECK(!GenerateGameRecipe(4244, "Gone", {{"", "", "Missing.exe", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(reason.find("Missing.exe") != std::string::npos);
    CHECK(!GenerateGameRecipe(4245, "Script", {{"", "", "run.sh", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(reason.find("no Windows program") != std::string::npos);
    std::filesystem::remove_all(game_root);
  }

  {  // Everything the recipe sets has a row the player can change and reset (issue 52).
    const ProfileTarget target{"z3cloudplay-steam-220", "/data/homebrew/FAKE77031/games/220"};
    EffectiveSettings settings;
    SettingsError error;
    // Required arguments start as the recipe's and can be replaced; {resolution} still expands.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("arguments").value == hl2.arguments && settings.values.at("arguments").source == ValueSource::kRecipe);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"arguments", "-game hl2 {resolution}"}, {"extra_arguments", "-dev"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.arguments == "-game hl2 -w 1920 -h 1080 -dev");
    CHECK(ValidateSettingValue(hl2, "arguments", "", &reason) && !ValidateSettingValue(hl2, "arguments", "{w}", &reason));
    // Library overrides: the whole effective list, from the recipe and what an earlier build added.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"library_overrides", "dinput8=n,b"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("dll_overrides").value == "d3d9=n;dinput8=n,b" && settings.dll_overrides == "d3d9=n;dinput8=n,b");
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"dll_overrides", ""}, {"library_overrides", "dinput8=n"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.dll_overrides.empty() && settings.values.at("dll_overrides").source == ValueSource::kUser);
    ProsperoProfile profile;
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("dll_overrides") == std::string::npos);
    // A generated DXVK recipe lists its DXVK overrides. They follow graphics, also in a list the
    // player saved: kept while graphics is DXVK, gone after a switch, and never removable.
    auto generated = hl2;
    generated.id = GeneratedRecipeId(220);
    generated.dll_overrides.clear();
    generated.locked.clear();
    CHECK(BuildEffectiveSettings(generated, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("dll_overrides").value == kDxvkOverrides);
    CHECK(IsGraphicsDllOverride(generated, kDxvkOverrides) && !IsGraphicsDllOverride(hl2, kDxvkOverrides));
    const std::string saved = std::string(kDxvkOverrides) + ";dinput8=n,b";
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", saved}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.dll_overrides == saved);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", "dinput8=n,b"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.dll_overrides == saved);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", saved}, {"graphics", "opengl"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.dll_overrides == "dinput8=n,b");
    // A locked row keeps the recipe's value until the player changes it anyway.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"graphics", "opengl"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.values.at("graphics").locked && !settings.values.at("graphics").unlocked);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"graphics", "opengl"}, {"unlock.graphics", "on"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.values.at("graphics").unlocked &&
          settings.values.at("graphics").reason.find("may not start") != std::string::npos);
    // ...including the Steam library a shipped recipe keeps.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"steam_features", "emulate"}, {"unlock.steam_features", "on"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate");
    // The unlock survives the saved record; an unlock for an unknown row does not.
    GameSettingsRecord record;
    record.overrides = {{"unlock.graphics", "on"}, {"graphics", "opengl"}};
    GameSettingsRecord reread;
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason) && reread == record);
    record.overrides["unlock.nothing"] = "on";
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason) && !reread.overrides.count("unlock.nothing"));
    // A program from the game folder, with its own folder and arguments; the architecture follows it.
    const std::map<std::string, std::string> program = {{"program", "bin\\win64\\hl2_win64.exe"}, {"architecture", "pe64"},
        {"program_folder", "bin"}, {"program_arguments", "-console"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, program, KitCapabilities{}, &settings, &error));
    CHECK(settings.launch->id == kProgramEntryId && settings.launch->executable == "bin/win64/hl2_win64.exe" &&
          settings.launch->label == "hl2_win64.exe" && settings.launch->working_directory == "bin");
    CHECK(settings.values.at("launch_option").value == kProgramEntryId && settings.architecture == "pe64");
    CHECK(settings.arguments == "-game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1 -console");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\\games\\220\\bin\\win64\\hl2_win64.exe\nworking_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\\bin\n") !=
          std::string::npos);
    CHECK(profile.text.find("\narchitecture = pe64\n") != std::string::npos);
    // Without a program of the player's, the architecture is the recipe's; a launch entry takes the folder too.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"architecture", "pe64"}, {"program_folder", "bin"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.architecture == "pe32" && settings.launch->id == "hl2" && settings.launch->working_directory == "bin");
    CHECK(settings.values.at("program_folder").source == ValueSource::kUser);
    CHECK(!ValidateSettingValue(hl2, "program", "run.bat", &reason) && !ValidateSettingValue(hl2, "program", "../x.exe", &reason));
    CHECK(!ValidateSettingValue(hl2, "program_folder", "C:\\Games", &reason) && ValidateSettingValue(hl2, "program_folder", "", &reason));
    // A recipe without a launch entry still gives a sheet: the Program row is where the player starts.
    auto empty = generated;
    empty.launch_entries.clear();
    CHECK(BuildEffectiveSettings(empty, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.launch && !RenderProsperoProfile(empty, settings, target, &profile, &reason));
    CHECK(BuildEffectiveSettings(empty, nullptr, {{"program", "Game.exe"}}, KitCapabilities{}, &settings, &error) && settings.launch);

    // Export as recipe: the effective settings as a community recipe, read back as a recipe and as one.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"resolution", "1280x720"}, {"program", "hl2_win64.exe"},
                                 {"architecture", "pe64"}, {"unlock.graphics", "on"}, {"graphics", "opengl"},
                                 {"arguments", "-game hl2 {resolution}"}, {"dll_overrides", "d3d9=n;xinput1_3=n"}},
                                 KitCapabilities{}, &settings, &error));
    auto community = BuildCommunityRecipe(hl2, settings);
    {  // What the files showed travels without the graphics value, which has its own field (#67).
      auto detected = hl2;
      detected.detected = "d3d9 graphics=dxvk";
      CHECK(BuildCommunityRecipe(detected, settings).recipe.detected == "d3d9");
    }
    community.min_kit = "kit-h";
    community.tested_date = "2026-10-10";
    community.status = "works";
    community.notes = "Starts \"fine\".";
    community.base_repository = "https://github.com/example/prospero-win-profiles";
    community.base_path = "profiles/hl2.profile";
    const auto text = SerializeCommunityRecipe(community);
    GameRecipe as_recipe;
    CHECK(ParseGameRecipe(text, &as_recipe, &reason));
    CHECK(as_recipe.id == "community-220" && as_recipe.launch_entries.size() == 1 &&
          as_recipe.launch_entries[0].executable == "hl2_win64.exe" && as_recipe.architecture == "pe64" &&
          as_recipe.graphics == "opengl" && as_recipe.arguments == "-game hl2 {resolution}" &&
          as_recipe.resolution_arguments == hl2.resolution_arguments && as_recipe.dll_overrides == "d3d9=n;xinput1_3=n");
    CHECK(as_recipe.defaults.at("resolution") == "1280x720" && as_recipe.defaults.at("button.options") == "escape");
    CHECK(!as_recipe.defaults.count("controller") && as_recipe.locked.empty());  // xinput is Z3CloudPlay's default too
    CommunityRecipe parsed;
    CHECK(ParseCommunityRecipe(text, &parsed, &reason));
    CHECK(parsed.executable == "hl2_win64.exe" && parsed.status == "works" && parsed.notes == "Starts \"fine\"." &&
          parsed.base_path == "profiles/hl2.profile" && SerializeCommunityRecipe(parsed) == text);
    // The same game started from the exported recipe gets the same command.
    EffectiveSettings again;
    CHECK(BuildEffectiveSettings(as_recipe, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.arguments == "-game hl2 -w 1280 -h 720" && again.dll_overrides == settings.dll_overrides);
    // A recipe exported from a generated one keeps the Steam emulator, unlocked.
    auto from_generated = generated;
    from_generated.defaults["steam_features"] = "emulate";
    CHECK(BuildEffectiveSettings(from_generated, nullptr, {}, KitCapabilities{}, &settings, &error));
    GameRecipe exported;
    CHECK(ParseGameRecipe(SerializeCommunityRecipe(BuildCommunityRecipe(from_generated, settings)), &exported, &reason));
    CHECK(exported.defaults.at("steam_features") == "emulate" && IsCommunityRecipeId(exported.id));
    CHECK(BuildEffectiveSettings(exported, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.values.at("steam_features").value == "emulate" && !again.values.at("steam_features").locked);
    CHECK(again.dll_overrides == kDxvkOverrides);  // the DXVK overrides are written out, not added again
    auto broken = text;
    broken.replace(broken.find("\"works\""), 7, "\"great\"");
    CHECK(!ParseCommunityRecipe(broken, &parsed, &reason) && reason.find("status") != std::string::npos);
  }

  std::puts("game settings tests passed");
}
