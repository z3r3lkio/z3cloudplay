#include "steam/backend/auth_client.h"
#include "steam/backend/proto_wire.h"
#include "steam/service.h"
#include <chrono>
#include <thread>

#include <cassert>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;
namespace {
std::string B64(std::string input) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned bits = 0, count = 0;
  for (unsigned char c : input) {
    bits = (bits << 8) | c; count += 8;
    while (count >= 6) { count -= 6; out += alphabet[(bits >> count) & 63]; }
  }
  if (count) out += alphabet[(bits << (6 - count)) & 63];
  return out;
}
std::string Token(std::string id = "76561198000000001", unsigned expiry = 2000000000) {
  return B64("{\"alg\":\"RS256\"}") + "." +
         B64("{\"sub\":\"" + id + "\",\"exp\":" + std::to_string(expiry) + "}") + "." + B64("synthetic-signature");
}
HttpResponse Reply(proto::Writer& w, int result = 1) {
  HttpResponse r; r.status_code = 200;
  r.headers.emplace_back("X-EResult", std::to_string(result)); r.body = w.bytes(); return r;
}
HttpResponse Begin(float interval = 1.5F, std::string url = "https://s.team/q/synthetic-one") {
  proto::Writer w; w.AddVarint(1, 123); w.AddString(2, url);
  w.AddString(3, "synthetic-request");
  std::uint32_t bits; std::memcpy(&bits, &interval, sizeof(bits)); w.AddFixed32(4, bits);
  return Reply(w);
}
HttpResponse Success(std::string refresh = Token(), std::string access = Token(), std::string name = "synthetic-account") {
  proto::Writer w; w.AddString(3, refresh); w.AddString(4, access); w.AddString(6, name);
  return Reply(w);
}
HttpResponse Renew(std::string access = Token(), std::string refresh = "") {
  proto::Writer w; w.AddString(1, access); if (!refresh.empty()) w.AddString(2, refresh);
  return Reply(w);
}
struct Fixture {
  std::uint64_t clock = 0;
  std::atomic<bool> cancel{false};
  std::deque<HttpResponse> replies;
  std::vector<HttpRequest> requests;
  std::vector<std::uint32_t> waits;
  std::vector<std::string> challenges;
  bool cancel_on_wait = false;
  std::uint32_t transport_ms = 0;
  bool cancel_on_response = false;
  AuthClientHooks Hooks() {
    AuthClientHooks h;
    h.perform = [this](const HttpRequest& req, const std::atomic<bool>&) {
      requests.push_back(req); assert(!replies.empty()); auto r = replies.front(); replies.pop_front();
      clock += transport_ms; if (cancel_on_response) cancel = true; return r;
    };
    h.monotonic_ms = [this] { return clock; };
    h.unix_seconds = [] { return std::uint64_t{1900000000}; };
    h.wait = [this](std::uint32_t ms, const std::atomic<bool>&) {
      waits.push_back(ms); clock += ms; if (cancel_on_wait) cancel = true; return !cancel;
    };
    return h;
  }
  ChallengeCallback Callback() { return [this](const std::string& s) { challenges.push_back(s); }; }
};
// The access token expires within the minute, so restoration must renew it.
StoredSession Stored() { return {76561198000000001ULL, "synthetic-account", Token(), Token("76561198000000001", 1900000030), 7}; }
void QrSuccessAndReplacement() {
  Fixture f; f.replies.push_back(Begin());
  proto::Writer w; w.AddVarint(1, 456); w.AddString(2, "https://s.team/q/synthetic-two");
  f.replies.push_back(Reply(w)); f.replies.push_back(Success());
  AuthClient client(f.Hooks()); auto r = client.AuthenticateQr(f.cancel, f.Callback());
  assert(r.error == AuthError::kNone && r.session && r.session->steam_id == Stored().steam_id);
  assert(f.challenges.size() == 2 && f.waits == std::vector<std::uint32_t>({1500, 1500}));
  assert(f.requests.size() == 3);
  const auto& req = f.requests[0];
  assert(req.url == "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaQR/v1/");
  assert(req.method == HttpMethod::kPost && req.timeout_ms == 10000 && req.max_response_bytes == 65536);
  assert(req.headers[0].second == "application/x-www-form-urlencoded");
  const std::string body(req.body.begin(), req.body.end());
  assert(body.rfind("input_protobuf_encoded=", 0) == 0 && body.find('+') == std::string::npos);
  // Byte-exact encoded poll requests pin both request bytes and the replaced client ID.
  assert(std::string(f.requests[1].body.begin(), f.requests[1].body.end()) ==
         "input_protobuf_encoded=CHsSEXN5bnRoZXRpYy1yZXF1ZXN0");
  assert(std::string(f.requests[2].body.begin(), f.requests[2].body.end()) ==
         "input_protobuf_encoded=CMgDEhFzeW50aGV0aWMtcmVxdWVzdA%3D%3D");
}
void FailuresAndDeadline() {
  {
    Fixture f; f.cancel = true; AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kCanceled && f.requests.empty());
  }
  {
    Fixture f; f.cancel_on_wait = true; f.replies.push_back(Begin()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kCanceled && f.requests.size() == 1);
  }
  for (float interval : {0.0F, -1.0F, INFINITY, NAN}) {
    Fixture f; f.replies.push_back(Begin(interval)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin(300.0F)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kExpired && f.requests.size() == 1);
  }
  for (int result : {9, 27, 15}) {
    Fixture f; f.replies.push_back(Begin()); proto::Writer w; f.replies.push_back(Reply(w,result)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == (result == 15 ? AuthError::kRejected : AuthError::kExpired));
  }
  {
    Fixture f; auto r = Begin(); r.error = HttpError::kTimeout; f.replies.push_back(r); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kNetwork && f.challenges.empty());
  }
  for (const std::string url : {"http://s.team/q/bad", "https://s.team.evil.invalid/q/bad", "https://s.team/q/bad\n"}) {
    Fixture f; f.replies.push_back(Begin(1.0F,url)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
}
void HeadersAndTokens() {
  for (int bad = 0; bad != 4; ++bad) {
    Fixture f; auto r = Begin();
    if (bad == 0) r.headers.emplace_back("x-eresult", "1");
    if (bad == 1) r.headers[0].second = "garbage";
    if (bad == 2) r.body = {0xff};
    if (bad == 3) r.body.resize(65537);
    f.replies.push_back(r); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol && f.challenges.empty());
  }
  for (const std::string& token : {std::string("synthetic.not-a-token.invalid"), Token("76561198000000002"), Token("76561198000000001", 1800000000)}) {
    Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(), token)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel, f.Callback()).error == AuthError::kProtocol);
  }
  {
    Fixture f; auto begin = Begin(); begin.headers.clear(); f.replies.push_back(begin); f.replies.push_back(Success(Token(), "")); f.replies.push_back(Renew());
    AuthClient c(f.Hooks()); auto r = c.AuthenticateQr(f.cancel, f.Callback());
    assert(r.error == AuthError::kNone && !r.session->access_token.empty() && f.requests.size() == 3);
  }
}
void FailedReplacementKeepsSavedSession() {
  class Repository : public SessionRepository {
   public:
    mutable std::mutex mutex;
    mutable std::optional<StoredSession> saved = Stored();
    mutable int clears = 0;
    Result<std::optional<StoredSession>> Load() const override {
      std::lock_guard<std::mutex> lock(mutex); return Result<std::optional<StoredSession>>::Success(saved);
    }
    Result<void> Save(const StoredSession& s) const override {
      std::lock_guard<std::mutex> lock(mutex); saved = s; return Result<void>::Success();
    }
    Result<void> Clear() const override {
      std::lock_guard<std::mutex> lock(mutex); ++clears; saved.reset(); return Result<void>::Success();
    }
  } repository;
  Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token("76561198000000002"),""));
  proto::Writer rejected; f.replies.push_back(Reply(rejected,15));
  AuthClient client(f.Hooks()); SteamService service(client,repository); service.BeginQrSignIn();
  bool failed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!failed && std::chrono::steady_clock::now() < deadline) {
    for (const auto& event : service.Pump(64)) if (event.kind == SteamEventKind::kFailure) {
      assert(event.error == AuthError::kRejected); failed = true;
    }
    if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(failed);
  const auto saved = repository.Load();
  assert(saved.ok() && saved.value() && saved.value()->steam_id == Stored().steam_id);
  std::lock_guard<std::mutex> lock(repository.mutex); assert(repository.clears == 0);
}
void AccountNames() {
  for (const std::string& name : {std::string(), std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                                 std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82"), std::string(257,'a')}) {
    Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(),Token(),name)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol);
  }
  Fixture f; f.replies.push_back(Begin()); f.replies.push_back(Success(Token(),Token(),"Synthetic \xc3\xa9")); AuthClient c(f.Hooks());
  assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
}
void DeadlineAndCancellationEdges() {
  {
    Fixture f; f.replies.push_back(Begin()); f.cancel_on_response = true; AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kCanceled && f.challenges.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,[&](const std::string&) { f.cancel = true; }).error == AuthError::kCanceled);
    assert(f.requests.size() == 1 && f.waits.empty());
  }
  {
    Fixture f; f.replies.push_back(Begin(299.0F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
    assert(f.requests[1].timeout_ms == 1000); // Never give a request more than the session's remaining lifetime.
  }
  {
    Fixture f; f.transport_ms = 1000; f.replies.push_back(Begin(298.0F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kExpired);
  }
  {
    Fixture f; f.replies.push_back(Begin(0.1F)); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone && f.waits[0] == 1000);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer bad;
    bad.AddVarint(1,0); f.replies.push_back(Reply(bad)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer bad;
    bad.AddString(2,"https://evil.invalid/"); f.replies.push_back(Reply(bad)); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kProtocol && f.challenges.size() == 1);
  }
  {
    Fixture f; f.replies.push_back(Begin()); proto::Writer rejected; f.replies.push_back(Reply(rejected,15));
    f.replies.push_back(Begin()); f.replies.push_back(Success()); AuthClient c(f.Hooks());
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kRejected);
    assert(c.AuthenticateQr(f.cancel,f.Callback()).error == AuthError::kNone);
    assert(f.requests[2].url.find("BeginAuthSessionViaQR") != std::string::npos);
  }
}
void Restoration() {
  {
    // As the reference client, an access token with more than a minute left is reused offline.
    Fixture f; auto saved = Stored(); saved.access_token = Token(); AuthClient c(f.Hooks());
    auto r = c.Restore(saved,f.cancel);
    assert(r.error == AuthError::kNone && r.session->access_token == saved.access_token && f.requests.empty());
  }
  {
    // The Web API refuses a SteamClient refresh token with AccessDenied: renew through the CM.
    Fixture f; proto::Writer w; f.replies.push_back(Reply(w,15)); int cm_calls = 0;
    auto hooks = f.Hooks();
    hooks.cm_renew = [&](const StoredSession& s, const std::atomic<bool>&) {
      ++cm_calls; auto renewed = s; renewed.access_token = Token(); return AuthResult{AuthError::kNone, renewed};
    };
    AuthClient c(hooks); auto r = c.Restore(Stored(),f.cancel);
    assert(r.error == AuthError::kNone && cm_calls == 1 && r.session->access_token == Token());
  }
  {
    // Some days the Web API answers AccountLogonDenied (63) instead: also renew through the CM (#51).
    Fixture f; proto::Writer w; f.replies.push_back(Reply(w,63)); int cm_calls = 0;
    auto hooks = f.Hooks();
    hooks.cm_renew = [&](const StoredSession& s, const std::atomic<bool>&) {
      ++cm_calls; auto renewed = s; renewed.access_token = Token(); return AuthResult{AuthError::kNone, renewed};
    };
    AuthClient c(hooks); auto r = c.Restore(Stored(),f.cancel);
    assert(r.error == AuthError::kNone && cm_calls == 1 && r.session->access_token == Token());
  }
  {
    // Any other refusal keeps its meaning; the CM is not consulted.
    Fixture f; proto::Writer w; f.replies.push_back(Reply(w,5)); int cm_calls = 0;
    auto hooks = f.Hooks();
    hooks.cm_renew = [&](const StoredSession&, const std::atomic<bool>&) { ++cm_calls; return AuthResult{}; };
    AuthClient c(hooks);
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kCredentialsRevoked && cm_calls == 0);
  }
  {
    Fixture f; const auto rotation = Token("76561198000000001",2100000000); f.replies.push_back(Renew(Token(), rotation));
    AuthClient c(f.Hooks()); auto r = c.Restore(Stored(),f.cancel);
    assert(r.error == AuthError::kNone && r.session->refresh_token == rotation && r.session->cell_id == 7);
    assert(f.requests[0].url.find("GenerateAccessTokenForApp") != std::string::npos);
  }
  {
    Fixture f; f.replies.push_back(Renew(Token(),Token("76561198000000002"))); AuthClient c(f.Hooks());
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kProtocol);
  }
  {
    Fixture f; f.replies.push_back(Renew()); f.cancel_on_response = true; AuthClient c(f.Hooks());
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kCanceled);
  }
  for (int eresult : {5,15,26,27}) {
    Fixture f; proto::Writer w; f.replies.push_back(Reply(w,eresult)); AuthClient c(f.Hooks());
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kCredentialsRevoked);
  }
  {
    Fixture f; auto saved = Stored(); saved.steam_id++; AuthClient c(f.Hooks());
    assert(c.Restore(saved,f.cancel).error == AuthError::kCredentialsRevoked && f.requests.empty());
  }
  {
    Fixture f; auto r = Renew(); r.status_code = 503; f.replies.push_back(r); AuthClient c(f.Hooks());
    assert(c.Restore(Stored(),f.cancel).error == AuthError::kNetwork);
  }
}
}
int main() {
  QrSuccessAndReplacement(); FailuresAndDeadline(); HeadersAndTokens(); DeadlineAndCancellationEdges(); Restoration(); AccountNames(); FailedReplacementKeepsSavedSession();
  std::cout << "auth_client_test passed\n";
}
