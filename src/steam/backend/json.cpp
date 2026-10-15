#include "steam/backend/json.h"

#include <algorithm>
#include <limits>

namespace z3cloudplay::steam::json {

const Value* Value::Find(std::string_view key) const {
  if (!is_object()) return nullptr;
  for (const auto& entry : object_)
    if (entry.first == key) return &entry.second;
  return nullptr;
}

std::optional<std::uint64_t> Value::AsUint64() const {
  if (!is_number() || text_.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const unsigned char byte : text_) {
    if (byte < '0' || byte > '9') return std::nullopt;
    const std::uint64_t digit = byte - '0';
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
      return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

class Parser {
 public:
  Parser(std::string_view input, Limits limits) : input_(input), limits_(limits) {}

  std::optional<Value> Run() {
    if (input_.size() > limits_.max_bytes || limits_.max_values == 0) return std::nullopt;
    Value value;
    SkipWhitespace();
    if (!ParseValue(0, value)) return std::nullopt;
    SkipWhitespace();
    if (offset_ != input_.size()) return std::nullopt;
    return value;
  }

 private:
  bool ParseValue(std::size_t depth, Value& value) {
    if (depth > limits_.max_depth || values_ >= limits_.max_values || offset_ >= input_.size())
      return false;
    ++values_;
    const char byte = input_[offset_];
    if (byte == 'n') return Literal("null", value);
    if (byte == 't') { if (!Literal("true", value)) return false; value.type_ = Value::Type::kBool; value.boolean_ = true; return true; }
    if (byte == 'f') { if (!Literal("false", value)) return false; value.type_ = Value::Type::kBool; return true; }
    if (byte == '"') { value.type_ = Value::Type::kString; return ParseString(value.text_); }
    if (byte == '[') return ParseArray(depth, value);
    if (byte == '{') return ParseObject(depth, value);
    return ParseNumber(value);
  }

  bool Literal(std::string_view literal, Value& value) {
    if (input_.substr(offset_, literal.size()) != literal) return false;
    offset_ += literal.size(); value.type_ = Value::Type::kNull; return true;
  }

  bool ParseNumber(Value& value) {
    const std::size_t start = offset_;
    if (Consume('-') && offset_ == input_.size()) return false;
    if (Consume('0')) {
      if (offset_ < input_.size() && IsDigit(input_[offset_])) return false;
    } else {
      if (offset_ >= input_.size() || input_[offset_] < '1' || input_[offset_] > '9') return false;
      while (offset_ < input_.size() && IsDigit(input_[offset_])) ++offset_;
    }
    if (Consume('.')) {
      if (offset_ >= input_.size() || !IsDigit(input_[offset_])) return false;
      while (offset_ < input_.size() && IsDigit(input_[offset_])) ++offset_;
    }
    if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
      ++offset_; if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) ++offset_;
      if (offset_ >= input_.size() || !IsDigit(input_[offset_])) return false;
      while (offset_ < input_.size() && IsDigit(input_[offset_])) ++offset_;
    }
    value.type_ = Value::Type::kNumber;
    value.text_.assign(input_.data() + start, offset_ - start);
    return true;
  }

  bool ParseArray(std::size_t depth, Value& value) {
    ++offset_; value.type_ = Value::Type::kArray; SkipWhitespace();
    if (Consume(']')) return true;
    while (true) {
      Value child;
      if (!ParseValue(depth + 1, child)) return false;
      value.array_.push_back(std::move(child)); SkipWhitespace();
      if (Consume(']')) return true;
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
  }

  bool ParseObject(std::size_t depth, Value& value) {
    ++offset_; value.type_ = Value::Type::kObject; SkipWhitespace();
    if (Consume('}')) return true;
    while (true) {
      std::string key;
      if (!ParseString(key)) return false;
      for (const auto& entry : value.object_) if (entry.first == key) return false;
      SkipWhitespace(); if (!Consume(':')) return false; SkipWhitespace();
      Value child; if (!ParseValue(depth + 1, child)) return false;
      value.object_.emplace_back(std::move(key), std::move(child)); SkipWhitespace();
      if (Consume('}')) return true;
      if (!Consume(',')) return false;
      SkipWhitespace();
    }
  }

  bool ParseString(std::string& output) {
    if (!Consume('"')) return false;
    while (offset_ < input_.size()) {
      const unsigned char byte = static_cast<unsigned char>(input_[offset_++]);
      if (byte == '"') return true;
      if (byte < 0x20) return false;
      if (byte == '\\') {
        if (offset_ >= input_.size()) return false;
        const char escape = input_[offset_++];
        switch (escape) {
          case '"': case '\\': case '/': if (!Append(output, static_cast<unsigned char>(escape))) return false; break;
          case 'b': if (!Append(output, '\b')) return false; break;
          case 'f': if (!Append(output, '\f')) return false; break;
          case 'n': if (!Append(output, '\n')) return false; break;
          case 'r': if (!Append(output, '\r')) return false; break;
          case 't': if (!Append(output, '\t')) return false; break;
          case 'u': { std::uint32_t scalar = 0; if (!Hex4(scalar)) return false;
            if (scalar >= 0xd800 && scalar <= 0xdbff) {
              if (offset_ + 2 > input_.size() || input_[offset_] != '\\' || input_[offset_ + 1] != 'u') return false;
              offset_ += 2; std::uint32_t low = 0; if (!Hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
              scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00);
            } else if (scalar >= 0xdc00 && scalar <= 0xdfff) return false;
            if (!AppendScalar(output, scalar)) return false; break;
          }
          default: return false;
        }
      } else if (byte < 0x80) {
        if (!Append(output, byte)) return false;
      } else {
        std::size_t continuation = 0; unsigned char second_min = 0x80, second_max = 0xbf;
        if (byte >= 0xc2 && byte <= 0xdf) continuation = 1;
        else if (byte >= 0xe0 && byte <= 0xef) { continuation = 2; if (byte == 0xe0) second_min = 0xa0; if (byte == 0xed) second_max = 0x9f; }
        else if (byte >= 0xf0 && byte <= 0xf4) { continuation = 3; if (byte == 0xf0) second_min = 0x90; if (byte == 0xf4) second_max = 0x8f; }
        else return false;
        if (continuation > input_.size() - offset_ || output.size() + continuation + 1 > limits_.max_string_bytes) return false;
        const unsigned char second = static_cast<unsigned char>(input_[offset_]);
        if (second < second_min || second > second_max) return false;
        output.push_back(static_cast<char>(byte));
        for (std::size_t index = 0; index < continuation; ++index) {
          const unsigned char next = static_cast<unsigned char>(input_[offset_++]);
          if (index != 0 && (next < 0x80 || next > 0xbf)) return false;
          output.push_back(static_cast<char>(next));
        }
      }
    }
    return false;
  }

  bool Hex4(std::uint32_t& value) {
    if (input_.size() - offset_ < 4) return false;
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const unsigned char byte = static_cast<unsigned char>(input_[offset_++]);
      unsigned digit = 0;
      if (byte >= '0' && byte <= '9') digit = byte - '0';
      else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
      else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10;
      else return false;
      value = value * 16 + digit;
    }
    return true;
  }

  bool Append(std::string& output, unsigned char byte) {
    if (output.size() >= limits_.max_string_bytes) return false;
    output.push_back(static_cast<char>(byte)); return true;
  }
  bool AppendScalar(std::string& output, std::uint32_t scalar) {
    unsigned char bytes[4]; std::size_t size = 0;
    if (scalar <= 0x7f) bytes[size++] = static_cast<unsigned char>(scalar);
    else if (scalar <= 0x7ff) { bytes[size++] = 0xc0 | (scalar >> 6); bytes[size++] = 0x80 | (scalar & 0x3f); }
    else if (scalar <= 0xffff) { bytes[size++] = 0xe0 | (scalar >> 12); bytes[size++] = 0x80 | ((scalar >> 6) & 0x3f); bytes[size++] = 0x80 | (scalar & 0x3f); }
    else { bytes[size++] = 0xf0 | (scalar >> 18); bytes[size++] = 0x80 | ((scalar >> 12) & 0x3f); bytes[size++] = 0x80 | ((scalar >> 6) & 0x3f); bytes[size++] = 0x80 | (scalar & 0x3f); }
    if (size > limits_.max_string_bytes - std::min(output.size(), limits_.max_string_bytes)) return false;
    output.append(reinterpret_cast<const char*>(bytes), size); return true;
  }
  bool Consume(char expected) { if (offset_ >= input_.size() || input_[offset_] != expected) return false; ++offset_; return true; }
  void SkipWhitespace() { while (offset_ < input_.size() && (input_[offset_] == ' ' || input_[offset_] == '\t' || input_[offset_] == '\r' || input_[offset_] == '\n')) ++offset_; }
  static bool IsDigit(char byte) { return byte >= '0' && byte <= '9'; }

  std::string_view input_; Limits limits_; std::size_t offset_ = 0; std::size_t values_ = 0;
};

std::optional<Value> Parse(std::string_view input, Limits limits) {
  return Parser(input, limits).Run();
}

}  // namespace z3cloudplay::steam::json
