#include "steam/local_session_repository.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace z3cloudplay;
int main() {
  const auto base=std::filesystem::temp_directory_path()/("z3cloudplay-local-session-"+std::to_string(getpid()));
  std::filesystem::remove_all(base); std::filesystem::create_directories(base);
  const auto root=base/"data";
  LocalSessionRepository repo(root.string());
  assert(!std::filesystem::exists(root)); // Construction does no filesystem IO.
  auto result=repo.Load(); assert(result.ok() && !result.value());
  assert(std::filesystem::is_directory(root));
  assert(!std::filesystem::exists(root/"session-storage.probe"));
  StoredSession session{123,"synthetic-account","synthetic-refresh-secret","synthetic-access-secret",{}};
  assert(repo.Save(session).ok());
  struct stat info{}; assert(stat((root/"session.v1").c_str(),&info)==0 && (info.st_mode&0777)==0600);
  LocalSessionRepository restarted(root.string());
  result=restarted.Load(); assert(result.ok() && result.value()->steam_id==123);
  // Authentication, library, details, installs and Cloud each own a repository
  // for the same root. Their first reads must not race on the storage probe.
  std::atomic<bool> start{false};
  std::atomic<int> failed_loads{0};
  std::vector<std::thread> readers;
  for (int worker=0;worker<5;++worker) readers.emplace_back([&] {
    while (!start.load()) std::this_thread::yield();
    for (int attempt=0;attempt<100;++attempt) {
      LocalSessionRepository concurrent(root.string());
      const auto loaded=concurrent.Load();
      if (!loaded.ok() || !loaded.value() || loaded.value()->steam_id!=123) ++failed_loads;
    }
  });
  start=true;
  for (auto& reader:readers) reader.join();
  assert(failed_loads==0 && "concurrent repositories must restore the saved session");
  for (const auto& entry:std::filesystem::directory_iterator(root))
    assert(entry.path().filename()=="session.v1" && "storage probes must be removed");
  assert(restarted.Clear().ok() && !repo.Load().value());
  const auto bad=base/"file"; std::ofstream(bad)<<"not a directory";
  std::string diagnostic;
  LocalSessionRepository unavailable(bad.string(), [&](const std::string& text) { diagnostic = text; });
  assert(!unavailable.Load().ok());
  assert(diagnostic.find("stat") != std::string::npos);
  assert(!unavailable.Save(session).ok());
  const auto missing=base/"missing"/"child";
  LocalSessionRepository absent(missing.string()); assert(!absent.Load().ok());
  assert(absent.Clear().ok());
  assert(unavailable.Load().message().find("synthetic-refresh-secret")==std::string::npos);
  std::filesystem::remove_all(base);
}
