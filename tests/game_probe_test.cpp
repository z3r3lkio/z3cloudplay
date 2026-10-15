#include "runtime/game_probe.h"
#include "runtime/game_settings.h"

#include "steam_stub_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace z3cloudplay::runtime;
namespace fs = std::filesystem;

namespace {

struct PeSpec {
  bool pe64 = true;
  std::vector<std::string> imports;
  bool clr = false;
  bool bind_entry = false;   // entry point inside a ".bind" section (SteamStub)
  std::string text;          // bytes placed in the code section (names loaded at run time)
  std::size_t data_at = 0x1400;  // file offset of the section holding the import table
  bool shared_data = false;  // the data section is writable and shared, on 4 KB pages
};

void Put32(std::string* bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}
void Put16(std::string* bytes, std::size_t at, std::uint16_t value) {
  (*bytes)[at] = static_cast<char>(value & 0xff);
  (*bytes)[at + 1] = static_cast<char>(value >> 8);
}

void WritePe(const fs::path& path, const PeSpec& spec) {
  fs::create_directories(path.parent_path());
  const std::size_t optional_size = spec.pe64 ? 0xf0 : 0xe0;
  std::string bytes(spec.data_at + 0x1000, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; Put32(&bytes, 0x3c, 0x40);
  bytes[0x40] = 'P'; bytes[0x41] = 'E';
  Put16(&bytes, 0x44, spec.pe64 ? 0x8664 : 0x014c);
  Put16(&bytes, 0x46, 2);
  Put16(&bytes, 0x54, static_cast<std::uint16_t>(optional_size));
  const std::size_t optional = 0x58;
  Put16(&bytes, optional, spec.pe64 ? 0x20b : 0x10b);
  Put32(&bytes, optional + 16, 0x1010);  // entry point, in the first section
  const std::size_t directories = optional + (spec.pe64 ? 112 : 96);
  if (!spec.imports.empty()) Put32(&bytes, directories + 8, 0x2000);
  if (spec.clr) Put32(&bytes, directories + 14 * 8, 0x2800);
  const std::size_t table = optional + optional_size;
  const auto section = [&](std::size_t row, const char* name, std::uint32_t address, std::uint32_t raw) {
    for (std::size_t i = 0; name[i]; ++i) bytes[table + row * 40 + i] = name[i];
    Put32(&bytes, table + row * 40 + 8, 0x1000);
    Put32(&bytes, table + row * 40 + 12, address);
    Put32(&bytes, table + row * 40 + 16, 0x1000);
    Put32(&bytes, table + row * 40 + 20, raw);
  };
  section(0, spec.bind_entry ? ".bind" : ".text", 0x1000, 0x400);
  section(1, ".rdata", 0x2000, static_cast<std::uint32_t>(spec.data_at));
  if (spec.shared_data) Put32(&bytes, table + 40 + 36, 0xd0000040);
  bytes.replace(0x400, spec.text.size(), spec.text);
  std::uint32_t name_rva = 0x2400;
  for (std::size_t i = 0; i < spec.imports.size(); ++i) {
    Put32(&bytes, spec.data_at + i * 20 + 12, name_rva);
    bytes.replace(spec.data_at + (name_rva - 0x2000), spec.imports[i].size(), spec.imports[i]);
    name_rva += static_cast<std::uint32_t>(spec.imports[i].size() + 1);
  }
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string Wide(const std::string& text) {
  std::string out;
  for (char value : text) out += value, out += '\0';
  return out;
}

void Touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << "x";
}

}  // namespace

int main() {
  const auto base = fs::temp_directory_path() / ("z3cloudplay-game-probe-" + std::to_string(::getpid()));
  fs::remove_all(base);
  int case_number = 0;
  const auto fresh = [&] { return base / std::to_string(++case_number); };

  {  // Imported Direct3D 9, with the import table far into a large program.
    const auto root = fresh();
    WritePe(root / "Game.exe", {false, {"KERNEL32.dll", "d3d9.dll"}, false, false, "", 9u * 1024u * 1024u});
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.apis.count("d3d9") && probe.engine.empty() && !probe.dotnet && !probe.steam_stub);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments.empty() && defaults.refusal.empty());
    CHECK(defaults.summary == "d3d9 graphics=dxvk");
  }
  {  // Unity loads Direct3D at run time (wide strings); D3D11 is forced.
    const auto root = fresh();
    WritePe(root / "Balls.exe", {true, {"UnityPlayer.dll", "KERNEL32.dll"}});
    WritePe(root / "UnityPlayer.dll", {true, {"KERNEL32.dll"}, false, false, Wide("d3d11.dll") + Wide("D3D12.dll")});
    WritePe(root / "GameAssembly.dll", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Balls_Data");
    const auto probe = ProbeGame(root.string(), "Balls.exe");
    CHECK(probe.engine == "unity" && probe.il2cpp && probe.apis.count("d3d11") && probe.apis.count("d3d12"));
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments == "-force-d3d11 {resolution}");
    CHECK(defaults.resolution_arguments == "-screen-width {w} -screen-height {h}");
    CHECK(defaults.summary == "unity-il2cpp,d3d11,d3d12 graphics=dxvk");
  }
  {  // Unity before 2017.2 (140): the player is in the program; Direct3D 9 is forced.
    const auto root = fresh();
    WritePe(root / "140.exe", {false, {"KERNEL32.dll", "d3d9.dll"}, false, false, "d3d11.dll"});
    fs::create_directories(root / "140_Data/Managed");
    const auto probe = ProbeGame(root.string(), "140.exe");
    CHECK(probe.engine == "unity" && probe.legacy_unity && !probe.il2cpp);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments == "-force-d3d9 {resolution}");
    CHECK(defaults.summary == "unity-legacy,d3d11,d3d9 graphics=dxvk");
    CHECK(DetectedLabel(defaults.summary) == "Unity \xC2\xB7 Direct3D 9");
  }
  {  // A linked UnityPlayer.dll is not followed.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"KERNEL32.dll"}});
    WritePe(base / "elsewhere.dll", {true, {}, false, false, "d3d11.dll"});
    fs::create_symlink(base / "elsewhere.dll", root / "UnityPlayer.dll");
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine.empty() && probe.apis.empty() && DefaultsFor(probe).graphics == "auto");
  }
  {  // GameMaker names D3D11 in upper case.
    const auto root = fresh();
    WritePe(root / "Risk.exe", {true, {"KERNEL32.dll"}, false, false, "D3D11.DLL"});
    Touch(root / "data.win");
    const auto probe = ProbeGame(root.string(), "Risk.exe");
    CHECK(probe.engine == "gamemaker" && probe.apis.count("d3d11") && !probe.shared_section);
    CHECK(DefaultsFor(probe).graphics == "dxvk");
  }
  {  // A shared writable section on 4 KB pages (GameMaker's ".mydata", Pizza Tower) can't be mapped.
    const auto root = fresh();
    WritePe(root / "PizzaTower.exe", {true, {"KERNEL32.dll"}, false, false, "D3D11.DLL", 0x1400, true});
    Touch(root / "data.win");
    const auto probe = ProbeGame(root.string(), "PizzaTower.exe");
    CHECK(probe.engine == "gamemaker" && probe.shared_section);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.refusal.find("shared memory section") != std::string::npos);
    CHECK(defaults.refusal_soft && defaults.summary == "gamemaker,d3d11,shared-section graphics=dxvk");
    // A kit whose Wine maps the section as a private copy starts it like any other game.
    RuntimeAbilities runtime;
    runtime.shared_sections = true;
    CHECK(DefaultsFor(probe, runtime).refusal.empty());
    GameRecipe recipe;
    std::string reason;
    CHECK(!GenerateGameRecipe(2231450, "Pizza Tower", {{"", "", "PizzaTower.exe", "", ""}}, root.string(), &recipe, &reason));
    KitCapabilities kit;
    kit.shared_sections = true;
    CHECK(GenerateGameRecipe(2231450, "Pizza Tower", {{"", "", "PizzaTower.exe", "", ""}}, root.string(), &recipe, &reason, kit));
    CHECK(KitCapabilitiesFromManifest({"dxvk", kKitSharedSections}, false).shared_sections);
  }
  {  // Programs the console cannot run are refused with a reason.
    const auto root = fresh();
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    WritePe(root / "Net.exe", {false, {"mscoree.dll"}});
    WritePe(root / "Clr.exe", {true, {"KERNEL32.dll"}, true});
    WritePe(root / "Twelve.exe", {true, {"d3d12.dll"}});
    CHECK(DefaultsFor(ProbeGame(root.string(), "Stub.exe")).refusal.find("Steam DRM") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Net.exe")).refusal.find(".NET") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Clr.exe")).refusal.find(".NET") != std::string::npos);
    // A kit with Wine Mono starts .NET programs; the other refusals stay.
    RuntimeAbilities mono;
    mono.wine_mono = true;
    CHECK(DefaultsFor(ProbeGame(root.string(), "Net.exe"), mono).refusal.empty());
    CHECK(DefaultsFor(ProbeGame(root.string(), "Clr.exe"), mono).refusal.empty());
    CHECK(DefaultsFor(ProbeGame(root.string(), "Stub.exe"), mono).refusal.find("Steam DRM") != std::string::npos);
    CHECK(KitCapabilitiesFromManifest({"dxvk", kKitWineMono}, false).wine_mono);
    CHECK(!KitCapabilitiesFromManifest({"dxvk"}, false).wine_mono);
    KitCapabilities kit;
    kit.wine_mono = true;
    CHECK(GameProgramRefusal(root.string(), "Net.exe", kit, false).empty());
    CHECK(GameProgramRefusal(root.string(), "Net.exe", KitCapabilities{}, false).find(".NET") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Twelve.exe")).refusal.find("Direct3D 12") != std::string::npos);
  }
  {  // A SteamStub variant Z3CloudPlay unpacks (BALL x PIT) is no reason to refuse; the recipe is made.
    const auto root = fresh();
    fs::create_directories(root);
    std::ofstream(root / "Balls.exe", std::ios::binary) << steam_stub_fixture::Build();
    WritePe(root / "UnityPlayer.dll", {true, {"KERNEL32.dll"}, false, false, Wide("d3d11.dll")});
    fs::create_directories(root / "Balls_Data");
    const auto probe = ProbeGame(root.string(), "Balls.exe");
    CHECK(probe.steam_stub && probe.steam_stub_supported && probe.engine == "unity");
    CHECK(DefaultsFor(probe).refusal.empty() && DefaultsFor(probe).graphics == "dxvk");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(2062430, "BALL x PIT", {{"", "", "Balls.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].executable == "Balls.exe");
    // Unsupported options are still refused.
    steam_stub_fixture::Spec unsupported;
    unsupported.flags = 0x100;
    std::ofstream(root / "Balls.exe", std::ios::binary | std::ios::trunc) << steam_stub_fixture::Build(unsupported);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Balls.exe")).refusal.find("can't unpack") != std::string::npos);
  }
  {  // OpenGL and plain Windows programs.
    const auto root = fresh();
    WritePe(root / "Gl.exe", {false, {"OPENGL32.dll"}});
    WritePe(root / "Plain.exe", {false, {"USER32.dll", "ddraw.dll"}});
    CHECK(DefaultsFor(ProbeGame(root.string(), "Gl.exe")).graphics == "opengl");
    const auto plain = ProbeGame(root.string(), "Plain.exe");
    CHECK(plain.apis.count("ddraw") && DefaultsFor(plain).graphics == "auto");
  }
  {  // Godot and Source.
    const auto root = fresh();
    WritePe(root / "win/garagerally.exe", {true, {"KERNEL32.dll"}, false, false, "vulkan-1.dll"});
    Touch(root / "win/garagerally.pck");
    const auto godot = DefaultsFor(ProbeGame(root.string(), "win/garagerally.exe"));
    CHECK(godot.graphics == "opengl" && godot.arguments == "--rendering-driver opengl3");
    WritePe(root / "hl2.exe", {false, {"KERNEL32.dll"}});
    WritePe(root / "bin/engine.dll", {false, {"d3d9.dll"}});
    const auto source = DefaultsFor(ProbeGame(root.string(), "hl2.exe"));
    CHECK(source.graphics == "dxvk" && source.resolution_arguments == "-w {w} -h {h}");
  }
  {  // LÖVE (Balatro): OpenGL, found through love.dll beside the program.
    const auto root = fresh();
    WritePe(root / "Balatro.exe", {true, {"love.dll", "lua51.dll", "KERNEL32.dll"}});
    WritePe(root / "love.dll", {true, {"SDL2.dll", "KERNEL32.dll"}});
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "Balatro.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.summary == "love graphics=opengl");
    CHECK(DetectedLabel(defaults.summary) == "L\xC3\x96VE \xC2\xB7 OpenGL");
  }
  {  // SIL (FTL): OpenGL, though the program also imports Direct3D 11; known by its settings names.
    const auto root = fresh();
    PeSpec ftl{false, {"OPENGL32.dll", "d3d11.dll", "KERNEL32.dll"}};
    ftl.text = "SIL_WINDOWS_USE_RAWINPUT";
    WritePe(root / "FTLGame.exe", ftl);
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "FTLGame.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.summary.rfind("sil,", 0) == 0);
    CHECK(DetectedLabel(defaults.summary) == "SIL \xC2\xB7 OpenGL");
  }
  {  // NW.js (Look Outside, RPG Maker MZ) is refused: Chromium can't start on the console.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"nw.dll", "KERNEL32.dll"}});
    WritePe(root / "nw.dll", {true, {"dxgi.dll", "KERNEL32.dll"}});
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "Game.exe"));
    CHECK(defaults.refusal.find("NW.js") != std::string::npos && defaults.summary.rfind("nwjs,", 0) == 0);
  }
  {  // An Unreal root program is replaced by its Shipping program, which starts in its own folder.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Engine/Binaries/Win64");
    WritePe(root / "Game/Binaries/Win64/Game-Win64-Shipping.exe", {true, {"d3d11.dll", "d3d12.dll"}});
    Touch(root / "EasyAntiCheat/settings.json");
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine == "unreal" && probe.unreal_program == "Game/Binaries/Win64/Game-Win64-Shipping.exe");
    CHECK(probe.anticheat && DefaultsFor(probe).arguments == "-dx11");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(9001, "Unreal", {{"", "", "Game.exe", "", "-nosplash"}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "Game/Binaries/Win64/Game-Win64-Shipping.exe");
    CHECK(recipe.launch_entries[0].working_directory == "Game/Binaries/Win64");
    CHECK(recipe.launch_entries[0].arguments == "-nosplash" && recipe.arguments == "-dx11" && recipe.graphics == "dxvk");
    CHECK(recipe.detected == "unreal,anticheat graphics=dxvk");
  }
  {  // Generated recipes: DXVK through overrides that follow the graphics value; graphics is open.
    const auto root = fresh();
    WritePe(root / "Balls.exe", {true, {"UnityPlayer.dll"}});
    WritePe(root / "UnityPlayer.dll", {true, {}, false, false, "d3d11.dll"});
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(2062430, "BALL x PIT", {{"", "", "Balls.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.graphics == "dxvk" && recipe.dll_overrides.empty() && recipe.locked.empty());
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(recipe), &reread, &reason) && reread.detected == recipe.detected);
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"library_overrides", "winmm=n"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("graphics").locked && settings.graphics == "dxvk");
    CHECK(settings.dll_overrides == std::string(kDxvkOverrides) + ";winmm=n");
    CHECK(settings.arguments == "-force-d3d11 -screen-width 1920 -screen-height 1080");
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"graphics", "auto"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "auto" && settings.dll_overrides.empty());
    // Steam features: emulation by default here, and the player may choose the game's own files.
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("steam_features").locked);
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"steam_features", "game"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game");
    // A shipped recipe keeps the game's own Steam library, whatever the player asks.
    auto shipped = recipe;
    shipped.id = "steam-2062430-shipped-v1";
    CHECK(BuildEffectiveSettings(shipped, nullptr, {{"steam_features", "emulate"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game" && settings.values.at("steam_features").locked);
    // Every program refused: the first reason is the player's.
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    GameRecipeDiagnostics diagnostics;
    CHECK(!GenerateGameRecipe(1, "Stub", {{"", "", "Stub.exe", "", ""}}, root.string(), &recipe, &reason, {}, {}, nullptr, &diagnostics));
    CHECK(diagnostics.steam_stub_program == "Stub.exe" && !diagnostics.steam_stub_detail.empty());
    CHECK(reason.find("Steam DRM") != std::string::npos);
  }

  {  // Final Fantasy XIII keeps its program four folders deep; the picker reaches it, but not
     // a tree that is deeper than any game ships.
    const auto root = fresh();
    Touch(root / "FFXiiiLauncher.exe");
    Touch(root / "white_data/prog/win/bin/ffxiiiimg.exe");
    Touch(root / "a/b/c/d/e/f/g/too-deep.exe");
    const auto programs = ListGamePrograms(root.string());
    CHECK(programs.size() == 2 && programs[0].path == "FFXiiiLauncher.exe" &&
          programs[1].path == "white_data/prog/win/bin/ffxiiiimg.exe");
  }

  {  // Selaco: Steam lists only .bat files around a .NET helper; the game's own program would start.
    const auto root = fresh();
    Touch(root / "Launch.bat");
    Touch(root / "LaunchEditor.bat");
    WritePe(root / "SELACO.exe", {true, {"KERNEL32.dll", "OPENGL32.dll"}});
    WritePe(root / "Tools/Editor.exe", {true, {"KERNEL32.dll"}, true});
    WritePe(root / "_CommonRedist/vcredist/vc_redist.x64.exe", {true, {"KERNEL32.dll"}});
    const std::vector<LaunchEntry> steam = {{"", "Play", "Launch.bat", "", ""}, {"", "Editor", "LaunchEditor.bat", "", ""}};
    GameRecipe recipe;
    std::string reason;
    bool soft = false;
    CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, {}, &soft));
    CHECK(reason.find("no Windows program") != std::string::npos && soft);  // choosing a program gets past it
    // The Program picker lists the folder's programs, not the redistributables.
    const auto programs = ListGamePrograms(root.string());
    CHECK(programs.size() == 2 && programs[0].path == "SELACO.exe" && programs[1].path == "Tools/Editor.exe");
    CHECK(programs[0].architecture == "pe64" && programs[0].refusal.empty() && programs[0].detected == "opengl graphics=opengl");
    CHECK(programs[1].refusal.find(".NET") != std::string::npos && programs[1].refusal_soft);
    CHECK(GameProgramArchitecture(root.string(), "selaco.EXE") == "pe64" && GameProgramArchitecture(root.string(), "x.exe").empty());
    // "Don't start" holds for a picked program; Try anyway gets past a soft refusal only.
    CHECK(GameProgramRefusal(root.string(), "Tools/Editor.exe", {}, false).find(".NET") != std::string::npos);
    CHECK(GameProgramRefusal(root.string(), "Tools/Editor.exe", {}, true).empty());
    CHECK(GameProgramRefusal(root.string(), "SELACO.exe", {}, false).empty());
    CHECK(!GameProgramRefusal(root.string(), "Missing.exe", {}, true).empty());
    // The player's program makes the recipe: first, with its architecture and engine defaults.
    RecipeChoices choices;
    choices.program = "SELACO.exe";
    CHECK(GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].id == kProgramEntryId &&
          recipe.launch_entries[0].executable == "SELACO.exe" && recipe.architecture == "pe64" && recipe.graphics == "opengl");
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"program", "SELACO.exe"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.launch && settings.launch->id == kProgramEntryId && settings.architecture == "pe64");
    // A .NET program is refused softly: "Try anyway" makes the recipe, with the engine's defaults.
    choices.program = "Tools/Editor.exe";
    CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices, &soft));
    CHECK(reason.find(".NET") != std::string::npos && soft);
    choices.try_anyway = true;
    CHECK(GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries[0].executable == "Tools/Editor.exe" && recipe.launch_entries[0].working_directory.empty());
    GameSettingsRecord record;
    record.overrides = {{"program", "Tools\\Editor.exe"}, {"start_check", "try"}};
    const auto from_record = RecipeChoicesFrom(record);
    CHECK(from_record.program == "Tools/Editor.exe" && from_record.try_anyway);
    // Hard limits stay refused, even when the player tries anyway.
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    WritePe(root / "Twelve.exe", {true, {"d3d12.dll"}});
    for (const char* program : {"Stub.exe", "Twelve.exe"}) {
      choices.program = program;
      CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices, &soft));
      CHECK(!soft);
    }
    WritePe(root / "Both.exe", {true, {"mscoree.dll", "d3d12.dll"}});
    CHECK(!DefaultsFor(ProbeGame(root.string(), "Both.exe")).refusal_soft);
    // Steam's entries stay as suggestions after the player's program, numbered as before.
    WritePe(root / "Other.exe", {true, {"KERNEL32.dll", "d3d11.dll"}});
    choices.program = "SELACO.exe";
    CHECK(GenerateGameRecipe(1592280, "Selaco", {{"", "Other", "Other.exe", "", "-x"}}, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[1].id == "launch-1" &&
          recipe.launch_entries[1].arguments == "-x");
  }

  {  // id Tech 3 (Return to Castle Wolfenstein): OpenGL, with Mesa's extension list cut to 2001.
    const auto root = fresh();
    WritePe(root / "WolfSP.exe", {false, {"KERNEL32.dll"}});
    Touch(root / "Main/pak0.pk3");
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "WolfSP.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.environment == "MESA_EXTENSION_MAX_YEAR=2001");
    CHECK(DetectedLabel(defaults.summary) == "id Tech 3 \xC2\xB7 OpenGL");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(9010, "Wolfenstein", {{"", "", "WolfSP.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.graphics == "opengl" && recipe.defaults.at("environment") == "MESA_EXTENSION_MAX_YEAR=2001");
    EffectiveSettings settings;
    SettingsError error;
    KitCapabilities zink;
    zink.opengl_zink = true;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {}, zink, &settings, &error));
    CHECK(settings.values.at("environment").value == "MESA_EXTENSION_MAX_YEAR=2001");
  }

  {  // Steam's default is a launcher (#70) or a .NET launcher (#46): the game's own program goes first.
    const auto root = fresh();
    WritePe(root / "Launcher.exe", {false, {"KERNEL32.dll", "USER32.dll", "DINPUT8.dll"}});
    WritePe(root / "BulletSoulWin.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
    WritePe(root / "Tools/Viewer.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
    WritePe(root / "Mono.exe", {false, {"mscoree.dll", "d3d9.dll"}});
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(544580, "BULLET SOUL", {{"", "Play", "Launcher.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "BulletSoulWin.exe" &&
          recipe.launch_entries[0].id == "game" && recipe.graphics == "dxvk");
    CHECK(recipe.launch_entries[1].executable == "Launcher.exe" && recipe.launch_entries[1].id == "launch-1");
    // In the launcher's folder first: an updater under bin/ picks the game beside it.
    WritePe(root / "bin/Updater.exe", {false, {"KERNEL32.dll"}});
    WritePe(root / "bin/game_d3d11.exe", {false, {"KERNEL32.dll", "d3d11.dll"}});
    CHECK(GenerateGameRecipe(233270, "Updater", {{"", "", "bin/Updater.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "bin/game_d3d11.exe");
    // A .NET launcher: the native game instead of a refusal.
    WritePe(root / "Hotline.exe", {false, {"mscoree.dll"}, true});
    CHECK(GenerateGameRecipe(219150, "Hotline", {{"", "", "Hotline.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "BulletSoulWin.exe" && recipe.launch_entries.size() == 1);
    {  // A redistributable installer is never the game, even when it imports Direct3D (Antichamber's UE3Redist.exe).
      const auto udk = fresh();
      WritePe(udk / "Binaries/Windows/UE3Redist.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
      WritePe(udk / "Binaries/Win32/UDK.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
      CHECK(GenerateGameRecipe(219890, "Antichamber", {{"", "", "Binaries/Windows/UE3Redist.exe", "", ""}}, udk.string(), &recipe, &reason));
      CHECK(recipe.launch_entries[0].executable == "Binaries/Win32/UDK.exe");
    }
    // A program that draws, or the player's choice, is started as listed.
    CHECK(GenerateGameRecipe(1, "Game", {{"", "", "Tools/Viewer.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].executable == "Tools/Viewer.exe");
    RecipeChoices choices;
    choices.program = "Launcher.exe";
    CHECK(GenerateGameRecipe(544580, "BULLET SOUL", {{"", "", "Launcher.exe", "", ""}}, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries[0].executable == "Launcher.exe" && recipe.launch_entries[0].id == kProgramEntryId);
  }

  // The player's words for what was found.
  CHECK(DetectedLabel("unity-il2cpp,d3d11,d3d12 graphics=dxvk") == "Unity \xC2\xB7 Direct3D 11");
  CHECK(DetectedLabel("d3d9 graphics=dxvk") == "Direct3D 9");
  CHECK(DetectedLabel("godot,opengl,vulkan graphics=opengl") == "Godot \xC2\xB7 OpenGL");
  CHECK(DetectedLabel("unknown graphics=auto").empty() && DetectedLabel("").empty());

  fs::remove_all(base);
  std::puts("game probe tests passed");
}
