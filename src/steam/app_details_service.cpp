#include "steam/app_details_service.h"

#include <algorithm>

namespace z3cloudplay::steam {

AppDetailsService::AppDetailsService(AppDetailsSource& source, SessionRepository& sessions)
    : source_(source), sessions_(sessions), worker_([this] { Run(); }) {}

AppDetailsService::~AppDetailsService() { Shutdown(); }

void AppDetailsService::Request(GameId id, std::uint64_t account_generation) {
  if (id == 0 || account_generation == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  if (account_generation != generation_) {
    generation_ = account_generation;
    pending_.clear();
    requested_.clear();
    results_.clear();
    cancel_.store(true, std::memory_order_release);
  }
  if (requested_.count(id) || pending_.size() >= kMaxPending) return;
  requested_.insert(id);
  pending_.push_back(id);
  wake_.notify_all();
}

void AppDetailsService::Invalidate(std::uint64_t account_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  generation_ = account_generation;
  pending_.clear();
  requested_.clear();
  results_.clear();
  cancel_.store(true, std::memory_order_release);
  wake_.notify_all();
}

std::vector<AppDetails> AppDetailsService::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AppDetails> output;
  while (!results_.empty() && output.size() < maximum) {
    output.push_back(std::move(results_.front()));
    results_.pop_front();
  }
  return output;
}

void AppDetailsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ && !worker_.joinable()) return;
    stopping_ = true;
    cancel_.store(true, std::memory_order_release);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void AppDetailsService::Run() {
  for (;;) {
    std::vector<GameId> batch;
    std::uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      while (!pending_.empty() && batch.size() < kBatch) {
        batch.push_back(pending_.front());
        pending_.pop_front();
      }
      generation = generation_;
      cancel_.store(false, std::memory_order_release);
    }
    std::vector<AppDetails> fetched;
    const auto session = sessions_.Load();
    if (session.ok() && session.value()) {
      fetched = source_.Fetch(*session.value(), batch, cancel_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (generation != generation_ || cancel_.load(std::memory_order_acquire)) continue;
    for (const auto id : batch) {
      const auto found = std::find_if(fetched.begin(), fetched.end(),
                                      [id](const AppDetails& details) { return details.id == id; });
      AppDetails result = found != fetched.end() ? *found : AppDetails{};
      result.id = id;
      result.account_generation = generation;
      if (found == fetched.end()) {
        result.available = false;
        result.reason = "Steam details are unavailable right now.";
        requested_.erase(id);  // allow a later retry
      }
      results_.push_back(std::move(result));
    }
  }
}

}  // namespace z3cloudplay::steam
