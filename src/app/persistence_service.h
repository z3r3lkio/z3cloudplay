#pragma once

#include "app/preferences.h"
#include "library/organization.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace z3cloudplay::app {

using PersistenceRequestId = std::uint64_t;

enum class PersistenceEventKind {
  kPreferencesLoaded,
  kPreferencesSaved,
  kOrganizationLoaded,
  kOrganizationSaved,
  kOrganizationCleared,
};

struct PersistenceEvent {
  PersistenceEventKind kind = PersistenceEventKind::kPreferencesLoaded;
  PersistenceRequestId request_id = 0;
  std::uint64_t account_id = 0;
  std::uint64_t account_generation = 0;
  std::uint64_t revision = 0;
  StorageError error = StorageError::kNone;
  PreferencesLoadStatus preferences_status = PreferencesLoadStatus::kLoaded;
  std::optional<Preferences> preferences;
  std::optional<OrganizationSnapshot> organization;
};

// A single worker owns all preferences and organization IO. Dependencies
// outlive the service. Accepted saves and organization clears are durable
// shutdown work; loads may be discarded when stale or during shutdown.
class PersistenceService {
 public:
  PersistenceService(PreferencesStore&, OrganizationRepository&);
  ~PersistenceService();
  PersistenceService(const PersistenceService&) = delete;
  PersistenceService& operator=(const PersistenceService&) = delete;

  PersistenceRequestId LoadPreferences();
  PersistenceRequestId SavePreferences(Preferences preferences,
                                       std::uint64_t revision);
  PersistenceRequestId LoadOrganization(std::uint64_t account_id,
                                        std::uint64_t account_generation);
  PersistenceRequestId SaveOrganization(std::uint64_t account_id,
                                        std::uint64_t account_generation,
                                        std::uint64_t revision,
                                        OrganizationSnapshot snapshot);
  PersistenceRequestId ClearOrganization(std::uint64_t account_id,
                                         std::uint64_t account_generation);
  void InvalidateOrganization(std::uint64_t account_generation);
  // Waits for all currently accepted work to finish without stopping the
  // worker. Used by shutdown coordination when a load must be merged before
  // the final save is submitted.
  void WaitIdle();
  void Shutdown();
  std::vector<PersistenceEvent> Pump(std::size_t max_events);

 private:
  enum class Command {
    kLoadPreferences,
    kSavePreferences,
    kLoadOrganization,
    kSaveOrganization,
    kClearOrganization,
  };
  struct Job {
    Command command = Command::kLoadPreferences;
    PersistenceRequestId request = 0;
    std::uint64_t account = 0;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    Preferences preferences;
    OrganizationSnapshot organization;
  };

  PersistenceRequestId Submit(Job);
  static bool IsOrganization(Command);
  static bool IsLoad(Command);
  void Run();
  void Execute(Job);
  void PushEvent(PersistenceEvent, bool clear_terminal = false);
  bool ClearRequest(PersistenceRequestId) const;

  PreferencesStore& preferences_;
  OrganizationRepository& organizations_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::deque<Job> pending_;
  std::optional<Job> active_;
  std::deque<PersistenceEvent> events_;
  std::deque<PersistenceRequestId> clear_requests_;
  PersistenceRequestId next_request_ = 0;
  std::uint64_t preferences_revision_ = 0;
  std::uint64_t organization_generation_ = 0;
  std::uint64_t organization_account_ = 0;
  std::uint64_t organization_revision_ = 0;
  bool clear_barrier_ = false;
  bool stopping_ = false;
  bool joined_ = false;
  std::thread worker_;
};

}  // namespace z3cloudplay::app
