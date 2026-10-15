#include "steam/backend/auth_messages.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace auth = z3cloudplay::steam::auth;
namespace proto = z3cloudplay::steam::proto;

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

template <typename T>
void ExpectGoldenEncode(const std::string& path, const T& message) {
  std::vector<std::uint8_t> encoded;
  const auto status = auth::Encode(message, &encoded);
  assert(status.ok());
  assert(encoded == ReadFile(path));
}

void TestGoldenRequests(const std::string& root) {
  auth::BeginAuthSessionViaQrRequest begin;
  begin.device_details.device_friendly_name = "Z3CloudPlay Synthetic Device";
  begin.device_details.platform_type = auth::PlatformType::kSteamClient;
  begin.device_details.os_type = 0;
  begin.website_id = "Client";
  ExpectGoldenEncode(root + "/begin_request.pb", begin);

  auth::PollAuthSessionStatusRequest poll;
  poll.client_id = 0x0102030405060708ULL;
  poll.request_id = {0x10, 0x20, 0x30, 0x40, 0x50};
  ExpectGoldenEncode(root + "/poll_request.pb", poll);

  auth::GenerateAccessTokenForAppRequest renew;
  renew.refresh_token = "synthetic.refresh.token";
  renew.steam_id = 0x0110000100001234ULL;
  renew.renewal_type = auth::TokenRenewalType::kNone;
  ExpectGoldenEncode(root + "/renew_request.pb", renew);
}

void TestGoldenResponses(const std::string& root) {
  auth::BeginAuthSessionViaQrResponse begin;
  auto status = auth::Decode(ReadFile(root + "/begin_response.pb"), &begin);
  assert(status.ok());
  assert(begin.client_id == 0x0102030405060708ULL);
  assert(begin.challenge_url == "https://synthetic.invalid/qr/challenge-a");
  assert(begin.request_id == std::vector<std::uint8_t>({0x10, 0x20, 0x30, 0x40, 0x50}));
  assert(begin.interval == 2.5f);
  assert(begin.allowed_confirmations.size() == 2);
  assert(begin.allowed_confirmations[0].confirmation_type ==
         auth::GuardType::kDeviceConfirmation);
  assert(begin.allowed_confirmations[1].confirmation_type ==
         auth::GuardType::kEmailConfirmation);
  assert(begin.allowed_confirmations[1].associated_message == "example.invalid");
  assert(begin.version == 3);

  auth::PollAuthSessionStatusResponse pending;
  status = auth::Decode(ReadFile(root + "/poll_pending_response.pb"), &pending);
  assert(status.ok());
  assert(pending.new_client_id == 0x1112131415161718ULL);
  assert(pending.new_challenge_url ==
         "https://synthetic.invalid/qr/challenge-b");
  assert(pending.refresh_token.empty());
  assert(pending.had_remote_interaction);

  auth::PollAuthSessionStatusResponse success;
  status = auth::Decode(ReadFile(root + "/poll_success_response.pb"), &success);
  assert(status.ok());
  assert(success.refresh_token == "synthetic.refresh.token");
  assert(success.access_token == "synthetic.access.token");
  assert(success.account_name == "synthetic_account");
  assert(success.new_guard_data == "synthetic-guard-data");
  assert(success.agreement_session_url ==
         "https://synthetic.invalid/agreement");

  auth::GenerateAccessTokenForAppResponse renew;
  status = auth::Decode(ReadFile(root + "/renew_response.pb"), &renew);
  assert(status.ok());
  assert(renew.access_token == "synthetic.renewed.access.token");
  assert(renew.refresh_token == "synthetic.rotated.refresh.token");
}

void TestUnknownFieldsAndWrongWireTypes(const std::string& root) {
  auto bytes = ReadFile(root + "/poll_success_response.pb");
  bytes.insert(bytes.end(), {0x98, 0x06, 0x7b});  // unknown field 99
  auth::PollAuthSessionStatusResponse response;
  assert(auth::Decode(bytes, &response).ok());
  assert(response.account_name == "synthetic_account");

  const std::vector<std::uint8_t> wrong_wire = {0x09, 0, 0, 0, 0, 0, 0, 0, 0};
  auth::BeginAuthSessionViaQrResponse begin;
  const auto status = auth::Decode(wrong_wire, &begin);
  assert(status.error == proto::DecodeError::kWrongWireType);
}

void TestMessageBoundsPropagate() {
  proto::DecodeLimits limits;
  limits.max_message_bytes = 2;
  auth::PollAuthSessionStatusResponse response;
  const std::vector<std::uint8_t> bytes = {0x1a, 0x03, 'a', 'b', 'c'};
  const auto status = auth::Decode(bytes, &response, limits);
  assert(status.error == proto::DecodeError::kMessageTooLarge);
}

void TestDecodeFailureDoesNotPublishPartialSecrets() {
  auth::PollAuthSessionStatusResponse response;
  response.refresh_token = "keep-existing-value";
  const std::vector<std::uint8_t> truncated = {0x1a, 0x05, 'n', 'e', 'w'};
  const auto status = auth::Decode(truncated, &response);
  assert(status.error == proto::DecodeError::kTruncated);
  assert(response.refresh_token == "keep-existing-value");
}

void TestNestedDepthLimitIsEnforced(const std::string& root) {
  proto::DecodeLimits limits;
  limits.max_depth = 0;
  auth::BeginAuthSessionViaQrResponse response;
  const auto status =
      auth::Decode(ReadFile(root + "/begin_response.pb"), &response, limits);
  assert(status.error == proto::DecodeError::kDepthLimit);
}

void TestOptionalSchemaFieldsKeepPresence() {
  auth::PollAuthSessionStatusRequest request;
  request.client_id = 9;
  request.request_id = {1};
  request.token_to_revoke = 0x8877665544332211ULL;
  std::vector<std::uint8_t> encoded;
  assert(auth::Encode(request, &encoded).ok());
  proto::Reader reader(encoded);
  proto::Field field;
  assert(reader.Next(&field) && field.number == 1);
  assert(reader.Next(&field) && field.number == 2);
  assert(reader.Next(&field) && field.number == 3);
  assert(field.wire_type == proto::WireType::kFixed64);
  assert(field.value == 0x8877665544332211ULL);

  const std::vector<std::uint8_t> no_rotated_refresh = {0x0a, 0x01, 'a'};
  auth::GenerateAccessTokenForAppResponse response;
  assert(auth::Decode(no_rotated_refresh, &response).ok());
  assert(response.access_token == "a");
  assert(!response.refresh_token.has_value());
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  TestGoldenRequests(root);
  TestGoldenResponses(root);
  TestUnknownFieldsAndWrongWireTypes(root);
  TestMessageBoundsPropagate();
  TestDecodeFailureDoesNotPublishPartialSecrets();
  TestNestedDepthLimitIsEnforced(root);
  TestOptionalSchemaFieldsKeepPresence();
  std::puts("auth_messages_test passed");
  return 0;
}
