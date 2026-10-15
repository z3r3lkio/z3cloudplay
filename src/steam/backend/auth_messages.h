#pragma once

#include "steam/backend/proto_wire.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace z3cloudplay::steam::auth {

enum class PlatformType : std::int32_t {
  kUnknown = 0,
  kSteamClient = 1,
  kWebBrowser = 2,
  kMobileApp = 3,
};

enum class GuardType : std::int32_t {
  kUnknown = 0,
  kNone = 1,
  kEmailCode = 2,
  kDeviceCode = 3,
  kDeviceConfirmation = 4,
  kEmailConfirmation = 5,
  kMachineToken = 6,
  kLegacyMachineAuth = 7,
};

enum class TokenRenewalType : std::int32_t {
  kNone = 0,
  kAllow = 1,
};

struct DeviceDetails {
  std::string device_friendly_name;
  PlatformType platform_type = PlatformType::kSteamClient;
  std::int32_t os_type = 0;
};

struct BeginAuthSessionViaQrRequest {
  DeviceDetails device_details;
  std::string website_id = "Client";
};

struct AllowedConfirmation {
  GuardType confirmation_type = GuardType::kUnknown;
  std::string associated_message;
};

struct BeginAuthSessionViaQrResponse {
  std::uint64_t client_id = 0;
  std::string challenge_url;
  std::vector<std::uint8_t> request_id;
  float interval = 0;
  std::vector<AllowedConfirmation> allowed_confirmations;
  std::int32_t version = 0;
};

struct PollAuthSessionStatusRequest {
  std::uint64_t client_id = 0;
  std::vector<std::uint8_t> request_id;
  std::optional<std::uint64_t> token_to_revoke;
};

struct PollAuthSessionStatusResponse {
  std::optional<std::uint64_t> new_client_id;
  std::optional<std::string> new_challenge_url;
  std::string refresh_token;
  std::string access_token;
  bool had_remote_interaction = false;
  std::string account_name;
  std::optional<std::string> new_guard_data;
  std::string agreement_session_url;
};

struct GenerateAccessTokenForAppRequest {
  std::string refresh_token;
  std::uint64_t steam_id = 0;
  TokenRenewalType renewal_type = TokenRenewalType::kNone;
};

struct GenerateAccessTokenForAppResponse {
  std::string access_token;
  std::optional<std::string> refresh_token;
};

proto::Status Encode(const BeginAuthSessionViaQrRequest& message,
                     std::vector<std::uint8_t>* output);
proto::Status Decode(const std::uint8_t* data, std::size_t size,
                     BeginAuthSessionViaQrResponse* message,
                     proto::DecodeLimits limits = {});

proto::Status Encode(const PollAuthSessionStatusRequest& message,
                     std::vector<std::uint8_t>* output);
proto::Status Decode(const std::uint8_t* data, std::size_t size,
                     PollAuthSessionStatusResponse* message,
                     proto::DecodeLimits limits = {});

proto::Status Encode(const GenerateAccessTokenForAppRequest& message,
                     std::vector<std::uint8_t>* output);
proto::Status Decode(const std::uint8_t* data, std::size_t size,
                     GenerateAccessTokenForAppResponse* message,
                     proto::DecodeLimits limits = {});

template <typename Message>
proto::Status Decode(const std::vector<std::uint8_t>& bytes, Message* message,
                     proto::DecodeLimits limits = {}) {
  return Decode(bytes.data(), bytes.size(), message, limits);
}

}  // namespace z3cloudplay::steam::auth
