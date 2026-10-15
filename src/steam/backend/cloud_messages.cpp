#include "steam/backend/cloud_messages.h"

#include "steam/backend/proto_wire.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace z3cloudplay::steam::cloud {
namespace {

constexpr proto::DecodeLimits kListLimits{16 * 1024 * 1024, 16 * 1024 * 1024, kMaxRemoteFiles * 2 + 64, 8};
constexpr proto::DecodeLimits kLimits{1024 * 1024, 1024 * 1024, 4096, 8};
// Year 10000: Steam timestamps past this are malformed.
constexpr std::uint64_t kMaxTimeStamp = 253'402'300'800ULL;

bool IsVarint(const proto::Field& field) { return field.wire_type == proto::WireType::kVarint; }
bool IsBytes(const proto::Field& field) { return field.wire_type == proto::WireType::kLengthDelimited; }

std::vector<std::uint8_t> Bytes(const proto::Field& field) {
  return std::vector<std::uint8_t>(field.data, field.data + field.size);
}

bool DecodeHeader(proto::Reader& reader, std::pair<std::string, std::string>* header) {
  proto::Field field;
  while (reader.Next(&field)) {
    if (!IsBytes(field)) continue;
    if (field.number == 1) header->first = field.AsString();
    else if (field.number == 2) header->second = field.AsString();
  }
  return reader.ok() && !header->first.empty();
}

std::string Lower(std::string text) {
  for (auto& value : text) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return text;
}

}  // namespace

std::vector<std::uint8_t> EncodeChangelistRequest(std::uint32_t app_id, std::uint64_t synced_change_number) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddVarint(2, synced_change_number);
  return body.bytes();
}

std::optional<std::uint64_t> DecodeChangelistChangeNumber(const std::vector<std::uint8_t>& body) {
  proto::Reader reader(body, kListLimits);
  proto::Field field;
  std::optional<std::uint64_t> change_number;
  while (reader.Next(&field))
    if (field.number == 1 && IsVarint(field)) change_number = field.value;
  if (!reader.ok()) return std::nullopt;
  return change_number;
}

std::optional<RemoteFileList> DecodeChangelistResponse(const std::vector<std::uint8_t>& body) {
  struct Raw {
    RemoteFile file;
    std::optional<std::uint64_t> prefix_index;
    std::optional<std::uint64_t> persist_state;
    bool has_raw_size = false;
  };
  proto::Reader reader(body, kListLimits);
  proto::Field field;
  std::optional<std::uint64_t> change_number;
  bool only_delta = false;
  std::vector<std::string> prefixes;
  std::vector<Raw> raws;
  while (reader.Next(&field)) {
    if (field.number == 1 && IsVarint(field)) change_number = field.value;
    else if (field.number == 3 && IsVarint(field)) only_delta = field.value != 0;
    else if (field.number == 4 && IsBytes(field)) prefixes.push_back(field.AsString());
    else if (field.number == 2 && IsBytes(field)) {
      if (raws.size() >= kMaxRemoteFiles) return std::nullopt;
      proto::Reader child;
      if (!reader.Submessage(field, &child)) return std::nullopt;
      Raw raw;
      proto::Field item;
      while (child.Next(&item)) {
        switch (item.number) {
          case 1: if (IsBytes(item)) raw.file.name = item.AsString(); break;
          case 2: if (IsBytes(item)) raw.file.sha1 = Bytes(item); break;
          case 3: if (IsVarint(item)) raw.file.time_stamp = item.value; break;
          case 4: if (IsVarint(item)) { raw.file.raw_size = item.value; raw.has_raw_size = true; } break;
          case 5: if (IsVarint(item)) raw.persist_state = item.value; break;
          case 7: if (IsVarint(item)) raw.prefix_index = item.value; break;
          case 9: if (IsVarint(item)) raw.file.reupload_requested = item.value != 0; break;
          default: break;
        }
      }
      if (!child.ok()) return std::nullopt;
      raws.push_back(std::move(raw));
    }
  }
  if (!reader.ok() || !change_number || only_delta) return std::nullopt;

  RemoteFileList list;
  list.change_number = *change_number;
  std::set<std::string> names;
  for (auto& raw : raws) {
    auto& file = raw.file;
    if (raw.prefix_index) {
      if (*raw.prefix_index >= prefixes.size()) return std::nullopt;
      const auto& prefix = prefixes[*raw.prefix_index];
      if (!prefix.empty()) file.name = prefix + (prefix.back() == '/' ? "" : "/") + file.name;
    }
    if (file.name.empty() || file.name.size() > 4096 || file.name.find('\0') != std::string::npos ||
        !names.insert(Lower(file.name)).second || file.time_stamp >= kMaxTimeStamp)
      return std::nullopt;
    switch (raw.persist_state.value_or(0)) {
      case 0: file.state = RemoteState::kPresent; break;
      case 1: file.state = RemoteState::kForgotten; break;
      case 2: file.state = RemoteState::kDeleted; break;
      default: return std::nullopt;
    }
    if (file.state == RemoteState::kPresent &&
        (!raw.has_raw_size || file.sha1.size() != 20 || file.raw_size > kMaxRemoteFileBytes))
      return std::nullopt;
    list.files.push_back(std::move(file));
  }
  return list;
}

std::vector<std::uint8_t> EncodeFileDownloadRequest(std::uint32_t app_id, const std::string& name) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddString(2, name);
  return body.bytes();
}

std::optional<DownloadTicket> DecodeFileDownloadResponse(const std::vector<std::uint8_t>& body) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  DownloadTicket ticket;
  while (reader.Next(&field)) {
    switch (field.number) {
      case 2: if (IsVarint(field)) ticket.file_size = field.value; break;
      case 3: if (IsVarint(field)) ticket.raw_file_size = field.value; break;
      case 4: if (IsBytes(field)) ticket.sha1 = Bytes(field); break;
      case 5: if (IsVarint(field)) ticket.time_stamp = field.value; break;
      case 6: if (IsVarint(field)) ticket.explicit_delete = field.value != 0; break;
      case 7: if (IsBytes(field)) ticket.host = field.AsString(); break;
      case 8: if (IsBytes(field)) ticket.path = field.AsString(); break;
      case 9: if (IsVarint(field)) ticket.use_https = field.value != 0; break;
      case 10:
        if (IsBytes(field)) {
          proto::Reader child;
          std::pair<std::string, std::string> header;
          if (!reader.Submessage(field, &child) || !DecodeHeader(child, &header)) return std::nullopt;
          ticket.headers.push_back(std::move(header));
        }
        break;
      case 11: if (IsVarint(field)) ticket.encrypted = field.value != 0; break;
      default: break;
    }
  }
  if (!reader.ok()) return std::nullopt;
  return ticket;
}

std::vector<std::uint8_t> EncodeBeginBatchRequest(std::uint32_t app_id, const std::string& machine_name,
                                                  const std::vector<std::string>& uploads,
                                                  const std::vector<std::string>& deletes,
                                                  std::uint64_t client_id, std::uint64_t app_build_id) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddString(2, machine_name);
  for (const auto& name : uploads) body.AddString(3, name);
  for (const auto& name : deletes) body.AddString(4, name);
  body.AddVarint(5, client_id);
  body.AddVarint(6, app_build_id);
  return body.bytes();
}

std::optional<BatchStart> DecodeBeginBatchResponse(const std::vector<std::uint8_t>& body) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  BatchStart start;
  bool has_batch = false;
  while (reader.Next(&field)) {
    if (field.number == 1 && IsVarint(field)) { start.batch_id = field.value; has_batch = true; }
    else if (field.number == 4 && IsVarint(field)) start.app_change_number = field.value;
  }
  if (!reader.ok() || !has_batch) return std::nullopt;
  return start;
}

std::vector<std::uint8_t> EncodeBeginFileUploadRequest(std::uint32_t app_id, std::uint64_t raw_size,
                                                       const std::vector<std::uint8_t>& sha1, std::uint64_t time_stamp,
                                                       const std::string& name, std::uint64_t batch_id) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddVarint(2, raw_size);  // uploaded uncompressed
  body.AddVarint(3, raw_size);
  body.AddBytes(4, sha1);
  body.AddVarint(5, time_stamp);
  body.AddString(6, name);
  body.AddVarint(10, 0);  // can_encrypt: no
  body.AddVarint(13, batch_id);
  return body.bytes();
}

std::optional<UploadPlan> DecodeBeginFileUploadResponse(const std::vector<std::uint8_t>& body) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  UploadPlan plan;
  while (reader.Next(&field)) {
    if (field.number == 1 && IsVarint(field)) plan.encrypt_file = field.value != 0;
    else if (field.number == 2 && IsBytes(field)) {
      if (plan.blocks.size() >= 512) return std::nullopt;
      proto::Reader child;
      if (!reader.Submessage(field, &child)) return std::nullopt;
      UploadBlock block;
      proto::Field item;
      while (child.Next(&item)) {
        switch (item.number) {
          case 1: if (IsBytes(item)) block.host = item.AsString(); break;
          case 2: if (IsBytes(item)) block.path = item.AsString(); break;
          case 3: if (IsVarint(item)) block.use_https = item.value != 0; break;
          case 4: if (IsVarint(item)) block.http_method = static_cast<int>(item.value); break;
          case 5:
            if (IsBytes(item)) {
              proto::Reader header_reader;
              std::pair<std::string, std::string> header;
              if (!child.Submessage(item, &header_reader) || !DecodeHeader(header_reader, &header)) return std::nullopt;
              block.headers.push_back(std::move(header));
            }
            break;
          case 6: if (IsVarint(item)) block.offset = item.value; break;
          case 7: if (IsVarint(item)) block.length = item.value; break;
          case 8: if (IsBytes(item)) block.explicit_body = Bytes(item); break;
          default: break;
        }
      }
      if (!child.ok()) return std::nullopt;
      plan.blocks.push_back(std::move(block));
    }
  }
  if (!reader.ok()) return std::nullopt;
  return plan;
}

std::vector<std::uint8_t> EncodeCommitFileUploadRequest(bool transfer_succeeded, std::uint32_t app_id,
                                                        const std::vector<std::uint8_t>& sha1, const std::string& name) {
  proto::Writer body;
  body.AddVarint(1, transfer_succeeded ? 1 : 0);
  body.AddVarint(2, app_id);
  body.AddBytes(3, sha1);
  body.AddString(4, name);
  return body.bytes();
}

std::optional<bool> DecodeCommitFileUploadResponse(const std::vector<std::uint8_t>& body) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  bool committed = false;
  while (reader.Next(&field))
    if (field.number == 1 && IsVarint(field)) committed = field.value != 0;
  if (!reader.ok()) return std::nullopt;
  return committed;
}

std::vector<std::uint8_t> EncodeDeleteFileRequest(std::uint32_t app_id, const std::string& name, std::uint64_t batch_id) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddString(2, name);
  body.AddVarint(3, 1);  // is_explicit_delete
  body.AddVarint(4, batch_id);
  return body.bytes();
}

std::vector<std::uint8_t> EncodeCompleteBatchRequest(std::uint32_t app_id, std::uint64_t batch_id, bool succeeded) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddVarint(2, batch_id);
  body.AddVarint(3, succeeded ? 1 : 2);  // EResult OK / Fail
  return body.bytes();
}

}  // namespace z3cloudplay::steam::cloud
