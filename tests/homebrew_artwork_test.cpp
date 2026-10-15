#include "homebrew/artwork.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace z3cloudplay::homebrew;
using namespace std::chrono_literals;

namespace {

ArtworkKey Key(std::uint64_t generation, z3cloudplay::GameId game,
               const char* source) {
  return {generation, game, source};
}

ArtworkImage Pixel(std::uint8_t value = 7) {
  return {1, 1, {value, value, value, 255}};
}

class ControlledBackend final : public ArtworkBackend {
 public:
  std::atomic<int> calls{0};
  std::atomic<bool> block{false};
  std::atomic<bool> release{false};
  std::atomic<bool> ignore_cancel{false};
  std::atomic<bool> saw_cancel{false};
  mutable std::mutex mutex;
  std::vector<ArtworkKey> requested;
  std::map<z3cloudplay::GameId, bool> fail;

  std::optional<ArtworkImage> Load(
      const ArtworkKey& key, const std::atomic<bool>& cancel) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      requested.push_back(key);
    }
    ++calls;
    while (block && !release) {
      if (cancel) {
        saw_cancel = true;
        if (!ignore_cancel) return std::nullopt;
      }
      std::this_thread::sleep_for(1ms);
    }
    if (cancel) saw_cancel = true;
    if (fail[key.game_id]) return std::nullopt;
    return Pixel(static_cast<std::uint8_t>(key.game_id));
  }
};

void WaitCalls(const ControlledBackend& backend, int count) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (backend.calls.load() < count && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(backend.calls.load() >= count);
}

void WaitCount(const std::atomic<int>& value, int count) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (value.load() < count && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(value.load() >= count);
}

ArtworkResult Await(ArtworkService& service) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto values = service.Pump();
    if (!values.empty()) return std::move(values.front());
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "artwork result deadline exceeded");
  return {};
}

void SameSubscriptionIsIdempotentAndResubscribeRetries() {
  ControlledBackend backend;
  ArtworkService service(backend);
  const auto key = Key(1, 10, "https://example.invalid/10.jpg");
  assert(service.ReplaceSubscriptions({key}));
  auto first = Await(service);
  assert(first.key == key && first.image && first.image->rgba[0] == 10);
  assert(service.ReplaceSubscriptions({key}));
  std::this_thread::sleep_for(20ms);
  assert(backend.calls == 1 && service.Pump().empty());

  assert(service.ReplaceSubscriptions({}));
  assert(service.ReplaceSubscriptions({key}));
  auto second = Await(service);
  assert(second.key == key && second.image && backend.calls == 2);
}

void FailureIsReportedOnceUntilExplicitResubscribe() {
  ControlledBackend backend;
  backend.fail[11] = true;
  ArtworkService service(backend);
  const auto key = Key(1, 11, "https://example.invalid/11.jpg");
  assert(service.ReplaceSubscriptions({key}));
  const auto failure = Await(service);
  assert(failure.key == key && !failure.image);
  assert(service.ReplaceSubscriptions({key}));
  std::this_thread::sleep_for(20ms);
  assert(backend.calls == 1 && service.Pump().empty());

  backend.fail[11] = false;
  assert(service.ReplaceSubscriptions({}));
  assert(service.ReplaceSubscriptions({key}));
  assert(Await(service).image && backend.calls == 2);
}

void RemovedLateResultCannotCrossCellAccountOrSourceIdentity() {
  ControlledBackend backend;
  backend.block = true;
  backend.ignore_cancel = true;
  ArtworkService service(backend);
  const auto old_key = Key(3, 12, "https://example.invalid/old.jpg");
  const auto new_key = Key(4, 12, "https://example.invalid/new.jpg");
  assert(service.ReplaceSubscriptions({old_key}));
  WaitCalls(backend, 1);
  assert(service.ReplaceSubscriptions({new_key}));
  const auto cancel_deadline = std::chrono::steady_clock::now() + 2s;
  while (!backend.saw_cancel && std::chrono::steady_clock::now() < cancel_deadline)
    std::this_thread::sleep_for(1ms);
  assert(backend.saw_cancel);
  backend.release = true;
  const auto result = Await(service);
  assert(result.key == new_key && result.image);
  assert(backend.calls == 2);
  assert(service.Pump(64).empty());
}

void InvalidOrExcessiveReplacementLeavesCurrentWorkIntact() {
  ControlledBackend backend;
  backend.block = true;
  ArtworkService service(backend);
  const auto valid = Key(1, 13, "https://example.invalid/13.jpg");
  assert(service.ReplaceSubscriptions({valid}));
  WaitCalls(backend, 1);
  assert(!service.ReplaceSubscriptions({Key(0, 13, "x")}));
  assert(!service.ReplaceSubscriptions({Key(1, 0, "x")}));
  assert(!service.ReplaceSubscriptions({Key(1, 13, "")}));
  assert(!service.ReplaceSubscriptions(
      {Key(1, 13, std::string(kMaxArtworkSourceBytes + 1, 'x').c_str())}));
  std::string embedded_nul("a\0b", 3);
  assert(!service.ReplaceSubscriptions({ArtworkKey{1, 13, embedded_nul}}));
  assert(!service.ReplaceSubscriptions({valid, valid}));
  std::vector<ArtworkKey> excessive;
  for (z3cloudplay::GameId id = 1; id <= 65; ++id)
    excessive.push_back(Key(1, id, "x"));
  assert(!service.ReplaceSubscriptions(std::move(excessive)));
  assert(!backend.saw_cancel);
  backend.release = true;
  assert(Await(service).key == valid);
}

void ResultBytesBackpressureBoundsCompletedImages() {
  class LargeBackend final : public ArtworkBackend {
   public:
    std::atomic<int> calls{0};
    std::optional<ArtworkImage> Load(
        const ArtworkKey&, const std::atomic<bool>&) override {
      ++calls;
      return ArtworkImage{1280, 1024,
                          std::vector<std::uint8_t>(kMaxArtworkImageBytes, 1)};
    }
  } backend;
  ArtworkService service(backend);
  std::vector<ArtworkKey> keys;
  for (z3cloudplay::GameId id = 1; id <= 5; ++id) keys.push_back(Key(1, id, "x"));
  assert(service.ReplaceSubscriptions(keys));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (backend.calls.load() < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(backend.calls == 4);
  std::this_thread::sleep_for(20ms);
  assert(backend.calls == 4);
  assert(service.Pump(1).size() == 1);
  WaitCount(backend.calls, 5);
}

void ShutdownCancelsAndJoinsBlockedLoad() {
  ControlledBackend backend;
  backend.block = true;
  ArtworkService service(backend);
  assert(service.ReplaceSubscriptions({Key(1, 14, "x")}));
  WaitCalls(backend, 1);
  service.Shutdown();
  assert(backend.saw_cancel);
  assert(!service.ReplaceSubscriptions({Key(1, 15, "x")}));
}

void InvalidBackendImageIsReducedToFailure() {
  class InvalidBackend final : public ArtworkBackend {
   public:
    std::optional<ArtworkImage> Load(
        const ArtworkKey&, const std::atomic<bool>&) override {
      return ArtworkImage{2, 2, {1, 2, 3, 4}};
    }
  } backend;
  ArtworkService service(backend);
  assert(service.ReplaceSubscriptions({Key(1, 16, "x")}));
  assert(!Await(service).image);
}

}  // namespace

class ThrowingBackend final : public ArtworkBackend {
 public:
  std::optional<ArtworkImage> Load(
      const ArtworkKey& key, const std::atomic<bool>&) override {
    if (key.game_id == 1) throw std::runtime_error("unexpected backend fault");
    return Pixel(static_cast<std::uint8_t>(key.game_id));
  }
};

class FlakyBackend final : public ArtworkBackend {
 public:
  explicit FlakyBackend(int failures) : failures_left(failures) {}
  std::atomic<int> failures_left;
  std::atomic<int> calls{0};
  std::optional<ArtworkImage> Load(
      const ArtworkKey& key, const std::atomic<bool>&) override {
    ++calls;
    if (failures_left.fetch_sub(1) > 0) return std::nullopt;
    return Pixel(static_cast<std::uint8_t>(key.game_id));
  }
};

// Subscribe every frame, as LiveArtworkCache does, until a result arrives.
ArtworkResult AwaitSubscribed(ArtworkService& service, const ArtworkKey& key) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    assert(service.ReplaceSubscriptions({key}));
    auto values = service.Pump();
    if (!values.empty()) return std::move(values.front());
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "artwork result deadline exceeded");
  return {};
}

void FailedLoadRetriesAfterDelayWhileSubscribed() {
  FlakyBackend backend(1);
  ArtworkService service(backend, 30ms);
  const auto key = Key(1, 5, "https://example.invalid/5.jpg");
  assert(!AwaitSubscribed(service, key).image);
  // Within the delay the continuous subscription does not retry.
  assert(service.ReplaceSubscriptions({key}));
  std::this_thread::sleep_for(5ms);
  assert(backend.calls == 1 && service.Pump().empty());
  const auto recovered = AwaitSubscribed(service, key);
  assert(recovered.image && backend.calls == 2);
  // A loaded key is never fetched again while it stays subscribed.
  for (int frame = 0; frame < 20; ++frame) {
    assert(service.ReplaceSubscriptions({key}));
    std::this_thread::sleep_for(5ms);
  }
  assert(backend.calls == 2 && service.Pump().empty());
}

void RetriesStopAfterMaximumAttempts() {
  FlakyBackend backend(100);
  ArtworkService service(backend, 1ms);
  const auto key = Key(1, 6, "https://example.invalid/6.jpg");
  for (std::uint32_t attempt = 0; attempt < kMaxArtworkAttempts; ++attempt)
    assert(!AwaitSubscribed(service, key).image);
  for (int frame = 0; frame < 30; ++frame) {
    assert(service.ReplaceSubscriptions({key}));
    std::this_thread::sleep_for(2ms);
  }
  assert(backend.calls == static_cast<int>(kMaxArtworkAttempts));
  assert(service.Pump().empty());
}

void UnexpectedBackendExceptionBecomesFailureAndWorkerSurvives() {
  ThrowingBackend backend;
  ArtworkService service(backend);
  assert(service.ReplaceSubscriptions({Key(1, 1, "throws")}));
  const auto failed = Await(service);
  assert(failed.key == Key(1, 1, "throws") && !failed.image);
  assert(service.ReplaceSubscriptions({Key(1, 1, "throws"), Key(1, 2, "ok")}));
  const auto loaded = Await(service);
  assert(loaded.key == Key(1, 2, "ok") && loaded.image &&
         loaded.image->rgba[0] == 2);
  service.Shutdown();
}

int main() {
  SameSubscriptionIsIdempotentAndResubscribeRetries();
  FailureIsReportedOnceUntilExplicitResubscribe();
  RemovedLateResultCannotCrossCellAccountOrSourceIdentity();
  InvalidOrExcessiveReplacementLeavesCurrentWorkIntact();
  ResultBytesBackpressureBoundsCompletedImages();
  ShutdownCancelsAndJoinsBlockedLoad();
  InvalidBackendImageIsReducedToFailure();
  UnexpectedBackendExceptionBecomesFailureAndWorkerSurvives();
  FailedLoadRetriesAfterDelayWhileSubscribed();
  RetriesStopAfterMaximumAttempts();
}
