#include "steam/backend/auth_messages.h"

#include <cstring>
#include <utility>

namespace z3cloudplay::steam::auth {
namespace {

using proto::DecodeError;
using proto::Field;
using proto::Reader;
using proto::Status;
using proto::WireType;
using proto::Writer;

Status WrongWire(const Field& field) {
  return {DecodeError::kWrongWireType, field.offset};
}

bool HasWire(const Field& field, WireType expected) {
  return field.wire_type == expected;
}

Status WriterStatus(const Writer& writer, std::vector<std::uint8_t>* output) {
  if (!writer.ok()) return writer.status();
  if (output == nullptr) return {DecodeError::kInvalidArgument, 0};
  *output = writer.bytes();
  return {};
}

bool EncodeDeviceDetails(const DeviceDetails& message, Writer* writer) {
  return writer->AddString(1, message.device_friendly_name) &&
         writer->AddVarint(2, static_cast<std::uint64_t>(message.platform_type)) &&
         writer->AddVarint(
             3, static_cast<std::uint64_t>(static_cast<std::int64_t>(message.os_type)));
}

Status DecodeAllowedConfirmation(Reader* reader,
                                 AllowedConfirmation* confirmation) {
  Field field;
  while (reader->Next(&field)) {
    switch (field.number) {
      case 1:
        if (!HasWire(field, WireType::kVarint)) return WrongWire(field);
        confirmation->confirmation_type = static_cast<GuardType>(field.value);
        break;
      case 2:
        if (!HasWire(field, WireType::kLengthDelimited))
          return WrongWire(field);
        confirmation->associated_message = field.AsString();
        break;
      default:
        break;
    }
  }
  return reader->status();
}

}  // namespace

Status Encode(const BeginAuthSessionViaQrRequest& message,
              std::vector<std::uint8_t>* output) {
  Writer device;
  EncodeDeviceDetails(message.device_details, &device);
  Writer writer;
  writer.AddMessage(3, device);
  writer.AddString(4, message.website_id);
  return WriterStatus(writer, output);
}

Status Decode(const std::uint8_t* data, std::size_t size,
              BeginAuthSessionViaQrResponse* message,
              proto::DecodeLimits limits) {
  if (message == nullptr) return {DecodeError::kInvalidArgument, 0};
  BeginAuthSessionViaQrResponse decoded;
  Reader reader(data, size, limits);
  Field field;
  while (reader.Next(&field)) {
    switch (field.number) {
      case 1:
        if (!HasWire(field, WireType::kVarint)) return WrongWire(field);
        decoded.client_id = field.value;
        break;
      case 2:
        if (!HasWire(field, WireType::kLengthDelimited))
          return WrongWire(field);
        decoded.challenge_url = field.AsString();
        break;
      case 3:
        if (!HasWire(field, WireType::kLengthDelimited))
          return WrongWire(field);
        decoded.request_id.assign(field.data, field.data + field.size);
        break;
      case 4: {
        if (!HasWire(field, WireType::kFixed32)) return WrongWire(field);
        const std::uint32_t bits = static_cast<std::uint32_t>(field.value);
        std::memcpy(&decoded.interval, &bits, sizeof(bits));
        break;
      }
      case 5: {
        if (!HasWire(field, WireType::kLengthDelimited))
          return WrongWire(field);
        Reader child;
        if (!reader.Submessage(field, &child)) return reader.status();
        AllowedConfirmation confirmation;
        const Status status = DecodeAllowedConfirmation(&child, &confirmation);
        if (!status.ok()) return status;
        decoded.allowed_confirmations.push_back(std::move(confirmation));
        break;
      }
      case 6:
        if (!HasWire(field, WireType::kVarint)) return WrongWire(field);
        decoded.version = static_cast<std::int32_t>(field.value);
        break;
      default:
        break;
    }
  }
  if (!reader.ok()) return reader.status();
  *message = std::move(decoded);
  return {};
}

Status Encode(const PollAuthSessionStatusRequest& message,
              std::vector<std::uint8_t>* output) {
  Writer writer;
  writer.AddVarint(1, message.client_id);
  writer.AddBytes(2, message.request_id);
  if (message.token_to_revoke)
    writer.AddFixed64(3, *message.token_to_revoke);
  return WriterStatus(writer, output);
}

Status Decode(const std::uint8_t* data, std::size_t size,
              PollAuthSessionStatusResponse* message,
              proto::DecodeLimits limits) {
  if (message == nullptr) return {DecodeError::kInvalidArgument, 0};
  PollAuthSessionStatusResponse decoded;
  Reader reader(data, size, limits);
  Field field;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!HasWire(field, WireType::kVarint)) return WrongWire(field);
      decoded.new_client_id = field.value;
    } else if (field.number >= 2 && field.number <= 4) {
      if (!HasWire(field, WireType::kLengthDelimited))
        return WrongWire(field);
      const std::string value = field.AsString();
      if (field.number == 2)
        decoded.new_challenge_url = value;
      else if (field.number == 3)
        decoded.refresh_token = value;
      else
        decoded.access_token = value;
    } else if (field.number == 5) {
      if (!HasWire(field, WireType::kVarint)) return WrongWire(field);
      decoded.had_remote_interaction = field.value != 0;
    } else if (field.number >= 6 && field.number <= 8) {
      if (!HasWire(field, WireType::kLengthDelimited))
        return WrongWire(field);
      const std::string value = field.AsString();
      if (field.number == 6)
        decoded.account_name = value;
      else if (field.number == 7)
        decoded.new_guard_data = value;
      else
        decoded.agreement_session_url = value;
    }
  }
  if (!reader.ok()) return reader.status();
  *message = std::move(decoded);
  return {};
}

Status Encode(const GenerateAccessTokenForAppRequest& message,
              std::vector<std::uint8_t>* output) {
  Writer writer;
  writer.AddString(1, message.refresh_token);
  writer.AddFixed64(2, message.steam_id);
  writer.AddVarint(3, static_cast<std::uint64_t>(message.renewal_type));
  return WriterStatus(writer, output);
}

Status Decode(const std::uint8_t* data, std::size_t size,
              GenerateAccessTokenForAppResponse* message,
              proto::DecodeLimits limits) {
  if (message == nullptr) return {DecodeError::kInvalidArgument, 0};
  GenerateAccessTokenForAppResponse decoded;
  Reader reader(data, size, limits);
  Field field;
  while (reader.Next(&field)) {
    if (field.number == 1 || field.number == 2) {
      if (!HasWire(field, WireType::kLengthDelimited))
        return WrongWire(field);
      if (field.number == 1)
        decoded.access_token = field.AsString();
      else
        decoded.refresh_token = field.AsString();
    }
  }
  if (!reader.ok()) return reader.status();
  *message = std::move(decoded);
  return {};
}

}  // namespace z3cloudplay::steam::auth
