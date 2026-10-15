#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace z3cloudplay::steam::json {

struct Limits {
  std::size_t max_bytes = 1024 * 1024;
  std::size_t max_depth = 16;
  std::size_t max_values = 4096;
  std::size_t max_string_bytes = 256 * 1024;
};

class Parser;

class Value {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  using Array = std::vector<Value>;
  using Object = std::vector<std::pair<std::string, Value>>;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }
  bool boolean() const { return boolean_; }
  const std::string& text() const { return text_; }
  const Array& array() const { return array_; }
  const Object& object() const { return object_; }
  const Value* Find(std::string_view key) const;
  std::optional<std::uint64_t> AsUint64() const;

 private:
  friend class Parser;
  Type type_ = Type::kNull;
  bool boolean_ = false;
  std::string text_;
  Array array_;
  Object object_;
};

std::optional<Value> Parse(std::string_view input, Limits limits = {});

}  // namespace z3cloudplay::steam::json
