#include "steam/backend/proto_wire.h"

#include <limits>

namespace z3cloudplay::steam::proto {
namespace {

constexpr std::uint32_t kMaxFieldNumber = (1U << 29U) - 1U;

std::size_t VarintSize(std::uint64_t value) {
  std::size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
}

}  // namespace

std::string Field::AsString() const {
  if (data == nullptr || size == 0) return {};
  return std::string(reinterpret_cast<const char*>(data), size);
}

Reader::Reader() = default;

Reader::Reader(const std::uint8_t* data, std::size_t size, DecodeLimits limits)
    : Reader(data, size, limits, 0, 0) {}

Reader::Reader(const std::vector<std::uint8_t>& bytes, DecodeLimits limits)
    : Reader(bytes.data(), bytes.size(), limits, 0, 0) {}

Reader::Reader(const std::uint8_t* data, std::size_t size, DecodeLimits limits,
               std::uint32_t depth, std::size_t base_offset)
    : data_(data),
      size_(size),
      limits_(limits),
      depth_(depth),
      base_offset_(base_offset) {
  if (size > limits_.max_message_bytes) {
    Fail(DecodeError::kMessageTooLarge, 0);
  } else if (size != 0 && data == nullptr) {
    Fail(DecodeError::kInvalidArgument, 0);
  }
}

bool Reader::Fail(DecodeError error, std::size_t offset) {
  if (status_.ok()) status_ = {error, base_offset_ + offset};
  return false;
}

bool Reader::ReadVarint(std::uint64_t* value) {
  const std::size_t start = offset_;
  std::uint64_t result = 0;
  for (unsigned index = 0; index < 10; ++index) {
    if (offset_ >= size_) return Fail(DecodeError::kTruncated, start);
    const std::uint8_t byte = data_[offset_++];
    if (index == 9 && byte > 1) {
      return Fail(DecodeError::kVarintOverflow, start);
    }
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << (index * 7U);
    if ((byte & 0x80U) == 0) {
      *value = result;
      return true;
    }
  }
  return Fail(DecodeError::kVarintOverflow, start);
}

bool Reader::Next(Field* field) {
  if (!ok() || offset_ == size_) return false;
  if (field == nullptr) return Fail(DecodeError::kInvalidArgument, offset_);
  if (field_count_ >= limits_.max_fields) {
    return Fail(DecodeError::kTooManyFields, offset_);
  }
  ++field_count_;

  const std::size_t tag_offset = offset_;
  std::uint64_t tag = 0;
  if (!ReadVarint(&tag)) return false;
  const std::uint64_t number = tag >> 3U;
  const std::uint8_t wire = static_cast<std::uint8_t>(tag & 7U);
  if (number == 0 || number > kMaxFieldNumber) {
    return Fail(DecodeError::kInvalidTag, tag_offset);
  }
  if (wire != 0 && wire != 1 && wire != 2 && wire != 5) {
    return Fail(DecodeError::kUnsupportedWireType, tag_offset);
  }

  *field = {};
  field->number = static_cast<std::uint32_t>(number);
  field->wire_type = static_cast<WireType>(wire);
  field->offset = base_offset_ + tag_offset;
  if (wire == 0) return ReadVarint(&field->value);

  if (wire == 1 || wire == 5) {
    const std::size_t width = wire == 1 ? 8 : 4;
    if (width > size_ - offset_) {
      return Fail(DecodeError::kTruncated, offset_);
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value |= static_cast<std::uint64_t>(data_[offset_ + index])
               << (index * 8U);
    }
    offset_ += width;
    field->value = value;
    return true;
  }

  std::uint64_t length = 0;
  const std::size_t length_offset = offset_;
  if (!ReadVarint(&length)) return false;
  if (length > limits_.max_length_delimited_bytes ||
      length > std::numeric_limits<std::size_t>::max()) {
    return Fail(DecodeError::kLengthTooLarge, length_offset);
  }
  const std::size_t size = static_cast<std::size_t>(length);
  if (size > size_ - offset_) return Fail(DecodeError::kTruncated, offset_);
  field->data = data_ + offset_;
  field->size = size;
  field->data_offset = base_offset_ + offset_;
  offset_ += size;
  return true;
}

bool Reader::Submessage(const Field& field, Reader* child) {
  if (!ok() || child == nullptr) return false;
  if (field.wire_type != WireType::kLengthDelimited) {
    return Fail(DecodeError::kWrongWireType, offset_);
  }
  if (depth_ >= limits_.max_depth) {
    return Fail(DecodeError::kDepthLimit, offset_);
  }
  *child =
      Reader(field.data, field.size, limits_, depth_ + 1, field.data_offset);
  if (!child->ok()) {
    status_ = child->status();
    return false;
  }
  return true;
}

Writer::Writer(std::size_t max_message_bytes)
    : max_message_bytes_(max_message_bytes) {}

bool Writer::Fail(DecodeError error) {
  if (status_.ok()) status_ = {error, bytes_.size()};
  return false;
}

void Writer::AppendVarint(std::uint64_t value) {
  while (value >= 0x80) {
    bytes_.push_back(static_cast<std::uint8_t>(value) | 0x80U);
    value >>= 7;
  }
  bytes_.push_back(static_cast<std::uint8_t>(value));
}

bool Writer::BeginField(std::uint32_t field_number, WireType wire_type,
                        std::size_t payload_size, std::uint64_t length,
                        bool has_length) {
  if (!ok()) return false;
  if (field_number == 0 || field_number > kMaxFieldNumber) {
    return Fail(DecodeError::kInvalidTag);
  }
  const std::uint64_t tag =
      (static_cast<std::uint64_t>(field_number) << 3U) |
      static_cast<std::uint8_t>(wire_type);
  const std::size_t prefix =
      VarintSize(tag) + (has_length ? VarintSize(length) : 0);
  if (bytes_.size() > max_message_bytes_ ||
      prefix > max_message_bytes_ - bytes_.size() ||
      payload_size > max_message_bytes_ - bytes_.size() - prefix) {
    return Fail(DecodeError::kMessageTooLarge);
  }
  AppendVarint(tag);
  if (has_length) AppendVarint(length);
  return true;
}

bool Writer::AddVarint(std::uint32_t field_number, std::uint64_t value) {
  if (!BeginField(field_number, WireType::kVarint, VarintSize(value)))
    return false;
  AppendVarint(value);
  return true;
}

bool Writer::AddFixed32(std::uint32_t field_number, std::uint32_t value) {
  if (!BeginField(field_number, WireType::kFixed32, 4)) return false;
  for (unsigned index = 0; index < 4; ++index)
    bytes_.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  return true;
}

bool Writer::AddFixed64(std::uint32_t field_number, std::uint64_t value) {
  if (!BeginField(field_number, WireType::kFixed64, 8)) return false;
  for (unsigned index = 0; index < 8; ++index)
    bytes_.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
  return true;
}

bool Writer::AddBytes(std::uint32_t field_number, const std::uint8_t* data,
                      std::size_t size) {
  if ((data == nullptr && size != 0) ||
      !BeginField(field_number, WireType::kLengthDelimited, size, size, true))
    return data == nullptr && size != 0 ? Fail(DecodeError::kInvalidArgument)
                                        : false;
  if (size != 0) bytes_.insert(bytes_.end(), data, data + size);
  return true;
}

bool Writer::AddBytes(std::uint32_t field_number,
                      const std::vector<std::uint8_t>& bytes) {
  return AddBytes(field_number, bytes.data(), bytes.size());
}

bool Writer::AddString(std::uint32_t field_number, const std::string& value) {
  return AddBytes(field_number,
                  reinterpret_cast<const std::uint8_t*>(value.data()),
                  value.size());
}

bool Writer::AddMessage(std::uint32_t field_number, const Writer& message) {
  if (!message.ok()) return Fail(message.error());
  return AddBytes(field_number, message.bytes());
}

}  // namespace z3cloudplay::steam::proto
