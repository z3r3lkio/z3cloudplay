// ps5.cpp - Direct Hui comparison lifecycle and controller adapter.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include "homebrew/render.hpp"
#include "homebrew/artwork_backend.hpp"
#include "homebrew/simulation.hpp"
#include "homebrew/controller.hpp"
#include "steam/install_volumes.h"
#include "app/diagnostic_log.h"
#include "platform/ps5/title_remover.h"
#include "platform/ps5/title_runtime.h"
#include "platform/ps5/native_startup.h"
#include "runtime/settings_storage.h"
#include "homebrew/runtime_host.hpp"
#include "homebrew/title_icon.hpp"
#include "platform/ps5/title_allow_list.h"
#include "platform/ps5/console_services.h"
#include "runtime/artifact_hash.h"
#include "runtime/kit_admission.h"
#include "runtime/kit_trust.h"
#include "homebrew/input_adapter.hpp"
#include "homebrew/usb_keyboard.hpp"
#include "core/input.hpp"
#include "steam/backend/auth_client.h"
#include "steam/backend/cm_app_details.h"
#include "steam/backend/entitled_library.h"
#include "steam/backend/steam_install_runner.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/backend/library_client.h"
#include "steam/local_session_repository.h"
#include "steam/ephemeral_session_repository.h"
#include "steam/secure_session_repository.h"
#include <mutex>
#include "library/cache.h"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <map>
#include "homebrew/store_fetch.hpp"
#include <memory>
#include <optional>
#include <sys/stat.h>

extern "C" void z3cloudplay_release_splash(void);
// The native signer requires every referenced export to resolve, including
// the kit's optional weak splash callback. Use the launcher's held splash.
extern "C" void hui_release_splash(void) { z3cloudplay_release_splash(); }

namespace {
void dispatch(z3cloudplay::homebrew::Simulation& simulation, const hui::InputFrame& input) {
    switch (input.nav) {
        case hui::Direction::up: simulation.action("up"); break;
        case hui::Direction::down: simulation.action("down"); break;
        case hui::Direction::left: simulation.action("left"); break;
        case hui::Direction::right: simulation.action("right"); break;
        default: break;
    }
    const std::array<std::pair<hui::Action, const char*>, 10> actions{{
        {hui::Action::confirm, "cross"}, {hui::Action::back, "circle"},
        {hui::Action::page_prev, "l1"}, {hui::Action::page_next, "r1"},
        {hui::Action::jump_prev, "l2"}, {hui::Action::jump_next, "r2"},
        {hui::Action::menu, "options"}, {hui::Action::north, "triangle"},
        {hui::Action::west, "square"}, {hui::Action::touch, "search"}
    }};
    // Dispatch one activation edge per frame so opening a modal cannot also
    // activate its default control through another buffered physical edge.
    for (const auto& [action, name] : actions) {
        if (input.is_pressed(action)) { simulation.action(name); break; }
    }
}
}

// Title-to-title launch, as Itemzflow does: the target receives argv as its arguments.
extern "C" {
int sceLncUtilInitialize(void);
uint32_t sceLncUtilLaunchApp(const char* title_id, const char* argv[], z3cloudplay::ps5::LaunchParameters* param);
uint32_t sceSystemServiceLaunchApp(const char* title_id, const char* argv[], void* param);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t* handle);
int sceKernelDlsym(int32_t handle, const char* name, void** address);
int sceUserServiceGetForegroundUser(uint32_t* user_id);
int sceUserServiceGetInitialUser(int* user_id);
}

namespace {
// A USB keyboard through the system's libSceKeyboard, as prospero-win reads it for games: the
// system loads the module (a title may not load it by path), then its functions are looked up.
constexpr uint16_t kKeyboardModule = 0x0106;

struct UsbKeyboard {
    int32_t handle = -1;
    int (*read_state)(int32_t, void*) = nullptr;
    bool failed = false;
    std::uint64_t retry_us = 0;

    // Opens the keyboard when one is plugged in; tried again every few seconds. Logs the first
    // failure and every success.
    void open(int user, std::uint64_t now_us, const std::string& log_root) {
        if (handle >= 0 || now_us < retry_us) return;
        retry_us = now_us + 3000000;
        int32_t module = -1;
        if (sceSysmoduleGetModuleHandleInternal(kKeyboardModule, &module) != 0 || module < 0) {
            module = -1;
            if (sceSysmoduleGetModuleHandleInternal(0x80000000u | kKeyboardModule, &module) != 0) module = -1;
        }
        int (*init)(void) = nullptr;
        int (*open_keyboard)(int32_t, int32_t, int32_t, void*) = nullptr;
        int init_rc = -1, opened = -1;
        if (module >= 0 && sceKernelDlsym(module, "sceKeyboardInit", reinterpret_cast<void**>(&init)) == 0 &&
            sceKernelDlsym(module, "sceKeyboardOpen", reinterpret_cast<void**>(&open_keyboard)) == 0 &&
            sceKernelDlsym(module, "sceKeyboardReadState", reinterpret_cast<void**>(&read_state)) == 0) {
            init_rc = init();
            uint8_t param[8] = {};
            opened = open_keyboard(user, 0, 0, param);
        }
        if (opened >= 0) handle = opened;
        if (opened >= 0 || !failed) {
            char line[128];
            std::snprintf(line, sizeof(line), "keyboard module=%d init=0x%08x open=0x%08x user=%d", int(module),
                          unsigned(init_rc), unsigned(opened), user);
            z3cloudplay::app::AppendLogLine(log_root, "launch.log", line);
        }
        failed = opened < 0;
    }

    z3cloudplay::homebrew::KeyboardReport read() {
        uint8_t state[z3cloudplay::homebrew::kKeyboardStateBytes] = {};
        if (handle < 0 || read_state(handle, state) != 0) return {};
        return z3cloudplay::homebrew::ParseKeyboardState(state);
    }
};

std::string title_root = "/app0";
std::string log_root = "/app0/logs";
// Development requests are processed in file order on the first frame.
std::vector<z3cloudplay::steam::InstallRequest> take_install_requests(bool runtime_ready) {
    const std::string request_path = title_root + "/dev/install-request.txt";
    FILE* request = std::fopen(request_path.c_str(), "r");
    if (!request) return {};
    std::string text;
    char line[64] = {};
    while (text.size() < 4096 && std::fgets(line, sizeof(line), request)) {
        // Reject an overlong line rather than executing its truncated prefix.
        if (!std::strchr(line, '\n') && !std::feof(request)) {
            int value;
            while ((value = std::fgetc(request)) != '\n' && value != EOF) {}
            continue;
        }
        text += line;
    }
    std::fclose(request);
    const auto requests = z3cloudplay::steam::ParseInstallRequests(text);
    if (!runtime_ready) {
        z3cloudplay::app::AppendLogLine(log_root, "install.log", "install-request kept: the game runtime is not available (" +
                                                                 std::to_string(requests.size()) + " commands)");
        return {};
    }
    std::remove(request_path.c_str());
    return requests;
}
}  // namespace

int main() {
    // Before the title's /data grant: a title whose credentials changed may fail to load system modules.
    const int keyboard_module = sceSysmoduleLoadModule(kKeyboardModule);
    const auto native_access = z3cloudplay::ps5::PrepareNativeStartup(Z3CLOUDPLAY_PS5_TITLE_ID);
    hui::ps5::Display display;
    if (display.width() <= 0 && !display.open(3840, 2160)) {
        hui::sys::log("Z3CloudPlay Hui display failed"); hui::sys::park();
    }
    if (!native_access.launcher_ready) {
        hui::sys::log("Z3CloudPlay launcher paths unavailable: %s", native_access.error.c_str());
        hui::sys::park();
    }
    const auto& paths = native_access.paths;
    title_root = paths.title_root;
    log_root = paths.log_root;
    // Startup checkpoints: a launch that never shows its first frame can be told
    // apart from one that never opened the display.
    z3cloudplay::app::AppendLogLine(log_root, "launch.log", "display open width=" + std::to_string(display.width()) +
                                                        " height=" + std::to_string(display.height()));
    if (!native_access.runtime_ready)
        hui::sys::log("Z3CloudPlay native runtime unavailable: %s", native_access.error.c_str());
    {
#if Z3CLOUDPLAY_UI_SIMULATION
        mkdir((paths.title_root + "/z3cloudplay-hui-comparison").c_str(), 0777);
        z3cloudplay::homebrew::Simulation simulation(
            paths.title_root + "/fixtures/ui-comparison", paths.title_root + "/z3cloudplay-hui-comparison/profile.json",
            z3cloudplay::homebrew::ModelMode::Simulation);
#else
        z3cloudplay::homebrew::Simulation simulation({}, {}, z3cloudplay::homebrew::ModelMode::Live);
#endif
        {
            std::vector<z3cloudplay::runtime::SettingsPreset> presets;
            std::vector<z3cloudplay::runtime::GameRecipe> recipes;
            std::string reason;
#if Z3CLOUDPLAY_UI_SIMULATION
            // The kit's DXVK reads dxvk.conf from the game folder (results/20261005-dxvk-conf-probe).
            z3cloudplay::runtime::KitCapabilities kit;
            kit.dxvk_config = true;
            simulation.SetKitCapabilities(kit);
#endif
            // Live: the controller passes on the verified kit's capabilities once the runtime starts.
            if (z3cloudplay::runtime::LoadSettingsData(paths.title_root + "/runtime", &presets, &recipes, &reason))
                simulation.SetSettingsData(std::move(presets), std::move(recipes));
            else
                z3cloudplay::app::AppendLogLine(paths.log_root, "launch.log", "game settings data unavailable: " + reason);
        }
#if !Z3CLOUDPLAY_UI_SIMULATION
        std::string settings_reason;
        const auto settings_file = z3cloudplay::runtime::PrepareGameSettingsStorage(paths.steam_state_root, &settings_reason);
        if (!settings_file)
            z3cloudplay::app::AppendLogLine(paths.log_root, "launch.log",
                "private game settings unavailable: " + settings_reason);
        else z3cloudplay::app::AppendLogLine(paths.log_root, "launch.log", "game settings storage initialized");
        // An empty path disables live writes and surfaces a settings load error.
        simulation.SetGameSettingsFile(settings_file.value_or(std::filesystem::path{}));
        z3cloudplay::Storage auth_storage(paths.steam_state_root);
        // One line per Steam CM logon or refusal, with Steam's result code (#51).
        z3cloudplay::steam::SetCmLog([](const std::string& line) {
            z3cloudplay::app::AppendLogLine(log_root, "steam-cm.log", line);
        });
        // One logged-on Steam connection for the whole run, shared by every CM client below (#51).
        const auto steam_cm = z3cloudplay::steam::MakeCmSharedSession();
        z3cloudplay::steam::CmClient auth_cm(paths.trust_store, auth_storage);
        auth_cm.SetOwner("auth");
        auth_cm.SetSharedSession(steam_cm);
        z3cloudplay::steam::AuthClient auth(paths.trust_store);
        auth.SetCmRenewal([&auth_cm](const z3cloudplay::StoredSession& stored, const std::atomic<bool>& cancel) {
            return z3cloudplay::steam::RenewThroughCm(auth_cm, stored, cancel);
        });
        // Sanitized: method, HTTP status and X-EResult number; never tokens or account identity.
        auth.SetFailureReport([](const char* method, long http_status, int eresult) {
            char line[160];
            std::snprintf(line, sizeof(line), "auth_failure method=%s http=%ld eresult=%d", method, http_status, eresult);
            z3cloudplay::app::AppendLogLine(log_root, "auth.log", line);
        });
        auth.SetRestoreReport([](const char* path, const std::string& before, const std::string& after) {
            z3cloudplay::app::AppendLogLine(log_root, "auth.log", std::string("restored path=") + path + " before " + before +
                " after " + after);
        });
        // Legacy plaintext tokens must not survive into this session.
        z3cloudplay::Storage credential_cleanup(paths.steam_state_root);
        // Parent directory is prepared by native startup; ensure the leaf exists.
        if (::mkdir(paths.steam_state_root.c_str(), 0700) != 0 && errno != EEXIST) {
            z3cloudplay::app::AppendLogLine(log_root, "storage.log",
                "private credential directory unavailable; authentication disabled");
            return 1;
        }
        const auto clean_primary = credential_cleanup.Remove("session.v1");
        const auto clean_temporary = credential_cleanup.Remove("session.v1.tmp");
        if (!clean_primary.ok() || !clean_temporary.ok()) {
            z3cloudplay::app::AppendLogLine(log_root, "storage.log",
                "legacy plaintext credential cleanup failed; authentication disabled");
            return 1;
        }
        // Session vault key remains in process memory only, guarded across workers.
        std::mutex vault_mutex;
        std::string vault_passphrase;
        z3cloudplay::security::SecureSessionRepository sessions(
            credential_cleanup, [&]() {
                std::lock_guard<std::mutex> lock(vault_mutex);
                return vault_passphrase;
            });
        z3cloudplay::steam::LibraryClient owned_library(paths.trust_store);
        auto& library_sessions = sessions;
        z3cloudplay::Storage library_storage(paths.steam_state_root);
        z3cloudplay::steam::CmClient cm_client(paths.trust_store, library_storage);
        cm_client.SetOwner("library");
        cm_client.SetSharedSession(steam_cm);
        z3cloudplay::steam::EntitledLibraryBackend library_backend(owned_library, cm_client);
        z3cloudplay::LibraryCache library_cache(library_storage);
        z3cloudplay::LibraryService library_service(library_backend,library_sessions,&library_cache);
        z3cloudplay::PreferencesStore preferences(library_storage);
        z3cloudplay::OrganizationStore organization(library_storage);
        z3cloudplay::app::PersistenceService persistence(preferences,organization);
        z3cloudplay::steam::CmClient details_cm(paths.trust_store, library_storage);
        details_cm.SetOwner("details");
        details_cm.SetSharedSession(steam_cm);
        z3cloudplay::steam::CmAppDetailsSource details_source(details_cm);
        auto& details_sessions = sessions;
        z3cloudplay::steam::AppDetailsService app_details(details_source, details_sessions);
        z3cloudplay::steam::CmClient install_cm(paths.trust_store, library_storage);
        install_cm.SetOwner("install");
        install_cm.SetSharedSession(steam_cm);
        z3cloudplay::steam::CdnClient install_cdn(paths.trust_store);
        z3cloudplay::steam::SteamInstallRunner install_runner(install_cm, install_cdn);
        install_runner.SetTuning([root = paths.title_root] {
            auto tuning = z3cloudplay::steam::ReadDownloadTuning(root + "/dev/download-tuning.txt");
            // Six parallel CDN chunk fetches instead of four. User overrides take priority.
            // Parallelism is bounded by DepotInstaller (maximum eight slots).
            if (tuning.slots == 4) tuning.slots = 6;
            return tuning;
        });
        install_runner.SetLog([](z3cloudplay::GameId app, const char* step, const char* category, const char* detail) {
            char line[640];
            if (std::string(step) == "install-removal")
                std::snprintf(line, sizeof(line), "install-removal done app=%u %s", app, detail);
            else
                std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
            z3cloudplay::app::AppendLogLine(log_root, "install.log", line);
        });
        auto& install_sessions = sessions;
        z3cloudplay::steam::InstallService installs(install_runner, install_sessions,
            z3cloudplay::steam::DiscoverInstallVolumes(z3cloudplay::steam::ConsoleVolumeCandidates(paths.title_root)));
        z3cloudplay::steam::CmClient cloud_cm(paths.trust_store, library_storage);
        cloud_cm.SetOwner("cloud");
        cloud_cm.SetSharedSession(steam_cm);
        z3cloudplay::steam::HttpClient cloud_http(paths.trust_store);
        z3cloudplay::steam::SteamCloudRunner cloud_runner(cloud_cm, cloud_http);
        cloud_runner.SetLog([](z3cloudplay::GameId app, const char* event, const std::string& message) {
            z3cloudplay::app::AppendLogLine(log_root, "cloud.log", "app=" + std::to_string(app) + " event=" + event + " " + message);
        });
        auto& cloud_sessions = sessions;
        z3cloudplay::steam::CloudService cloud(cloud_runner, cloud_sessions);
        simulation.SetLogDirectory(paths.log_root);
        // "Export as recipe" writes next to the logs, which are read over FTP already.
        simulation.SetRecipeExportTarget(paths.log_root, z3cloudplay::ps5::ConsoleFirmwareVersion().value_or(""));
        // Per-game runtime: private bottles and PS5 titles from the admitted kit.
        // Its worker owns OWNER records, native inventory and publication.
        std::unique_ptr<z3cloudplay::homebrew::RuntimeHost> runtime_host;
        if (native_access.runtime_ready && !paths.runtime_data_root.empty()) {
            namespace rt = z3cloudplay::runtime;
            z3cloudplay::homebrew::RuntimeHostConfig config;
            config.boundary = "/data";
            config.state_root = std::filesystem::path(paths.runtime_data_root) / "runtime-state";
            config.homebrew_root = "/data/homebrew";
            config.registry_transport = z3cloudplay::ps5::ShadowMountPost;
            config.admit_kit = [](std::string* reason) -> std::optional<z3cloudplay::homebrew::RuntimeKit> {
                const auto firmware = z3cloudplay::ps5::ConsoleFirmwareVersion();
                z3cloudplay::app::AppendLogLine(log_root, "runtime.log", "console_firmware=" + firmware.value_or("unknown") + " " +
                    z3cloudplay::ps5::ConsoleFirmwareDiagnostics());
                const auto admission = rt::DevelopmentKitAdmission();
                auto admitted = rt::VerifyInstalledKit(rt::kDevelopmentKitRoot, admission);
                std::string ignored;
                for (const auto& name : admitted.ignored_capabilities) ignored += (ignored.empty() ? "" : ",") + name;
                const auto wrapper = admitted.sources.find("wrapper");
                const auto revision = wrapper != admitted.sources.end() && wrapper->second.count("revision")
                    ? wrapper->second.at("revision") : std::string{};
                z3cloudplay::app::AppendLogLine(log_root, "runtime.log", std::string("kit_admission root=") +
                    rt::kDevelopmentKitRoot + " status=" + std::to_string(static_cast<int>(admitted.status)) + " reason=" + admitted.reason +
                    " manifest=" + admitted.manifest_sha256 + " expected=" + admission.expected_manifest_sha256 +
                    " prospero_win=" + revision + (admitted.source_dirty ? "-dirty" : "") +
                    " ignored_capabilities=" + (ignored.empty() ? "-" : ignored));
                if (admitted.status != rt::KitManifestStatus::kVerified || !admitted.view) {
                    *reason = "The compatibility kit is missing or incomplete.";
                    return std::nullopt;
                }
                return z3cloudplay::homebrew::RuntimeKit{*admitted.view, admitted.capabilities, admitted.manifest_sha256,
                                                     admission.expected_manifest_sha256, revision, admitted.source_dirty};
            };
            config.package_operations.hash_file = rt::HashArtifactFile;
            config.package_operations.hash_bytes = rt::HashArtifactBytes;
            config.package_operations.free_bytes = [](const std::filesystem::path& path) -> std::optional<std::uint64_t> {
                const auto space = z3cloudplay::steam::QueryVolumeSpace(path.string());
                return space ? std::optional<std::uint64_t>{space->free_bytes} : std::nullopt;
            };
            config.package_operations.free_space_unverifiable = !z3cloudplay::steam::VolumeSpaceSupported();
            config.package_operations.validate_bottle = [](const std::filesystem::path& bottle) {
                std::error_code error;
                for (const char* name : {"system.reg", "user.reg", "userdef.reg"})
                    if (!std::filesystem::is_regular_file(bottle / name, error)) return false;
                return true;
            };
            config.runtime_dir = paths.title_root + "/runtime";
            config.settings_record = [file = settings_file](std::uint32_t app) -> std::optional<rt::GameSettingsRecord> {
                std::map<std::uint32_t, rt::GameSettingsRecord> records; std::string reason;
                if (!file || !rt::LoadGameSettingsFile(file->string(), &records, &reason)) return std::nullopt;
                const auto found = records.find(app);
                return found == records.end() ? std::nullopt : std::optional<rt::GameSettingsRecord>(found->second);
            };
            config.log = [](const std::string& line) { z3cloudplay::app::AppendLogLine(log_root, "runtime.log", line); };
            config.title_log = [](const std::string& title, const std::string& text) {
                const auto path = log_root + "/title-" + title + ".log";
                if (FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(text.data(), 1, text.size(), file);
                    std::fclose(file);
                }
            };
            // The game's PS5 title icon, from its Steam cover; without one, the packaged
            // generic icon. Only a missing generic icon leaves the kit's.
            std::optional<std::string> generic_icon;
            if (FILE* file = std::fopen((paths.assets_root + "/generic-title-icon.png").c_str(), "rb")) {
                std::string bytes;
                char chunk[4096];
                for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
                    bytes.append(chunk, count);
                std::fclose(file);
                if (!bytes.empty()) generic_icon = std::move(bytes);
            }
            config.title_icon = [trust = paths.trust_store, generic_icon](std::uint32_t app) -> std::optional<std::string> {
                z3cloudplay::homebrew::HttpArtworkBackend backend(trust);
                const std::atomic<bool> cancel{false};
                const auto cover = backend.Load({1, app, "https://cdn.cloudflare.steamstatic.com/steam/apps/" +
                                                 std::to_string(app) + "/library_600x900.jpg"}, cancel);
                if (auto icon = cover ? z3cloudplay::homebrew::MakeTitleIcon(*cover) : std::nullopt) return icon;
                z3cloudplay::app::AppendLogLine(log_root, "runtime.log", "title icon app=" + std::to_string(app) +
                                            (generic_icon ? " uses the generic icon: no cover" : " has no cover or generic icon"));
                return generic_icon;
            };
            // An any-title kit's helper serves the game titles listed here (root-owned).
            config.write_title_allow_list = z3cloudplay::ps5::WriteTitleAllowList;
            // Play: start a prepared title with its session argument. LncUtil is
            // resolved through the system service module (its static imports are null).
            config.launch_title = [](const std::string& title, const std::vector<std::string>& arguments) {
                static auto api = [] {
                    auto resolved = z3cloudplay::ps5::ResolveTitleApi(
                        {sceSysmoduleLoadModuleInternal, sceSysmoduleGetModuleHandleInternal, sceKernelDlsym},
                        sceUserServiceGetForegroundUser);
                    if (!resolved.launch && !resolved.public_launch) {
                        resolved.public_launch = sceSystemServiceLaunchApp;
                        resolved.foreground_user = sceUserServiceGetForegroundUser;
                    }
                    return resolved;
                }();
                static z3cloudplay::ps5::TitleBackend backend(api);
                const auto result = backend.Launch(title, arguments);
                char line[160];
                std::snprintf(line, sizeof(line), "launch_title title=%s stage=%s accepted=%d result=0x%08x",
                              title.c_str(), result.stage.c_str(), result.ok, unsigned(result.code));
                z3cloudplay::app::AppendLogLine(log_root, "runtime.log", line);
                return result;
            };
            // Uninstall removes the game's PS5 title through Z3CloudPlay's one-shot title
            // remover payload, which the payload loader runs outside this title.
            config.unregister_title = [title_root = paths.title_root](const std::string& title, const std::string& nonce) {
                z3cloudplay::ps5::TitleRemoverConfig remover;
                remover.payload = title_root + "/title-remover.elf";
                remover.send = z3cloudplay::ps5::SendToPayloadLoader;
                const auto outcome = z3cloudplay::ps5::RunTitleRemover(remover, title, nonce);
                return z3cloudplay::homebrew::TitleRemoverResult{outcome.sent, outcome.code, outcome.detail};
            };
            runtime_host = std::make_unique<z3cloudplay::homebrew::RuntimeHost>(std::move(config));
            if (!runtime_host->Start())
                z3cloudplay::app::AppendLogLine(paths.log_root, "runtime.log", "runtime worker could not start");
        }
        z3cloudplay::homebrew::Controller services(simulation, auth, sessions,&library_service,&persistence,&app_details,&installs,&cloud,
                                               runtime_host.get(), true);
        z3cloudplay::steam::StoreDetailsService store_details(z3cloudplay::homebrew::StoreFetch(paths.trust_store));
        services.SetStoreDetails(&store_details);
        simulation.BeginSecurePassphraseEntry();
        // Without its runtime Z3CloudPlay still lists and downloads games; say why nothing can start.
        if (!native_access.runtime_ready) simulation.UpdateRuntimeProblem(native_access.error);
#endif
#if Z3CLOUDPLAY_UI_SIMULATION
        z3cloudplay::homebrew::Render renderer(paths.assets_root);
#else
        z3cloudplay::homebrew::HttpArtworkBackend artwork_backend(paths.trust_store);
        z3cloudplay::homebrew::ArtworkService artwork(artwork_backend);
        z3cloudplay::homebrew::Render renderer(paths.assets_root, &artwork);
#endif
        hui::ps5::Pad pad;
        pad.open();
        {
            // The controller Z3CloudPlay reads belongs to the initial user; log both
            // users so a controller that does nothing can be explained.
            int initial = -1;
            uint32_t foreground = 0;
            const int initial_rc = sceUserServiceGetInitialUser(&initial);
            const int foreground_rc = sceUserServiceGetForegroundUser(&foreground);
            char line[160];
            std::snprintf(line, sizeof(line), "pad users initial=%d rc=0x%08x foreground=%u rc=0x%08x", initial,
                          unsigned(initial_rc), unsigned(foreground), unsigned(foreground_rc));
            z3cloudplay::app::AppendLogLine(log_root, "launch.log", line);
        }
        int keyboard_user = -1;
        sceUserServiceGetInitialUser(&keyboard_user);
        z3cloudplay::app::AppendLogLine(log_root, "launch.log", "keyboard preload status=" + std::to_string(keyboard_module));
        UsbKeyboard keyboard;
        z3cloudplay::homebrew::KeyboardInput keyboard_input;
        hui::InputTracker tracker;
        std::array<hui::PadSample, 64> samples{};
        bool presented = false;
        bool was_connected = true;
        std::uint64_t last_render_us = 0;
        int slow_frames = 0;
        for (;;) {
            const auto now = hui::sys::monotonic_us();
            const auto count = pad.read(samples);
            const auto input = tracker.update(std::span<const hui::PadSample>(samples.data(), count), now);
            if (input.connected != was_connected) {
                simulation.SetControllerConnected(input.connected);
                was_connected = input.connected;
            }
            dispatch(simulation, input);
            // A USB keyboard types into the open text box and also moves around Z3CloudPlay.
            keyboard.open(keyboard_user, now, log_root);
            for (const auto& command : keyboard_input.Update(keyboard.read(), simulation.state().editor.active, now)) {
                if (!command.text.empty()) simulation.text(command.text);
                else simulation.action(command.action);
            }
            simulation.controller_sample(z3cloudplay::homebrew::diagnostic_sample(input));
#if !Z3CLOUDPLAY_UI_SIMULATION
            if (auto candidate = simulation.TakeSecurePassphrase()) {
                {
                    std::lock_guard<std::mutex> lock(vault_mutex);
                    // Replace any previous failed attempt without persisting it.
                    std::fill(vault_passphrase.begin(), vault_passphrase.end(), '\0');
                    vault_passphrase = std::move(*candidate);
                }
                // An existing encrypted file must authenticate before allowing
                // a new sign-in to overwrite it with a different passphrase.
                const auto verified = sessions.Load();
                if (verified.ok()) {
                    services.RestoreAfterUnlock();
                } else {
                    {
                        std::lock_guard<std::mutex> lock(vault_mutex);
                        std::fill(vault_passphrase.begin(), vault_passphrase.end(), '\0');
                        vault_passphrase.clear();
                    }
                    simulation.BeginSecurePassphraseEntry();
                }
            }
#endif
            const auto dispatched = hui::sys::monotonic_us();
#if !Z3CLOUDPLAY_UI_SIMULATION
            services.Tick();
#endif
            const auto ticked = hui::sys::monotonic_us();
            if (simulation.state().quit_requested) break;
            const float frame_dt = last_render_us == 0 ? 1.0F / 60.0F :
                static_cast<float>(now - last_render_us) / 1000000.0F;
            last_render_us = now;
            renderer.draw(simulation, display.width(), display.height(), frame_dt);
            const auto drawn = hui::sys::monotonic_us();
            if (!display.swap()) break;
            // Frames over 50 ms are logged with where the time went (milliseconds), at most a few hundred per run.
            if (const auto swapped = hui::sys::monotonic_us(); presented && swapped - now > 50000 && slow_frames < 400) {
                ++slow_frames;
                std::string line = "slow frame ms=" + std::to_string((swapped - now) / 1000) +
                                   " input=" + std::to_string((dispatched - now) / 1000) +
                                   " tick=" + std::to_string((ticked - dispatched) / 1000) +
                                   " draw=" + std::to_string((drawn - ticked) / 1000) +
                                   " swap=" + std::to_string((swapped - drawn) / 1000);
#if !Z3CLOUDPLAY_UI_SIMULATION
                if (const auto profile = services.LastTickProfile(); !profile.empty()) line += " [" + profile + "]";
#endif
                z3cloudplay::app::AppendLogLine(log_root, "frames.log", line);
            }
            if (!presented) {
                hui::sys::hide_splash_screen();
                presented = true;
                z3cloudplay::app::AppendLogLine(log_root, "launch.log", "first frame presented");
#if !Z3CLOUDPLAY_UI_SIMULATION
                for (const auto& request : take_install_requests(native_access.runtime_ready)) {
                    using Action = z3cloudplay::steam::InstallRequest::Action;
                    const char* action = "queued";
                    if (request.action == Action::kUninstall) { action = "uninstall"; installs.Uninstall(request.app); }
                    else if (request.action == Action::kCancel) { action = "cancel"; installs.Cancel(request.app); }
                    else installs.Install(request.app);
                    z3cloudplay::app::AppendLogLine(log_root, "install.log", "install-request " + std::string(action) +
                                               " app=" + std::to_string(request.app));
                }
#endif
            }
            // Work advances only through the documented manual fixture clock.
            pad.tick(1.0F / 60.0F);
        }
#if !Z3CLOUDPLAY_UI_SIMULATION
        services.Shutdown();
        {
            std::lock_guard<std::mutex> lock(vault_mutex);
            volatile char* secret = vault_passphrase.empty() ? nullptr : &vault_passphrase[0];
            for (std::size_t i = 0; i < vault_passphrase.size(); ++i) secret[i] = 0;
            vault_passphrase.clear();
        }
        artwork.Shutdown();
#endif
    }
    display.close();
    hui::sys::quit();
}
