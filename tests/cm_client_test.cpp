#include "steam/backend/cm_client.h"
#include "steam/backend/proto_wire.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;
using namespace std::chrono_literals;

namespace {

std::string g_root;
constexpr const char* kSeed = "6F1C2B8A-3D4E-4F50-9A6B-7C8D9E0F1A2B";
constexpr std::uint64_t kAccountSteamId = 76561197960269970ull;  // account 4242

std::vector<std::uint8_t> Fixture(const std::string& name) {
  std::ifstream in(g_root + "/" + name, std::ios::binary);
  assert(in && "missing fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> ServerFrame(std::uint32_t emsg, const std::vector<std::uint8_t>& body,
                                      std::optional<std::int32_t> eresult = std::nullopt,
                                      std::optional<std::uint64_t> jobid_target = std::nullopt) {
  cm::Header header;
  header.steam_id = kAccountSteamId;
  header.session_id = 77;
  header.eresult = eresult;
  header.jobid_target = jobid_target;
  return cm::EncodeFrame(emsg, header, body);
}

std::vector<std::uint8_t> LogonResponseBody(std::int32_t eresult, std::int32_t heartbeat) {
  proto::Writer body;
  body.AddVarint(1, static_cast<std::uint32_t>(eresult));
  body.AddVarint(3, static_cast<std::uint32_t>(heartbeat));
  return body.bytes();
}

struct Script {
  std::mutex mutex;
  int failing_connects = 0;
  int connects = 0;
  bool closed = false;
  std::vector<cm::Frame> sent;
  std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<std::uint8_t>>> inbox;
  // What the fake server answers.
  std::vector<std::vector<std::uint8_t>> on_logon;
  std::vector<std::vector<std::uint8_t>> on_pics;
  std::vector<std::vector<std::uint8_t>> on_service;
  std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> on_emsg;
  std::vector<std::vector<std::uint8_t>> raw_sent;
  std::chrono::milliseconds pics_delay{0};

  void Queue(std::vector<std::uint8_t> frame, std::chrono::milliseconds delay = 0ms) {
    inbox.emplace_back(std::chrono::steady_clock::now() + delay, std::move(frame));
  }
};

class FakeTransport final : public MessageTransport {
 public:
  explicit FakeTransport(std::shared_ptr<Script> script) : script_(std::move(script)) {}
  HttpError Connect(const std::string& url, std::uint32_t, const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    assert(url.rfind("wss://", 0) == 0 && url.find("/cmsocket/") != std::string::npos);
    ++script_->connects;
    if (script_->failing_connects-- > 0) return HttpError::kConnectionFailed;
    return HttpError::kNone;
  }
  HttpError Send(const std::vector<std::uint8_t>& message, std::uint32_t,
                 const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    cm::Frame frame;
    assert(cm::DecodeFrame(message.data(), message.size(), &frame));
    if (frame.emsg == cm::kEMsgClientLogon)
      for (const auto& reply : script_->on_logon) script_->Queue(reply);
    if (frame.emsg == cm::kEMsgClientPICSProductInfoRequest)
      for (const auto& reply : script_->on_pics) script_->Queue(reply, script_->pics_delay);
    if (frame.emsg == cm::kEMsgServiceMethodCallFromClient)
      for (const auto& reply : script_->on_service) script_->Queue(reply);
    if (const auto found = script_->on_emsg.find(frame.emsg); found != script_->on_emsg.end())
      for (const auto& reply : found->second) script_->Queue(reply);
    script_->raw_sent.push_back(message);
    script_->sent.push_back(std::move(frame));
    return HttpError::kNone;
  }
  HttpError Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                    const std::atomic<bool>& cancel) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
      if (cancel) return HttpError::kCanceled;
      {
        std::lock_guard<std::mutex> lock(script_->mutex);
        if (!script_->inbox.empty() &&
            script_->inbox.front().first <= std::chrono::steady_clock::now()) {
          *message = std::move(script_->inbox.front().second);
          script_->inbox.pop_front();
          return HttpError::kNone;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) return HttpError::kTimeout;
      std::this_thread::sleep_for(1ms);
    }
  }
  void Close() override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    script_->closed = true;
  }

 private:
  std::shared_ptr<Script> script_;
};

HttpResponse Directory(const std::string& json) {
  HttpResponse response;
  response.status_code = 200;
  response.body.assign(json.begin(), json.end());
  return response;
}

constexpr const char* kTwoEndpoints =
    R"({"response":{"serverlist":[{"endpoint":"cm1.example.invalid:27022"},)"
    R"({"endpoint":"bad/endpoint?x"},{"endpoint":"cm2.example.invalid:443"}],"success":true}})";

CmClient MakeClient(std::shared_ptr<Script> script, std::uint32_t deadline_ms = 3000,
                    std::string directory = kTwoEndpoints) {
  CmClientHooks hooks;
  hooks.directory = [directory](const HttpRequest& request, const std::atomic<bool>&) {
    assert(request.url.find("ISteamDirectory/GetCMListForConnect") != std::string::npos);
    return Directory(directory);
  };
  hooks.transport = [script] { return std::make_unique<FakeTransport>(script); };
  hooks.unix_now = [] { return std::uint64_t{1700000000}; };
  hooks.deadline_ms = deadline_ms;
  return CmClient(hooks, MakeDeviceIdentity(kSeed));
}

StoredSession Session() {
  StoredSession session;
  session.steam_id = kAccountSteamId;
  session.account_name = "synthetic_user";
  session.refresh_token = "synthetic.refresh.token";
  return session;
}

std::shared_ptr<Script> HappyScript() {
  auto script = std::make_shared<Script>();
  script->on_logon = {Fixture("server_multi_logon.bin")};
  script->on_pics = {Fixture("server_pics_part1.bin"), Fixture("server_pics_part2.bin")};
  return script;
}

void AcquisitionDatesFromGzipMultiLicensesAndMultipartPics() {
  auto script = HappyScript();
  script->failing_connects = 1;  // the first endpoint fails; the next valid one is used
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.ok());
  // Package 100 (1600000000) holds apps 10, 20; package 200 (1500000000) holds 20, 30. Packages
  // 300 (expired), 400 (another owner) and 500 (minutes used up) are not this account's.
  assert(result.acquired_at.size() == 3);
  assert(result.acquired_at.at(10) == 1600000000u);
  assert(result.acquired_at.at(20) == 1500000000u);
  assert(result.acquired_at.at(30) == 1500000000u);

  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 2 && script->closed);
  assert(script->sent.size() == 3);  // closed without a log-off
  assert(script->sent[0].emsg == cm::kEMsgClientHello);
  assert(script->sent[0].header.steam_id == cm::kAnonymousSteamId);
  assert(script->sent[1].emsg == cm::kEMsgClientLogon);
  assert(script->sent[1].body == Fixture("logon_request.pb"));
  assert(script->sent[1].header.session_id == 0);
  // After logon the frame header carries the adopted session identity and the job ID.
  assert(script->sent[2].emsg == cm::kEMsgClientPICSProductInfoRequest);
  assert(script->sent[2].body == Fixture("pics_request.pb"));
  assert(script->sent[2].header.steam_id == kAccountSteamId);
  assert(script->sent[2].header.session_id == 77);
  assert(script->sent[2].header.jobid_source == 1u);
}

void RejectedLogonIsReportedWithoutDates() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse,
                                  LogonResponseBody(cm::kEResultInvalidPassword, 9))};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.error == CmError::kRejected && result.acquired_at.empty());
}

void FailedLicenseListIsRejected() {
  auto script = std::make_shared<Script>();
  proto::Writer licenses;
  licenses.AddVarint(1, cm::kEResultFail);
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)),
                      ServerFrame(cm::kEMsgClientLicenseList, licenses.bytes())};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
}

// One odd license must not block every install (issue #1): a package Steam leaves out or sends without its token is
// skipped, and only its apps stay unowned.
void PackagesSteamCannotDescribeAreSkipped() {
  std::atomic<bool> cancel{false};
  // Package 100 is left out; the reply holds package 200 only, not pending.
  auto left_out = HappyScript();
  left_out->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, Fixture("pics_response_part2.pb"), std::nullopt, 1)};
  auto client = MakeClient(left_out);
  const auto partial = client.FetchAcquisitionDates(Session(), cancel);
  assert(partial.ok() && partial.skipped_packages == 1);
  assert(partial.owned_apps == (std::set<std::uint32_t>{20, 30}));
  assert(partial.acquired_at.size() == 2 && partial.acquired_at.at(20) == 1500000000u);

  // Package 100 comes back marked as missing its token, with no buffer.
  auto tokenless = HappyScript();
  auto body = Fixture("pics_response_part2.pb");
  proto::Writer entry;
  entry.AddVarint(1, 100);
  entry.AddVarint(3, 1);
  proto::Writer missing;
  missing.AddMessage(3, entry);
  body.insert(body.end(), missing.bytes().begin(), missing.bytes().end());
  tokenless->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, body, std::nullopt, 1)};
  auto other = MakeClient(tokenless);
  const auto skipped = other.FetchAcquisitionDates(Session(), cancel);
  assert(skipped.ok() && skipped.skipped_packages == 1 && skipped.owned_apps == (std::set<std::uint32_t>{20, 30}));

  // A reply with none of the packages is still a protocol error.
  auto empty = HappyScript();
  empty->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, {}, std::nullopt, 1)};
  auto third = MakeClient(empty);
  const auto none = third.FetchAcquisitionDates(Session(), cancel);
  assert(none.error == CmError::kProtocol && none.owned_apps.empty());
}

void JobErrorResultFailsTheBatch() {
  auto script = HappyScript();
  script->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, {}, cm::kEResultFail, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
}

void SilentServerTimesOut() {
  auto script = std::make_shared<Script>();
  auto client = MakeClient(script, 150);
  std::atomic<bool> cancel{false};
  const auto start = std::chrono::steady_clock::now();
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kTimeout);
  assert(std::chrono::steady_clock::now() - start < 1s);
}

void CancelStopsWaiting() {
  auto script = std::make_shared<Script>();
  auto client = MakeClient(script, 5000);
  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kCanceled);
  assert(std::chrono::steady_clock::now() - start < 1s);
  canceler.join();
}

void HeartbeatIsSentOnTheServerInterval() {
  auto script = HappyScript();
  // Logon with a one-second heartbeat; the license push arrives only after it.
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 1))};
  auto client = MakeClient(script, 4000);
  std::thread pusher([script] {
    std::this_thread::sleep_for(1300ms);
    std::lock_guard<std::mutex> lock(script->mutex);
    auto licenses = Fixture("license_list.pb");
    script->Queue(ServerFrame(cm::kEMsgClientLicenseList, licenses));
  });
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  pusher.join();
  assert(result.ok());
  std::lock_guard<std::mutex> lock(script->mutex);
  bool heartbeat = false;
  for (const auto& frame : script->sent) heartbeat |= frame.emsg == cm::kEMsgClientHeartBeat;
  assert(heartbeat);
}

void MissingSessionOrEndpointsFailEarly() {
  auto script = HappyScript();
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  StoredSession empty;
  assert(client.FetchAcquisitionDates(empty, cancel).error == CmError::kNoSession);
  auto no_endpoints = MakeClient(script, 3000, R"({"response":{"serverlist":[]}})");
  assert(no_endpoints.FetchAcquisitionDates(Session(), cancel).error == CmError::kProtocol);
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 0);
}

void AccessTokenRenewsThroughAuthenticatedServiceMethod() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer reply;
  reply.AddString(1, "renewed.access.token");
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, reply.bytes(), std::nullopt, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto renewal = client.RenewAccessToken(Session(), cancel);
  assert(renewal.ok() && renewal.access_token == "renewed.access.token" && !renewal.refresh_token);
  std::lock_guard<std::mutex> lock(script->mutex);
  // The session closes after the call without a log-off, which would sign the device out.
  assert(script->closed && script->sent.back().emsg == cm::kEMsgServiceMethodCallFromClient);
  const auto& call = script->sent.back();
  assert(call.emsg == cm::kEMsgServiceMethodCallFromClient);
  assert(call.header.jobid_source == 1u && call.header.session_id == 77);
  const auto& raw_call = script->raw_sent.back();
  const std::string raw(raw_call.begin(), raw_call.end());
  assert(raw.find("Authentication.GenerateAccessTokenForApp#1") != std::string::npos);
  assert(raw.find("synthetic.refresh.token") != std::string::npos);
}

void RefusedServiceCallIsNotARefusedLogon() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, cm::kEResultAccessDenied, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.RenewAccessToken(Session(), cancel).error == CmError::kProtocol);
  auto refused = std::make_shared<Script>();
  refused->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(cm::kEResultInvalidPassword, 9))};
  auto refused_client = MakeClient(refused);
  assert(refused_client.RenewAccessToken(Session(), cancel).error == CmError::kRejected);
}

int CountSent(const std::shared_ptr<Script>& script, std::uint32_t emsg) {
  std::lock_guard<std::mutex> lock(script->mutex);
  return static_cast<int>(std::count_if(script->sent.begin(), script->sent.end(),
                                        [emsg](const cm::Frame& frame) { return frame.emsg == emsg; }));
}

CmError Ping(CmClient& client, const std::atomic<bool>& cancel) {
  return client.WithServiceSession(Session(), 3000, cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  });
}

void SharedSessionServesEveryClientWithOneLogon() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 1))};
  {
    auto shared = MakeCmSharedSession();
    auto cloud = MakeClient(script);
    auto details = MakeClient(script);
    cloud.SetSharedSession(shared);
    details.SetSharedSession(shared);
    std::atomic<bool> cancel{false};
    for (std::uint64_t job = 1; job <= 4; ++job) {
      {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, job)};
      }
      assert(Ping(job % 2 ? cloud : details, cancel) == CmError::kNone);
    }
    // Idle for longer than the server's heartbeat interval: the session is kept alive meanwhile.
    std::this_thread::sleep_for(1600ms);
    assert(CountSent(script, cm::kEMsgClientHeartBeat) >= 1);
    {
      std::lock_guard<std::mutex> lock(script->mutex);
      script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 5)};
    }
    assert(Ping(cloud, cancel) == CmError::kNone);
    assert(CountSent(script, cm::kEMsgClientLogon) == 1);
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1 && !script->closed);
  }
  // The last owner gone, the shared session closes without a log-off.
  assert(CountSent(script, cm::kEMsgClientLogOff) == 0);
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->closed);
}

void SharedSessionLogsOnAgainForNewCredentials() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto shared = MakeCmSharedSession();
  auto client = MakeClient(script);
  client.SetSharedSession(shared);
  std::atomic<bool> cancel{false};
  assert(Ping(client, cancel) == CmError::kNone);
  auto rotated = Session();
  rotated.refresh_token = "synthetic.rotated.token";
  const auto result = client.WithServiceSession(rotated, 3000, cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  });
  assert(result == CmError::kNone);
  // The old session closed before the new logon.
  assert(CountSent(script, cm::kEMsgClientLogon) == 2 && CountSent(script, cm::kEMsgClientLogOff) == 0);
}

void WithoutKeepAliveEachCallLogsOnAgain() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(Ping(client, cancel) == CmError::kNone);
  assert(Ping(client, cancel) == CmError::kNone);
  assert(CountSent(script, cm::kEMsgClientLogon) == 2 && CountSent(script, cm::kEMsgClientLogOff) == 0);
}

void RefusedTokenIsNotTriedAgain() {
  ResetCmBackoff();
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(cm::kEResultAccessDenied, 9))};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1);  // the second call never reached Steam
  }
  auto renewed = Session();
  renewed.refresh_token = "synthetic.new.token";
  assert(client.FetchAcquisitionDates(renewed, cancel).error == CmError::kRejected);  // tried: a new token
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 2);
  ResetCmBackoff();
}

void BusyLogonBacksOffEveryClient() {
  ResetCmBackoff();
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  SetCmLog([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  });
  auto script = std::make_shared<Script>();
  constexpr std::int32_t kRateLimitExceeded = 84;
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(kRateLimitExceeded, 9))};
  auto client = MakeClient(script);
  client.SetOwner("cloud");
  std::atomic<bool> cancel{false};
  // Steam being busy is not a refusal of the credentials.
  assert(client.RenewAccessToken(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  auto other = MakeClient(script);
  assert(other.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1);  // the second client didn't even connect
  }
  {
    std::lock_guard<std::mutex> lock(lines_mutex);
    assert(lines.size() == 2);
    assert(lines[0].rfind("owner=cloud step=logon error=network eresult=84 fp=", 0) == 0);
    assert(lines[1] == "owner=cm step=backoff error=network");
    for (const auto& line : lines) assert(line.find("synthetic") == std::string::npos);
  }
  SetCmLog(nullptr);
  ResetCmBackoff();
  assert(!CmBackoffActive());
}

void LogOffInsteadOfLogonResponseBacksOff() {
  ResetCmBackoff();
  auto script = std::make_shared<Script>();
  proto::Writer off;
  off.AddVarint(1, cm::kEResultTryAnotherCM);
  script->on_logon = {ServerFrame(cm::kEMsgClientLoggedOff, off.bytes())};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  ResetCmBackoff();
}

void DirectoryErrorBacksOff() {
  ResetCmBackoff();
  auto script = HappyScript();
  CmClientHooks hooks;
  hooks.directory = [](const HttpRequest&, const std::atomic<bool>&) {
    HttpResponse response;
    response.status_code = 429;
    return response;
  };
  hooks.transport = [script] { return std::make_unique<FakeTransport>(script); };
  hooks.deadline_ms = 3000;
  CmClient client(hooks, MakeDeviceIdentity(kSeed));
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  ResetCmBackoff();
}

void AppInfoUsesAccessTokensAndCollectsMultipartBuffers() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer token;
  token.AddVarint(1, 10);
  token.AddVarint(2, 555);
  proto::Writer tokens;
  tokens.AddMessage(3, token);
  script->on_emsg[cm::kEMsgClientPICSAccessTokenRequest] = {
      ServerFrame(cm::kEMsgClientPICSAccessTokenResponse, tokens.bytes(), std::nullopt, 1)};
  auto app_part = [](std::uint32_t id, const std::string& text, bool pending) {
    proto::Writer app;
    app.AddVarint(1, id);
    std::vector<std::uint8_t> buffer(text.begin(), text.end());
    buffer.push_back(0);
    buffer.push_back(0);
    app.AddBytes(5, buffer);
    proto::Writer response;
    response.AddMessage(1, app);
    if (pending) response.AddVarint(6, 1);
    return response.bytes();
  };
  script->on_emsg[cm::kEMsgClientPICSProductInfoRequest] = {
      ServerFrame(cm::kEMsgClientPICSProductInfoResponse, app_part(10, "\"appinfo\" { }", true), std::nullopt, 2),
      ServerFrame(cm::kEMsgClientPICSProductInfoResponse, app_part(20, "\"appinfo\" { \"x\" \"y\" }", false), std::nullopt, 2)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAppInfo(Session(), {10, 20, 30}, cancel);
  assert(result.ok() && result.buffers.size() == 2);
  assert(result.buffers.at(10) == "\"appinfo\" { }");  // trailing NULs removed
  assert(result.buffers.at(20).find("\"x\"") != std::string::npos);
  std::lock_guard<std::mutex> lock(script->mutex);
  const auto& request = script->sent.back();
  assert(request.emsg == cm::kEMsgClientPICSProductInfoRequest && request.header.jobid_source == 2u);
  // The granted token rides along with app 10 only.
  assert(request.body == cm::EncodePicsAppsRequest({{10, 555}, {20, 0}, {30, 0}}));
  assert(client.FetchAppInfo(Session(), {}, cancel).error == CmError::kProtocol);
}

std::shared_ptr<Script> DepotScript(std::int32_t key_result, std::uint64_t code) {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer key;
  key.AddVarint(1, static_cast<std::uint32_t>(key_result));
  key.AddVarint(2, 441);
  key.AddBytes(3, std::vector<std::uint8_t>(32, 0x5a));
  script->on_emsg[cm::kEMsgClientGetDepotDecryptionKey] = {
      ServerFrame(cm::kEMsgClientGetDepotDecryptionKeyResponse, key.bytes(), std::nullopt, 1)};
  proto::Writer request_code;
  request_code.AddVarint(1, code);
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, request_code.bytes(), std::nullopt, 2)};
  return script;
}

void DepotAccessReturnsKeysAndManifestCodes() {
  auto script = DepotScript(cm::kEResultOk, 987654321);
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto access = client.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel);
  assert(access.ok() && access.grants.size() == 1);
  assert(access.grants[0].depot_id == 441 && access.grants[0].manifest_gid == 11);
  assert(access.grants[0].key[0] == 0x5a && access.grants[0].manifest_request_code == 987654321u);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    const auto& last_call = script->raw_sent.back();
    const std::string raw(last_call.begin(), last_call.end());
    assert(raw.find("ContentServerDirectory.GetManifestRequestCode#1") != std::string::npos);
    assert(raw.find("public") != std::string::npos);
  }
  auto refused = MakeClient(DepotScript(cm::kEResultAccessDenied, 1));
  assert(refused.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel).error == CmError::kRejected);
  auto no_code = MakeClient(DepotScript(cm::kEResultOk, 0));
  assert(no_code.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel).error == CmError::kRejected);
}

void DeviceSeedPersistsPerInstall() {
  const auto root = std::filesystem::temp_directory_path() / "z3cloudplay-cm-seed-test";
  std::filesystem::remove_all(root);
  Storage storage(root.string());
  const auto first = LoadOrCreateDeviceSeed(storage);
  const auto second = LoadOrCreateDeviceSeed(storage);
  assert(first.size() == 32 && first == second);
  assert(MakeDeviceIdentity(first).machine_id == MakeDeviceIdentity(second).machine_id);
  assert(MakeDeviceIdentity(first).login_id != MakeDeviceIdentity(kSeed).login_id);
  std::filesystem::remove_all(root);
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  g_root = argv[1];
  // A token one test refused stays refused for the process, so every test starts clean.
  ResetCmBackoff();
  AcquisitionDatesFromGzipMultiLicensesAndMultipartPics();
  ResetCmBackoff();
  RejectedLogonIsReportedWithoutDates();
  ResetCmBackoff();
  FailedLicenseListIsRejected();
  ResetCmBackoff();
  PackagesSteamCannotDescribeAreSkipped();
  ResetCmBackoff();
  JobErrorResultFailsTheBatch();
  ResetCmBackoff();
  SilentServerTimesOut();
  ResetCmBackoff();
  CancelStopsWaiting();
  ResetCmBackoff();
  HeartbeatIsSentOnTheServerInterval();
  ResetCmBackoff();
  MissingSessionOrEndpointsFailEarly();
  ResetCmBackoff();
  AccessTokenRenewsThroughAuthenticatedServiceMethod();
  ResetCmBackoff();
  RefusedServiceCallIsNotARefusedLogon();
  ResetCmBackoff();
  AppInfoUsesAccessTokensAndCollectsMultipartBuffers();
  ResetCmBackoff();
  DepotAccessReturnsKeysAndManifestCodes();
  ResetCmBackoff();
  DeviceSeedPersistsPerInstall();
  ResetCmBackoff();
  SharedSessionServesEveryClientWithOneLogon();
  ResetCmBackoff();
  SharedSessionLogsOnAgainForNewCredentials();
  ResetCmBackoff();
  WithoutKeepAliveEachCallLogsOnAgain();
  ResetCmBackoff();
  RefusedTokenIsNotTriedAgain();
  ResetCmBackoff();
  BusyLogonBacksOffEveryClient();
  ResetCmBackoff();
  DirectoryErrorBacksOff();
  ResetCmBackoff();
  LogOffInsteadOfLogonResponseBacksOff();
}
