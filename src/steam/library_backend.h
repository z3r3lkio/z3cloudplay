#pragma once

#include "library/model.h"
#include "steam/session_store.h"

#include <atomic>
#include <vector>

namespace z3cloudplay::steam {

enum class LibraryError {
  kNone,
  kCanceled,
  kNetwork,
  kProtocol,
  kCredentialsRevoked,
  kNoSession,
  kPersistence,
};

struct LibraryResult {
  LibraryError error = LibraryError::kProtocol;
  std::vector<z3cloudplay::GameSummary> games;
  bool metadata_partial = false;
};

class LibraryBackend {
 public:
  virtual ~LibraryBackend() = default;
  virtual LibraryResult Fetch(const StoredSession& session,
                              const std::atomic<bool>& cancel) = 0;
};

}  // namespace z3cloudplay::steam
