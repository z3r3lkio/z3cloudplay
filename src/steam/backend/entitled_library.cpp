#include "steam/backend/entitled_library.h"

#include <chrono>
#include <thread>

namespace z3cloudplay::steam {

LibraryResult EntitledLibraryBackend::Fetch(const StoredSession& session,
                                            const std::atomic<bool>& cancel) {
  // The CM side gets its own flag so a failed or canceled owned fetch stops it promptly.
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  AcquisitionResult acquisitions;
  std::thread worker([&] {
    acquisitions = acquisitions_.FetchAcquisitionDates(session, stop);
    done.store(true, std::memory_order_release);
  });

  LibraryResult result = owned_.Fetch(session, cancel);
  if (result.error != LibraryError::kNone) stop.store(true, std::memory_order_release);
  while (!done.load(std::memory_order_acquire)) {
    if (cancel.load(std::memory_order_acquire)) stop.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  worker.join();

  last_error_.store(acquisitions.error);
  if (result.error != LibraryError::kNone || !acquisitions.ok()) return result;
  for (auto& game : result.games) {
    const auto found = acquisitions.acquired_at.find(game.id);
    if (found != acquisitions.acquired_at.end()) game.added_at_unix_seconds = found->second;
  }
  return result;
}

}  // namespace z3cloudplay::steam
