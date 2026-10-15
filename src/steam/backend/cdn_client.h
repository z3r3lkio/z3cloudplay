#pragma once

#include "steam/backend/http_client.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace z3cloudplay::steam {

struct ContentServer {
  std::string host;
  std::string vhost;  // the name used in URLs and TLS
  int load = 100;
};

struct CdnStats {
  std::uint64_t requests = 0, failures = 0;
  std::uint64_t resolve_ms = 0, connect_ms = 0, tls_ms = 0, first_byte_ms = 0, total_ms = 0;
  std::uint64_t new_connections = 0, bytes = 0;
};

enum class CdnError { kNone, kCanceled, kNetwork, kNotFound, kProtocol };

// Steam CDN access over the verified HTTPS client: the content server list for the account's
// cell, manifests and chunks. Only HTTPS-capable SteamCache/CDN servers are used. One worker
// thread owns each instance.
class CdnClient {
 public:
  using Perform = std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)>;
  explicit CdnClient(std::string ca_bundle);
  explicit CdnClient(Perform perform);  // tests
  ~CdnClient();

  // Servers sorted by load, from IContentServerDirectoryService/GetServersForSteamPipe.
  CdnError Servers(std::uint32_t cell_id, const std::atomic<bool>& cancel, std::vector<ContentServer>* out);
  CdnError Manifest(const ContentServer& server, std::uint32_t depot_id, std::uint64_t gid,
                    std::uint64_t request_code, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out);
  CdnError Chunk(const ContentServer& server, std::uint32_t depot_id, const std::string& sha_hex,
                 std::size_t max_bytes, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out);

  // Bounded by the HTTP client's response cap.
  static constexpr std::size_t kMaxManifestDownloadBytes = kMaxHttpResponseBytes;
  // The last failure as "http=<status>" or "transport=<category>", for sanitized logs.
  const CdnStats& stats() const { return stats_; }
  const std::string& last_failure() const { return last_failure_; }
  const std::string& ca_bundle() const { return ca_bundle_; }

 private:
  CdnError Get(const std::string& url, std::size_t max_bytes, std::uint32_t timeout_ms,
               const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out);
  std::string ca_bundle_;
  Perform perform_;
  std::unique_ptr<HttpClient> http_;
  std::string last_failure_;
  CdnStats stats_;
};

// A host or vhost from the directory may only contain DNS name characters.
bool ValidCdnHost(const std::string& host);

}  // namespace z3cloudplay::steam
