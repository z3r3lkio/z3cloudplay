#include "homebrew/render.hpp"
#include "app/library_controller.h"
#include "app/persistence_controller.h"
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <chrono>
#include <thread>
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include <SDL.h>
#include <cassert>
#include <filesystem>

namespace {
using namespace z3cloudplay;
struct Sessions final : SessionRepository {
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(StoredSession{123,"Synthetic account","synthetic-refresh","synthetic-access",{}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};
struct Backend final : steam::LibraryBackend {
  std::vector<GameSummary> games;
  steam::LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    return {steam::LibraryError::kNone,games,false};
  }
};
struct Cache final : LibraryRepository {
  mutable std::optional<LibrarySnapshot> snapshot;
  Result<LibrarySnapshot> Load(std::uint64_t id) const override {
    assert(id==123);
    return snapshot ? Result<LibrarySnapshot>::Success(*snapshot)
        : Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
  }
  Result<void> Save(std::uint64_t id,const LibrarySnapshot& value) const override {
    assert(id==123);snapshot=value;return Result<void>::Success();
  }
  Result<void> Clear(std::uint64_t) const override { snapshot.reset();return Result<void>::Success(); }
};
}

int main(int argc,char** argv) {
  using namespace z3cloudplay::homebrew;
  if(argc!=2) return 2;
  assert(SDL_Init(SDL_INIT_VIDEO)==0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  auto* window=SDL_CreateWindow("Synthetic live library verification",0,0,1920,1080,
                                 SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  assert(window);
  auto context=SDL_GL_CreateContext(window); assert(context);
  hui::gfx::set_glsl_prefix("#version 410 core\n");
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  {
    Simulation model({}, {},ModelMode::Live);
    model.SetWallClock(1700000000);
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
    z3cloudplay::GameSummary first{42,"Synthetic library title",{},"Known synthetic description","Example studio"};
    first.playtime_minutes=91; first.last_played_unix_seconds=1699827200;
    Sessions sessions;Backend backend;Cache cache;
    backend.games={first,{43,"Café on the coast",{},{},{}},{44,"Αθήνα",{},{},{}}};
    z3cloudplay::LibraryService service(backend,sessions,&cache,[]{return 1700000000;});
    z3cloudplay::app::LibraryController controller(model,service);
    auto settle=[&] {
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      do { controller.Tick(); if(!model.state().library_loading) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while(std::chrono::steady_clock::now()<deadline);
      assert(false && "graphical library service deadline");
    };
    controller.Activate(123,false);settle();
    assert(model.state().games.size()==3 && model.state().account.cache_available);
    Render renderer(Z3CLOUDPLAY_HOMEBREW_ASSETS_DIR);
    auto capture=[&](const char* name) {
      for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
      assert(renderer.capture_png(output/name,1920,1080));
    };
    capture("home.png");
    assert(model.action("r1") && model.state().navigation.route==Route::Library);
    capture("library.png");
    assert(model.action("search"));model.text("CAFÉ");assert(model.action("done"));
    assert(model.visible_games().size()==1 && model.visible_games().front()->id==43);
    capture("search.png");
    assert(model.action("search"));model.text(" discarded");assert(model.action("circle"));
    assert(model.state().library.query=="CAFÉ" && model.visible_games().size()==1);
    assert(model.action("search") && model.action("clear"));model.text("no result");assert(model.action("done"));
    assert(model.visible_games().empty());
    assert(model.action("search") && model.action("clear") && model.action("done"));
    assert(model.visible_games().size()==3);
    // The first Library focus can be its sidebar. Move right into its grid.
    if(model.state().navigation.library_rail_focused) assert(model.action("right"));
    assert(model.action("cross") && model.state().navigation.route==Route::Details);
    capture("details.png");
    {
      // An installed, prepared game: Play is the same height as the other actions,
      // in either state, and the uninstall dialog's choice fits its button.
      Simulation live({}, {}, ModelMode::Live);
      live.SetWallClock(1700000000);
      live.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(live.UpdateLibrary({backend.games,false,{},false,false,{}}));
      live.UpdateAppDetails(42, 300'000'000, 600'000'000, true);
      live.UpdateInstalls({}, {{42, 600'000'000}});
      LiveRuntimeGame ready; ready.game_id=42; ready.status="Ready"; ready.ready=true; ready.can_play=true; ready.has_title=true;
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      assert(live.action("details:42"));
      auto capture_live=[&](const char* name) {
        for(int i=0;i<16;++i) renderer.draw(live,1920,1080);
        assert(renderer.capture_png(output/name,1920,1080));
      };
      capture_live("details-play.png");
      ready.can_play=false; ready.play_status="Starting…";
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      capture_live("details-play-disabled.png");
      {
        LiveRuntimeGame copying; copying.game_id=42; copying.status="Preparing"; copying.has_title=true;
        copying.detail="Copying the Windows environment"; copying.progress=0.42F;
        live.UpdateRuntime(RuntimeState::Ready, {copying});
        capture_live("details-preparing-progress.png");
      }
      {
        // Prepared and waiting for the PS5: Play reads Ready to play, and the prompt offers it.
        LiveRuntimeGame waiting; waiting.game_id=42; waiting.status="Waiting for the PS5"; waiting.has_title=true;
        waiting.awaiting_registration=true; waiting.can_prepare=true;
        waiting.detail="Waiting for the PS5, which adds the game to Home when Z3CloudPlay quits";
        live.UpdateRuntime(RuntimeState::Ready, {waiting});
        capture_live("details-ready-to-play.png");
        live.NotifyReadyToPlay(42);
        capture_live("ready-to-play-prompt.png");
        assert(live.action("focus:ready-later") && live.action("cross"));
      }
      ready.can_play=true; ready.play_status.clear();
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      assert(live.action("focus:details-more") && live.action("cross"));
      assert(live.action("focus:uninstall") && live.action("cross"));
      capture_live("uninstall-dialog.png");
      assert(live.action("focus:uninstall-delete-saves") && live.action("cross"));
      capture_live("uninstall-dialog-delete-saves.png");
      // Uninstalled with its saves kept, on a console with two drives: Details says so beside Install.
      while(!live.state().navigation.modals.empty()) assert(live.action("circle"));
      live.UpdateVolumes({{"internal","Internal",true,{},{}},{"usb0","USB drive",true,{},{}}}, "usb0");
      live.UpdateInstalls({}, {});
      live.UpdateRuntime(RuntimeState::Ready, {});
      live.UpdateKeptBottles({{42, "Synthetic library title", 1700000000, 450'000'000}});
      assert(live.action("details:42"));
      capture_live("details-kept.png");
      live.UpdateVolumes({{"internal","Console storage",true,674'000'000'000ull,156'000'000'000ull},
                          {"m2","M.2 SSD",true,1'000'000'000'000ull,249'000'000'000ull}}, "m2");
      capture_live("details-free-space.png");
      // Too little room on the default drive: Install opens the sheet, and another drive is for this game only.
      live.UpdateVolumes({{"internal","Console storage",true,674'000'000'000ull,156'000'000'000ull},
                          {"m2","M.2 SSD",true,1'000'000'000'000ull,64'000'000ull}}, "m2");
      assert(live.action("focus:details-install") && live.action("cross"));
      assert(live.TakeInstallRequests().empty());
      capture_live("install-low-space.png");
      assert(live.action("focus:storage-picker") && live.action("cross"));
      capture_live("install-picker.png");
      assert(live.action("focus:volume-internal") && live.action("cross"));
      capture_live("install-other-drive.png");
      assert(live.TakeInstallRequests().empty());
      assert(live.action("circle"));
      // Settings: kept game data is one row that opens the manager with a checkbox per game.
      live.UpdateKeptBottles({{42, "Synthetic library title", 1700000000, 450'000'000},
                              {43, "Café on the coast", 1699000000, 1'200'000'000}});
      while(live.state().navigation.route!=Route::Settings) assert(live.action("r1"));
      assert(live.action("focus:settings-kept"));
      capture_live("settings-kept.png");
      assert(live.action("cross"));
      assert(live.action("focus:kept-toggle-43") && live.action("cross"));
      capture_live("kept-manager.png");
    }
    {
      // Rate and remaining time share the downloaded-size baseline, aligned to the card's right edge.
      Simulation downloads({}, {}, ModelMode::Live);
      downloads.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(downloads.UpdateLibrary({backend.games,false,{},false,false,{}}));
      LiveInstallJob job{42, JobState::Downloading, 47ULL << 30, 59ULL << 30, {}, "internal"};
      job.bytes_per_second = 40'055'603; // 38.2 MiB/s in the UI's existing size convention.
      job.eta_seconds = 300;
      downloads.UpdateInstalls({job}, {});
      while(downloads.state().navigation.route != Route::Downloads) assert(downloads.action("r1"));
      auto capture_download = [&](const char* name) {
        for(int i=0;i<16;++i) renderer.draw(downloads,1920,1080);
        assert(renderer.capture_png(output/name,1920,1080));
      };
      capture_download("download-rate-eta.png");
      job.state = JobState::Paused;
      downloads.UpdateInstalls({job}, {});
      capture_download("download-rate-paused.png");
      downloads.UpdateAppDetails(42, 25ULL << 30, 35ULL << 30, true);
      downloads.UpdateVolumes({{"internal", "Console storage", true, 100ULL << 30, 20ULL << 30}}, "internal");
      capture_download("download-reservation-over-capacity.png");
    }
    model.UpdateAccount({AuthState::Offline,"123","Synthetic account",{},{},true});
    controller.Activate(123,true);settle();
    assert(model.state().games.size()==3 && model.state().library_offline);
    capture("offline.png");
    // Exercise actual private persistence and renderer restoration with synthetic data.
    auto pattern=(std::filesystem::temp_directory_path()/"z3cloudplay-graphical-persistence-XXXXXX").string();
    std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
    const char* made=mkdtemp(temporary.data());assert(made);
    const std::filesystem::path private_root(made);
    {
      z3cloudplay::Storage storage(private_root.string());z3cloudplay::PreferencesStore preferences(storage);
      z3cloudplay::OrganizationStore organization(storage);
      {
        z3cloudplay::app::PersistenceService persistence(preferences,organization);
        z3cloudplay::app::PersistenceController bridge(model,persistence);bridge.Activate(123);
        persistence.WaitIdle();bridge.Tick();
        assert(model.OrganizationAccount()==123);
        assert(model.action("square") && model.state().library.favorites.contains(43));
        assert(model.action("circle"));
        for(int n=0;n<4 && model.state().navigation.route!=Route::Settings;++n)assert(model.action("r1"));
        const auto& rows=model.view().rows;
        const auto target=std::find_if(rows.begin(),rows.end(),[](const ViewRow& row){return row.action=="toggle-motion";});
        assert(target!=rows.end());const int index=static_cast<int>(target-rows.begin());
        for(int n=0;n<80 && model.view().focused_row!=index;++n)assert(model.action(model.view().focused_row<index?"down":"up"));
        assert(model.view().focused_row==index && model.action("cross"));
        bridge.FlushAndShutdown();
      }
      Simulation restored({}, {},ModelMode::Live);
      restored.UpdateAccount({AuthState::Offline,"123","Synthetic account",{},{},true});
      assert(restored.UpdateLibrary({backend.games,false,{},true,true,1700000000}));
      z3cloudplay::app::PersistenceService persistence(preferences,organization);
      z3cloudplay::app::PersistenceController bridge(restored,persistence);bridge.Activate(123);
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while((!restored.state().library.favorites.contains(43) || !restored.state().reduced_motion) && std::chrono::steady_clock::now()<deadline) {
        bridge.Tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      assert(restored.state().reduced_motion && restored.state().library.favorites.contains(43));
      assert(restored.action("r1"));
      for(int i=0;i<16;++i)renderer.draw(restored,1920,1080);
      assert(renderer.capture_png(output/"restored.png",1920,1080));
      bridge.FlushAndShutdown();
    }
    std::filesystem::remove_all(private_root);
  }
  SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
}
