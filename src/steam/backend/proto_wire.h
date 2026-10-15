#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace z3cloudplay::steam::proto {

enum class WireType : std::uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kFixed32 = 5,
};

enum class DecodeError : std::uint8_t {
  kNone = 0,
  kTruncated,
  kVarintOverflow,
  kInvalidTag,
  kUnsupportedWireType,
  kWrongWireType,
  kLengthTooLarge,
  kMessageTooLarge,
  kTooManyFields,
  kDepthLimit,
  kInvalidArgument,
};

struct Status {
  DecodeError error = DecodeError::kNone;
  std::size_t offset = 0;

  bool ok() const { return error == DecodeError::kNone; }
};

struct DecodeLimits {
  std::size_t max_message_bytes = 1024 * 1024;
  std::size_t max_length_delimited_bytes = 1024 * 1024;
  std::size_t max_fields = 256;
  std::uint32_t max_depth = 8;
};

struct Field {
  std::uint32_t number = 0;
  WireType wire_type = WireType::kVarint;
  std::size_t offset = 0;
  std::uint64_t value = 0;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t data_offset = 0;

  std::string AsString() const;
};

class Reader {
 public:
  Reader();
  Reader(const std::uint8_t* data, std::size_t size,
         DecodeLimits limits = {});
  explicit Reader(const std::vector<std::uint8_t>& bytes,
                  DecodeLimits limits = {});

  // Parses and consumes a complete field. Unknown fields therefore need no
  // special skip call.
  bool Next(Field* field);
  bool Submessage(const Field& field, Reader* child);

  bool ok() const { return status_.ok(); }
  DecodeError error() const { return status_.error; }
  Status status() const { return status_; }

 private:
  Reader(const std::uint8_t* data, std::size_t size, DecodeLimits limits,
         std::uint32_t depth, std::size_t base_offset);
  bool ReadVarint(std::uint64_t* value);
  bool Fail(DecodeError error, std::size_t offset);

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
  std::size_t field_count_ = 0;
  DecodeLimits limits_{};
  std::uint32_t depth_ = 0;
  std::size_t base_offset_ = 0;
  Status status_{};
};

class Writer {
 public:
  explicit Writer(std::size_t max_message_bytes = 1024 * 1024);

  bool AddVarint(std::uint32_t field_number, std::uint64_t value);
  bool AddFixed32(std::uint32_t field_number, std::uint32_t value);
  bool AddFixed64(std::uint32_t field_number, std::uint64_t value);
  bool AddBytes(std::uint32_t field_number, const std::uint8_t* data,
                std::size_t size);
  bool AddBytes(std::uint32_t field_number,
                const std::vector<std::uint8_t>& bytes);
  bool AddString(std::uint32_t field_number, const std::string& value);
  bool AddMessage(std::uint32_t field_number, const Writer& message);

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  bool ok() const { return status_.ok(); }
  Status status() const { return status_; }
  DecodeError error() const { return status_.error; }

 private:
  bool BeginField(std::uint32_t field_number, WireType wire_type,
                  std::size_t payload_size, std::uint64_t length = 0,
                  bool has_length = false);
  void AppendVarint(std::uint64_t value);
  bool Fail(DecodeError error);

  std::vector<std::uint8_t> bytes_;
  std::size_t max_message_bytes_;
  Status status_{};
};

}  // namespace z3cloudplay::steam::proto
