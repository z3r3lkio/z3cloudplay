#pragma once

#include "runtime/prospero_profile.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Per-game settings: the catalogue the settings sheet is drawn from, the recipe and preset
// data that supply defaults, and the effective values that become the prospero profile and
// dxvk.conf.
//
// Resolution order for each setting:
//   Playden default -> recipe default -> preset -> player override,
// except that a setting the recipe locks keeps the recipe value until the player chooses
// "Change anyway" (an "unlock.<id>" entry in the overrides). An absent override means
// "inherit"; an explicit value, including "off" or an empty text, is never treated as absent.
//
// A recipe only supplies starting values: the program, its folder and arguments, the
// arguments the game needs and the library overrides each have a row the player can change.

enum class SettingTier { kBasic, kCompatibility, kAdvanced };
enum class ApplyMode { kNextLaunch, kGameClosed, kPreparesBottle };
enum class SettingKind { kChoice, kText };
enum class ValueSource { kPlayden, kRecipe, kPreset, kUser };

// What the runtime kit can apply beyond the prospero profile.
struct KitCapabilities {
  bool dxvk_config = false;   // dxvk.conf in the game folder is read (roadmap step S0)
  bool dxvk_present = false;  // the display honors DXVK's present interval (VSync off)
  bool environment = false;  // prospero-win [environment] section (upstream U1, not released)
  bool registry = false;     // Playden can edit the stopped bottle's user.reg (decision D2)
  bool input_tuning = false;  // prospero-win [input] stick_deadzone, rumble, trigger_deadzone (U4, not released)
  bool input_wheel = false;   // prospero-win [input] wheel, with Wine patch 0745 (U4, not built yet)
  bool audio_volume = false;  // prospero-win [audio] volume (U4, not released)
  bool shared_sections = false;  // Wine maps unaligned shared PE sections as private copies (patch 0602)
  bool title_any_id = false;     // one title shell for every game; no fixed set of titles
  bool game_root_unowned = false;  // games start from drives that record no file owners (exFAT)
  bool opengl_zink = false;        // OpenGL games draw through Mesa's Zink, not the PS5 OpenGL SDK
  bool wine_mono = false;          // .NET programs start on the kit's Wine Mono
};

struct SettingChoice {
  std::string value;
  std::string label;
  std::string explanation;
};

struct SettingDefinition {
  std::string id;
  std::string label;
  std::string effect;       // one-line description under the name
  std::string also_called;  // technical names: profile keys, DXVK options
  SettingTier tier = SettingTier::kBasic;
  int phase = 1;            // roadmap phase that ships it
  ApplyMode apply = ApplyMode::kNextLaunch;
  SettingKind kind = SettingKind::kChoice;
  std::vector<SettingChoice> choices;  // empty for text and for launch_option (from the recipe)
  std::string playden_default;         // empty means "not written"
};

const std::vector<SettingDefinition>& SettingsCatalogue();
const SettingDefinition* FindSetting(std::string_view id);

// Whether the sheet lists a setting at all. Every setting is listed, so players see what is
// coming; one the kit, the game or Playden cannot apply yet is disabled with its reason
// (EffectiveValue::available). Button rows live under Button mapping.
bool SettingShown(const SettingDefinition& definition, const KitCapabilities& kit);

// The 17 controller buttons prospero-win binds, as setting ids "button.<name>".
// The rows a controller preset sets, by preset id: setting id → value.
const std::map<std::string, std::map<std::string, std::string>>& ControllerPresetInputs();
const std::vector<std::string>& ProsperoButtons();

struct LaunchEntry {
  std::string id;
  std::string label;
  std::string executable;         // relative to the install root, '/' or '\' separated
  std::string working_directory;  // relative to the install root; empty = executable's folder
  std::string arguments;          // this entry's own arguments, after the recipe's
};

struct GameRecipe {
  std::string id;
  int version = 0;
  std::uint32_t app_id = 0;
  std::string name;
  std::vector<LaunchEntry> launch_entries;  // the first is the default
  // Arguments the game needs. "{resolution}" is replaced by resolution_arguments with
  // {w} and {h} filled from the Resolution setting. Player arguments are appended after.
  std::string arguments;
  std::string resolution_arguments;
  std::string dll_overrides;  // always written first; the player can only add entries
  std::string architecture = "pe32";
  std::string runtime = "wine-wow64";
  std::string prefix = "game";
  std::string graphics = "auto";
  std::map<std::string, std::string> defaults;  // setting id -> value
  std::set<std::string> locked;                 // setting ids the player cannot change
  std::string detected;  // generated recipes: what the game's files showed (GameDefaults::summary)
};

struct SettingsPreset {
  std::string id;
  std::string name;
  std::string try_when;
  std::string short_hint;
  std::map<std::string, std::string> settings;  // only the settings it changes
};

// What Playden persists per game.
struct GameSettingsRecord {
  std::string preset_id;  // empty = Playden default
  std::map<std::string, std::string> overrides;
  std::uint64_t revision = 0;
};

inline bool operator==(const GameSettingsRecord& left, const GameSettingsRecord& right) {
  return left.preset_id == right.preset_id && left.overrides == right.overrides && left.revision == right.revision;
}
inline bool operator!=(const GameSettingsRecord& left, const GameSettingsRecord& right) { return !(left == right); }

struct EffectiveValue {
  std::string value;  // empty = not written
  ValueSource source = ValueSource::kPlayden;
  bool locked = false;
  bool unlocked = false;   // locked by the recipe, but the player chose "Change anyway"
  bool available = true;   // false: the row is disabled and the value is not applied
  std::string reason;      // why it is unavailable or locked
};

struct EffectiveSettings {
  std::map<std::string, EffectiveValue> values;
  // The program that starts: a recipe launch entry, or the player's own program
  // (id kProgramEntryId), with the player's folder and arguments applied. Null when the
  // recipe has no launch entry and the player chose no program yet.
  const LaunchEntry* launch = nullptr;
  std::shared_ptr<const LaunchEntry> launch_storage;  // owns *launch
  std::string arguments;                // full argument string written to the profile
  std::string dll_overrides;
  std::string graphics;
  std::string architecture;             // pe32 or pe64; follows the program
  // The runtime takes Environment variables as [debug] env lines (upstream prospero-win), not [environment].
  bool environment_as_debug_env = false;
  // The runtime reads [debug] env lines (Zink kits). With [environment] as well, a variable [environment] does not
  // take (PW_, GALLIUM_, ZINK_, VK_) still goes there.
  bool debug_env_lines = false;
};

// The launch entry id of a program the player chose from the game folder.
inline constexpr const char* kProgramEntryId = "program";
// Override key prefix for "Change anyway" on a row the recipe locks: "unlock.<setting id>" = "on".
inline constexpr const char* kUnlockPrefix = "unlock.";
bool IsUnlockKey(std::string_view key);

struct SettingsError {
  std::string setting_id;
  std::string message;
};

// A WINEDLLOVERRIDES list split at ';', without empty entries.
std::vector<std::string> SplitDllOverrides(const std::string& list);
// The DXVK entry a generated recipe adds while graphics is "dxvk": it follows graphics, so the
// player can't remove it from the list.
bool IsGraphicsDllOverride(const GameRecipe& recipe, const std::string& entry);

bool BuildEffectiveSettings(const GameRecipe& recipe, const SettingsPreset* preset,
                            const std::map<std::string, std::string>& overrides, const KitCapabilities& kit,
                            EffectiveSettings* settings, SettingsError* error);

// Checks one player value for one setting; used by the UI before saving an override.
bool ValidateSettingValue(const GameRecipe& recipe, std::string_view id, std::string_view value, std::string* reason);

struct ProfileTarget {
  std::string id;            // prospero profile id, also the file stem
  std::string install_root;  // console path of the game files
};

// The full prospero profile: [application], [display], [input] and [debug].
bool RenderProsperoProfile(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                           ProsperoProfile* profile, std::string* reason);

// dxvk.conf for the game folder; empty when no DXVK option differs from DXVK's default.
std::string RenderDxvkConfig(const EffectiveSettings& settings);

// Writes what the next launch reads: <prospero_root>/profiles/<id>.profile (listed in
// profiles.lst) and <install_root>/dxvk.conf. An empty config removes Playden's dxvk.conf.
// A dxvk.conf Playden did not write (no Playden header) is never replaced or removed; the
// call fails with a reason instead. Call only while the game is not running.
bool ApplyGameSettings(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                       const std::string& prospero_root, std::string* reason);

// JSON data files: runtime/recipes/<id>.json and runtime/presets.json.
bool ParseGameRecipe(std::string_view json, GameRecipe* recipe, std::string* reason);
bool ParseSettingsPresets(std::string_view json, std::vector<SettingsPreset>* presets, std::string* reason);
bool ParseGameSettingsRecord(std::string_view json, GameSettingsRecord* record, std::string* reason);
// The recipe as JSON that ParseGameRecipe reads back.
std::string SerializeGameRecipe(const GameRecipe& recipe);

// Games without a shipped recipe get one generated from Steam's Windows launch options.
// Its id is GeneratedRecipeId(app_id); it starts the first launch entry with the game's own
// arguments, takes graphics and engine arguments from the game's files (game_probe.h) and
// locks nothing. With graphics "dxvk", BuildEffectiveSettings adds kDxvkOverrides.
std::string GeneratedRecipeId(std::uint32_t app_id);
bool IsGeneratedRecipeId(std::string_view id);
// A community recipe (community-<app id>, docs/community-recipes.md). Like a generated one, it
// is not a shipped recipe, so it does not force the game's own Steam library.
bool IsCommunityRecipeId(std::string_view id);
// What the player chose in Game settings that changes how the recipe is generated.
struct RecipeChoices {
  std::string program;      // game-relative .exe to start (entry kProgramEntryId, first); empty: Steam's
  bool try_anyway = false;  // soft refusals (.NET, a shared section) do not skip a program
};
// The player's choices from a settings record.
RecipeChoices RecipeChoicesFrom(const GameSettingsRecord& record);

// Builds that recipe from Steam's launch options (default first). Entries whose executable is
// not a game-relative .exe are skipped. The architecture comes from the first usable entry's
// PE header under install_root; entries built for the other architecture are skipped, and so
// are programs the console cannot run (SteamStub, NW.js, Direct3D 12 only, and .NET or a
// shared section unless the player chose "Try anyway"). An Unreal root program from Steam is
// replaced by its *-Shipping.exe. The player's program comes first and Steam's entries stay
// as suggestions. On failure, reason is a short sentence for the player and refusal_soft
// says whether "Try anyway" or another program could get past it.
// What a refused recipe saw, for logs: the first SteamStub program and why it was not unpacked.
struct GameRecipeDiagnostics {
  std::string steam_stub_program;
  std::string steam_stub_detail;
};
bool GenerateGameRecipe(std::uint32_t app_id, const std::string& name, const std::vector<LaunchEntry>& launches,
                        const std::string& install_root, GameRecipe* recipe, std::string* reason,
                        const KitCapabilities& kit = {}, const RecipeChoices& choices = {},
                        bool* refusal_soft = nullptr, GameRecipeDiagnostics* diagnostics = nullptr);

// One Windows program in a game folder, for the Program picker.
struct GameProgram {
  std::string path;          // relative to the install root, '/' separated, as spelled on disk
  std::string architecture;  // pe32 or pe64; empty: not a program the console can start
  std::string refusal;       // why the console cannot run it; empty when it may
  bool refusal_soft = false; // "Try anyway" can get past the refusal
  std::string detected;      // GameDefaults::summary
};
inline constexpr std::size_t kMaxGamePrograms = 32;
// The .exe files under install_root, up to six folders deep, without following symbolic links
// or entering redistributable folders; at most kMaxGamePrograms, shallowest first.
std::vector<GameProgram> ListGamePrograms(const std::string& install_root, const KitCapabilities& kit = {});
// "pe32" or "pe64" for a game-relative program under install_root (any case); empty when it
// is missing or not a Windows program.
std::string GameProgramArchitecture(const std::string& install_root, const std::string& relative);
// Why the console won't start this program (a sentence for the player), or empty when it may:
// a soft refusal (.NET, a shared section) counts only without "Try anyway".
std::string GameProgramRefusal(const std::string& install_root, const std::string& relative, const KitCapabilities& kit,
                               bool try_anyway);
std::string SerializeGameSettingsRecord(const GameSettingsRecord& record);

// Deletes the DXVK pipeline cache for the launch entry's executable (<install_root>/
// <exe stem>.dxvk-cache, next to where DXVK writes it from the working directory) and
// nothing else. Missing cache is success. Call only while the game is not running.
bool ClearShaderCache(const LaunchEntry& launch, const std::string& install_root, std::string* reason);

// One value Playden owns in the bottle's user.reg (HKEY_CURRENT_USER). An empty data
// removes the value, so the game inherits Wine's default again.
struct RegistryValue {
  std::string key;   // e.g. Software\Wine\AppDefaults\hl2.exe (single backslashes)
  std::string name;  // e.g. Version
  std::string data;  // Wine's text form: "\"win7\"" or "dword:00000078"; empty removes
};

// The registry values the effective settings ask for: Windows version for the launch
// executable (AppDefaults) and text scale (Wine's LogPixels). Empty when the kit cannot
// edit the bottle.
std::vector<RegistryValue> RenderRegistryValues(const EffectiveSettings& settings);

// Applies values to Wine's user.reg text, keeping every other line. Sections Playden adds
// carry the given Unix time. Returns false for text that is not a Wine registry file.
bool ApplyRegistryValues(std::string* user_reg, const std::vector<RegistryValue>& values, std::int64_t unix_time,
                         std::string* reason);

// Edits <prefix>/user.reg in place: copies it to user.reg.playden-backup first, then replaces
// it atomically. Call only while the bottle is stopped (no wineserver).
bool ApplyRegistrySettings(const EffectiveSettings& settings, const std::string& prefix_dir, std::int64_t unix_time,
                           std::string* reason);

// Applies values to one hive of the stopped bottle (user.reg or system.reg) the same way:
// backed up to <hive>.playden-backup first, replaced atomically, untouched when nothing changes.
bool ApplyBottleRegistryValues(const std::vector<RegistryValue>& values, const std::string& prefix_dir,
                               std::int64_t unix_time, const std::string& hive, std::string* reason);

// The <InstallGuidString> of an Unreal Engine 3 (UDK) Binaries/InstallInfo.xml: exactly one,
// a 36-character 8-4-4-4-12 hex GUID once surrounding whitespace is trimmed.
bool ParseUe3InstallGuid(std::string_view install_info, std::string* guid);

// A UDK game (launch program in Binaries/Win32 or Binaries/Win64, InstallInfo.xml and
// UnSetup.exe in Binaries) asks for its EULA through UnSetup.exe on first start, and the PS5
// cannot start a second program. This marks the EULA accepted in the bottle's system.reg
// (HKLM Uninstall\UDK-<guid>, both registry views), backing the hive up first. Idempotent;
// the bottle must be stopped. found is false for other games, which are left alone.
struct Ue3EulaResult {
  bool found = false;
  bool ok = true;
  bool written = false;
  std::string guid;
  std::string message;  // on failure
};
Ue3EulaResult AcceptUe3Eula(const std::string& game_root, const std::vector<std::string>& programs,
                            const std::string& prefix_dir, std::int64_t unix_time);

// A prospero-win-profiles sidecar (sidecars/<id>.json, schema_version 1) as a preset for
// the game it describes: "Community · <name>", its newest console check as the hint, and
// only the settings this build knows and accepts. Returns false for other schema versions.
struct CommunityPreset {
  SettingsPreset preset;
  std::uint32_t steam_appid = 0;  // 0 when the sidecar names none
  std::string profile_id;
  std::string rating;             // newest check: works, playable or broken
  std::vector<std::string> requires_prospero_win;
};
bool ParseProfileSidecar(std::string_view json, CommunityPreset* community, std::string* reason);

// A game's effective settings in the community recipe format (docs/community-recipes.md):
// a Playden recipe that ParseGameRecipe reads, plus who tested it where.
struct CommunityRecipe {
  GameRecipe recipe;  // id community-<app id>; the effective program, arguments, overrides and settings
  std::string executable;       // identity: the program, relative to the install root
  std::string min_kit;          // the runtime kit it was tested with
  std::string tested_firmware;  // PS5 system software, when known
  std::string tested_date;      // YYYY-MM-DD
  std::string status;           // works, playable, broken or untested
  std::string notes;
  std::string author;
  // Optional prospero-win-profiles profile used as the base; Playden's values apply on top.
  std::string base_repository;
  std::string base_path;
  std::string base_revision;
};
// The recipe the effective settings describe. Only values that differ from Playden's default
// and rows the runtime applies become defaults; rows the recipe locks stay locked unless the
// player changed them anyway.
CommunityRecipe BuildCommunityRecipe(const GameRecipe& recipe, const EffectiveSettings& settings);
std::string SerializeCommunityRecipe(const CommunityRecipe& community);
bool ParseCommunityRecipe(std::string_view json, CommunityRecipe* community, std::string* reason);

// Whether the kit's runtime has every newer profile feature the sidecar declares in
// requires.prospero_win: the kit capability names without "settings-" (profile-environment,
// input-tuning, input-wheel, audio-volume; see prospero-win-profiles SIDECARS.md). Other entries
// (pull requests, versions) are not checked.
bool CommunityPresetSupported(const CommunityPreset& community, const KitCapabilities& kit);

// --- Integration contract (roadmap S4, integration Tasks 4-6 and 8) ---

// Capability names a runtime kit manifest may list for settings ("capabilities", sorted).
// The kit trust config should allow exactly these besides its own runtime names.
inline constexpr const char* kKitDxvkConfig = "settings-dxvk-config";        // DXVK reads dxvk.conf
// A kit that ships DXVK: DXVK reads dxvk.conf from the working directory, which prospero-win sets to the game's.
inline constexpr const char* kKitDxvk = "dxvk";
inline constexpr const char* kKitDxvkPresent = "settings-dxvk-present";      // VSync off honored
inline constexpr const char* kKitEnvironment = "settings-profile-environment";  // [environment]
inline constexpr const char* kKitInputTuning = "settings-input-tuning";      // stick/trigger dead zones, rumble
inline constexpr const char* kKitInputWheel = "settings-input-wheel";        // [input] wheel
inline constexpr const char* kKitAudioVolume = "settings-audio-volume";      // [audio] volume
// Runtime ability (not a setting): games with a shared section, such as GameMaker's, can start.
inline constexpr const char* kKitSharedSections = "wine-shared-sections";
// Runtime ability: the kit carries one title shell for every game (an any-title variant), so
// Playden makes a new PS5 title per game instead of using a fixed set of titles.
inline constexpr const char* kKitTitleAnyId = "title-any-id";
// Runtime ability: launch admission accepts a game folder on a drive that records no file
// owners or permissions, such as an exFAT USB disk (publication metadata schema 3).
inline constexpr const char* kKitGameRootUnowned = "game-root-unowned";
// Runtime ability: OpenGL games draw through Mesa's WGL/Zink on Vulkan (prospero-win upstream #463-#465)
// instead of the PS5 OpenGL SDK, which had the 120 Hz output and the OpenGL worker thread.
inline constexpr const char* kKitOpenGLZink = "opengl-zink";
// Runtime ability: the kit carries Wine Mono (at mono/wine-mono-<version>) and its template bottle points
// Wine's mscoree at it, so .NET and mixed C++/CLI programs can start.
inline constexpr const char* kKitWineMono = "wine-mono";
const std::vector<std::string>& SettingsKitCapabilityNames();

// Settings gates from a verified kit's capabilities. bottle_access: the adapter can edit the
// stopped bottle's user.reg (decision D2); it is not a kit property.
KitCapabilities KitCapabilitiesFromManifest(const std::vector<std::string>& capabilities, bool bottle_access);

// Everything one game's settings put on disk for its next launch.
struct SettingsLaunchPlan {
  std::uint32_t app_id = 0;
  std::string recipe_id;
  std::string recipe_version;
  std::string profile_id;
  std::string profile_bytes;             // full prospero profile, checked for the kit's features
  std::uint64_t settings_revision = 0;   // GameSettingsRecord::revision it was built from
  std::string launch_entry_id;
  // Input preset files the profile references ([input] preset), as <name>.input and bytes.
  std::vector<std::pair<std::string, std::string>> input_presets;
  std::string dxvk_config;               // empty: no Playden dxvk.conf (an old one is removed)
  std::vector<RegistryValue> registry;   // empty unless the kit has bottle access
  LaunchEntry launch_entry;             // owned snapshot; recipe reloads cannot change the plan
};

struct SettingsLaunchInput {
  const GameRecipe* recipe = nullptr;
  const SettingsPreset* preset = nullptr;  // exact record preset; empty record accepts null or empty playden-default
  GameSettingsRecord record;
  KitCapabilities kit;
  ProfileTarget target;     // profile id and console path of the game files
  std::string runtime_dir;  // bundled settings data; input presets are read from <runtime_dir>/input
};

// Builds the plan, or false with a reason (invalid override, missing input preset, a profile
// the kit's runtime would refuse). Pure apart from reading the input preset files.
bool BuildSettingsLaunchPlan(const SettingsLaunchInput& input, SettingsLaunchPlan* plan, std::string* reason);

// Writes the plan's game-folder and bottle files: dxvk.conf in the launch working folder
// (never replacing one Playden did not write) and the registry values in <prefix_dir>/user.reg
// (with a backup). If the registry edit fails, dxvk.conf is put back; should that rewrite itself
// fail, the folder keeps the new dxvk.conf and the call still returns false. The profile is not written here: the integration publishes profile_bytes
// through its journaled profile/descriptor publication. Call only while the title is stopped.
// Writes include file/directory durability barriers and preserve file modes. This helper
// is not a cross-directory transaction: the owner must journal apply/publication and
// block readiness on failure/interruption until reconciled. A failed config rollback
// returns a distinct reconciliation diagnostic; false does not promise unchanged files.
bool ApplySettingsFiles(const SettingsLaunchPlan& plan, const std::string& install_root, const std::string& prefix_dir,
                        std::int64_t unix_time, std::string* reason);

// Per-game records in one small JSON file ({"version":1,"games":{"<app id>":record}}).
// A missing file loads as no records. Saving replaces the file atomically.
bool LoadGameSettingsFile(const std::string& path, std::map<std::uint32_t, GameSettingsRecord>* records,
                          std::string* reason);
bool SaveGameSettingsFile(const std::string& path, const std::map<std::uint32_t, GameSettingsRecord>& records,
                          std::string* reason);
// One-time import from the legacy approved state tree into an already-created
// private settings root. The owned legacy file must be 0600/0644; the private
// destination must be absent and is published as 0600. The source is preserved.
bool MigrateLegacyGameSettingsFile(const std::string& legacy_path,
                                   const std::string& private_path,
                                   std::string* reason);

// Loads <runtime_dir>/presets.json and the recipes listed in <runtime_dir>/recipes/recipes.lst.
// The console title cannot list folders, so recipes are found through the list file.
bool LoadSettingsData(const std::string& runtime_dir, std::vector<SettingsPreset>* presets,
                      std::vector<GameRecipe>* recipes, std::string* reason);

}  // namespace playden::runtime
