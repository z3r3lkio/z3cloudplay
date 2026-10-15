#pragma once

#include "steam/backend/cm_client.h"
#include "steam/library_backend.h"

namespace z3cloudplay::steam {

// The owned library from GetOwnedGames, enriched with CM acquisition dates. As in the macOS
// reference, both run in parallel and a CM failure is never fatal: games then simply lack a date.
// A CM logon rejection never reports revoked credentials.
class EntitledLibraryBackend final : public LibraryBackend {
 public:
  EntitledLibraryBackend(LibraryBackend& owned, AcquisitionSource& acquisitions)
      : owned_(owned), acquisitions_(acquisitions) {}

  LibraryResult Fetch(const StoredSession& session, const std::atomic<bool>& cancel) override;

  // Category of the most recent CM attempt, for diagnostics without identifiers.
  CmError last_acquisition_error() const { return last_error_.load(); }

 private:
  LibraryBackend& owned_;
  AcquisitionSource& acquisitions_;
  std::atomic<CmError> last_error_{CmError::kNone};
};

}  // namespace z3cloudplay::steam
