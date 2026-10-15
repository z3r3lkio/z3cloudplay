#pragma once
#include "homebrew/simulation.hpp"
#include "library/service.h"
#include <optional>

namespace z3cloudplay::app {
// Presentation-thread projection. The service owns IO and outlives this bridge.
class LibraryController {
 public:
  LibraryController(homebrew::Simulation& model,LibraryService& service);
  void Activate(std::uint64_t account_id,bool offline);
  void Refresh();
  void Reset();
  void Clear();
  void Tick();
  std::optional<steam::LibraryError> TakeClearResult();
  bool NeedsRenewal() const { return offline_ || needs_renewal_; }
 private:
  void Loading();
  homebrew::Simulation& model_;
  LibraryService& service_;
  std::uint64_t account_=0,generation_=0,request_=0;
  bool offline_=false,clearing_=false;
  bool needs_renewal_=false;
  std::optional<steam::LibraryError> clear_result_;
};
}
