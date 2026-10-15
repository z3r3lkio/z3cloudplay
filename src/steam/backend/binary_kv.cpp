#include "steam/backend/binary_kv.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <unordered_set>
#include <utility>

namespace z3cloudplay::steam {
namespace {

constexpr std::size_t kMaxPackageBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaxStringBytes = 1024U * 1024U;
constexpr std::uint32_t kMaxDepth = 32;
constexpr std::uint32_t kMaxNodes = 100000;

constexpr std::uint8_t kTypeSection = 0;
constexpr std::uint8_t kTypeString = 1;
constexpr std::uint8_t kTypeInt32 = 2;
constexpr std::uint8_t kTypeFloat = 3;
constexpr std::uint8_t kTypePointer = 4;
constexpr std::uint8_t kTypeColor = 6;
constexpr std::uint8_t kTypeUint64 = 7;
constexpr std::uint8_t kTypeEnd = 8;
constexpr std::uint8_t kTypeInt64 = 10;

struct Node {
  bool is_dict = false;
  std::string text;
  std::vector<std::pair<std::string, Node>> entries;

  const Node* Find(const std::string& key) const {
    if (!is_dict) return nullptr;
    for (const auto& entry : entries) {
      if (entry.first == key) return &entry.second;
    }
    return nullptr;
  }
};

bool IsValidUtf8(const std::uint8_t* p, std::size_t n) {
  std::size_t i = 0;
  while (i < n) {
    const std::uint8_t b = p[i];
    if (b < 0x80) {
      ++i;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t min = 0;
    std::uint32_t cp = 0;
    if (b >= 0xC2 && b <= 0xDF) {
      extra = 1;
      min = 0x80;
      cp = b & 0x1FU;
    } else if (b >= 0xE0 && b <= 0xEF) {
      extra = 2;
      min = 0x800;
      cp = b & 0x0FU;
    } else if (b >= 0xF0 && b <= 0xF4) {
      extra = 3;
      min = 0x10000;
      cp = b & 0x07U;
    } else {
      return false;
    }
    if (n - i - 1 < extra) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      if ((p[i + k] & 0xC0U) != 0x80U) return false;
      cp = (cp << 6U) | (p[i + k] & 0x3FU);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

std::string AsciiLower(const std::string& s) {
  std::string r = s;
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  std::size_t offset() const { return offset_; }

  bool Section(std::uint32_t depth, Node* out) {
    if (depth >= kMaxDepth) return false;
    out->is_dict = true;
    std::unordered_set<std::string> lowered_keys;
    while (offset_ < size_) {
      const std::uint8_t type = data_[offset_++];
      if (type == kTypeEnd) return true;
      if (++nodes_ > kMaxNodes) return false;
      std::string key;
      if (!ReadString(&key)) return false;
      Node value;
      switch (type) {
        case kTypeSection:
          if (!Section(depth + 1, &value)) return false;
          break;
        case kTypeString:
          if (!ReadString(&value.text)) return false;
          break;
        case kTypeInt32:
        case kTypePointer:
        case kTypeColor: {
          std::uint64_t v = 0;
          if (!ReadInteger(4, &v)) return false;
          value.text = std::to_string(v);
          break;
        }
        case kTypeFloat: {
          std::uint64_t v = 0;
          if (!ReadInteger(4, &v)) return false;
          value.text = "float";  // never a valid id
          break;
        }
        case kTypeUint64:
        case kTypeInt64: {
          std::uint64_t v = 0;
          if (!ReadInteger(8, &v)) return false;
          value.text = std::to_string(v);
          break;
        }
        default:
          return false;
      }
      std::string lowered = AsciiLower(key);
      if (!lowered_keys.insert(std::move(lowered)).second) return false;  // duplicate key
      out->entries.emplace_back(std::move(key), std::move(value));
    }
    return false;  // truncated: no terminator
  }

 private:
  bool ReadInteger(std::size_t bytes, std::uint64_t* value) {
    if (size_ - offset_ < bytes) return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
      v |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8U * i);
    }
    offset_ += bytes;
    *value = v;
    return true;
  }

  bool ReadString(std::string* value) {
    std::size_t end = offset_;
    while (end < size_ && data_[end] != 0) ++end;
    if (end >= size_) return false;  // missing NUL
    const std::size_t length = end - offset_;
    if (length > kMaxStringBytes) return false;
    if (!IsValidUtf8(data_ + offset_, length)) return false;
    value->assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ = end + 1;
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  std::uint32_t nodes_ = 0;
};

bool CollectIds(const Node& root, const char* key, std::vector<std::uint32_t>* ids) {
  const Node* section = root.Find(key);
  if (section == nullptr || !section->is_dict) return true;
  for (const auto& entry : section->entries) {
    const Node& value = entry.second;
    if (value.is_dict) return false;
    const std::string& raw = value.text;
    if (raw.empty() || raw.front() < '0' || raw.front() > '9') return false;
    std::uint32_t id = 0;
    const char* begin = raw.data();
    const char* end = begin + raw.size();
    const auto result = std::from_chars(begin, end, id);
    if (result.ec != std::errc() || result.ptr != end) return false;
    ids->push_back(id);
  }
  std::sort(ids->begin(), ids->end());
  ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
  return true;
}

}  // namespace

bool ParsePackageBuffer(const std::uint8_t* data, std::size_t size, std::uint32_t package_id,
                        PackageContents* out) {
  if (out == nullptr || data == nullptr) return false;
  *out = {};
  if (size < 5 || size > kMaxPackageBytes) return false;
  const std::uint32_t header = static_cast<std::uint32_t>(data[0]) |
                               (static_cast<std::uint32_t>(data[1]) << 8U) |
                               (static_cast<std::uint32_t>(data[2]) << 16U) |
                               (static_cast<std::uint32_t>(data[3]) << 24U);
  if (header != 1) return false;

  Reader reader(data + 4, size - 4);
  Node document;
  if (!reader.Section(0, &document)) return false;
  if (reader.offset() != size - 4 || document.entries.size() != 1) return false;
  const Node* root = document.Find(std::to_string(package_id));
  if (root == nullptr || !root->is_dict) return false;

  PackageContents contents;
  if (!CollectIds(*root, "appids", &contents.app_ids)) return false;
  if (!CollectIds(*root, "depotids", &contents.depot_ids)) return false;
  *out = std::move(contents);
  return true;
}

}  // namespace z3cloudplay::steam
