#pragma once

#include "steam/backend/http_client.h"
#include "steam/library_backend.h"

#include <functional>
#include <memory>
#include <string>

namespace z3cloudplay::steam {

struct LibraryClientHooks {
  std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> perform;
};

// Worker-owned synchronous adapter for the authenticated GetOwnedGames endpoint.
class LibraryClient final : public LibraryBackend {
 public:
  explicit LibraryClient(std::string ca_bundle);
  explicit LibraryClient(LibraryClientHooks hooks);

  LibraryResult Fetch(const StoredSession& session,
                      const std::atomic<bool>& cancel) override;

 private:
  LibraryClientHooks hooks_;
  std::string ca_bundle_;
  std::unique_ptr<HttpClient> http_;
};

}  // namespace z3cloudplay::steam
