#include "steam/backend/cdn_client.h"

#include <cassert>
#include <string>
#include <vector>

using namespace z3cloudplay::steam;

namespace {

HttpResponse Body(long status, const std::string& text) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(text.begin(), text.end());
  return response;
}

void TotalsIncludeFailedRequests() {
  int calls = 0;
  CdnClient client([&](const HttpRequest&, const std::atomic<bool>&) {
    auto response = Body(++calls == 1 ? 200 : 503, "payload");
    response.timing = {1, 2, 3, 4, 5, 1, 7};
    return response;
  });
  std::atomic<bool> cancel{false};
  std::vector<std::uint8_t> out;
  ContentServer server{"h.example.invalid", "v.example.invalid", 1};
  assert(client.Chunk(server, 1, std::string(40, 'a'), 1024, cancel, &out) == CdnError::kNone);
  assert(client.Chunk(server, 1, std::string(40, 'a'), 1024, cancel, &out) == CdnError::kNetwork);
  const auto& stats = client.stats();
  assert(stats.requests == 2 && stats.failures == 1 && stats.bytes == 14);
  assert(stats.resolve_ms == 2 && stats.connect_ms == 4 && stats.tls_ms == 6);
  assert(stats.first_byte_ms == 8 && stats.total_ms == 10 && stats.new_connections == 2);
}

void ServersFilterAndSortByLoad() {
  std::vector<std::string> urls;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    urls.push_back(request.url);
    return Body(200, R"({"response":{"servers":[
      {"type":"CDN","host":"b.example.invalid","vhost":"vb.example.invalid","load":50,"https_support":"mandatory"},
      {"type":"SteamCache","host":"a.example.invalid","load":10,"https_support":"optional"},
      {"type":"CDN","host":"c.example.invalid","load":1,"https_support":"unavailable"},
      {"type":"OpenCache","host":"d.example.invalid","load":0},
      {"type":"CDN","host":"bad host/x","load":0}]}})");
  });
  std::atomic<bool> cancel{false};
  std::vector<ContentServer> servers;
  assert(client.Servers(42, cancel, &servers) == CdnError::kNone);
  assert(servers.size() == 2);
  assert(servers[0].host == "a.example.invalid" && servers[0].vhost == "a.example.invalid");
  assert(servers[1].vhost == "vb.example.invalid");
  assert(urls[0].find("GetServersForSteamPipe/v1/?cell_id=42") != std::string::npos);
}

void ManifestAndChunkUrlsAndErrors() {
  std::vector<std::string> urls;
  long status = 200;
  CdnClient client([&](const HttpRequest& request, const std::atomic<bool>&) {
    urls.push_back(request.url);
    return Body(status, "payload");
  });
  std::atomic<bool> cancel{false};
  const ContentServer server{"h.example.invalid", "v.example.invalid", 1};
  std::vector<std::uint8_t> out;
  assert(client.Manifest(server, 441, 11, 99, cancel, &out) == CdnError::kNone && out.size() == 7);
  assert(urls.back() == "https://v.example.invalid/depot/441/manifest/11/5/99");
  const std::string sha(40, 'a');
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNone);
  assert(urls.back() == "https://v.example.invalid/depot/441/chunk/" + sha);
  assert(client.Chunk(server, 441, "../x", 1024, cancel, &out) == CdnError::kProtocol);
  status = 404;
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNotFound);
  status = 503;
  assert(client.Chunk(server, 441, sha, 1024, cancel, &out) == CdnError::kNetwork);
  assert(client.Manifest({"h", "bad host", 1}, 1, 1, 1, cancel, &out) == CdnError::kProtocol);
}

}  // namespace

int main() {
  TotalsIncludeFailedRequests();
  ServersFilterAndSortByLoad();
  ManifestAndChunkUrlsAndErrors();
}
