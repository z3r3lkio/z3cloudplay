#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
namespace z3cloudplay::steam {
struct DownloadEstimate {
  std::optional<std::uint64_t> bytes_per_second;
  std::optional<int> eta_seconds;
};
// Download speed and time left for display; all times are steady-clock milliseconds. Speed averages the
// last 8 seconds and time left the last 30, so chunk bursts do not make the labels jump. Shown() changes
// speed at most every 2 seconds and time left every 5, except that a stall shows at once.
class DownloadRate {
 public:
  static constexpr std::int64_t kWarmupMs = 3000, kStallMs = 5000;
  static constexpr std::int64_t kSpeedWindowMs = 8000, kEtaWindowMs = 30000;
  static constexpr std::int64_t kSpeedRefreshMs = 2000, kEtaRefreshMs = 5000;

  void Reset() { *this = DownloadRate{}; }
  void Record(std::uint64_t done, std::int64_t now) {
    if (now < 0) { Reset(); return; }
    if (!started_ || now < last_ms_ || done < last_done_) {
      Reset(); started_ = true; start_ms_ = progress_ms_ = last_ms_ = now; last_done_ = done;
      samples_.push_back({now, done});
      return; // A journal's resumed bytes are the baseline, never new traffic.
    }
    if (done > last_done_) progress_ms_ = now;
    last_ms_ = now; last_done_ = done;
    // One sample per quarter second is plenty; the baseline stays until a newer sample covers the longest window.
    if (samples_.size() > 1 && now - samples_[samples_.size() - 2].ms < kSampleMs) samples_.back() = {now, done};
    else samples_.push_back({now, done});
    // Keep one sample at or before the longest window's start.
    while (samples_.size() > 2 && samples_[1].ms <= now - kEtaWindowMs) samples_.pop_front();
  }
  // The current estimate, unthrottled.
  DownloadEstimate Estimate(std::int64_t now, std::uint64_t done, std::uint64_t total) const {
    if (!started_ || now < last_ms_ || now - start_ms_ < kWarmupMs) return {};
    DownloadEstimate result;
    if (now - progress_ms_ >= kStallMs) { result.bytes_per_second = 0; return result; }
    result.bytes_per_second = Average(now, done, kSpeedWindowMs);
    const auto eta_rate = Average(now, done, kEtaWindowMs);
    if (total && eta_rate) {
      const auto remaining = total > done ? total - done : 0;
      const auto seconds = remaining ? (remaining - 1) / eta_rate + 1 : 0;
      result.eta_seconds = static_cast<int>(std::min<std::uint64_t>(seconds, std::numeric_limits<int>::max()));
    }
    return result;
  }
  // The estimate to show: refreshed at most every kSpeedRefreshMs / kEtaRefreshMs.
  DownloadEstimate Shown(std::int64_t now, std::uint64_t done, std::uint64_t total) {
    const auto fresh = Estimate(now, done, total);
    if (!fresh.bytes_per_second) { shown_ = {}; return shown_; }
    const bool was_stalled = !shown_.bytes_per_second || *shown_.bytes_per_second == 0;
    if (was_stalled || *fresh.bytes_per_second == 0 || now - speed_ms_ >= kSpeedRefreshMs) {
      shown_.bytes_per_second = fresh.bytes_per_second; speed_ms_ = now;
    }
    if (!fresh.eta_seconds || !shown_.eta_seconds || now - eta_ms_ >= kEtaRefreshMs) {
      shown_.eta_seconds = fresh.eta_seconds; eta_ms_ = now;
    }
    return shown_;
  }

 private:
  static constexpr std::int64_t kSampleMs = 250;
  struct Sample { std::int64_t ms = 0; std::uint64_t done = 0; };
  std::uint64_t Average(std::int64_t now, std::uint64_t done, std::int64_t window) const {
    const Sample* reference = &samples_.front();
    for (const auto& sample : samples_) {
      if (sample.ms > now - window) break;
      reference = &sample;
    }
    const auto elapsed = now - reference->ms;
    if (elapsed <= 0 || done <= reference->done) return 0;
    const long double per_second = static_cast<long double>(done - reference->done) * 1000 / elapsed;
    return per_second >= std::numeric_limits<std::uint64_t>::max() ? std::numeric_limits<std::uint64_t>::max()
                                                                   : static_cast<std::uint64_t>(per_second);
  }
  std::deque<Sample> samples_;
  DownloadEstimate shown_;
  bool started_ = false;
  std::int64_t start_ms_ = 0, last_ms_ = 0, progress_ms_ = 0, speed_ms_ = 0, eta_ms_ = 0;
  std::uint64_t last_done_ = 0;
};
}
