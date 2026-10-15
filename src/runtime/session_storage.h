#pragma once

#include "platform/storage.h"
#include "platform/managed_directory.h"
#include "runtime/session_reconciler.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace playden::runtime {

// Host-only mapping of the canonical console /data mount. RuntimeInstall
// identity remains canonical; arbitrary record roots are never accepted.
struct HostDataRootForTests {
  std::filesystem::path path;
};

class FilesystemReconcileSessionSource final : public ReconcileSessionSource {
public:
  static constexpr std::size_t kMaxSessionBytes = 4096;
  static constexpr std::size_t kMaxMarkerBytes = 4096;
  static constexpr std::size_t kMaxDirectoryEntries = 1024;

  FilesystemReconcileSessionSource();
  explicit FilesystemReconcileSessionSource(HostDataRootForTests mapping);

  SessionEnumeration Enumerate(const RuntimeInstall &record,
                               std::size_t limit) override;
  // One session record by id, with the same ownership checks and no bound on
  // how many other records exist. A record not written yet is an empty result.
  SessionEnumeration Read(const RuntimeInstall &record,
                          const std::string &session_id);

  // Historical runtime/revision identities are returned unchanged so the
  // reconciler can reject each one without hiding later records. This adapter
  // does not archive old records; reaching a bound or finding temporary/lock
  // evidence leaves the whole install pending for explicit recovery.

private:
  std::optional<platform::ManagedDirectory> OpenSessions(const RuntimeInstall &record,
                                               SessionEnumeration *failure);
  std::filesystem::path host_data_root_;
};

// Storage must name a caller-provisioned private 0700 root dedicated to
// reconciliation journals, separate from the wrapper's logs/sessions tree.
class DurableReconcileJournal final : public ReconcileJournal {
public:
  static constexpr std::size_t kMaxJournalBytes = 16 * 1024;

  explicit DurableReconcileJournal(Storage &storage);
  ReconcileJournalLoad Load(const std::string &session_id) override;
  ReconcileJournalResult Save(const ReconcileEntry &entry) override;

private:
  Storage &storage_;
};

} // namespace playden::runtime
