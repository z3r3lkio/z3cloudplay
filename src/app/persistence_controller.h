#pragma once

#include "app/persistence_service.h"
#include "homebrew/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace z3cloudplay::app {

// Main-thread bridge between the renderer-neutral model and persistence IO.
// PersistenceService and Simulation outlive this object.
class PersistenceController {
 public:
  PersistenceController(homebrew::Simulation&, PersistenceService&);

  // Starts an account-scoped organization load. Re-activating the current
  // account is idempotent.
  void Activate(std::uint64_t account_id);

  // Invalidates account-scoped work while preserving global preferences.
  void Reset();
  void Tick();
  void Retry();

  // Begins the non-droppable organization deletion barrier. The terminal
  // result remains available until consumed.
  void Clear();
  std::optional<StorageError> TakeClearResult();

  // Resolves required startup loads, captures the final model revisions, and
  // drains accepted saves before stopping the worker.
  void FlushAndShutdown();

 private:
  void Pump(std::size_t maximum);
  void ObserveAndSave();
  void BeginPreferencesLoad();
  void BeginOrganizationLoad();
  void Apply(PersistenceEvent);

  homebrew::Simulation& model_;
  PersistenceService& service_;
  std::uint64_t account_id_ = 0;
  std::uint64_t account_generation_ = 0;
  PersistenceRequestId preferences_load_request_ = 0;
  PersistenceRequestId organization_load_request_ = 0;
  PersistenceRequestId clear_request_ = 0;
  homebrew::PersistenceLoadToken preferences_load_token_;
  homebrew::PersistenceLoadToken organization_load_token_;
  std::uint64_t submitted_preferences_revision_ = 0;
  std::uint64_t submitted_organization_revision_ = 0;
  std::optional<StorageError> clear_result_;
  bool preferences_load_failed_ = false;
  bool organization_load_failed_ = false;
  bool clear_had_pending_load_ = false;
  bool shutting_down_ = false;
};

}  // namespace z3cloudplay::app
