#include "steam/backend/entitled_library.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;

namespace {

struct Owned final : LibraryBackend {
  LibraryResult result{LibraryError::kNone, {{10, "Ten", {}, {}, {}}, {20, "Twenty", {}, {}, {}}}, false};
  LibraryResult Fetch(const StoredSession&, const std::atomic<bool>&) override { return result; }
};

struct Acquisitions final : AcquisitionSource {
  AcquisitionResult result{CmError::kNone, {{10, 1500000000u}, {99, 1400000000u}}};
  bool block_until_stopped = false;
  std::atomic<bool> saw_stop{false};
  AcquisitionResult FetchAcquisitionDates(const StoredSession&, const std::atomic<bool>& cancel) override {
    while (block_until_stopped) {
      if (cancel) {
        saw_stop = true;
        return {CmError::kCanceled, {}};
      }
      std::this_thread::sleep_for(1ms);
    }
    return result;
  }
};

StoredSession Session() { return StoredSession{123, "synthetic", "refresh", "access", {}}; }

void DatesMergeIntoOwnedGames() {
  Owned owned;
  Acquisitions acquisitions;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(result.games[0].added_at_unix_seconds == 1500000000u);
  // App 99 is licensed but not in the owned list; app 20 has no license date.
  assert(!result.games[1].added_at_unix_seconds);
}

void CmFailureIsNeverFatal() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.result = {CmError::kRejected, {}};
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kNone && result.games.size() == 2);
  assert(!result.games[0].added_at_unix_seconds && !result.games[1].added_at_unix_seconds);
  assert(backend.last_acquisition_error() == CmError::kRejected);
}

void OwnedFailureStopsTheCmSession() {
  Owned owned;
  owned.result = {LibraryError::kCredentialsRevoked, {}, false};
  Acquisitions acquisitions;
  acquisitions.block_until_stopped = true;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  const auto start = std::chrono::steady_clock::now();
  const auto result = backend.Fetch(Session(), cancel);
  assert(result.error == LibraryError::kCredentialsRevoked && acquisitions.saw_stop);
  assert(std::chrono::steady_clock::now() - start < 1s);
}

void CancelReachesARunningCmSession() {
  Owned owned;
  Acquisitions acquisitions;
  acquisitions.block_until_stopped = true;
  EntitledLibraryBackend backend(owned, acquisitions);
  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(30ms);
    cancel = true;
  });
  const auto result = backend.Fetch(Session(), cancel);
  canceler.join();
  assert(acquisitions.saw_stop && result.error == LibraryError::kNone);
  assert(!result.games[0].added_at_unix_seconds);
}

}  // namespace

int main() {
  DatesMergeIntoOwnedGames();
  CmFailureIsNeverFatal();
  OwnedFailureStopsTheCmSession();
  CancelReachesARunningCmSession();
}
