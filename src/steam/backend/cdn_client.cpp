#include "steam/backend/cdn_client.h"

#include "steam/backend/json.h"

#include <algorithm>

namespace z3cloudplay::steam {
namespace {

CdnError FromHttp(const HttpResponse& response) {
  if (response.error == HttpError::kCanceled) return CdnError::kCanceled;
  if (!response.ok()) return CdnError::kNetwork;
  if (response.status_code == 404) return CdnError::kNotFound;
  if (response.status_code == 429 || response.status_code >= 500) return CdnError::kNetwork;
  if (response.status_code != 200) return CdnError::kProtocol;
  return CdnError::kNone;
}

}  // namespace

bool ValidCdnHost(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  return std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
}

CdnClient::CdnClient(std::string ca_bundle) : ca_bundle_(std::move(ca_bundle)) {}
CdnClient::CdnClient(Perform perform) : perform_(std::move(perform)) {}
CdnClient::~CdnClient() = default;

CdnError CdnClient::Get(const std::string& url, std::size_t max_bytes, std::uint32_t timeout_ms,
                        const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out) {
  HttpRequest request;
  request.url = url;
  request.headers = {{"User-Agent", "Valve/Steam HTTP Client 1.0"}};
  request.timeout_ms = timeout_ms;
  request.max_response_bytes = max_bytes;
  HttpResponse response;
  if (perform_) {
    response = perform_(request, cancel);
  } else {
    if (ca_bundle_.empty()) return CdnError::kProtocol;
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(request, cancel);
  }
  const auto error = FromHttp(response);
  ++stats_.requests;
  if (error != CdnError::kNone) ++stats_.failures;
  stats_.resolve_ms += response.timing.resolve_ms;
  stats_.connect_ms += response.timing.connect_ms;
  stats_.tls_ms += response.timing.tls_ms;
  stats_.first_byte_ms += response.timing.first_byte_ms;
  stats_.total_ms += response.timing.total_ms;
  stats_.new_connections += response.timing.new_connections;
  stats_.bytes += response.timing.body_bytes;
  if (error == CdnError::kNone) {
    *out = std::move(response.body);
  } else {
    last_failure_ = response.ok() ? "http=" + std::to_string(response.status_code)
                                  : std::string("transport=") + HttpErrorName(response.error);
  }
  return error;
}

CdnError CdnClient::Servers(std::uint32_t cell_id, const std::atomic<bool>& cancel,
                            std::vector<ContentServer>* out) {
  std::vector<std::uint8_t> body;
  const auto error = Get(
      "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=" +
          std::to_string(cell_id) + "&format=json",
      512 * 1024, 15'000, cancel, &body);
  if (error != CdnError::kNone) return error;
  const std::string_view text(body.empty() ? "" : reinterpret_cast<const char*>(body.data()), body.size());
  const auto root = json::Parse(text, json::Limits{512 * 1024, 8, 8192, 4096});
  const auto* response = root ? root->Find("response") : nullptr;
  const auto* servers = response ? response->Find("servers") : nullptr;
  if (!servers || !servers->is_array()) return CdnError::kProtocol;
  std::vector<ContentServer> usable;
  for (const auto& server : servers->array()) {
    const auto* type = server.Find("type");
    const auto* host = server.Find("host");
    const auto* vhost = server.Find("vhost");
    const auto* https = server.Find("https_support");
    const auto* load = server.Find("load");
    if (!type || !type->is_string() || (type->text() != "SteamCache" && type->text() != "CDN")) continue;
    if (https && https->is_string() && https->text() == "unavailable") continue;
    if (!host || !host->is_string() || !ValidCdnHost(host->text())) continue;
    ContentServer entry;
    entry.host = host->text();
    entry.vhost = vhost && vhost->is_string() && ValidCdnHost(vhost->text()) ? vhost->text() : entry.host;
    if (load) {
      if (const auto value = load->AsUint64(); value && *value < 100000) entry.load = static_cast<int>(*value);
    }
    usable.push_back(std::move(entry));
  }
  if (usable.empty()) return CdnError::kProtocol;
  std::stable_sort(usable.begin(), usable.end(),
                   [](const ContentServer& left, const ContentServer& right) { return left.load < right.load; });
  *out = std::move(usable);
  return CdnError::kNone;
}

CdnError CdnClient::Manifest(const ContentServer& server, std::uint32_t depot_id, std::uint64_t gid,
                             std::uint64_t request_code, const std::atomic<bool>& cancel,
                             std::vector<std::uint8_t>* out) {
  if (!ValidCdnHost(server.vhost)) return CdnError::kProtocol;
  return Get("https://" + server.vhost + "/depot/" + std::to_string(depot_id) + "/manifest/" +
                 std::to_string(gid) + "/5/" + std::to_string(request_code),
             kMaxManifestDownloadBytes, 60'000, cancel, out);
}

CdnError CdnClient::Chunk(const ContentServer& server, std::uint32_t depot_id, const std::string& sha_hex,
                          std::size_t max_bytes, const std::atomic<bool>& cancel,
                          std::vector<std::uint8_t>* out) {
  if (!ValidCdnHost(server.vhost) || sha_hex.size() != 40 ||
      !std::all_of(sha_hex.begin(), sha_hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    return CdnError::kProtocol;
  return Get("https://" + server.vhost + "/depot/" + std::to_string(depot_id) + "/chunk/" + sha_hex,
             max_bytes, 30'000, cancel, out);
}

}  // namespace z3cloudplay::steam
