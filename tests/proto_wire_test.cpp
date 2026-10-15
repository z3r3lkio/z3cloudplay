#include "steam/backend/proto_wire.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using z3cloudplay::steam::proto::DecodeError;
using z3cloudplay::steam::proto::DecodeLimits;
using z3cloudplay::steam::proto::Field;
using z3cloudplay::steam::proto::Reader;
using z3cloudplay::steam::proto::WireType;
using z3cloudplay::steam::proto::Writer;

void TestAllSupportedWireTypesRoundTrip() {
  Writer nested;
  assert(nested.AddVarint(1, 42));

  Writer writer;
  assert(writer.AddVarint(1, 300));
  assert(writer.AddFixed64(2, 0x8877665544332211ULL));
  assert(writer.AddString(3, "hello"));
  assert(writer.AddFixed32(4, 0x44332211U));
  assert(writer.AddMessage(5, nested));

  Reader reader(writer.bytes());
  Field field;
  assert(reader.Next(&field));
  assert(field.number == 1 && field.wire_type == WireType::kVarint &&
         field.value == 300);
  assert(reader.Next(&field));
  assert(field.number == 2 && field.wire_type == WireType::kFixed64 &&
         field.value == 0x8877665544332211ULL);
  assert(reader.Next(&field));
  assert(field.number == 3 && field.wire_type == WireType::kLengthDelimited &&
         field.AsString() == "hello");
  assert(reader.Next(&field));
  assert(field.number == 4 && field.wire_type == WireType::kFixed32 &&
         field.value == 0x44332211U);
  assert(reader.Next(&field));
  assert(field.number == 5 && field.wire_type == WireType::kLengthDelimited);

  Reader child;
  assert(reader.Submessage(field, &child));
  assert(child.Next(&field));
  assert(field.number == 1 && field.value == 42);
  assert(!child.Next(&field) && child.ok());
  assert(!reader.Next(&field) && reader.ok());
}

void TestUnknownFieldsAreConsumedByNext() {
  Writer writer;
  assert(writer.AddVarint(99, 7));
  assert(writer.AddString(100, "ignored"));
  assert(writer.AddVarint(1, 9));
  Reader reader(writer.bytes());
  Field field;
  assert(reader.Next(&field) && field.number == 99);
  assert(reader.Next(&field) && field.number == 100);
  assert(reader.Next(&field) && field.number == 1 && field.value == 9);
  assert(!reader.Next(&field) && reader.ok());
}

void ExpectError(const std::vector<std::uint8_t>& bytes, DecodeError error,
                 DecodeLimits limits = {}) {
  Reader reader(bytes.data(), bytes.size(), limits);
  Field field;
  while (reader.Next(&field)) {
  }
  assert(reader.error() == error);
}

void TestMalformedInputIsRejected() {
  ExpectError({0x08, 0x80}, DecodeError::kTruncated);
  ExpectError({0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
               0x02},
              DecodeError::kVarintOverflow);
  ExpectError({0x00}, DecodeError::kInvalidTag);
  ExpectError({0x0b}, DecodeError::kUnsupportedWireType);
  ExpectError({0x0c}, DecodeError::kUnsupportedWireType);
  ExpectError({0x0e}, DecodeError::kUnsupportedWireType);
  ExpectError({0x0a, 0x05, 'a'}, DecodeError::kTruncated);
  ExpectError({0x09, 0x01}, DecodeError::kTruncated);
  ExpectError({0x0d, 0x01, 0x02}, DecodeError::kTruncated);
  ExpectError({0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
               0x80, 0x02},
              DecodeError::kVarintOverflow);
  ExpectError({0xff, 0xff, 0xff, 0xff, 0x10, 0x00},
              DecodeError::kInvalidTag);
}

void TestReaderLimitsAreEnforced() {
  DecodeLimits limits;
  limits.max_message_bytes = 1;
  ExpectError({0x08, 0x01}, DecodeError::kMessageTooLarge, limits);

  limits = {};
  limits.max_fields = 1;
  ExpectError({0x08, 0x01, 0x10, 0x02}, DecodeError::kTooManyFields, limits);

  limits = {};
  limits.max_length_delimited_bytes = 2;
  ExpectError({0x0a, 0x03, 'a', 'b', 'c'}, DecodeError::kLengthTooLarge,
              limits);

  Writer outer;
  Writer middle;
  Writer inner;
  assert(inner.AddVarint(1, 1));
  assert(middle.AddMessage(1, inner));
  assert(outer.AddMessage(1, middle));
  limits = {};
  limits.max_depth = 1;
  Reader root(outer.bytes().data(), outer.bytes().size(), limits);
  Field field;
  assert(root.Next(&field));
  Reader child;
  assert(root.Submessage(field, &child));
  assert(child.Next(&field));
  Reader rejected;
  assert(!child.Submessage(field, &rejected));
  assert(child.error() == DecodeError::kDepthLimit);

  limits = {};
  limits.max_depth = 0;
  Reader no_nesting(outer.bytes().data(), outer.bytes().size(), limits);
  assert(no_nesting.Next(&field));
  assert(!no_nesting.Submessage(field, &rejected));
  assert(no_nesting.status().offset == outer.bytes().size());
}

void TestWriterRejectsInvalidFieldsAndOverflow() {
  Writer writer(2);
  assert(!writer.AddVarint(0, 1));
  assert(writer.error() == DecodeError::kInvalidTag);

  Writer bounded(1);
  assert(!bounded.AddVarint(1, 1));
  assert(bounded.error() == DecodeError::kMessageTooLarge);

  Reader invalid(nullptr, 1);
  assert(invalid.error() == DecodeError::kInvalidArgument);
}

}  // namespace

int main() {
  TestAllSupportedWireTypesRoundTrip();
  TestUnknownFieldsAreConsumedByNext();
  TestMalformedInputIsRejected();
  TestReaderLimitsAreEnforced();
  TestWriterRejectsInvalidFieldsAndOverflow();
  return 0;
}
