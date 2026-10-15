// json.hpp - Small dependency-free JSON reader for deterministic fixture data.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace z3cloudplay::homebrew::json {

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data = nullptr;

    [[nodiscard]] const Object* object() const { return std::get_if<Object>(&data); }
    [[nodiscard]] const Array* array() const { return std::get_if<Array>(&data); }
    [[nodiscard]] const std::string* string() const { return std::get_if<std::string>(&data); }
    [[nodiscard]] std::optional<bool> boolean() const {
        if (const auto* value = std::get_if<bool>(&data)) return *value;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<double> number() const {
        if (const auto* value = std::get_if<double>(&data)) return *value;
        return std::nullopt;
    }
    [[nodiscard]] const Value* get(std::string_view key) const {
        const auto* values = object();
        if (!values) return nullptr;
        const auto found = values->find(std::string(key));
        return found == values->end() ? nullptr : &found->second;
    }
};

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    [[nodiscard]] std::optional<Value> parse() {
        skip_space();
        auto value = parse_value();
        skip_space();
        if (!value || position_ != input_.size()) return std::nullopt;
        return value;
    }

private:
    std::string_view input_;
    std::size_t position_ = 0;

    void skip_space() {
        while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    [[nodiscard]] bool consume(char wanted) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != wanted) return false;
        ++position_;
        return true;
    }

    [[nodiscard]] std::optional<Value> parse_value() {
        skip_space();
        if (position_ >= input_.size()) return std::nullopt;
        if (input_[position_] == '{') return parse_object();
        if (input_[position_] == '[') return parse_array();
        if (input_[position_] == '"') {
            auto value = parse_string();
            if (!value) return std::nullopt;
            return Value{*value};
        }
        if (input_.substr(position_, 4) == "true") { position_ += 4; return Value{true}; }
        if (input_.substr(position_, 5) == "false") { position_ += 5; return Value{false}; }
        if (input_.substr(position_, 4) == "null") { position_ += 4; return Value{}; }
        return parse_number();
    }

    [[nodiscard]] std::optional<Value> parse_object() {
        if (!consume('{')) return std::nullopt;
        Value::Object object;
        skip_space();
        if (consume('}')) return Value{std::move(object)};
        for (;;) {
            auto key = parse_string();
            if (!key || !consume(':')) return std::nullopt;
            auto value = parse_value();
            if (!value) return std::nullopt;
            object.emplace(std::move(*key), std::move(*value));
            if (consume('}')) break;
            if (!consume(',')) return std::nullopt;
        }
        return Value{std::move(object)};
    }

    [[nodiscard]] std::optional<Value> parse_array() {
        if (!consume('[')) return std::nullopt;
        Value::Array array;
        skip_space();
        if (consume(']')) return Value{std::move(array)};
        for (;;) {
            auto value = parse_value();
            if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            if (consume(']')) break;
            if (!consume(',')) return std::nullopt;
        }
        return Value{std::move(array)};
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        skip_space();
        if (position_ >= input_.size() || input_[position_++] != '"') return std::nullopt;
        std::string result;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return result;
            if (character != '\\') { result.push_back(character); continue; }
            if (position_ >= input_.size()) return std::nullopt;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': case '\\': case '/': result.push_back(escaped); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
                if (position_ + 4 > input_.size()) return std::nullopt;
                {
                    unsigned codepoint = 0;
                    const auto first = input_.data() + position_;
                    const auto last = first + 4;
                    const auto parsed = std::from_chars(first, last, codepoint, 16);
                    if (parsed.ec != std::errc{} || parsed.ptr != last) return std::nullopt;
                    position_ += 4;
                    if (codepoint <= 0x7fU) result.push_back(static_cast<char>(codepoint));
                    else if (codepoint <= 0x7ffU) {
                        result.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
                        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                    } else {
                        result.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
                        result.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
                        result.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                    }
                }
                break;
            default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Value> parse_number() {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (begin == position_) return std::nullopt;
        const std::string token(input_.substr(begin, position_ - begin));
        char* end = nullptr;
        errno = 0;
        const double result = std::strtod(token.c_str(), &end);
        if (errno != 0 || end != token.c_str() + token.size()) return std::nullopt;
        return Value{result};
    }
};

inline std::optional<Value> parse(std::string_view input) { return Parser(input).parse(); }

inline std::string escape(std::string_view value) {
    std::string output;
    for (const char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output.push_back(character); break;
        }
    }
    return output;
}

} // namespace z3cloudplay::homebrew::json
