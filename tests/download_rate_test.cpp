#include "steam/download_rate.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace z3cloudplay::steam;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while(false)
int main() {
  {
    // Resumed bytes seed the baseline; nothing shows during the warm-up.
    DownloadRate rate;
    CHECK(!rate.Estimate(0, 0, 100000000).bytes_per_second);
    rate.Record(9000000000ull, 0);
    rate.Record(9002000000ull, 1000);
    rate.Record(9004000000ull, 2000);
    CHECK(!rate.Estimate(2999, 9004000000ull, 9100000000ull).bytes_per_second);
    rate.Record(9006000000ull, 3000);
    const auto result = rate.Estimate(3000, 9006000000ull, 9100000000ull);
    CHECK(result.bytes_per_second == 2000000 && result.eta_seconds == 47);
  }
  {
    // A steady 10 MB/s reads as 10 MB/s, and time left follows it.
    DownloadRate rate;
    rate.Record(0, 0);
    std::uint64_t done = 0;
    for (std::int64_t now = 100; now <= 40000; now += 100) {
      done += 1000000;
      rate.Record(done, now);
      if (now >= 10000) {
        const auto result = rate.Estimate(now, done, 1000000000);
        CHECK(*result.bytes_per_second >= 9900000 && *result.bytes_per_second <= 10100000);
        const auto expected = static_cast<int>((1000000000 - done) / 10000000);
        CHECK(result.eta_seconds && *result.eta_seconds >= expected - 1 && *result.eta_seconds <= expected + 1);
      }
    }
  }
  {
    // Bursts of 30 MB every third second still read as about 10 MB/s, and the shown values hold still
    // between refreshes.
    DownloadRate rate;
    rate.Record(0, 0);
    std::uint64_t done = 0;
    DownloadEstimate last;
    std::int64_t speed_changed = 0, eta_changed = 0;
    for (std::int64_t now = 100; now <= 60000; now += 100) {
      if (now % 3000 == 0) { done += 30000000; rate.Record(done, now); }
      const auto shown = rate.Shown(now, done, 2000000000);
      if (now >= 12000) {
        CHECK(*shown.bytes_per_second >= 6000000 && *shown.bytes_per_second <= 12000000);
        if (shown.bytes_per_second != last.bytes_per_second) {
          CHECK(now - speed_changed >= DownloadRate::kSpeedRefreshMs); speed_changed = now;
        }
        if (shown.eta_seconds != last.eta_seconds) {
          CHECK(now - eta_changed >= DownloadRate::kEtaRefreshMs); eta_changed = now;
        }
      }
      last = shown;
    }
    CHECK(speed_changed > 0 && eta_changed > 0);
  }
  {
    // A stall shows at once as zero with no time left, and recovers when bytes arrive again.
    DownloadRate rate;
    rate.Record(0, 0);
    for (std::int64_t now = 1000; now <= 10000; now += 1000) rate.Record(now * 1000, now);
    CHECK(*rate.Shown(10000, 10000000, 100000000).bytes_per_second > 0);
    CHECK(*rate.Shown(14999, 10000000, 100000000).bytes_per_second > 0);
    const auto stalled = rate.Shown(15000, 10000000, 100000000);
    CHECK(stalled.bytes_per_second == 0 && !stalled.eta_seconds);
    rate.Record(11000000, 15100);
    CHECK(*rate.Shown(15100, 11000000, 100000000).bytes_per_second > 0);
  }
  {
    DownloadRate rate;
    rate.Record(1000, 0);
    rate.Record(5000000, 4000);
    CHECK(rate.Estimate(4000, 5000000, 5000000).eta_seconds == 0);
    CHECK(!rate.Estimate(4000, 5000000, 0).eta_seconds);
    CHECK(rate.Estimate(4000, 5000000, std::numeric_limits<std::uint64_t>::max()).eta_seconds ==
          std::numeric_limits<int>::max());
    rate.Record(5, 5000); // regressing progress starts a new baseline
    CHECK(!rate.Estimate(5000, 5, 10).bytes_per_second);
    rate.Record(10, 500); // a changed clock resets too
    CHECK(!rate.Estimate(500, 10, 20).bytes_per_second);
    rate.Reset();
    CHECK(!rate.Shown(9000, 10, 20).bytes_per_second);
  }
}
