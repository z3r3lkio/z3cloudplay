#include "steam/backend/auth_client.h"
#include "steam/backend/auth_messages.h"
#include "steam/backend/token_fingerprint.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <string_view>
#include <thread>

namespace z3cloudplay::steam {
namespace {
constexpr std::uint64_t kLifetimeMs = 300000;
constexpr std::size_t kBodyLimit = 65536;
constexpr std::uint32_t kRequestTimeoutMs = 10000;

AuthResult Failure(AuthError e) { return {e, std::nullopt}; }
AuthClientHooks DefaultHooks() {
  AuthClientHooks h;
  h.monotonic_ms = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  h.unix_seconds = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
  };
  h.wait = [](std::uint32_t ms, const std::atomic<bool>& cancel) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cancel.load()) {
      const auto remaining = until - std::chrono::steady_clock::now();
      if (remaining <= decltype(remaining)::zero()) return true;
      std::this_thread::sleep_for(std::min(remaining,
          std::chrono::duration_cast<decltype(remaining)>(std::chrono::milliseconds(25))));
    }
    return false;
  };
  return h;
}
std::string FormBase64(const std::vector<std::uint8_t>& message) {
  const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  unsigned bits = 0, count = 0;
  for (auto byte : message) {
    bits = (bits << 8) | byte; count += 8;
    while (count >= 6) { count -= 6; b64 += alphabet[(bits >> count) & 63]; }
  }
  if (count) b64 += alphabet[(bits << (6 - count)) & 63];
  while (b64.size() % 4) b64 += '=';
  std::string encoded = "input_protobuf_encoded=";
  for (char c : b64) {
    if (c == '+') encoded += "%2B";
    else if (c == '/') encoded += "%2F";
    else if (c == '=') encoded += "%3D";
    else encoded += c;
  }
  return encoded;
}
bool ResultHeader(std::string name) {
  for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return name == "x-eresult";
}
bool ChallengeValid(const std::string& s) {
  constexpr std::string_view prefix = "https://s.team/q/";
  if (s.size() <= prefix.size() || s.size() > 4096 || s.compare(0, prefix.size(), prefix) != 0) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c > 32 && c < 127 && c != '#' && c != '\\'; });
}
bool AccountNameValid(const std::string& s) {
  if (s.empty() || s.size() > 256) return false;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 32 || c == 127) return false;
    if (c < 128) continue;
    unsigned count;
    unsigned char low = 0x80, high = 0xbf;
    if (c >= 0xc2 && c <= 0xdf) count = 1;
    else if (c >= 0xe0 && c <= 0xef) { count = 2; if (c == 0xe0) low = 0xa0; if (c == 0xed) high = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; if (c == 0xf0) low = 0x90; if (c == 0xf4) high = 0x8f; }
    else return false;
    if (count > s.size() - i) return false;
    for (unsigned part = 0; part < count; ++part) {
      const auto next = static_cast<unsigned char>(s[i++]);
      if (next < low || next > high) return false;
      low = 0x80; high = 0xbf;
    }
  }
  return true;
}
bool ValidToken(const std::string& token, std::uint64_t id, std::uint64_t now) {
  const auto claims = InspectTokenClaims(token);
  return claims && claims->steam_id == id && claims->expires_at_unix_seconds > now;
}
std::uint32_t Remaining(std::uint64_t start, const AuthClientHooks& hooks) {
  const auto elapsed = hooks.monotonic_ms() - start;
  return elapsed >= kLifetimeMs ? 0 : static_cast<std::uint32_t>(kLifetimeMs - elapsed);
}
}

AuthClient::AuthClient(std::string ca_bundle)
    : hooks_(DefaultHooks()), ca_bundle_(std::move(ca_bundle)) {}
AuthClient::AuthClient(AuthClientHooks hooks) : hooks_(std::move(hooks)) {}

AuthError AuthClient::Post(const char* method, const std::vector<std::uint8_t>& message,
                          const std::atomic<bool>& cancel, std::uint32_t timeout_ms,
                          bool restoring, std::vector<std::uint8_t>* body) {
  body->clear();
  if (cancel.load()) return AuthError::kCanceled;
  if (!timeout_ms) return AuthError::kExpired;
  if (message.size() > kBodyLimit) return AuthError::kProtocol;
  HttpRequest req;
  req.method = HttpMethod::kPost;
  req.url = std::string("https://api.steampowered.com/IAuthenticationService/") + method + "/v1/";
  req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", "Z3CloudPlay/1.0"}};
  req.timeout_ms = std::min(timeout_ms, kRequestTimeoutMs); req.max_response_bytes = kBodyLimit;
  const auto form = FormBase64(message); req.body.assign(form.begin(), form.end());
  HttpResponse response;
  if (hooks_.perform) response = hooks_.perform(req, cancel);
  else {
    if (ca_bundle_.empty()) return AuthError::kProtocol;
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(req, cancel);
  }
  if (cancel.load() || response.error == HttpError::kCanceled) return AuthError::kCanceled;
  last_eresult_ = 1;
  auto report = [&](AuthError error, int eresult) {
    last_eresult_ = eresult;
    if (hooks_.failure) hooks_.failure(method, response.status_code, eresult);
    return error;
  };
  if (!response.ok()) return report(AuthError::kNetwork, 0);
  if (response.status_code == 401 || response.status_code == 403)
    return report(restoring ? AuthError::kCredentialsRevoked : AuthError::kRejected, 0);
  if (response.status_code == 429 || response.status_code >= 500) return report(AuthError::kNetwork, 0);
  if (response.status_code != 200 || response.body.size() > kBodyLimit) return report(AuthError::kProtocol, 0);
  int result = 1; bool seen = false;
  for (const auto& header : response.headers) {
    if (!ResultHeader(header.first)) continue;
    if (seen) return AuthError::kProtocol;
    seen = true;
    std::string_view value = header.second;
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < 1)
      return AuthError::kProtocol;
  }
  // Match the reference's absent-header success policy; malformed/duplicate
  // headers fail closed instead of silently becoming success.
  if (result != 1) {
    if (restoring && (result == 5 || result == 15 || result == 26 || result == 27))
      return report(AuthError::kCredentialsRevoked, result);
    if (result == 9 || result == 27) return report(AuthError::kExpired, result);
    return report(AuthError::kRejected, result);
  }
  *body = std::move(response.body);
  return AuthError::kNone;
}

AuthResult AuthClient::AuthenticateQr(const std::atomic<bool>& cancel,
                                       const ChallengeCallback& challenge) {
  if (cancel.load()) return Failure(AuthError::kCanceled);
  if (!hooks_.monotonic_ms || !hooks_.unix_seconds || !hooks_.wait || !challenge)
    return Failure(AuthError::kProtocol);
  const auto started = hooks_.monotonic_ms();
  auth::BeginAuthSessionViaQrRequest begin;
  begin.device_details.device_friendly_name = "Z3CloudPlay (PS5)";
  std::vector<std::uint8_t> request, response;
  if (!auth::Encode(begin, &request).ok()) return Failure(AuthError::kProtocol);
  auto error = Post("BeginAuthSessionViaQR", request, cancel, Remaining(started,hooks_), false, &response);
  if (error != AuthError::kNone) return Failure(error);
  if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
  auth::BeginAuthSessionViaQrResponse session;
  if (!auth::Decode(response,&session).ok() || !session.client_id || session.request_id.empty() ||
      session.request_id.size() > 1024 || !ChallengeValid(session.challenge_url) ||
      !std::isfinite(session.interval) || session.interval <= 0) return Failure(AuthError::kProtocol);
  const auto interval = static_cast<std::uint32_t>(std::min<double>(kLifetimeMs,
      std::ceil(std::max<double>(session.interval,1.0) * 1000.0)));
  challenge(session.challenge_url);
  while (!cancel.load()) {
    const auto remaining = Remaining(started,hooks_);
    if (!remaining) return Failure(AuthError::kExpired);
    if (!hooks_.wait(std::min(interval, remaining),cancel) || cancel.load()) return Failure(AuthError::kCanceled);
    if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
    auth::PollAuthSessionStatusRequest poll;
    poll.client_id = session.client_id; poll.request_id = session.request_id;
    if (!auth::Encode(poll,&request).ok()) return Failure(AuthError::kProtocol);
    error = Post("PollAuthSessionStatus",request,cancel,Remaining(started,hooks_),false,&response);
    if (error != AuthError::kNone) return Failure(error);
    if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
    auth::PollAuthSessionStatusResponse approval;
    if (!auth::Decode(response,&approval).ok()) return Failure(AuthError::kProtocol);
    if (approval.new_client_id) {
      if (!*approval.new_client_id) return Failure(AuthError::kProtocol);
      session.client_id = *approval.new_client_id;
    }
    if (approval.new_challenge_url) {
      if (!ChallengeValid(*approval.new_challenge_url)) return Failure(AuthError::kProtocol);
      challenge(*approval.new_challenge_url);
    }
    if (cancel.load()) return Failure(AuthError::kCanceled);
    if (approval.refresh_token.empty()) continue;
    const auto claims = InspectTokenClaims(approval.refresh_token);
    const auto now = hooks_.unix_seconds();
    if (!claims || claims->expires_at_unix_seconds <= now || !AccountNameValid(approval.account_name))
      return Failure(AuthError::kProtocol);
    StoredSession stored{claims->steam_id, approval.account_name, std::move(approval.refresh_token),
                         std::move(approval.access_token), std::nullopt};
    if (stored.access_token.empty()) {
      auto renewed = Renew(stored,cancel,Remaining(started,hooks_),false);
      if (cancel.load()) return Failure(AuthError::kCanceled);
      if (!Remaining(started,hooks_)) return Failure(AuthError::kExpired);
      return renewed;
    }
    if (!ValidToken(stored.access_token,stored.steam_id,now)) return Failure(AuthError::kProtocol);
    return {AuthError::kNone,std::move(stored)};
  }
  return Failure(AuthError::kCanceled);
}

AuthResult AuthClient::Renew(const StoredSession& session, const std::atomic<bool>& cancel,
                            std::uint32_t timeout_ms, bool restoring) {
  auth::GenerateAccessTokenForAppRequest req;
  req.refresh_token = session.refresh_token; req.steam_id = session.steam_id;
  std::vector<std::uint8_t> request, response;
  if (!auth::Encode(req,&request).ok()) return Failure(AuthError::kProtocol);
  const auto error = Post("GenerateAccessTokenForApp",request,cancel,timeout_ms,restoring,&response);
  if (error != AuthError::kNone) return Failure(error);
  auth::GenerateAccessTokenForAppResponse renewed;
  if (!auth::Decode(response,&renewed).ok()) return Failure(AuthError::kProtocol);
  const auto now = hooks_.unix_seconds();
  if (!ValidToken(renewed.access_token,session.steam_id,now) ||
      (renewed.refresh_token && !ValidToken(*renewed.refresh_token,session.steam_id,now))) return Failure(AuthError::kProtocol);
  auto updated = session;
  updated.access_token = std::move(renewed.access_token);
  if (renewed.refresh_token) updated.refresh_token = std::move(*renewed.refresh_token);
  return {AuthError::kNone,std::move(updated)};
}

AuthResult AuthClient::Restored(const char* path, const StoredSession& before, AuthResult result) {
  if (result.error == AuthError::kNone && result.session && hooks_.restored)
    hooks_.restored(path, TokenFingerprint(before.refresh_token), TokenFingerprint(result.session->refresh_token));
  return result;
}

AuthResult AuthClient::Restore(const StoredSession& session,const std::atomic<bool>& cancel) {
  if (cancel.load()) return Failure(AuthError::kCanceled);
  if (!hooks_.unix_seconds || !AccountNameValid(session.account_name)) return Failure(AuthError::kProtocol);
  const auto now = hooks_.unix_seconds();
  if (!session.steam_id || !ValidToken(session.refresh_token,session.steam_id,now))
    return Failure(AuthError::kCredentialsRevoked);
  // As the reference client: a stored access token with more than a minute left is used as is.
  if (const auto claims = InspectTokenClaims(session.access_token);
      claims && claims->steam_id == session.steam_id && claims->expires_at_unix_seconds > now + 60)
    return Restored("stored", session, {AuthError::kNone, session});
  auto renewed = Renew(session,cancel,kRequestTimeoutMs,true);
  // The Web API refuses SteamClient refresh tokens with AccessDenied (15), and some days with
  // AccountLogonDenied (63), although the CM accepts them; only a refused CM logon then means the
  // credentials are gone.
  const bool web_refused = (renewed.error == AuthError::kCredentialsRevoked && last_eresult_ == 15) ||
                           (renewed.error == AuthError::kRejected && last_eresult_ == 63);
  if (web_refused && hooks_.cm_renew && !cancel.load()) {
    // Its outcome, with Steam's result code, is in steam-cm.log (owner=auth).
    return Restored("cm", session, hooks_.cm_renew(session, cancel));
  }
  return Restored("web", session, std::move(renewed));
}
}  // namespace z3cloudplay::steam
