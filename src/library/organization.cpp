#include "library/organization.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace z3cloudplay {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{{'P', 'L', 'D', 'O', 'R', 'G', 0, 0}};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaximumSnapshotBytes = 1024 * 1024;
constexpr std::size_t kMaximumGameIds = 10000;
constexpr std::size_t kMaximumCollections = 256;
constexpr std::size_t kMaximumCollectionIdBytes = 128;
constexpr std::size_t kMaximumCollectionNameBytes = 128;
constexpr std::size_t kMaximumNoteBytes = 4096;
constexpr std::size_t kMaximumQueryBytes = 256;
constexpr std::size_t kMaximumScopeBytes = 128;
constexpr std::size_t kMaximumSortBytes = 32;
constexpr std::size_t kMaximumFilterBytes = 32;

std::string Path(std::uint64_t account_id) {
  return "organization-" + std::to_string(account_id) + ".v1";
}

template <typename T>
Result<T> InvalidValue(const char* reason) {
  return Result<T>::Failure(StorageError::kInvalidData,
                            std::string("organization data is invalid: ") + reason);
}

Result<void> Invalid(const char* reason) {
  return InvalidValue<void>(reason);
}

bool ValidUtf8(const std::string& value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0) return false;
    if (first < 0x80) {
      ++index;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (count > value.size() - index) return false;
    for (std::size_t offset = 1; offset < count; ++offset) {
      const auto continuation = static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      return false;
    }
    index += count;
  }
  return true;
}

bool ValidText(const std::string& value, std::size_t maximum, bool allow_empty) {
  return (allow_empty || !value.empty()) && value.size() <= maximum && ValidUtf8(value);
}

bool ValidCollectionId(const std::string& value) {
  if (value.empty() || value.size() > kMaximumCollectionIdBytes) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
  });
}

bool CanonicalCollectionName(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  return first == 0 && value.find_last_not_of(" \t\r\n") == value.size() - 1;
}

bool BuiltInScope(const std::string& value) {
  return value == "all" || value == "favorites" || value == "hidden";
}

bool ReservedCollectionId(const std::string& value) {
  return BuiltInScope(value) || value == "installed" || value == "new-collection";
}

bool ValidSort(const std::string& value) {
  return value == "name" || value == "recent-played" || value == "recent-added" ||
         value == "playtime";
}

bool ValidFilter(const std::string& value) {
  return value == "compat-playable";
}

bool AddGameId(std::set<GameId>& all_ids, GameId id) {
  if (id == 0) return false;
  all_ids.insert(id);
  return all_ids.size() <= kMaximumGameIds;
}

bool Validate(const OrganizationSnapshot& snapshot) {
  if (snapshot.schema_version != kVersion ||
      snapshot.collections.size() > kMaximumCollections ||
      !ValidText(snapshot.query, kMaximumQueryBytes, true) ||
      !ValidText(snapshot.scope, kMaximumScopeBytes, false) ||
      !ValidText(snapshot.sort, kMaximumSortBytes, false) ||
      !ValidSort(snapshot.sort)) {
    return false;
  }

  std::set<GameId> game_ids;
  for (GameId id : snapshot.favorites) if (!AddGameId(game_ids, id)) return false;
  for (GameId id : snapshot.hidden) if (!AddGameId(game_ids, id)) return false;
  // Selection is one fixed-width scalar, independent of the annotation/member ID budget.

  std::set<std::string> collection_ids;
  std::set<std::string> collection_names;
  for (const auto& collection : snapshot.collections) {
    if (!ValidCollectionId(collection.id) || ReservedCollectionId(collection.id) ||
        !collection_ids.insert(collection.id).second ||
        !ValidText(collection.name, kMaximumCollectionNameBytes, false) ||
        !CanonicalCollectionName(collection.name) ||
        !collection_names.insert(collection.name).second ||
        collection.members.size() > kMaximumGameIds) {
      return false;
    }
    for (GameId id : collection.members) if (!AddGameId(game_ids, id)) return false;
  }
  // "installed" is the Library rail of installed games; it is a scope, never a collection ID.
  if (!BuiltInScope(snapshot.scope) && snapshot.scope != "installed" && !collection_ids.count(snapshot.scope))
    return false;

  if (snapshot.ratings.size() > kMaximumGameIds || snapshot.notes.size() > kMaximumGameIds)
    return false;
  for (const auto& rating : snapshot.ratings) {
    if (!AddGameId(game_ids, rating.first) ||
        static_cast<std::uint8_t>(rating.second) >
            static_cast<std::uint8_t>(CompatibilityRating::kBroken)) {
      return false;
    }
  }
  for (const auto& note : snapshot.notes) {
    if (!AddGameId(game_ids, note.first) ||
        !ValidText(note.second, kMaximumNoteBytes, true)) {
      return false;
    }
  }
  if (snapshot.filters.size() > 1) return false;
  for (const auto& filter : snapshot.filters) {
    if (!ValidText(filter, kMaximumFilterBytes, false) || !ValidFilter(filter)) return false;
  }
  return true;
}

std::uint32_t Checksum(const Bytes& bytes, std::size_t size) {
  std::uint32_t value = 2166136261u;
  for (std::size_t index = 0; index < size; ++index) {
    value ^= bytes[index];
    value *= 16777619u;
  }
  return value;
}

class Writer {
 public:
  bool U8(std::uint8_t value) { return Append(&value, 1); }
  bool U16(std::uint16_t value) { return Integer(value, 2); }
  bool U32(std::uint32_t value) { return Integer(value, 4); }
  bool U64(std::uint64_t value) { return Integer(value, 8); }
  bool String(const std::string& value) {
    return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
           U32(static_cast<std::uint32_t>(value.size())) &&
           Append(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  }
  bool Raw(const std::uint8_t* bytes, std::size_t size) { return Append(bytes, size); }
  Bytes Take() { return std::move(bytes_); }

 private:
  bool Append(const std::uint8_t* bytes, std::size_t size) {
    constexpr std::size_t kChecksumBytes = 4;
    if (size > kMaximumSnapshotBytes - kChecksumBytes - bytes_.size()) return false;
    bytes_.insert(bytes_.end(), bytes, bytes + size);
    return true;
  }
  bool Integer(std::uint64_t value, unsigned size) {
    std::uint8_t encoded[8]{};
    for (unsigned index = 0; index < size; ++index) {
      encoded[index] = static_cast<std::uint8_t>(value);
      value >>= 8;
    }
    return Append(encoded, size);
  }
  Bytes bytes_;
};

class Reader {
 public:
  Reader(const Bytes& bytes, std::size_t offset, std::size_t limit)
      : bytes_(bytes), offset_(offset), limit_(limit) {}
  bool U8(std::uint8_t& value) { return Integer(value, 1); }
  bool U16(std::uint16_t& value) { return Integer(value, 2); }
  bool U32(std::uint32_t& value) { return Integer(value, 4); }
  bool U64(std::uint64_t& value) { return Integer(value, 8); }
  bool String(std::string& value, std::size_t maximum) {
    std::uint32_t size = 0;
    if (!U32(size) || size > maximum || size > Remaining()) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  std::size_t offset() const { return offset_; }
  std::size_t limit() const { return limit_; }

 private:
  std::size_t Remaining() const {
    return offset_ <= limit_ ? limit_ - offset_ : 0;
  }
  template <typename Value>
  bool Integer(Value& value, unsigned size) {
    if (size > Remaining()) return false;
    value = 0;
    for (unsigned index = 0; index < size; ++index)
      value |= static_cast<Value>(bytes_[offset_++]) << (8 * index);
    return true;
  }
  const Bytes& bytes_;
  std::size_t offset_;
  std::size_t limit_;
};

bool WriteIds(Writer& writer, const std::set<GameId>& ids) {
  if (!writer.U32(static_cast<std::uint32_t>(ids.size()))) return false;
  for (GameId id : ids) if (!writer.U32(id)) return false;
  return true;
}

bool ReadIds(Reader& reader, std::set<GameId>& ids) {
  std::uint32_t count = 0;
  if (!reader.U32(count) || count > kMaximumGameIds) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t id = 0;
    if (!reader.U32(id) || id == 0 || !ids.insert(id).second) return false;
  }
  return true;
}

}  // namespace

bool operator==(const OrganizationCollection& left,
                const OrganizationCollection& right) {
  return left.id == right.id && left.name == right.name &&
         left.pinned == right.pinned && left.members == right.members;
}

bool operator==(const OrganizationSnapshot& left,
                const OrganizationSnapshot& right) {
  return left.schema_version == right.schema_version &&
         left.favorites == right.favorites && left.hidden == right.hidden &&
         left.collections == right.collections && left.ratings == right.ratings &&
         left.notes == right.notes && left.query == right.query &&
         left.scope == right.scope && left.sort == right.sort &&
         left.filters == right.filters &&
         left.selected_game_id == right.selected_game_id;
}

bool OrganizationSnapshotIsValid(const OrganizationSnapshot& snapshot) {
  if(!Validate(snapshot)) return false;
  // Version 1: header, all count/string prefixes, and trailing checksum.
  std::size_t bytes=62 + snapshot.query.size() + snapshot.scope.size() + snapshot.sort.size();
  bytes += 4*(snapshot.favorites.size()+snapshot.hidden.size());
  bytes += 5*snapshot.ratings.size();
  for(const auto& [id,note]:snapshot.notes) { (void)id;bytes+=8+note.size(); }
  for(const auto& collection:snapshot.collections)
    bytes += 13+collection.id.size()+collection.name.size()+4*collection.members.size();
  for(const auto& filter:snapshot.filters) bytes+=4+filter.size();
  return bytes<=kMaximumSnapshotBytes;
}

Result<void> OrganizationStore::Save(
    std::uint64_t account_id, const OrganizationSnapshot& snapshot) const {
  if (account_id == 0) return Invalid("account identifier");
  if (!OrganizationSnapshotIsValid(snapshot)) return Invalid("snapshot fields or encoded size");
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return ready;

  Writer writer;
  bool okay = writer.Raw(kMagic.data(), kMagic.size()) && writer.U16(kVersion) &&
              writer.U64(account_id) && writer.U32(snapshot.selected_game_id) &&
              WriteIds(writer, snapshot.favorites) && WriteIds(writer, snapshot.hidden) &&
              writer.U32(static_cast<std::uint32_t>(snapshot.collections.size()));
  for (const auto& collection : snapshot.collections) {
    okay = okay && writer.String(collection.id) && writer.String(collection.name) &&
           writer.U8(collection.pinned ? 1 : 0) && WriteIds(writer, collection.members);
  }
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.ratings.size()));
  for (const auto& rating : snapshot.ratings) {
    okay = okay && writer.U32(rating.first) &&
           writer.U8(static_cast<std::uint8_t>(rating.second));
  }
  okay = okay && writer.U32(static_cast<std::uint32_t>(snapshot.notes.size()));
  for (const auto& note : snapshot.notes)
    okay = okay && writer.U32(note.first) && writer.String(note.second);
  okay = okay && writer.String(snapshot.query) && writer.String(snapshot.scope) &&
         writer.String(snapshot.sort) &&
         writer.U32(static_cast<std::uint32_t>(snapshot.filters.size()));
  for (const auto& filter : snapshot.filters) okay = okay && writer.String(filter);
  if (!okay) return Invalid("snapshot exceeds one MiB limit");

  Bytes encoded = writer.Take();
  const std::uint32_t checksum = Checksum(encoded, encoded.size());
  for (unsigned shift = 0; shift < 32; shift += 8)
    encoded.push_back(static_cast<std::uint8_t>(checksum >> shift));
  return storage_.WriteAtomicPrivate(Path(account_id), encoded);
}

Result<std::optional<OrganizationSnapshot>> OrganizationStore::Load(
    std::uint64_t account_id) const {
  if (account_id == 0)
    return InvalidValue<std::optional<OrganizationSnapshot>>("account identifier");
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return Result<std::optional<OrganizationSnapshot>>::Failure(ready.error(),ready.message());
  const auto loaded = storage_.Read(Path(account_id), kMaximumSnapshotBytes);
  if (!loaded.ok()) {
    if (loaded.error() == StorageError::kNotFound)
      return Result<std::optional<OrganizationSnapshot>>::Success(std::nullopt);
    return Result<std::optional<OrganizationSnapshot>>::Failure(loaded.error(),
                                                                 loaded.message());
  }
  const Bytes& bytes = loaded.value();
  constexpr std::size_t kFixedBytes = kMagic.size() + 2 + 8 + 4 + 4;
  if (bytes.size() < kFixedBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return InvalidValue<std::optional<OrganizationSnapshot>>("header");
  }
  const std::size_t payload_limit = bytes.size() - 4;
  Reader checksum_reader(bytes, payload_limit, bytes.size());
  std::uint32_t stored_checksum = 0;
  if (!checksum_reader.U32(stored_checksum) ||
      stored_checksum != Checksum(bytes, payload_limit)) {
    return InvalidValue<std::optional<OrganizationSnapshot>>("checksum");
  }

  Reader reader(bytes, kMagic.size(), payload_limit);
  std::uint16_t version = 0;
  std::uint64_t embedded_account = 0;
  OrganizationSnapshot snapshot;
  if (!reader.U16(version) || version != kVersion ||
      !reader.U64(embedded_account) || embedded_account != account_id ||
      !reader.U32(snapshot.selected_game_id) ||
      !ReadIds(reader, snapshot.favorites) || !ReadIds(reader, snapshot.hidden)) {
    return InvalidValue<std::optional<OrganizationSnapshot>>("identity or fields");
  }
  snapshot.schema_version = version;

  std::uint32_t collection_count = 0;
  if (!reader.U32(collection_count) || collection_count > kMaximumCollections)
    return InvalidValue<std::optional<OrganizationSnapshot>>("collection count");
  snapshot.collections.reserve(collection_count);
  for (std::uint32_t index = 0; index < collection_count; ++index) {
    OrganizationCollection collection;
    std::uint8_t pinned = 0;
    if (!reader.String(collection.id, kMaximumCollectionIdBytes) ||
        !reader.String(collection.name, kMaximumCollectionNameBytes) ||
        !reader.U8(pinned) || pinned > 1 || !ReadIds(reader, collection.members)) {
      return InvalidValue<std::optional<OrganizationSnapshot>>("collection");
    }
    collection.pinned = pinned != 0;
    snapshot.collections.push_back(std::move(collection));
  }

  std::uint32_t rating_count = 0;
  if (!reader.U32(rating_count) || rating_count > kMaximumGameIds)
    return InvalidValue<std::optional<OrganizationSnapshot>>("rating count");
  for (std::uint32_t index = 0; index < rating_count; ++index) {
    std::uint32_t id = 0;
    std::uint8_t rating = 0;
    if (!reader.U32(id) || id == 0 || !reader.U8(rating) ||
        rating > static_cast<std::uint8_t>(CompatibilityRating::kBroken) ||
        !snapshot.ratings.emplace(id, static_cast<CompatibilityRating>(rating)).second) {
      return InvalidValue<std::optional<OrganizationSnapshot>>("rating");
    }
  }

  std::uint32_t note_count = 0;
  if (!reader.U32(note_count) || note_count > kMaximumGameIds)
    return InvalidValue<std::optional<OrganizationSnapshot>>("note count");
  for (std::uint32_t index = 0; index < note_count; ++index) {
    std::uint32_t id = 0;
    std::string note;
    if (!reader.U32(id) || id == 0 || !reader.String(note, kMaximumNoteBytes) ||
        !snapshot.notes.emplace(id, std::move(note)).second) {
      return InvalidValue<std::optional<OrganizationSnapshot>>("note");
    }
  }

  std::uint32_t filter_count = 0;
  if (!reader.String(snapshot.query, kMaximumQueryBytes) ||
      !reader.String(snapshot.scope, kMaximumScopeBytes) ||
      !reader.String(snapshot.sort, kMaximumSortBytes) ||
      !reader.U32(filter_count) || filter_count > 1) {
    return InvalidValue<std::optional<OrganizationSnapshot>>("view state");
  }
  for (std::uint32_t index = 0; index < filter_count; ++index) {
    std::string filter;
    if (!reader.String(filter, kMaximumFilterBytes) ||
        !snapshot.filters.insert(std::move(filter)).second) {
      return InvalidValue<std::optional<OrganizationSnapshot>>("filter");
    }
  }
  if (reader.offset() != reader.limit() || !Validate(snapshot))
    return InvalidValue<std::optional<OrganizationSnapshot>>("trailing or semantic data");
  return Result<std::optional<OrganizationSnapshot>>::Success(std::move(snapshot));
}

Result<void> OrganizationStore::Clear(std::uint64_t account_id) const {
  if (account_id == 0) return Invalid("account identifier");
  const std::string path = Path(account_id);
  const auto final_result = storage_.Remove(path);
  const auto temporary_result = storage_.Remove(path + ".tmp");
  if (!final_result.ok()) return final_result;
  if (!temporary_result.ok()) return temporary_result;
  return Result<void>::Success();
}

}  // namespace z3cloudplay
